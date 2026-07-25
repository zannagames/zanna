//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the CheckOpt pass for optimizing check opcodes. The pass performs
// dominance-based redundancy elimination and loop-invariant check hoisting to
// reduce runtime overhead from bounds checks and division-by-zero checks.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements redundancy elimination and safe motion for IL checks.
/// @details The pass combines constant and range proofs, dominating equivalent
///          checks, guarded overflow facts, and conservative loop hoisting while
///          preserving trap ordering and result types.

#include "il/transform/CheckOpt.hpp"

#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/analysis/IntRangeAnalysis.hpp"
#include "il/transform/analysis/LoopInfo.hpp"

#include "il/analysis/Dominators.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"
#include "il/utils/CheckedIntRange.hpp"
#include "il/utils/UseDefInfo.hpp"
#include "il/utils/Utils.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {
using il::utils::addOverflows;
using il::utils::IntRange;
using il::utils::mulOverflows;
using il::utils::subOverflows;
using zanna::analysis::applyRangeTransfer;
using zanna::analysis::IntRangeInfo;
using zanna::analysis::RangeMap;
using zanna::analysis::rangeForValue;

/// @brief Check if an opcode is a check operation that can be optimized.
/// @param op Opcode to classify.
/// @return True for bounds, checked division/remainder, checked cast, or checked
///         arithmetic operations handled by this pass.
bool isCheckOpcode(Opcode op) {
    switch (op) {
        case Opcode::IdxChk:
        case Opcode::SDivChk0:
        case Opcode::UDivChk0:
        case Opcode::SRemChk0:
        case Opcode::URemChk0:
        case Opcode::CastFpToSiRteChk:
        case Opcode::CastFpToUiRteChk:
        case Opcode::CastSiNarrowChk:
        case Opcode::CastUiNarrowChk:
        case Opcode::IAddOvf:
        case Opcode::ISubOvf:
        case Opcode::IMulOvf:
            return true;
        default:
            return false;
    }
}

/// @brief Check if an opcode is an overflow-checked arithmetic operation.
/// @param op Opcode to classify.
/// @return True for checked add, subtract, or multiply.
bool isOverflowOpcode(Opcode op) {
    return op == Opcode::IAddOvf || op == Opcode::ISubOvf || op == Opcode::IMulOvf;
}

/// @brief Test whether a checked instruction can use a plain I64 result shape.
/// @param instr Checked arithmetic instruction.
/// @return True for explicit I64 or parser-placeholder Void result types.
bool canDemoteToPlainI64(const Instr &instr) {
    return instr.type.kind == Type::Kind::Void || instr.type.kind == Type::Kind::I64;
}

/// @brief Stamp the canonical I64 result type after checked-op demotion.
/// @param instr Rewritten instruction; result-less instructions are unchanged.
void stampPlainI64ResultType(Instr &instr) {
    if (instr.result)
        instr.type = Type(Type::Kind::I64);
}

/// @brief Return the plain arithmetic opcode corresponding to a checked op.
/// @param op Checked arithmetic opcode.
/// @return Plain add, subtract, or multiply opcode; empty for other operations.
std::optional<Opcode> plainOpcodeForOverflow(Opcode op) {
    switch (op) {
        case Opcode::IAddOvf:
            return Opcode::Add;
        case Opcode::ISubOvf:
            return Opcode::Sub;
        case Opcode::IMulOvf:
            return Opcode::Mul;
        default:
            return std::nullopt;
    }
}


/// @brief Try to strength-reduce an overflow-checked arithmetic op to its plain
///        counterpart when both operands are constant and the operation is known
///        not to overflow at compile time.
/// @param instr The overflow instruction to evaluate.
/// @return True when the instruction was rewritten to a plain op.
bool tryConstantFoldOverflow(Instr &instr) {
    if (instr.operands.size() < 2)
        return false;

    const Value &lhs = instr.operands[0];
    const Value &rhs = instr.operands[1];

    if (lhs.kind != Value::Kind::ConstInt || rhs.kind != Value::Kind::ConstInt)
        return false;

    bool overflows = false;
    switch (instr.op) {
        case Opcode::IAddOvf:
            overflows = addOverflows(lhs.i64, rhs.i64);
            break;
        case Opcode::ISubOvf:
            overflows = subOverflows(lhs.i64, rhs.i64);
            break;
        case Opcode::IMulOvf:
            overflows = mulOverflows(lhs.i64, rhs.i64);
            break;
        default:
            return false;
    }

    if (overflows)
        return false;

    const auto plain = plainOpcodeForOverflow(instr.op);
    if (!plain || !canDemoteToPlainI64(instr))
        return false;
    instr.op = *plain;
    stampPlainI64ResultType(instr);
    return true;
}

