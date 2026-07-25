//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/analysis/IntRangeAnalysis.cpp
// Purpose: Forward value-range dataflow over IL SSA with bounded widening.
// Key invariants:
//   - Every recorded range is a sound over-approximation of the values a temp
//     can hold at that program point.
//   - Post-conditions of trapping checks (idx.chk, narrowing casts) are only
//     recorded for the fall-through continuation, which is the only path on
//     which execution proceeds.
// Ownership/Lifetime:
//   - Pure computation; the returned IntRangeInfo owns all of its storage.
// Links: il/analysis/IntRangeAnalysis.hpp, il/transform/CheckOpt.cpp, il/verify/InstructionChecker.cpp
//
//===----------------------------------------------------------------------===//

#include "il/analysis/IntRangeAnalysis.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace zanna::analysis {

using ::il::utils::addRanges;
using ::il::utils::exactRange;
using ::il::utils::intersectRanges;
using ::il::utils::IntRange;
using ::il::utils::mergeIncomingRange;
using ::il::utils::mulRanges;
using ::il::utils::subRanges;

/// @brief Resolve a constant or SSA temporary to its current interval.
/// @param value Operand whose integer range is requested.
/// @param ranges Current temporary facts.
/// @return Exact constant range, mapped temporary range, or no value when unknown.
std::optional<IntRange> rangeForValue(const Value &value, const RangeMap &ranges) {
    if (value.kind == Value::Kind::ConstInt)
        return exactRange(value.i64);
    if (value.kind != Value::Kind::Temp)
        return std::nullopt;
    auto it = ranges.find(value.id);
    if (it == ranges.end())
        return std::nullopt;
    return it->second;
}

/// @brief Derive a signed interval refinement from a compare-controlled edge.
/// @param cmp Compare instruction with one temporary and one integer constant.
/// @param branchIndex Zero for the true edge and one for the false edge.
/// @param constrainedValue Receives the temporary constrained by the comparison.
/// @param range Receives the inclusive edge-specific interval.
/// @return True when the comparison and edge imply a representable range fact.
bool deriveCompareBranchRange(const Instr &cmp,
                              size_t branchIndex,
                              Value &constrainedValue,
                              IntRange &range) {
    if (cmp.operands.size() != 2)
        return false;

    Opcode op = cmp.op;
    Value variable;
    int64_t constant = 0;

    if (cmp.operands[0].kind == Value::Kind::Temp &&
        cmp.operands[1].kind == Value::Kind::ConstInt) {
        variable = cmp.operands[0];
        constant = cmp.operands[1].i64;
    } else if (cmp.operands[0].kind == Value::Kind::ConstInt &&
               cmp.operands[1].kind == Value::Kind::Temp) {
        variable = cmp.operands[1];
        constant = cmp.operands[0].i64;
        switch (op) {
            case Opcode::SCmpLT:
                op = Opcode::SCmpGT;
                break;
            case Opcode::SCmpLE:
                op = Opcode::SCmpGE;
                break;
            case Opcode::SCmpGT:
                op = Opcode::SCmpLT;
                break;
            case Opcode::SCmpGE:
                op = Opcode::SCmpLE;
                break;
            default:
                break;
        }
    } else {
        return false;
    }

    if (variable.kind != Value::Kind::Temp)
        return false;

    IntRange fact;
    const bool trueBranch = branchIndex == 0;
    switch (op) {
        case Opcode::SCmpLT:
            if (trueBranch) {
                if (constant == std::numeric_limits<int64_t>::min())
                    return false;
                fact.upper = constant - 1;
            } else {
                fact.lower = constant;
            }
            break;
        case Opcode::SCmpLE:
            if (trueBranch) {
                fact.upper = constant;
            } else {
                if (constant == std::numeric_limits<int64_t>::max())
                    return false;
                fact.lower = constant + 1;
            }
            break;
        case Opcode::SCmpGT:
            if (trueBranch) {
                if (constant == std::numeric_limits<int64_t>::max())
                    return false;
                fact.lower = constant + 1;
            } else {
                fact.upper = constant;
            }
            break;
        case Opcode::SCmpGE:
            if (trueBranch) {
                fact.lower = constant;
            } else {
                if (constant == std::numeric_limits<int64_t>::min())
                    return false;
                fact.upper = constant - 1;
            }
            break;
        case Opcode::ICmpEq:
            if (!trueBranch)
                return false;
            fact = exactRange(constant);
            break;
        default:
            return false;
    }

    constrainedValue = variable;
    range = fact;
    return true;
}

