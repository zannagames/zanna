//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements a conservative induction variable simplification and loop strength
// reduction pass. The pass recognizes simple counted loops with a single latch
// updating an integer induction variable by a small constant and rewrites
// repeated linear expressions of the form `base + i * stride` into incremental
// updates of a loop-carried temporary.
//
// The transformation relies on LoopSimplify providing a preheader and uses
// LoopInfo + Dominators to limit changes to well-structured loops.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements induction variable simplification and strength reduction.
/// @details Detects simple counted loops with a single latch and rewrites
///          address computations of the form `base + i * stride` into a loop-
///          carried temporary that is incremented each iteration. The pass is
///          conservative and only applies when structural and dataflow checks
///          prove the transformation is safe.

#include "il/transform/IndVarSimplify.hpp"

#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/analysis/LoopInfo.hpp"

#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/analysis/IntRangeAnalysis.hpp"

#include "il/utils/CheckedIntRange.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

#include "il/transform/analysis/Liveness.hpp" // for CFGInfo

#include "il/utils/UseDefInfo.hpp"
#include "il/utils/Utils.hpp"

#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace il::core;

namespace il::transform {

namespace {
/// @brief Test whether a block ends in a recognized IL terminator.
/// @param block Block whose final instruction is inspected.
/// @return `true` when @p block is nonempty and its last instruction is a terminator.
bool hasTerminator(const BasicBlock &block) {
    return !block.instructions.empty() && zanna::il::isTerminator(block.instructions.back());
}

/// @brief Find a basic block by label within a function.
/// @details Delegates to the shared IL utility and returns nullptr when the
///          label is not present.
/// @param function Function containing the blocks.
/// @param label Label to search for.
/// @return Pointer to the matching block, or nullptr if not found.
BasicBlock *findBlock(Function &function, const std::string &label) {
    return zanna::il::findBlock(function, label);
}

/// @brief Locate the unique loop preheader that jumps to the loop header.
/// @details Scans blocks outside the loop for a terminator that targets the
///          header. If multiple distinct predecessors are found, returns
///          nullptr to avoid unsafe hoisting.
/// @param function Function containing the loop.
/// @param loop Loop metadata describing membership.
/// @param header Loop header block.
/// @return Pointer to the unique preheader block, or nullptr if ambiguous.
BasicBlock *findPreheader(Function &function, const Loop &loop, BasicBlock &header) {
    BasicBlock *preheader = nullptr;
    for (auto &block : function.blocks) {
        if (loop.contains(block.label))
            continue;
        if (!hasTerminator(block))
            continue;
        const Instr &term = block.instructions.back();
        bool targetsHeader = false;
        for (const auto &label : term.labels) {
            if (label == header.label) {
                targetsHeader = true;
                break;
            }
        }
        if (!targetsHeader)
            continue;

        if (preheader && preheader != &block)
            return nullptr;
        preheader = &block;
    }
    return preheader;
}

// Find instruction in a block by result temp id.
/// @brief Find an instruction that defines the given temporary id.
/// @details Performs a linear scan over the block's instruction list.
/// @param B Block to scan.
/// @param tempId Temporary id to locate.
/// @return Pointer to the defining instruction, or nullptr if not found.
Instr *findInstrByResult(BasicBlock &B, unsigned tempId) {
    for (auto &I : B.instructions) {
        if (I.result && *I.result == tempId)
            return &I;
    }
    return nullptr;
}

// Count uses of a temp across a function (cheap scan, conservative).
/// @brief Count uses of a temporary across a function.
/// @details Scans operands and branch argument lists to approximate use count.
///          This is used as a conservative single-use check during matching.
/// @param F Function to scan.
/// @param tempId Temporary id to count.
/// @return Number of occurrences of @p tempId.
size_t countTempUses(Function &F, unsigned tempId) {
    size_t uses = 0;
    for (auto &B : F.blocks) {
        for (auto &I : B.instructions) {
            for (auto &Op : I.operands)
                if (Op.kind == Value::Kind::Temp && Op.id == tempId)
                    ++uses;
            for (auto &ArgList : I.brArgs)
                for (auto &Arg : ArgList)
                    if (Arg.kind == Value::Kind::Temp && Arg.id == tempId)
                        ++uses;
        }
    }
    return uses;
}

// Return index of a target label in a terminator's labels list.
/// @brief Locate a label within a terminator's successor list.
/// @details Returns the index so the corresponding branch argument list can be
///          accessed. If the label is not present, returns std::nullopt.
/// @param term Terminator instruction to inspect.
/// @param target Label to locate in @p term.labels.
/// @return Index of the label, or std::nullopt if not found.
std::optional<size_t> labelIndex(const Instr &term, const std::string &target) {
    for (size_t i = 0; i < term.labels.size(); ++i)
        if (term.labels[i] == target)
            return i;
    return std::nullopt;
}

// Count how many successor slots of a terminator name a target label.
/// @brief Count occurrences of a label in a terminator's successor list.
/// @details The rewrite extends one branch-argument list per edge it knows
///          about; a terminator that names the same target on several edges
///          would leave the remaining edges short, so callers skip those.
/// @param term Terminator instruction to inspect.
/// @param target Label to count in @p term.labels.
/// @return Number of successor slots equal to @p target.
size_t labelOccurrences(const Instr &term, const std::string &target) {
    size_t count = 0;
    for (const auto &label : term.labels)
        if (label == target)
            ++count;
    return count;
}

/// @brief Description of a simple loop induction variable.
/// @details Captures which header parameter is the induction variable, the
///          constant step per iteration, and the latch parameter id that feeds
///          the backedge update.
struct IndVar {
    size_t headerParamIndex{}; ///< Index into header.params for the IV.
    long long step{};          ///< Step per iteration (+C or -C).
    unsigned latchParamId{};   ///< Temp id of the corresponding latch param.
};

// Try to recognize a simple i' = i +/- C update on the backedge L->H.
/// @brief Detect a simple linear induction variable on the latch backedge.
/// @details Matches updates of the form `i' = i +/- C` where `i` is a latch
///          parameter and `C` is a constant. The function also verifies that
///          the latch parameter maps back to a header parameter via the header
///          -> latch branch arguments so the variable is truly loop-carried.
/// @param F Function containing the loop.
/// @param H Loop header block.
/// @param L Loop latch block.
/// @return IndVar description on success; std::nullopt if no match.
std::optional<IndVar> detectIndVar(Function &F, BasicBlock &H, BasicBlock &L) {
    if (!hasTerminator(L))
        return std::nullopt;
    const Instr &LTerm = L.instructions.back();
    auto toHIndex = labelIndex(LTerm, H.label);
    if (!toHIndex)
        return std::nullopt;
    if (*toHIndex >= LTerm.brArgs.size())
        return std::nullopt;
    const std::vector<Value> &argsToH = LTerm.brArgs[*toHIndex];
    if (argsToH.size() != H.params.size())
        return std::nullopt;

    // For each header param, see if backedge argument is (add/sub latchParam, const)
    for (size_t i = 0; i < argsToH.size(); ++i) {
        const Value &arg = argsToH[i];
        if (arg.kind != Value::Kind::Temp)
            continue;
        Instr *upd = findInstrByResult(L, arg.id);
        if (!upd)
            continue;
        if (!(upd->op == Opcode::Add || upd->op == Opcode::Sub))
            continue;
        // Match (temp, const)
        const Value &A = upd->operands.size() > 0 ? upd->operands[0] : Value::constInt(0);
        const Value &B = upd->operands.size() > 1 ? upd->operands[1] : Value::constInt(0);
        const Value *var = nullptr;
        const Value *cst = nullptr;
        if (A.kind == Value::Kind::Temp && B.kind == Value::Kind::ConstInt) {
            var = &A;
            cst = &B;
        } else if (upd->op == Opcode::Add && B.kind == Value::Kind::Temp &&
                   A.kind == Value::Kind::ConstInt) {
            var = &B;
            cst = &A;
        }
        if (!var || !cst)
            continue;
        // Identify latch param id corresponding to this var temp id
        unsigned latchParamId = 0;
        bool matchedLatchParam = false;
        for (const auto &p : L.params) {
            if (p.id == var->id) {
                latchParamId = p.id;
                matchedLatchParam = true;
                break;
            }
        }
        if (!matchedLatchParam)
            continue; // only handle direct use of latch param
        // Determine the corresponding header param index by looking at header->latch args
        if (!hasTerminator(H))
            return std::nullopt;
        const Instr &HTerm = H.instructions.back();
        auto toLIndex = labelIndex(HTerm, L.label);
        if (!toLIndex || *toLIndex >= HTerm.brArgs.size())
            return std::nullopt;
        const auto &argsToL = HTerm.brArgs[*toLIndex];
        if (argsToL.size() != L.params.size())
            return std::nullopt;
        // Find which latch param corresponds to which header param
        int headerParamIdx = -1;
        for (size_t k = 0; k < L.params.size(); ++k) {
            if (L.params[k].id != latchParamId)
                continue;
            const Value &fromH = argsToL[k];
            if (fromH.kind != Value::Kind::Temp)
                return std::nullopt;
            // That temp id should be one of header params
            for (size_t hp = 0; hp < H.params.size(); ++hp) {
                if (H.params[hp].id == fromH.id) {
                    headerParamIdx = static_cast<int>(hp);
                    break;
                }
            }
            break;
        }
        if (headerParamIdx < 0)
            continue;

        long long step = cst->i64;
        if (upd->op == Opcode::Sub) {
            if (step == std::numeric_limits<long long>::min())
                return std::nullopt;
            step = -step;
        }
        IndVar iv{static_cast<size_t>(headerParamIdx), step, latchParamId};
        return iv;
    }

    return std::nullopt;
}

// Try to find `addr = base + (i * stride)` in header H using header param i.
// Returns <addrAddId, stride, baseValue, mulId> when matched.
/// @brief Matched address expression in the loop header.
/// @details Captures the add instruction result, the constant stride, the base
///          value, and the multiply result id so it can be validated and removed.
struct AddrExpr {
    unsigned addrId{};  ///< Temp id of the `base + i * stride` add result.
    long long stride{}; ///< Constant stride used in the multiply.
    Value base;         ///< Base value added to the scaled induction variable.
    unsigned mulId{};   ///< Temp id of the multiply result (for single-use checks).
};

/// @brief Find `base + (i * stride)` in the loop header.
/// @details Searches for an add whose one operand is a multiply of the header
///          induction variable by a constant. The multiply must be single-use
///          to allow safe removal after rewriting.
/// @param F Function containing the loop.
/// @param H Loop header block to scan.
/// @param indVarId Temp id of the induction variable header parameter.
/// @return Address expression match on success; std::nullopt if no match.
std::optional<AddrExpr> findAddrExpr(Function &F, BasicBlock &H, unsigned indVarId) {
    for (size_t idx = 0; idx < H.instructions.size(); ++idx) {
        Instr &I = H.instructions[idx];
        if (!I.result || I.op != Opcode::Add)
            continue;
        // Look for one operand being a mul
        Value A = I.operands.size() > 0 ? I.operands[0] : Value::constInt(0);
        Value B = I.operands.size() > 1 ? I.operands[1] : Value::constInt(0);
        Value *base = nullptr;
        unsigned mulId = 0;
        if (A.kind == Value::Kind::Temp) {
            Instr *mulI = findInstrByResult(H, A.id);
            if (mulI && mulI->op == Opcode::Mul) {
                // Check mul is (indVar * const)
                const Value &M0 = mulI->operands[0];
                const Value &M1 = mulI->operands[1];
                const Value *var = nullptr;
                const Value *cst = nullptr;
                if (M0.kind == Value::Kind::Temp && M0.id == indVarId &&
                    M1.kind == Value::Kind::ConstInt) {
                    var = &M0;
                    cst = &M1;
                } else if (M1.kind == Value::Kind::Temp && M1.id == indVarId &&
                           M0.kind == Value::Kind::ConstInt) {
                    var = &M1;
                    cst = &M0;
                }
                if (var && cst) {
                    base = &B;
                    mulId = *mulI->result;
                    // Ensure mul is only used by this add
                    if (countTempUses(F, mulId) != 1)
                        continue;
                    return AddrExpr{*I.result, cst->i64, *base, mulId};
                }
            }
        }
        if (B.kind == Value::Kind::Temp) {
            Instr *mulI = findInstrByResult(H, B.id);
            if (mulI && mulI->op == Opcode::Mul) {
                const Value &M0 = mulI->operands[0];
                const Value &M1 = mulI->operands[1];
                const Value *var = nullptr;
                const Value *cst = nullptr;
                if (M0.kind == Value::Kind::Temp && M0.id == indVarId &&
                    M1.kind == Value::Kind::ConstInt) {
                    var = &M0;
                    cst = &M1;
                } else if (M1.kind == Value::Kind::Temp && M1.id == indVarId &&
                           M0.kind == Value::Kind::ConstInt) {
                    var = &M1;
                    cst = &M0;
                }
                if (var && cst) {
                    base = &A;
                    mulId = *mulI->result;
                    if (countTempUses(F, mulId) != 1)
                        continue;
                    return AddrExpr{*I.result, cst->i64, *base, mulId};
                }
            }
        }
    }
    return std::nullopt;
}

/// @brief Find the positional index of a block parameter by temporary id.
/// @param block Block whose parameter list is searched.
/// @param tempId Temporary id assigned to the desired parameter.
/// @return The matching parameter index, or `std::nullopt` when the id is absent.
std::optional<size_t> blockParamIndex(const BasicBlock &block, unsigned tempId) {
    for (size_t i = 0; i < block.params.size(); ++i) {
        if (block.params[i].id == tempId)
            return i;
    }
    return std::nullopt;
}

/// @brief Determine whether a temporary is defined anywhere inside a loop.
/// @param function Function containing the blocks named by @p loop.
/// @param loop Loop membership information used to limit the search.
/// @param tempId Temporary id whose definition is sought.
/// @return `true` for a matching block parameter or instruction result in the loop.
bool tempDefinedInLoop(const Function &function, const Loop &loop, unsigned tempId) {
    for (const auto &block : function.blocks) {
        if (!loop.contains(block.label))
            continue;
        for (const auto &param : block.params)
            if (param.id == tempId)
                return true;
        for (const auto &instr : block.instructions)
            if (instr.result && *instr.result == tempId)
                return true;
    }
    return false;
}

/// @brief Translate a value used by the header into the value available on entry.
/// @param function Function containing the loop and possible defining instruction.
/// @param loop Loop used to reject temporaries defined within its body.
/// @param header Header whose parameters are mapped through @p preheaderArgs.
/// @param preheaderArgs Arguments carried by the selected preheader-to-header edge.
/// @param value Header-context value to translate.
/// @return The incoming argument, an unchanged constant or loop-invariant value, or
///         `std::nullopt` when the value has no safe preheader representation.
std::optional<Value> valueForPreheader(const Function &function,
                                       const Loop &loop,
                                       const BasicBlock &header,
                                       const std::vector<Value> &preheaderArgs,
                                       const Value &value) {
    if (value.kind != Value::Kind::Temp)
        return value;

    if (auto idx = blockParamIndex(header, value.id)) {
        if (*idx >= preheaderArgs.size())
            return std::nullopt;
        return preheaderArgs[*idx];
    }

    if (tempDefinedInLoop(function, loop, value.id))
        return std::nullopt;

    return value;
}

/// @brief Multiply two signed integers without invoking overflow.
/// @param lhs Left multiplication operand.
/// @param rhs Right multiplication operand.
/// @return The product when representable as `long long`, otherwise `std::nullopt`.
std::optional<long long> checkedMul(long long lhs, long long rhs) {
    if (lhs == 0 || rhs == 0)
        return 0;
    constexpr long long max = std::numeric_limits<long long>::max();
    constexpr long long min = std::numeric_limits<long long>::min();
    if (lhs > 0) {
        if (rhs > 0) {
            if (lhs > max / rhs)
                return std::nullopt;
        } else if (rhs < min / lhs) {
            return std::nullopt;
        }
    } else {
        if (rhs > 0) {
            if (lhs < min / rhs)
                return std::nullopt;
        } else if (rhs < max / lhs) {
            return std::nullopt;
        }
    }
    if ((lhs == -1 && rhs == min) || (rhs == -1 && lhs == min))
        return std::nullopt;
    return lhs * rhs;
}

/// @brief Assign a diagnostic name to a temporary, growing the name table as needed.
/// @param function Function whose value-name table is updated.
/// @param id Temporary id that indexes the table.
/// @param name Human-readable name associated with @p id.
void setValueName(Function &function, unsigned id, const std::string &name) {
    if (function.valueNames.size() <= id)
        function.valueNames.resize(id + 1);
    function.valueNames[id] = name;
}

} // namespace

/// @brief Return the unique identifier for the IndVarSimplify pass.
/// @details Used by the pass registry and pipeline definitions.
/// @return The canonical pass id string "indvars".
std::string_view IndVarSimplify::id() const {
    return "indvars";
}

/// @brief Execute induction variable simplification on a function.
/// @details For each well-formed loop with a single latch, the pass:
///          1) Detects a simple induction variable update on the backedge.
///          2) Finds an address expression `base + i * stride` in the header.
///          3) Adds a loop-carried address parameter and computes its initial
///             value in the preheader.
///          4) Increments the address in the latch and threads it through the
///             backedge arguments and every other edge into the latch.
///          5) Replaces uses of the original address computation and removes
///             the now-dead add/mul instructions.
///          The transformation is conservative and skips loops that do not meet
///          structural requirements or single-use guarantees. It requires a
///          constant base and initial counter, a counter the shared range
///          prover can bound at the latch, and overflow-freedom of every
///          address value the rewrite computes; the new arithmetic is emitted
///          in checked form so the strict verifier accepts it while the
///          overflow proof guarantees the checks never fire.
/// @param function Function to optimize in place.
/// @param analysis Analysis manager supplying loop and dominance info.
/// @return Preserved analysis set; conservative invalidation on change.
PreservedAnalyses IndVarSimplify::run(Function &function, AnalysisManager &analysis) {
    // Analyses
    auto &loopInfo = analysis.getFunctionResult<LoopInfo>(kAnalysisLoopInfo, function);
    (void)analysis.getFunctionResult<il::transform::CFGInfo>(kAnalysisCFG, function);
    auto &dom = analysis.getFunctionResult<zanna::analysis::DomTree>(kAnalysisDominators, function);
    (void)dom;

    bool changed = false;

    for (const Loop &loop : loopInfo.loops()) {
        BasicBlock *header = findBlock(function, loop.headerLabel);
        if (!header)
            continue;
        BasicBlock *preheader = findPreheader(function, loop, *header);
        if (!preheader)
            continue;
        // Single latch loop only
        if (loop.latchLabels.size() != 1)
            continue;
        BasicBlock *latch = findBlock(function, loop.latchLabels.front());
        if (!latch)
            continue;
        // The rewrite threads distinct header and latch parameters; a block
        // that is its own latch cannot host both roles.
        if (latch == header)
            continue;

        // Find an induction variable on backedge L->H
        auto iv = detectIndVar(function, *header, *latch);
        if (!iv)
            continue;

        // Find a candidate address expression in header using header param at iv->headerParamIndex
        unsigned indVarTempId = header->params[iv->headerParamIndex].id;
        auto addrExpr = findAddrExpr(function, *header, indVarTempId);
        if (!addrExpr)
            continue;

        const auto *addrInstr = findInstrByResult(*header, addrExpr->addrId);
        if (!addrInstr)
            continue;
        const auto *mulInstr = findInstrByResult(*header, addrExpr->mulId);
        if (!mulInstr)
            continue;

        // Compute addr0 in preheader: base + (init_i * stride)
        // Get initial i from preheader->header branch
        if (!hasTerminator(*preheader))
            continue;
        Instr &PHTerm = preheader->instructions.back();
        auto toH = labelIndex(PHTerm, header->label);
        if (!toH || *toH >= PHTerm.brArgs.size())
            continue;
        // Only the located edge is extended below, so a degenerate terminator
        // that reaches the header on several edges must be skipped.
        if (labelOccurrences(PHTerm, header->label) != 1)
            continue;
        auto &phArgs = PHTerm.brArgs[*toH];
        if (phArgs.size() != header->params.size())
            continue;

        auto preheaderBase = valueForPreheader(function, loop, *header, phArgs, addrExpr->base);
        if (!preheaderBase)
            continue;

        if (!hasTerminator(*header))
            continue;
        Instr &HTerm = header->instructions.back();
        auto toL = labelIndex(HTerm, latch->label);
        if (!toL || *toL >= HTerm.brArgs.size())
            continue;
        if (HTerm.brArgs[*toL].size() != latch->params.size())
            continue;

        if (!hasTerminator(*latch))
            continue;
        Instr &LTerm = latch->instructions.back();
        auto li = labelIndex(LTerm, header->label);
        if (!li || *li >= LTerm.brArgs.size())
            continue;
        if (LTerm.brArgs[*li].size() != header->params.size())
            continue;
        if (labelOccurrences(LTerm, header->label) != 1)
            continue;

        auto inc = checkedMul(addrExpr->stride, iv->step);
        if (!inc)
            continue;

        // The latch gains a parameter below, so every predecessor edge into it
        // must supply the new address argument — not just the header's edge: a
        // conditional in the rotated body (e.g. a wrap adjustment) rejoins the
        // latch with branch arguments of its own. Collect the edges before any
        // mutation. A predecessor outside the loop could not reference the
        // header-defined address at all, so such shapes are skipped entirely.
        std::vector<std::pair<Instr *, size_t>> latchPredEdges;
        bool latchPredEdgesUsable = true;
        for (auto &block : function.blocks) {
            if (!hasTerminator(block))
                continue;
            Instr &predTerm = block.instructions.back();
            for (size_t target = 0; target < predTerm.labels.size(); ++target) {
                if (predTerm.labels[target] != latch->label)
                    continue;
                if (!loop.contains(block.label) || target >= predTerm.brArgs.size() ||
                    predTerm.brArgs[target].size() != latch->params.size()) {
                    latchPredEdgesUsable = false;
                    break;
                }
                latchPredEdges.emplace_back(&predTerm, target);
            }
            if (!latchPredEdgesUsable)
                break;
        }
        if (!latchPredEdgesUsable || latchPredEdges.empty())
            continue;

        // The strength-reduced address stays synchronized with the counter
        // only if every path into the latch carries the unmodified header
        // induction value at the counter's position; a body that re-enters the
        // latch with an adjusted counter would silently desynchronize them.
        size_t ivLatchParamIndex = latch->params.size();
        for (size_t k = 0; k < latch->params.size(); ++k) {
            if (latch->params[k].id == iv->latchParamId) {
                ivLatchParamIndex = k;
                break;
            }
        }
        if (ivLatchParamIndex == latch->params.size())
            continue;
        const unsigned headerIvId = header->params[iv->headerParamIndex].id;
        bool ivConsistentOnAllEdges = true;
        for (const auto &edge : latchPredEdges) {
            const Value &carried = edge.first->brArgs[edge.second][ivLatchParamIndex];
            if (carried.kind != Value::Kind::Temp || carried.id != headerIvId) {
                ivConsistentOnAllEdges = false;
                break;
            }
        }
        if (!ivConsistentOnAllEdges)
            continue;

        // The verifier accepts plain signed arithmetic only where its shared
        // range prover can re-establish a bound, and an incremental
        // loop-carried address is exactly the shape an interval fixpoint
        // cannot bound (the exit guard tests the counter, not the address).
        // The rewrite therefore emits checked (.ovf) arithmetic — always
        // verifier-legal — and fires only when every value it will compute,
        // including the one-past-exit increment the original never formed, is
        // provably free of i64 overflow, so the checked ops can never
        // introduce a trap the source program lacked.
        if (addrExpr->base.kind != Value::Kind::ConstInt)
            continue;
        const Value &initValue = phArgs[iv->headerParamIndex];
        if (initValue.kind != Value::Kind::ConstInt)
            continue;
        const zanna::analysis::IntRangeInfo ivRanges = zanna::analysis::computeIntRanges(function);
        const zanna::analysis::RangeMap *latchEntry = ivRanges.entryFor(latch->label);
        if (!latchEntry)
            continue;
        const auto ivRangeIt = latchEntry->find(iv->latchParamId);
        if (ivRangeIt == latchEntry->end() || !ivRangeIt->second.lower || !ivRangeIt->second.upper)
            continue;
        const int64_t base = addrExpr->base.i64;
        bool addrProvablySafe = true;
        const int64_t ivEndpoints[] = {
            *ivRangeIt->second.lower, *ivRangeIt->second.upper, initValue.i64};
        const int64_t ivOffsets[] = {0, static_cast<int64_t>(iv->step)};
        for (int64_t endpoint : ivEndpoints) {
            for (int64_t offset : ivOffsets) {
                if (il::utils::addOverflows(endpoint, offset)) {
                    addrProvablySafe = false;
                    break;
                }
                const int64_t stepped = endpoint + offset;
                if (il::utils::mulOverflows(stepped, addrExpr->stride)) {
                    addrProvablySafe = false;
                    break;
                }
                if (il::utils::addOverflows(base, stepped * addrExpr->stride)) {
                    addrProvablySafe = false;
                    break;
                }
            }
            if (!addrProvablySafe)
                break;
        }
        if (!addrProvablySafe)
            continue;

        unsigned nextId = zanna::il::nextTempId(function);
        Param addrParam{"addr", addrInstr->type, nextId++};
        const unsigned addrParamId = addrParam.id;
        setValueName(function, addrParamId, addrParam.name);

        // init_i
        Value initI = phArgs[iv->headerParamIndex];
        // Insert mul + add before terminator. Checked forms keep the emission
        // verifier-legal; the provability gate above guarantees they never trap.
        // mul
        Instr mul0;
        mul0.result = nextId++;
        mul0.op = Opcode::IMulOvf;
        mul0.type = mulInstr->type; // integer type
        mul0.operands.push_back(initI);
        mul0.operands.push_back(Value::constInt(addrExpr->stride));
        setValueName(function, *mul0.result, "addr.init.mul");
        // add
        Instr add0;
        add0.result = nextId++;
        add0.op = Opcode::IAddOvf;
        add0.type = addrInstr->type;
        add0.operands.push_back(*preheaderBase);
        add0.operands.push_back(Value::temp(*mul0.result));
        setValueName(function, *add0.result, "addr.init");

        size_t insertIdx = preheader->instructions.size();
        if (hasTerminator(*preheader) && insertIdx > 0)
            --insertIdx;

        header->params.push_back(addrParam);

        // Extend branch args from preheader to header before inserting into the
        // instruction vector because insertion can invalidate PHTerm/phArgs.
        Value addr0 = Value::temp(nextId - 1);
        phArgs.push_back(addr0);

        preheader->instructions.insert(preheader->instructions.begin() + insertIdx,
                                       std::move(mul0));
        preheader->instructions.insert(preheader->instructions.begin() + insertIdx + 1,
                                       std::move(add0));

        // Extend latch with a new param to carry addr
        Param latchAddr{"addr.l", header->params.back().type, nextId++};
        setValueName(function, latchAddr.id, latchAddr.name);
        latch->params.push_back(latchAddr);

        // Append the current address on every predecessor edge into the latch —
        // the header's edge plus any body join that re-enters the latch. The
        // header parameter dominates the whole natural-loop body, so it is in
        // scope at each of these branches, and its value is the same along
        // every in-iteration path (only the latch advances it).
        for (auto &edge : latchPredEdges)
            edge.first->brArgs[edge.second].push_back(Value::temp(addrParamId));

        // In latch, compute addr_next = latchAddr + (stride * step)
        Instr addInc;
        addInc.result = nextId++;
        const unsigned addIncId = *addInc.result;
        addInc.op = Opcode::IAddOvf;
        addInc.type = header->params.back().type;
        addInc.operands.push_back(Value::temp(latchAddr.id));
        addInc.operands.push_back(Value::constInt(*inc));
        setValueName(function, *addInc.result, "addr.next");

        // Update latch -> header branch args before inserting into the vector
        // because insertion can invalidate the terminator reference.
        LTerm.brArgs[*li].push_back(Value::temp(addIncId));

        // Insert before latch terminator
        size_t latchInsert = latch->instructions.size() - 1;
        latch->instructions.insert(latch->instructions.begin() + latchInsert, std::move(addInc));

        // Inside header, replace uses of computed addr with header param and erase the instructions
        zanna::il::UseDefInfo useInfo(function);
        useInfo.replaceAllUses(addrExpr->addrId, Value::temp(addrParamId));
        // Erase the add and its mul if dead (single-use guaranteed earlier)
        // Remove add first
        for (size_t i = 0; i < header->instructions.size(); ++i) {
            Instr &I = header->instructions[i];
            if (I.result && *I.result == addrExpr->addrId) {
                header->instructions.erase(header->instructions.begin() + i);
                break;
            }
        }
        // Now mul if unused
        if (countTempUses(function, addrExpr->mulId) == 0) {
            for (size_t i = 0; i < header->instructions.size(); ++i) {
                Instr &I = header->instructions[i];
                if (I.result && *I.result == addrExpr->mulId) {
                    header->instructions.erase(header->instructions.begin() + i);
                    break;
                }
            }
        }

        changed = true;
    }

    if (!changed)
        return PreservedAnalyses::all();
    PreservedAnalyses p; // conservatively invalidate
    p.preserveAllModules();
    return p;
}

/// @brief Register the IndVarSimplify pass with the pass registry.
/// @details Associates the "indvars" identifier with a factory that constructs
///          a new @ref IndVarSimplify instance.
/// @param registry Pass registry to update.
void registerIndVarSimplifyPass(PassRegistry &registry) {
    // Sequential: queries whole-module loop/dominator analyses while mutating loop blocks.
    registry.registerFunctionPass(
        "indvars", []() { return std::make_unique<IndVarSimplify>(); }, false);
}

} // namespace il::transform