/// @brief Return true when a checked div/rem instruction can be represented by
///        its plain counterpart because the divisor check is statically proven.
/// @param instr Candidate division or remainder instruction, rewritten on success.
/// @return True when zero and signed overflow traps are impossible.
bool tryDemoteCheckedDivRem(Instr &instr) {
    if (instr.operands.size() < 2 || instr.operands[1].kind != Value::Kind::ConstInt)
        return false;
    if (!canDemoteToPlainI64(instr))
        return false;

    const Value &lhs = instr.operands[0];
    const int64_t divisor = instr.operands[1].i64;
    if (divisor == 0)
        return false;

    switch (instr.op) {
        case Opcode::SDivChk0:
            // sdiv.chk0 also traps on INT_MIN / -1. A constant divisor other than
            // -1 proves the overflow case impossible; divisor -1 needs a known
            // non-minimum numerator.
            if (divisor == -1 && (lhs.kind != Value::Kind::ConstInt ||
                                  lhs.i64 == std::numeric_limits<int64_t>::min())) {
                return false;
            }
            instr.op = Opcode::SDiv;
            stampPlainI64ResultType(instr);
            return true;
        case Opcode::UDivChk0:
            instr.op = Opcode::UDiv;
            stampPlainI64ResultType(instr);
            return true;
        case Opcode::SRemChk0:
            // MIN % -1 is defined as 0 for Zanna's checked remainder semantics,
            // but the plain opcode may lower through a native signed divide path.
            // Like sdiv, demotion to the plain form needs a known non-minimum
            // numerator when the divisor is -1.
            if (divisor == -1 && (lhs.kind != Value::Kind::ConstInt ||
                                  lhs.i64 == std::numeric_limits<int64_t>::min())) {
                return false;
            }
            instr.op = Opcode::SRem;
            stampPlainI64ResultType(instr);
            return true;
        case Opcode::URemChk0:
            instr.op = Opcode::URem;
            stampPlainI64ResultType(instr);
            return true;
        default:
            return false;
    }
}

/// @brief Find a temporary definition earlier than a limiting instruction.
/// @param block Block whose instructions are scanned in order.
/// @param limit Instruction at which the search stops.
/// @param id Temporary identifier to locate.
/// @return Borrowed definition pointer, or null when absent before @p limit.
const Instr *findDefBefore(const BasicBlock &block, const Instr &limit, unsigned id) {
    for (const auto &instr : block.instructions) {
        if (&instr == &limit)
            return nullptr;
        if (instr.result && *instr.result == id)
            return &instr;
    }
    return nullptr;
}

/// @brief Recognize the nonnegative sign-bias term used by signed division lowering.
/// @param block Block containing the candidate chain.
/// @param limit Addition instruction that must follow the definitions.
/// @param dividend Value shifted to extract its sign.
/// @param biasValue Candidate `(dividend >> 63) & mask` result.
/// @return True when the definition chain proves the bias cannot overflow.
bool isSignBiasForDividend(const BasicBlock &block,
                           const Instr &limit,
                           const Value &dividend,
                           const Value &biasValue) {
    if (dividend.kind != Value::Kind::Temp || biasValue.kind != Value::Kind::Temp)
        return false;

    const Instr *bias = findDefBefore(block, limit, biasValue.id);
    if (!bias || bias->op != Opcode::And || bias->operands.size() != 2 ||
        bias->operands[0].kind != Value::Kind::Temp ||
        bias->operands[1].kind != Value::Kind::ConstInt || bias->operands[1].i64 < 0) {
        return false;
    }

    const Instr *sign = findDefBefore(block, *bias, bias->operands[0].id);
    return sign && sign->op == Opcode::AShr && sign->operands.size() == 2 &&
           valueEquals(sign->operands[0], dividend) &&
           sign->operands[1].kind == Value::Kind::ConstInt && sign->operands[1].i64 == 63;
}

/// @brief Demote a checked add used solely to apply a signed-division bias.
/// @param block Block containing @p instr and its operands' definitions.
/// @param instr Candidate checked add, rewritten on success.
/// @return True when either operand is a recognized sign-bias value.
bool tryDemoteSignBiasAdd(const BasicBlock &block, Instr &instr) {
    if (instr.op != Opcode::IAddOvf || instr.operands.size() != 2)
        return false;

    if (isSignBiasForDividend(block, instr, instr.operands[0], instr.operands[1]) ||
        isSignBiasForDividend(block, instr, instr.operands[1], instr.operands[0])) {
        if (!canDemoteToPlainI64(instr))
            return false;
        instr.op = Opcode::Add;
        stampPlainI64ResultType(instr);
        return true;
    }
    return false;
}