namespace {

/// @brief Inclusive value range of a narrowing-cast target type.
/// @param kind Target integer type kind.
/// @param isUnsigned Whether to use the target's unsigned domain.
/// @return Inclusive target range for supported I16/I32 casts, otherwise no value.
std::optional<IntRange> narrowTypeRange(Type::Kind kind, bool isUnsigned) {
    switch (kind) {
        case Type::Kind::I16:
            return isUnsigned ? IntRange{0, 65535} : IntRange{-32768, 32767};
        case Type::Kind::I32:
            return isUnsigned ? IntRange{0, 4294967295LL}
                              : IntRange{std::numeric_limits<int32_t>::min(),
                                         std::numeric_limits<int32_t>::max()};
        default:
            return std::nullopt;
    }
}

/// @brief Intersect a temp's recorded range in place with a new fact.
/// @param ranges Mutable temporary facts.
/// @param value Temporary to refine; non-temporary values are ignored.
/// @param fact New inclusive constraint.
void refineTemp(RangeMap &ranges, const Value &value, const IntRange &fact) {
    if (value.kind != Value::Kind::Temp)
        return;
    auto it = ranges.find(value.id);
    if (it == ranges.end()) {
        ranges[value.id] = fact;
        return;
    }
    if (auto tighter = intersectRanges(it->second, fact))
        it->second = *tighter;
}

/// @brief True when the compare-family opcode always yields a 0/1 result.
/// @param op Opcode to classify.
/// @return True for integer/floating comparisons and one-bit extend/truncate operations.
bool producesBool(Opcode op) {
    switch (op) {
        case Opcode::SCmpLT:
        case Opcode::SCmpLE:
        case Opcode::SCmpGT:
        case Opcode::SCmpGE:
        case Opcode::UCmpLT:
        case Opcode::UCmpLE:
        case Opcode::UCmpGT:
        case Opcode::UCmpGE:
        case Opcode::ICmpEq:
        case Opcode::ICmpNe:
        case Opcode::FCmpEQ:
        case Opcode::FCmpNE:
        case Opcode::FCmpLT:
        case Opcode::FCmpLE:
        case Opcode::FCmpGT:
        case Opcode::FCmpGE:
        case Opcode::FCmpOrd:
        case Opcode::FCmpUno:
        case Opcode::Zext1:
        case Opcode::Trunc1:
            return true;
        default:
            return false;
    }
}

} // namespace

/// @brief Apply one instruction's integer-range transfer and check postconditions.
/// @param instr Instruction to interpret.
/// @param ranges Mutable facts before the instruction; updated to the fall-through state.
/// @return Computed result interval when sufficient operand/type facts are available.
std::optional<IntRange> applyRangeTransfer(const Instr &instr, RangeMap &ranges) {
    std::optional<IntRange> result;
    // For add/sub, `stored` differs from `result`: addRanges/subRanges treat a
    // missing bound as the int64 extreme, so a partially-bounded input yields
    // pseudo-bounds like INT64_MIN+k. Those are TRUE (they prove the checked
    // op cannot trap, which is what the returned `result` communicates), but
    // recording them would make loop-carried bounds creep by a constant every
    // fixpoint sweep and prevent convergence. The recorded state therefore
    // keeps a bound only when both contributing operand sides were bounded.
    std::optional<IntRange> stored;
    bool storedIsResult = true;

    if (producesBool(instr.op)) {
        result = IntRange{0, 1};
    } else if (instr.op == Opcode::Select && instr.operands.size() == 3) {
        // The result is one of the two arms, so its range is the union of
        // the arm ranges. Operand 0 is the condition and contributes nothing.
        auto tRange = rangeForValue(instr.operands[1], ranges);
        auto fRange = rangeForValue(instr.operands[2], ranges);
        if (tRange && fRange) {
            IntRange unionRange{};
            if (tRange->lower && fRange->lower)
                unionRange.lower = std::min(*tRange->lower, *fRange->lower);
            if (tRange->upper && fRange->upper)
                unionRange.upper = std::max(*tRange->upper, *fRange->upper);
            if (unionRange.lower || unionRange.upper)
                result = unionRange;
        }
    } else if (instr.operands.size() >= 2) {
        auto lhs = rangeForValue(instr.operands[0], ranges);
        auto rhs = rangeForValue(instr.operands[1], ranges);
        const Value &lhsValue = instr.operands[0];
        const Value &rhsValue = instr.operands[1];
        switch (instr.op) {
            case Opcode::Add:
            case Opcode::IAddOvf:
                if (lhs && rhs) {
                    // `result` is the trap-freedom proof (nullopt when the
                    // extreme-endpoint arithmetic could overflow). The stored
                    // state is computed per side from the REAL bounds only:
                    // checked ops trap instead of wrapping and plain ops are
                    // verifier-proven non-wrapping, so each independently
                    // supported bound is sound even when the other side (and
                    // hence the proof) is unavailable.
                    result = addRanges(*lhs, *rhs);
                    storedIsResult = false;
                    stored = IntRange{};
                    if (lhs->lower && rhs->lower)
                        stored->lower = ::il::utils::addCheckedValue(*lhs->lower, *rhs->lower);
                    if (lhs->upper && rhs->upper)
                        stored->upper = ::il::utils::addCheckedValue(*lhs->upper, *rhs->upper);
                    if (!stored->lower && !stored->upper)
                        stored.reset();
                }
                break;
            case Opcode::Sub:
            case Opcode::ISubOvf:
                if (lhs && rhs) {
                    result = subRanges(*lhs, *rhs);
                    storedIsResult = false;
                    stored = IntRange{};
                    if (lhs->lower && rhs->upper)
                        stored->lower = ::il::utils::subCheckedValue(*lhs->lower, *rhs->upper);
                    if (lhs->upper && rhs->lower)
                        stored->upper = ::il::utils::subCheckedValue(*lhs->upper, *rhs->lower);
                    if (!stored->lower && !stored->upper)
                        stored.reset();
                }
                break;
            case Opcode::Mul:
            case Opcode::IMulOvf:
                // mulRanges requires both sides fully bounded, so its result
                // never contains placeholder-derived bounds.
                if (lhs && rhs)
                    result = mulRanges(*lhs, *rhs);
                break;
            case Opcode::And:
                if (rhsValue.kind == Value::Kind::ConstInt && rhsValue.i64 >= 0)
                    result = IntRange{0, rhsValue.i64};
                else if (lhsValue.kind == Value::Kind::ConstInt && lhsValue.i64 >= 0)
                    result = IntRange{0, lhsValue.i64};
                break;
            case Opcode::LShr:
                if (rhsValue.kind == Value::Kind::ConstInt && rhsValue.i64 > 0 &&
                    rhsValue.i64 < 64) {
                    const uint64_t max = ~0ULL >> static_cast<uint64_t>(rhsValue.i64);
                    result = IntRange{0, static_cast<int64_t>(max)};
                }
                break;
            case Opcode::AShr:
                if (rhsValue.kind == Value::Kind::ConstInt && rhsValue.i64 >= 0 &&
                    rhsValue.i64 < 64 && lhs && lhs->lower && lhs->upper) {
                    result = IntRange{*lhs->lower >> rhsValue.i64, *lhs->upper >> rhsValue.i64};
                }
                break;
            case Opcode::Shl:
                if (rhsValue.kind == Value::Kind::ConstInt && rhsValue.i64 >= 0 &&
                    rhsValue.i64 < 63 && lhs && lhs->lower && lhs->upper && *lhs->lower >= 0 &&
                    *lhs->upper <= (std::numeric_limits<int64_t>::max() >> rhsValue.i64)) {
                    result = IntRange{*lhs->lower << rhsValue.i64, *lhs->upper << rhsValue.i64};
                }
                break;
            case Opcode::UDivChk0:
            case Opcode::UDiv:
                // Unsigned quotient by a constant >= 2 fits in the non-negative
                // signed range regardless of the dividend's bit pattern.
                if (rhsValue.kind == Value::Kind::ConstInt && rhsValue.i64 >= 2) {
                    const uint64_t max = ~0ULL / static_cast<uint64_t>(rhsValue.i64);
                    result = IntRange{0, static_cast<int64_t>(max)};
                }
                break;
            case Opcode::URemChk0:
            case Opcode::URem:
                if (rhsValue.kind == Value::Kind::ConstInt && rhsValue.i64 > 0)
                    result = IntRange{0, rhsValue.i64 - 1};
                break;
            case Opcode::SRemChk0:
            case Opcode::SRem:
                if (rhsValue.kind == Value::Kind::ConstInt &&
                    rhsValue.i64 != std::numeric_limits<int64_t>::min() && rhsValue.i64 != 0) {
                    const int64_t mag = rhsValue.i64 < 0 ? -rhsValue.i64 : rhsValue.i64;
                    if (lhs && lhs->lower && *lhs->lower >= 0)
                        result = IntRange{0, mag - 1};
                    else
                        result = IntRange{-(mag - 1), mag - 1};
                }
                break;
            case Opcode::SDivChk0:
            case Opcode::SDiv:
                if (rhsValue.kind == Value::Kind::ConstInt && rhsValue.i64 > 0 && lhs &&
                    lhs->lower && lhs->upper) {
                    result = IntRange{*lhs->lower / rhsValue.i64, *lhs->upper / rhsValue.i64};
                }
                break;
            case Opcode::IdxChk:
                // idx.chk idx, lo, hi traps unless lo <= idx < hi and returns the
                // normalized index idx - lo. On fall-through both facts hold.
                if (instr.operands.size() >= 3 &&
                    instr.operands[1].kind == Value::Kind::ConstInt &&
                    instr.operands[2].kind == Value::Kind::ConstInt) {
                    const int64_t lo = instr.operands[1].i64;
                    const int64_t hi = instr.operands[2].i64;
                    if (lo < hi) {
                        result = IntRange{0, hi - lo - 1};
                        refineTemp(ranges, instr.operands[0], IntRange{lo, hi - 1});
                    }
                }
                break;
            default:
                break;
        }
    } else if (instr.operands.size() == 1) {
        switch (instr.op) {
            case Opcode::CastSiNarrowChk:
            case Opcode::CastUiNarrowChk: {
                const bool isUnsigned = instr.op == Opcode::CastUiNarrowChk;
                auto typeRange = narrowTypeRange(instr.type.kind, isUnsigned);
                if (typeRange) {
                    result = typeRange;
                    if (auto operand = rangeForValue(instr.operands[0], ranges))
                        if (auto tighter = intersectRanges(*operand, *typeRange))
                            result = tighter;
                    // The operand itself is known in-range after the check passes.
                    refineTemp(ranges, instr.operands[0], *typeRange);
                }
                break;
            }
            default:
                break;
        }
    }

    if (instr.result) {
        const std::optional<IntRange> &toStore = storedIsResult ? result : stored;
        if (toStore)
            ranges[*instr.result] = *toStore;
        else
            ranges.erase(*instr.result);
    }
    return result;
}