/// @brief Count incoming CFG edges for every referenced block label.
/// @param function Function whose terminator labels are scanned.
/// @return Map from block label to predecessor-edge count.
std::unordered_map<std::string, unsigned> computePredecessorCounts(const Function &function) {
    std::unordered_map<std::string, unsigned> counts;
    for (const auto &block : function.blocks) {
        if (block.instructions.empty())
            continue;
        const auto &term = block.instructions.back();
        for (const auto &label : term.labels)
            ++counts[label];
    }
    return counts;
}

/// @brief Key representing a check condition for redundancy detection.
/// @details Two checks with the same key test the same condition. Uses the
///          shared valueEquals() helper for consistent value comparison.
struct CheckKey {
    /// Check opcode.
    Opcode op{Opcode::Count};
    /// Instruction type relevant to check identity.
    Type type;
    /// Ordered checked values and limits.
    std::vector<Value> operands;

    /// @brief Compare two check conditions structurally.
    /// @param other Candidate key.
    /// @return True when opcode, type, and every operand are equivalent.
    bool operator==(const CheckKey &other) const {
        if (op != other.op || operands.size() != other.operands.size())
            return false;
        // Type comparison (only relevant for typed checks like IdxChk)
        if (type.kind != other.type.kind)
            return false;
        for (size_t i = 0; i < operands.size(); ++i) {
            if (!valueEquals(operands[i], other.operands[i]))
                return false;
        }
        return true;
    }
};

/// @brief Hash functor for CheckKey using shared value hashing.
/// @details Combines opcode and type with each operand hash using the
///          shared valueHash() helper for consistency.
struct CheckKeyHash {
    /// @brief Hash a structural check key.
    /// @param key Key to hash.
    /// @return Combined opcode, type, and operand hash.
    size_t operator()(const CheckKey &key) const {
        size_t h = static_cast<size_t>(key.op);
        h ^= static_cast<size_t>(key.type.kind) << 8;
        for (const auto &v : key.operands) {
            h ^= valueHash(v) + kHashPhiMix + (h << 6) + (h >> 2);
        }
        return h;
    }
};

/// @brief Build a CheckKey from an instruction.
/// @param instr Check instruction to snapshot.
/// @return Owning key containing its opcode, type, and operands.
CheckKey makeCheckKey(const Instr &instr) {
    CheckKey key;
    key.op = instr.op;
    key.type = instr.type;
    key.operands = instr.operands;
    return key;
}