/// @brief Recover the bound of a lowered signed power-of-two remainder idiom.
/// @param block Block containing the complete straight-line defining chain.
/// @param subInstr Candidate outer subtract.
/// @param ranges Facts available before @p subInstr.
/// @return Symmetric remainder interval on an exact safe match, otherwise no value.
std::optional<IntRange> matchPow2ModuloRange(const BasicBlock &block, const Instr &subInstr,
                                             const RangeMap &ranges) {
    if (subInstr.op != Opcode::Sub && subInstr.op != Opcode::ISubOvf)
        return std::nullopt;
    if (subInstr.operands.size() != 2)
        return std::nullopt;

    // Straight-line definition of a temp earlier in this block.
    auto defBefore = [&](unsigned id) -> const Instr * {
        for (const Instr &instr : block.instructions) {
            if (&instr == &subInstr)
                break;
            if (instr.result && *instr.result == id)
                return &instr;
        }
        return nullptr;
    };
    auto sameTemp = [](const Value &a, const Value &b) {
        return a.kind == Value::Kind::Temp && b.kind == Value::Kind::Temp && a.id == b.id;
    };

    const Value &z = subInstr.operands[0];
    const Value &aligned = subInstr.operands[1];
    if (aligned.kind != Value::Kind::Temp)
        return std::nullopt;

    // %aligned = and %biased, -M   (mask is the negation of a power of two)
    const Instr *andInstr = defBefore(aligned.id);
    if (!andInstr || andInstr->op != Opcode::And || andInstr->operands.size() != 2)
        return std::nullopt;
    const Value *biased = nullptr;
    int64_t mask = 0;
    if (andInstr->operands[0].kind == Value::Kind::ConstInt &&
        andInstr->operands[1].kind == Value::Kind::Temp) {
        mask = andInstr->operands[0].i64;
        biased = &andInstr->operands[1];
    } else if (andInstr->operands[1].kind == Value::Kind::ConstInt &&
               andInstr->operands[0].kind == Value::Kind::Temp) {
        mask = andInstr->operands[1].i64;
        biased = &andInstr->operands[0];
    }
    if (!biased || mask >= 0 || mask == std::numeric_limits<int64_t>::min())
        return std::nullopt;
    const int64_t mag = -mask;  // 2^k
    if ((mag & (mag - 1)) != 0)  // require an exact power of two
        return std::nullopt;

    // %biased = add %z, %bias   (commutative; one addend must be the minuend Z)
    const Instr *addInstr = defBefore(biased->id);
    if (!addInstr || (addInstr->op != Opcode::Add && addInstr->op != Opcode::IAddOvf) ||
        addInstr->operands.size() != 2)
        return std::nullopt;
    const Value *bias = nullptr;
    if (sameTemp(addInstr->operands[0], z))
        bias = &addInstr->operands[1];
    else if (sameTemp(addInstr->operands[1], z))
        bias = &addInstr->operands[0];
    if (!bias)
        return std::nullopt;

    // The bias must be confined to [0, mag - 1]: the sign correction that turns
    // an arithmetic-shift quotient into a truncated remainder never exceeds the
    // low-bit mask. With that, `%z - ((%z + bias) & -mag)` lies in [-B, mag-1-B]
    // for every %z, hence in [-(mag-1), mag-1].
    auto biasRange = rangeForValue(*bias, ranges);
    if (!biasRange || !biasRange->lower || !biasRange->upper)
        return std::nullopt;
    if (*biasRange->lower < 0 || *biasRange->upper > mag - 1)
        return std::nullopt;

    return IntRange{-(mag - 1), mag - 1};
}

namespace {

/// @brief Compute the fact map carried by one CFG edge.
/// @param pred Source block whose exit state is @p outState.
/// @param term Terminator of @p pred.
/// @param branchIndex Index of the edge within the terminator's label list.
/// @param target Destination block (for param binding).
/// @param outState Exit range state of @p pred.
/// @return Edge-refined facts with destination parameters rebound.
RangeMap edgeFacts(const BasicBlock &pred,
                   const Instr &term,
                   size_t branchIndex,
                   const BasicBlock &target,
                   const RangeMap &outState) {
    RangeMap facts = outState;

    // Branch-condition refinement for conditional branches.
    if (term.op == Opcode::CBr && term.operands.size() == 1 &&
        term.operands.front().kind == Value::Kind::Temp) {
        const unsigned condId = term.operands.front().id;
        const Instr *cmp = nullptr;
        for (const auto &instr : pred.instructions) {
            if (instr.result && *instr.result == condId) {
                cmp = &instr;
                break;
            }
        }
        if (cmp) {
            Value constrained;
            IntRange range;
            if (deriveCompareBranchRange(*cmp, branchIndex, constrained, range)) {
                auto it = facts.find(constrained.id);
                if (it != facts.end()) {
                    if (auto tighter = intersectRanges(it->second, range))
                        it->second = *tighter;
                } else {
                    facts[constrained.id] = range;
                }
            }
        }
    }

    // Bind branch arguments to the target's block params. Every param is
    // explicitly bound or erased so no stale fact from a previous loop
    // iteration survives on the back edge.
    if (branchIndex < term.brArgs.size()) {
        const auto &args = term.brArgs[branchIndex];
        for (size_t i = 0; i < target.params.size(); ++i) {
            const unsigned paramId = target.params[i].id;
            if (i < args.size()) {
                if (auto range = rangeForValue(args[i], facts)) {
                    facts[paramId] = *range;
                    continue;
                }
            }
            facts.erase(paramId);
        }
    } else {
        for (const auto &param : target.params)
            facts.erase(param.id);
    }

    return facts;
}

/// @brief Compare both optional endpoints of two intervals.
/// @param lhs First interval.
/// @param rhs Second interval.
/// @return True when lower and upper bounds are identical.
bool rangesEqual(const IntRange &lhs, const IntRange &rhs) {
    return lhs.lower == rhs.lower && lhs.upper == rhs.upper;
}

/// @brief Compare two temporary-range maps by key and interval.
/// @param lhs First map.
/// @param rhs Second map.
/// @return True when both maps contain exactly the same facts.
bool mapsEqual(const RangeMap &lhs, const RangeMap &rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (const auto &[id, range] : lhs) {
        auto it = rhs.find(id);
        if (it == rhs.end() || !rangesEqual(range, it->second))
            return false;
    }
    return true;
}

/// @brief Merge @p incoming into an accumulated pointwise union in place.
/// @details Keys absent from either side are unknown at the join and removed.
///          Mutating the accumulator avoids allocating and copying a complete
///          RangeMap for every predecessor beyond the first.
/// @param accumulator Existing join facts, replaced with the pointwise union.
/// @param incoming Facts from one additional predecessor edge.
void mergeMapInto(RangeMap &accumulator, const RangeMap &incoming) {
    for (auto it = accumulator.begin(); it != accumulator.end();) {
        auto incomingIt = incoming.find(it->first);
        if (incomingIt == incoming.end()) {
            it = accumulator.erase(it);
            continue;
        }
        if (auto combined = mergeIncomingRange(it->second, incomingIt->second)) {
            it->second = *combined;
            ++it;
        } else {
            it = accumulator.erase(it);
        }
    }
}

} // namespace