/// @brief Test whether a standalone check instruction is trivially satisfied by constant operands.
///
/// @details After SCCP runs its constant-rewriting phase, any operand that was proven
///          constant appears in the IR as a ConstInt/ConstFloat literal rather than a
///          Temp reference.  This helper exploits that property to eliminate checks
///          whose condition can be verified at compile time without consulting the
///          SCCP lattice directly.
///
///          Rules applied per opcode:
///          - IdxChk(index, lo, hi)     — all three ConstInt and lo <= index < hi
///          Checked div/rem result-producing instructions are handled earlier
///          by tryDemoteCheckedDivRem(), which preserves the result and rewrites
///          to the corresponding plain arithmetic opcode when safe.
///
/// @param instr Check instruction to inspect.
/// @param replacementOut When the function returns true and the check has a result,
///        this is set to the value that should replace all uses of the result
///        (the normalized value produced by the check).
/// @return True when the check condition is statically guaranteed to succeed.
bool isCheckTriviallyTrue(const Instr &instr, Value &replacementOut) {
    /// Test whether one operand is an integer literal.
    auto isConstInt = [](const Value &v) { return v.kind == Value::Kind::ConstInt; };

    switch (instr.op) {
        case Opcode::IdxChk: {
            // idx.chk index lo hi — passes when lo <= index < hi
            if (instr.operands.size() < 3)
                return false;
            const Value &index = instr.operands[0];
            const Value &lo = instr.operands[1];
            const Value &hi = instr.operands[2];
            if (!isConstInt(index) || !isConstInt(lo) || !isConstInt(hi))
                return false;
            if (lo.i64 <= index.i64 && index.i64 < hi.i64) {
                replacementOut = Value::constInt(index.i64 - lo.i64);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

/// @brief Information about a dominating check instruction.
struct DominatingCheck {
    /// Block containing the available check.
    BasicBlock *block{nullptr};
    /// Optional SSA result reusable by a dominated equivalent.
    std::optional<unsigned> resultId;
};

/// @brief Find a basic block by label using a pre-built map for O(1) lookup.
/// @param blockMap Pre-computed label → block pointer map.
/// @param label The block label to search for.
/// @return Pointer to the block, or nullptr if not found.
BasicBlock *findBlock(const std::unordered_map<std::string, BasicBlock *> &blockMap,
                      const std::string &label) {
    auto it = blockMap.find(label);
    return it != blockMap.end() ? it->second : nullptr;
}

/// @brief Find the preheader block for a loop.
/// @details The preheader is the unique block outside the loop that branches
///          to the loop header. It provides a safe location for hoisting
///          loop-invariant operations. Returns nullptr if no unique preheader
///          exists (e.g., multiple entry edges or missing block).
/// @param function The function containing the loop.
/// @param loop The loop to find the preheader for.
/// @param header The loop's header block.
/// @return Pointer to the preheader block, or nullptr if not canonical.
BasicBlock *findPreheader(Function &function, const Loop &loop, BasicBlock &header) {
    BasicBlock *preheader = nullptr;
    for (auto &block : function.blocks) {
        if (loop.contains(block.label))
            continue;
        if (!zanna::il::isTerminated(block))
            continue;
        const Instr &term = block.instructions.back();
        if (term.op != Opcode::Br || term.labels.size() != 1 || term.labels.front() != header.label)
            continue;
        if (preheader && preheader != &block)
            return nullptr; // Multiple preheaders - not canonical form
        preheader = &block;
    }
    return preheader;
}

/// @brief Seed the invariant set with values defined outside the loop.
/// @details Populates the invariant set with all SSA temporaries that are:
///          - Function parameters (always loop-invariant)
///          - Block parameters of blocks outside the loop
///          - Results of instructions in blocks outside the loop
/// @param loop The loop whose invariants are being computed.
/// @param function The function containing the loop.
/// @param invariants Output set to populate with invariant temporary IDs.
void seedInvariants(const Loop &loop,
                    Function &function,
                    std::unordered_set<unsigned> &invariants) {
    // Function parameters are always invariant
    for (const auto &param : function.params)
        invariants.insert(param.id);

    // Values defined in blocks outside the loop are invariant
    for (auto &block : function.blocks) {
        if (loop.contains(block.label))
            continue;
        for (const auto &param : block.params)
            invariants.insert(param.id);
        for (const auto &instr : block.instructions)
            if (instr.result)
                invariants.insert(*instr.result);
    }
}

/// @brief Check if all operands of an instruction are loop-invariant.
/// @details An operand is invariant if it's a constant (non-Temp) or if its
///          temporary ID is in the invariant set (defined outside the loop).
/// @param instr The instruction to check.
/// @param invariants Set of known loop-invariant temporary IDs.
/// @return True if all operands are loop-invariant.
bool operandsInvariant(const Instr &instr, const std::unordered_set<unsigned> &invariants) {
    auto isInvariantValue = [&invariants](const Value &value) {
        if (value.kind != Value::Kind::Temp)
            return true; // Constants are always invariant
        return invariants.contains(value.id);
    };

    for (const auto &operand : instr.operands) {
        if (!isInvariantValue(operand))
            return false;
    }
    return true;
}

/// @brief Check if an instruction would execute on every loop iteration.
/// @details Only instructions that must execute can be safely hoisted to the
///          preheader. Currently uses a conservative approximation: only
///          instructions in the loop header are considered guaranteed.
/// @param block The block containing the instruction.
/// @param loop The loop being analyzed.
/// @return True if instructions in this block are guaranteed to execute.
bool isGuaranteedToExecute(const BasicBlock &block, const Loop &loop) {
    // Conservative: only hoist from header where check must execute on loop entry
    return block.label == loop.headerLabel;
}

/// @brief Return whether moving the instruction at @p index to the preheader
/// preserves the ordering of observable effects and traps.
/// @details Header execution alone is insufficient: a check after a call,
/// memory operation, or earlier trapping instruction must not be moved ahead
/// of it. Instructions already hoisted are absent from the prefix, which lets
/// consecutive checks move while retaining their original order.
/// @param block Candidate loop-header block.
/// @param index Index of the instruction considered for hoisting.
/// @return True when every preceding instruction is side-effect and memory free.
bool hasSpeculatablePrefix(const BasicBlock &block, size_t index) {
    for (size_t i = 0; i < index; ++i) {
        const Instr &prefix = block.instructions[i];
        const auto &info = getOpcodeInfo(prefix.op);
        if (info.hasSideEffects || hasMemoryRead(prefix.op) || hasMemoryWrite(prefix.op))
            return false;
    }
    return true;
}

/// @brief Check if a loop contains exception handling operations.
/// @details Loops with EH-sensitive operations (exception handlers, resume
///          instructions, trap instructions) require special care when
///          hoisting. This function conservatively detects such loops.
/// @param loop The loop to check.
/// @param blockMap Function block-label lookup map.
/// @return True if the loop contains any EH-sensitive operations.
bool loopHasEHSensitiveOps(const Loop &loop,
                           const std::unordered_map<std::string, BasicBlock *> &blockMap) {
    for (const auto &label : loop.blockLabels) {
        BasicBlock *block = findBlock(blockMap, label);
        if (!block)
            continue;
        for (const auto &instr : block->instructions) {
            switch (instr.op) {
                case Opcode::ResumeSame:
                case Opcode::ResumeNext:
                case Opcode::ResumeLabel:
                case Opcode::EhPush:
                case Opcode::EhPop:
                case Opcode::TrapFromErr:
                case Opcode::TrapErr:
                    return true;
                default:
                    break;
            }
        }
    }
    return false;
}

} // namespace

/// @brief Return the pass registry identifier.
/// @return Stable `check-opt` identifier.
std::string_view CheckOpt::id() const {
    return "check-opt";
}

/// @brief Optimize check instructions in one function.
/// @details Runs guarded and range-based demotion, constant elimination,
///          dominance-based reuse, and trap-order-preserving loop hoisting.
/// @param function Function updated in place.
/// @param analysis Manager providing dominators, loop info, and integer ranges.
/// @return All analyses when unchanged; otherwise preserves module analyses and
///         the unchanged CFG, dominator, and loop structure.
PreservedAnalyses CheckOpt::run(Function &function, AnalysisManager &analysis) {
    if (function.blocks.empty())
        return PreservedAnalyses::all();

    auto &domTree =
        analysis.getFunctionResult<zanna::analysis::DomTree>(kAnalysisDominators, function);
    auto &loopInfo = analysis.getFunctionResult<LoopInfo>(kAnalysisLoopInfo, function);
    // Fetch value ranges before this pass mutates anything. Later phases only
    // rewrite opcodes in place until Phase 1, so the facts stay valid where
    // Phase 0.6 consumes them.
    const auto &rangeInfo = analysis.getFunctionResult<IntRangeInfo>(kAnalysisIntRanges, function);

    bool changed = false;

    // Build label → block map once for O(1) lookup (replaces O(n) findBlock).
    std::unordered_map<std::string, BasicBlock *> blockMap;
    for (auto &bb : function.blocks)
        blockMap[bb.label] = &bb;
    const auto predecessorCounts = computePredecessorCounts(function);

    // Build initial use-count info once for safe temp replacement queries.
    zanna::il::UseDefInfo useInfo(function);

    // =========================================================================
    // Phase 0: Check opcode demotion
    // =========================================================================
    // Result-producing checks cannot be erased when their value is used. For
    // checked div/rem, a statically nonzero divisor proves the guard safe, so
    // demote to the matching plain opcode and let later arithmetic passes keep
    // optimizing the value.
    for (auto &block : function.blocks) {
        for (auto &instr : block.instructions) {
            if (isOverflowOpcode(instr.op) && tryConstantFoldOverflow(instr))
                changed = true;
            if (tryDemoteCheckedDivRem(instr))
                changed = true;
            if (tryDemoteSignBiasAdd(block, instr))
                changed = true;
        }
    }

    // =========================================================================
    // Phase 0.5: Guard-based overflow elimination
    // =========================================================================
    // When a CBr on a signed comparison guards an overflow-checked subtraction,
    // the overflow check may be provably safe. For example:
    //   %cmp = scmp_le %n, 1
    //   cbr %cmp, base, recurse
    // On the false branch (recurse): n > 1, so n >= 2.
    // Therefore: isub.ovf %n, 1 → n-1 >= 1 (no overflow)
    //            isub.ovf %n, 2 → n-2 >= 0 (no overflow)
    //
    // We scan each block for CBr instructions on signed comparisons, propagate
    // range constraints to the target blocks, and demote overflow ops to plain
    // ops when the range proves safety.
    if (runGuardOverflowElim(function, blockMap, predecessorCounts))
        changed = true;

    // =========================================================================
    // Phase 0.6: Range-backed overflow elimination
    // =========================================================================
    // Seed each block from the whole-function value-range analysis (computed
    // before any of this pass's mutations) and walk the block applying the
    // shared range transfer function. Three rewrites fire from range proofs:
    //   - overflow ops whose result range is computable demote to plain ops;
    //   - idx.chk whose index is provably inside constant bounds is deleted;
    //   - checked div/rem whose divisor range excludes the trap values demote.
    std::vector<std::pair<BasicBlock *, size_t>> rangeErase;
    for (auto &block : function.blocks) {
        RangeMap ranges;
        if (const RangeMap *seeded = rangeInfo.entryFor(block.label))
            ranges = *seeded;

        for (size_t instrIdx = 0; instrIdx < block.instructions.size(); ++instrIdx) {
            Instr &instr = block.instructions[instrIdx];

            // Provably in-bounds idx.chk: delete the check. The result is the
            // normalized index (idx - lo); substitution is only performed for
            // lo == 0 where it equals the index operand, and only for I64
            // results (the same type guard as the trivially-true path below).
            if (instr.op == Opcode::IdxChk && instr.operands.size() >= 3 &&
                instr.operands[1].kind == Value::Kind::ConstInt &&
                instr.operands[2].kind == Value::Kind::ConstInt) {
                const int64_t lo = instr.operands[1].i64;
                const int64_t hi = instr.operands[2].i64;
                auto idxRange = rangeForValue(instr.operands[0], ranges);
                if (idxRange && idxRange->lower && idxRange->upper && lo < hi &&
                    *idxRange->lower >= lo && *idxRange->upper <= hi - 1) {
                    const bool hasUses = instr.result && useInfo.hasUses(*instr.result);
                    // The spec fixes idx.chk's semantic result type to i64; an
                    // unannotated instruction parses with a Void placeholder
                    // type, which substitution treats the same as i64.
                    const bool resultTypeIsI64 = instr.type.kind == Type::Kind::I64 ||
                                                 instr.type.kind == Type::Kind::Void;
                    if (!hasUses) {
                        rangeErase.push_back({&block, instrIdx});
                        changed = true;
                        continue;
                    }
                    if (lo == 0 && resultTypeIsI64) {
                        useInfo.replaceAllUses(*instr.result, instr.operands[0]);
                        rangeErase.push_back({&block, instrIdx});
                        changed = true;
                        continue;
                    }
                }
            }

            // Checked div/rem with a range-proven safe divisor. The divisor
            // range must exclude 0 (all four ops); the signed forms also trap
            // on INT64_MIN / -1, so demotion additionally needs the divisor
            // range to exclude -1 or the dividend range to exclude INT64_MIN.
            if (instr.op == Opcode::SDivChk0 || instr.op == Opcode::UDivChk0 ||
                instr.op == Opcode::SRemChk0 || instr.op == Opcode::URemChk0) {
                auto divisorRange = rangeForValue(instr.operands[1], ranges);
                if (divisorRange && canDemoteToPlainI64(instr)) {
                    const bool excludesZero =
                        (divisorRange->lower && *divisorRange->lower > 0) ||
                        (divisorRange->upper && *divisorRange->upper < 0);
                    bool safeToDemote = excludesZero;
                    if (safeToDemote &&
                        (instr.op == Opcode::SDivChk0 || instr.op == Opcode::SRemChk0)) {
                        const bool excludesMinusOne =
                            (divisorRange->lower && *divisorRange->lower > -1) ||
                            (divisorRange->upper && *divisorRange->upper < -1);
                        auto dividendRange = rangeForValue(instr.operands[0], ranges);
                        const bool dividendNotMin =
                            dividendRange && dividendRange->lower &&
                            *dividendRange->lower > std::numeric_limits<int64_t>::min();
                        safeToDemote = excludesMinusOne || dividendNotMin;
                    }
                    if (safeToDemote) {
                        switch (instr.op) {
                            case Opcode::SDivChk0:
                                instr.op = Opcode::SDiv;
                                break;
                            case Opcode::UDivChk0:
                                instr.op = Opcode::UDiv;
                                break;
                            case Opcode::SRemChk0:
                                instr.op = Opcode::SRem;
                                break;
                            default:
                                instr.op = Opcode::URem;
                                break;
                        }
                        stampPlainI64ResultType(instr);
                        changed = true;
                    }
                }
            }

            std::optional<IntRange> resultRange = applyRangeTransfer(instr, ranges);
            if (resultRange) {
                if (auto plainOp = plainOpcodeForOverflow(instr.op)) {
                    if (canDemoteToPlainI64(instr)) {
                        instr.op = *plainOp;
                        stampPlainI64ResultType(instr);
                        changed = true;
                    }
                }
            }
        }
    }
    // Erase deleted checks back-to-front so recorded indices stay valid.
    for (auto it = rangeErase.rbegin(); it != rangeErase.rend(); ++it) {
        auto &instructions = it->first->instructions;
        instructions.erase(instructions.begin() + static_cast<std::ptrdiff_t>(it->second));
    }

    // =========================================================================
    // Phase 1: Dominance-based redundancy elimination
    // =========================================================================
    // Walk dominator tree with scoped map so siblings do not incorrectly share
    // availability. Only checks that dominate the current block may be reused.
    std::unordered_map<CheckKey, DominatingCheck, CheckKeyHash> available;
    std::unordered_map<CheckKey, unsigned, CheckKeyHash> depthCount;
    std::vector<std::pair<BasicBlock *, size_t>> toErase;

    /// Visit the dominator tree while maintaining path-scoped available checks.
    std::function<void(BasicBlock *)> visit = [&](BasicBlock *block) {
        if (!block)
            return;
        std::vector<CheckKey> added;
        for (size_t idx = 0; idx < block->instructions.size(); ++idx) {
            Instr &instr = block->instructions[idx];
            if (!isCheckOpcode(instr.op))
                continue;

            // Constant-operand elimination: if SCCP has already inlined the
            // operands as literals and the check condition is statically
            // satisfied, remove the check and replace its result with the
            // pass-through value.  This avoids redundant runtime checks for
            // patterns like idx.chk(5, 0, 10) where the bounds are known.
            //
            // Type constraint: ConstInt values type as I64 in the verifier.
            // When the check result type is narrower (e.g. I32 for IdxChk)
            // and the result has uses, substituting an I64 constant would
            // produce a type mismatch that the verifier rejects.  In that
            // case fall through to the dominance-based check instead.
            Value trivialReplacement;
            if (isCheckTriviallyTrue(instr, trivialReplacement)) {
                if (instr.result && useInfo.hasUses(*instr.result)) {
                    // Check whether the replacement type matches the result type.
                    // ConstInt values type as I64; if the instruction result is
                    // narrower we cannot safely substitute without a type error.
                    const bool replacementIsI64ConstInt =
                        trivialReplacement.kind == Value::Kind::ConstInt &&
                        !trivialReplacement.isBool;
                    if (replacementIsI64ConstInt && instr.type.kind != Type::Kind::I64) {
                        // Type mismatch — fall through to dominance-based check.
                        // The dominance check can still replace with a same-typed
                        // Temp if a dominating occurrence already holds the result.
                    } else {
                        useInfo.replaceAllUses(*instr.result, trivialReplacement);
                        toErase.push_back({block, idx});
                        changed = true;
                        continue;
                    }
                } else {
                    // Result is either absent or has no live uses — safe to erase
                    // without substitution regardless of type.
                    toErase.push_back({block, idx});
                    changed = true;
                    continue;
                }
            }

            CheckKey key = makeCheckKey(instr);
            auto it = available.find(key);
            if (it != available.end() && domTree.dominates(it->second.block, block)) {
                if (instr.result && it->second.resultId) {
                    useInfo.replaceAllUses(*instr.result, Value::temp(*it->second.resultId));
                }
                toErase.push_back({block, idx});
                changed = true;
            } else {
                available[key] = DominatingCheck{block, instr.result};
                ++depthCount[key];
                added.push_back(key);
            }
        }

        auto childIt = domTree.children.find(block);
        if (childIt != domTree.children.end()) {
            for (auto *child : childIt->second)
                visit(child);
        }

        for (const auto &k : added) {
            auto cntIt = depthCount.find(k);
            if (cntIt != depthCount.end()) {
                if (--cntIt->second == 0) {
                    depthCount.erase(cntIt);
                    available.erase(k);
                }
            }
        }
    };

    visit(&function.blocks.front());

    for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
        BasicBlock *block = it->first;
        size_t idx = it->second;
        block->instructions.erase(block->instructions.begin() + static_cast<std::ptrdiff_t>(idx));
    }

    // =========================================================================
    // Phase 2: Loop-invariant check hoisting
    // =========================================================================
    // For each loop, identify checks whose operands are loop-invariant and
    // hoist them to the preheader.

    for (const Loop &loop : loopInfo.loops()) {
        BasicBlock *header = findBlock(blockMap, loop.headerLabel);
        if (!header)
            continue;

        BasicBlock *preheader = findPreheader(function, loop, *header);
        if (!preheader)
            continue;
        if (loopHasEHSensitiveOps(loop, blockMap))
            continue;

        // Seed invariants with out-of-loop definitions
        std::unordered_set<unsigned> invariants;
        invariants.reserve(function.params.size() + 32);
        seedInvariants(loop, function, invariants);

        // Also include results from hoisted checks as invariant
        // (enabling cascading hoists in a single pass)

        // Process each block in the loop
        for (const std::string &blockLabel : loop.blockLabels) {
            BasicBlock *block = findBlock(blockMap, blockLabel);
            if (!block)
                continue;

            // Only hoist from blocks where the check is guaranteed to execute
            if (!isGuaranteedToExecute(*block, loop))
                continue;

            for (size_t idx = 0; idx < block->instructions.size();) {
                Instr &instr = block->instructions[idx];

                if (!isCheckOpcode(instr.op) || !operandsInvariant(instr, invariants) ||
                    !hasSpeculatablePrefix(*block, idx)) {
                    ++idx;
                    continue;
                }

                // Hoist the check to the preheader
                Instr hoisted = std::move(instr);
                block->instructions.erase(block->instructions.begin() +
                                          static_cast<std::ptrdiff_t>(idx));

                // Insert before the terminator in the preheader
                size_t insertIdx = preheader->instructions.size();
                if (zanna::il::isTerminated(*preheader) && insertIdx > 0)
                    --insertIdx;
                auto inserted = preheader->instructions.insert(
                    preheader->instructions.begin() + static_cast<std::ptrdiff_t>(insertIdx),
                    std::move(hoisted));

                // Copy result before any further vector operations could
                // invalidate the iterator returned by insert().
                auto hoistedResult = inserted->result;
                if (hoistedResult)
                    invariants.insert(*hoistedResult);

                changed = true;
                // Don't increment idx - we removed the current instruction
            }
        }
    }

    if (!changed)
        return PreservedAnalyses::all();

    // Invalidate analyses since we modified instructions
    PreservedAnalyses preserved;
    preserved.preserveAllModules();
    // CFG structure is preserved (no block additions/removals or edge changes)
    preserved.preserveFunction(kAnalysisCFG);
    // Dominators are preserved (we only remove instructions, not blocks)
    preserved.preserveFunction(kAnalysisDominators);
    // Loop info is preserved (loop structure unchanged)
    preserved.preserveFunction(kAnalysisLoopInfo);
    return preserved;
}

/// @brief Demote subtraction guarded by a unique signed-comparison edge.
/// @param function Function whose conditional branches are scanned.
/// @param blockMap Label-to-block lookup for guard successors.
/// @param predecessorCounts Incoming edge counts used to require unique guards.
/// @return True when at least one checked subtraction became plain.
bool CheckOpt::runGuardOverflowElim(
    Function &function,
    const std::unordered_map<std::string, BasicBlock *> &blockMap,
    const std::unordered_map<std::string, unsigned> &predecessorCounts) {
    bool changed = false;
    for (auto &block : function.blocks) {
        if (block.instructions.empty())
            continue;

        const auto &term = block.instructions.back();
        if (term.op != Opcode::CBr || term.labels.size() != 2 || term.operands.empty())
            continue;

        // Find the comparison instruction feeding the CBr.
        const Value &condVal = term.operands[0];
        if (condVal.kind != Value::Kind::Temp)
            continue;

        const Instr *cmpInstr = nullptr;
        for (const auto &instr : block.instructions) {
            if (instr.result && *instr.result == condVal.id) {
                cmpInstr = &instr;
                break;
            }
        }
        if (!cmpInstr || cmpInstr->operands.size() < 2)
            continue;

        // Only handle scmp_le %x, C where C is a constant.
        if (cmpInstr->op != Opcode::SCmpLE)
            continue;

        const Value &cmpLHS = cmpInstr->operands[0]; // %x
        const Value &cmpRHS = cmpInstr->operands[1]; // C
        if (cmpRHS.kind != Value::Kind::ConstInt || cmpLHS.kind != Value::Kind::Temp)
            continue;

        const int64_t threshold = cmpRHS.i64;
        const unsigned guardedVar = cmpLHS.id;

        // Avoid overflow in threshold + 1.
        if (threshold == std::numeric_limits<int64_t>::max())
            continue;

        // CBr: labels[0] = true branch (cmp is true → x <= C)
        //      labels[1] = false branch (cmp is false → x > C → x >= C+1)
        // On the FALSE branch, we know: guardedVar >= threshold + 1
        const std::string &falseBranch = term.labels[1];
        auto predCountIt = predecessorCounts.find(falseBranch);
        if (predCountIt == predecessorCounts.end() || predCountIt->second != 1)
            continue;
        const int64_t lowerBound = threshold + 1; // x >= lowerBound on false branch

        // Find the false-branch target block and demote safe overflow ops.
        auto tgtIt = blockMap.find(falseBranch);
        if (tgtIt == blockMap.end())
            continue;
        BasicBlock *targetBlock = tgtIt->second;

        for (auto &instr : targetBlock->instructions) {
            if (instr.op != Opcode::ISubOvf)
                continue;
            if (instr.operands.size() < 2)
                continue;

            const Value &subLHS = instr.operands[0];
            const Value &subRHS = instr.operands[1];

            // Match: isub.ovf %guardedVar, K where K is a constant
            if (subLHS.kind != Value::Kind::Temp || subLHS.id != guardedVar)
                continue;
            if (subRHS.kind != Value::Kind::ConstInt)
                continue;

            const int64_t K = subRHS.i64;

            // Check: lowerBound - K >= INT64_MIN (subtraction can't overflow).
            if (K >= 0 && lowerBound >= std::numeric_limits<int64_t>::min() + K) {
                if (!canDemoteToPlainI64(instr))
                    continue;
                instr.op = Opcode::Sub;
                stampPlainI64ResultType(instr);
                changed = true;
            }
        }
    }
    return changed;
}

/// @brief Register the sequential check optimization factory.
/// @param registry Pass registry updated with `check-opt`.
void registerCheckOptPass(PassRegistry &registry) {
    // Sequential: consumes whole-module dominator/loop analyses while moving checks.
    /// Construct a fresh check optimization pass for each pipeline request.
    registry.registerFunctionPass("check-opt", []() { return std::make_unique<CheckOpt>(); }, false);
}

} // namespace il::transform