/// @brief Compute conservative block-entry integer ranges to a widened fixpoint.
/// @param fn Function whose label CFG and SSA instructions are analyzed.
/// @return Entry facts for reachable blocks, including bounded narrowing recovery.
IntRangeInfo computeIntRanges(const Function &fn) {
    IntRangeInfo info;
    if (fn.blocks.empty())
        return info;

    const size_t blockCount = fn.blocks.size();
    std::unordered_map<std::string, size_t> indexOf;
    indexOf.reserve(blockCount);
    for (size_t i = 0; i < blockCount; ++i)
        indexOf[fn.blocks[i].label] = i;

    // Reverse post-order over the label CFG so predecessors are usually
    // processed before successors within a sweep. Targets of back edges (loop
    // headers) are recorded as the only widening points: widening anywhere
    // else would erase branch refinements that single-pred blocks legally
    // recover from their (eventually stable) predecessors.
    std::vector<size_t> rpo;
    rpo.reserve(blockCount);
    std::vector<char> isWideningPoint(blockCount, 0);
    {
        std::vector<char> seen(blockCount, 0);
        std::vector<char> onStack(blockCount, 0);
        std::vector<std::pair<size_t, size_t>> stack; // (block, next successor)
        stack.emplace_back(0, 0);
        seen[0] = 1;
        onStack[0] = 1;
        while (!stack.empty()) {
            auto &[blockIdx, succPos] = stack.back();
            const BasicBlock &block = fn.blocks[blockIdx];
            const Instr *term =
                block.instructions.empty() ? nullptr : &block.instructions.back();
            const size_t succCount = term ? term->labels.size() : 0;
            if (succPos < succCount) {
                const std::string &label = term->labels[succPos];
                ++succPos;
                auto it = indexOf.find(label);
                if (it == indexOf.end())
                    continue;
                if (onStack[it->second])
                    isWideningPoint[it->second] = 1;
                if (!seen[it->second]) {
                    seen[it->second] = 1;
                    onStack[it->second] = 1;
                    stack.emplace_back(it->second, 0);
                }
                continue;
            }
            rpo.push_back(blockIdx);
            onStack[blockIdx] = 0;
            stack.pop_back();
        }
        std::reverse(rpo.begin(), rpo.end());
    }

    std::vector<RangeMap> entry(blockCount);
    std::vector<char> reached(blockCount, 0);
    // Per-bound change counters and sticky widening state. Widening decisions
    // are made per (block, temp, side): only the bound that keeps moving is
    // discarded, so a stable lower bound survives an upper bound that creeps
    // by one per sweep (the common counted-loop shape).
    std::vector<std::unordered_map<unsigned, unsigned>> lowerChanges(blockCount);
    std::vector<std::unordered_map<unsigned, unsigned>> upperChanges(blockCount);
    std::vector<std::unordered_set<unsigned>> widenedLower(blockCount);
    std::vector<std::unordered_set<unsigned>> widenedUpper(blockCount);
    reached[0] = 1;

    constexpr unsigned kWidenAfterUpdates = 3;
    // Edge facts flowing into each block, rebuilt every sweep so entries are a
    // FRESH merge over the current in-edges. Merging into the previous entry
    // instead would union in stale history and erode branch refinements.
    std::vector<std::optional<RangeMap>> incoming(blockCount);

    // One dataflow sweep. In widening mode, oscillating bounds at loop headers
    // are stripped (sticky) so the ascent terminates. In narrowing mode the
    // stripping is disabled: entries are recomputed honestly from the
    // converged state, which recovers bounds that stabilized after their
    // widening (monotonicity keeps every narrowing iterate sound).
    auto runSweep = [&](bool widening) -> bool {
        bool changed = false;

        for (auto &facts : incoming)
            facts.reset();

        // Phase A: push every reached block's exit state along its out-edges.
        // Blocks first reached during this sweep must NOT propagate yet: their
        // entry is still empty, and pushing empty-seeded facts would bake
        // missing bounds into successors that later sweeps cannot recover
        // (edge merges drop any key absent on one incoming edge).
        const std::vector<char> canPropagate = reached;
        for (size_t blockIdx : rpo) {
            if (!canPropagate[blockIdx])
                continue;
            const BasicBlock &block = fn.blocks[blockIdx];
            if (block.instructions.empty())
                continue;

            RangeMap out = entry[blockIdx];
            for (const auto &instr : block.instructions) {
                applyRangeTransfer(instr, out);
                // Recover the modulo-by-power-of-two bound the peephole pass
                // hides when it lowers `srem` to bit-twiddling (see
                // matchPow2ModuloRange). Only fills a gap the transfer left.
                if (instr.result && out.find(*instr.result) == out.end())
                    if (auto idiom = matchPow2ModuloRange(block, instr, out))
                        out[*instr.result] = *idiom;
            }

            const Instr &term = block.instructions.back();
            for (size_t branchIndex = 0; branchIndex < term.labels.size(); ++branchIndex) {
                auto succIt = indexOf.find(term.labels[branchIndex]);
                if (succIt == indexOf.end())
                    continue;
                const size_t succIdx = succIt->second;
                RangeMap facts =
                    edgeFacts(block, term, branchIndex, fn.blocks[succIdx], out);
                if (incoming[succIdx])
                    mergeMapInto(*incoming[succIdx], facts);
                else
                    incoming[succIdx] = std::move(facts);
                if (!reached[succIdx]) {
                    reached[succIdx] = 1;
                    changed = true;
                }
            }
        }

        // Phase B: recompute each entry as the merge of its current in-edges.
        for (size_t blockIdx : rpo) {
            // The function entry has an implicit caller edge with no facts, so
            // its entry state is pinned empty regardless of back edges.
            if (blockIdx == 0 || !incoming[blockIdx])
                continue;

            RangeMap fresh = std::move(*incoming[blockIdx]);

            if (widening) {
                // Strip bounds that were widened away in earlier sweeps.
                for (auto it = fresh.begin(); it != fresh.end();) {
                    if (widenedLower[blockIdx].count(it->first))
                        it->second.lower.reset();
                    if (widenedUpper[blockIdx].count(it->first))
                        it->second.upper.reset();
                    if (!it->second.lower && !it->second.upper) {
                        it = fresh.erase(it);
                        continue;
                    }
                    ++it;
                }
            }

            if (mapsEqual(fresh, entry[blockIdx]))
                continue;
            if (const char *dbg = std::getenv("ZANNA_DEBUG_INTRANGES"); dbg && dbg[0] == '2') {
                std::fprintf(stderr, "  [sweep %s] block %s changed:", widening ? "W" : "N",
                             fn.blocks[blockIdx].label.c_str());
                for (const auto &[id, range] : fresh)
                    std::fprintf(stderr, " t%u=[%s,%s]", id,
                                 range.lower ? std::to_string(*range.lower).c_str() : "-inf",
                                 range.upper ? std::to_string(*range.upper).c_str() : "+inf");
                std::fprintf(stderr, "\n");
            }
            if (widening && isWideningPoint[blockIdx]) {
                // Count changes per bound; a bound that moves more than the
                // grace budget is part of an unstable chain and is widened
                // away (sticky) so the ascent terminates. Bounds that stayed
                // stable keep their counters at zero and survive.
                for (auto it = fresh.begin(); it != fresh.end();) {
                    auto old = entry[blockIdx].find(it->first);
                    if (old != entry[blockIdx].end()) {
                        if (it->second.lower != old->second.lower &&
                            ++lowerChanges[blockIdx][it->first] > kWidenAfterUpdates) {
                            widenedLower[blockIdx].insert(it->first);
                            it->second.lower.reset();
                        }
                        if (it->second.upper != old->second.upper &&
                            ++upperChanges[blockIdx][it->first] > kWidenAfterUpdates) {
                            widenedUpper[blockIdx].insert(it->first);
                            it->second.upper.reset();
                        }
                    }
                    if (!it->second.lower && !it->second.upper) {
                        it = fresh.erase(it);
                        continue;
                    }
                    ++it;
                }
                if (mapsEqual(fresh, entry[blockIdx]))
                    continue;
            }
            entry[blockIdx] = std::move(fresh);
            changed = true;
        }

        return changed;
    };

    // Run to the widening fixed point. Reachability can advance only one edge
    // per sweep, so a numeric cap makes otherwise ordinary deep CFGs lose all
    // facts. Termination is instead guaranteed by the finite CFG/value set and
    // sticky per-bound widening above.
    while (runSweep(/*widening=*/true)) {
    }
    const bool converged = true;

    // Narrowing: recover bounds that stabilized after being widened (e.g. a
    // rotated loop header whose latch compare caps the induction variable —
    // the header is the widening point, so the ascent stripped the very bound
    // the latch refinement provides). Bounded sweep count; each iterate stays
    // a sound over-approximation by monotonicity of the transfer functions.
    if (converged) {
        constexpr unsigned kNarrowSweeps = 2;
        for (unsigned sweep = 0; sweep < kNarrowSweeps; ++sweep) {
            if (!runSweep(/*widening=*/false))
                break;
        }
    }

    // Debug observability: ZANNA_DEBUG_INTRANGES=1 dumps convergence status and
    // per-block entry facts to stderr.
    if (std::getenv("ZANNA_DEBUG_INTRANGES")) {
        std::fprintf(stderr, "[int-ranges] fn=%s converged=%d\n", fn.name.c_str(),
                     converged ? 1 : 0);
        for (size_t i = 0; i < blockCount; ++i) {
            if (!reached[i])
                continue;
            std::fprintf(stderr, "  block %s:", fn.blocks[i].label.c_str());
            for (const auto &[id, range] : entry[i]) {
                std::fprintf(stderr, " t%u=[%s,%s]", id,
                             range.lower ? std::to_string(*range.lower).c_str() : "-inf",
                             range.upper ? std::to_string(*range.upper).c_str() : "+inf");
            }
            std::fprintf(stderr, "\n");
        }
    }

    for (size_t i = 0; i < blockCount; ++i) {
        if (reached[i])
            info.blockEntry.emplace(fn.blocks[i].label, std::move(entry[i]));
    }
    return info;
}

} // namespace zanna::analysis
