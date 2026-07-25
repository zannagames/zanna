//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the trivial dead-code elimination pass used by the IL optimizer.
// The pass performs syntactic use counting, removes instructions whose results
// are never consumed, and prunes unused block parameters together with their
// corresponding branch arguments.  Additionally, it eliminates pure runtime
// helper calls whose results are unused, consulting the runtime signatures
// registry for side-effect metadata.  All mutations happen in place so callers
// can run DCE over a fully materialised module without rebuilding auxiliary
// data structures.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the dead-code elimination (DCE) pass for the IL pipeline.
/// @details The pass complements the lightweight optimiser by erasing trivially
///          dead temporaries, redundant stack traffic, and unused block
///          parameters.  It relies solely on syntactic information so that it
///          can run quickly and deterministically even in debug builds where
///          richer analysis infrastructure may be disabled.  The implementation
///          lives out of line to keep the public header minimal while providing a
///          single, well-documented reference for the pass' heuristics.

#include "il/transform/DCE.hpp"
#include "il/analysis/AllocaRoots.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Value.hpp"
#include "il/transform/CallEffects.hpp"
#include "il/transform/LoadSafety.hpp"
#include "il/transform/OverflowArithmetic.hpp"
#include <cstdlib>
#include <iostream>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {
/// @brief Query whether verbose DCE tracing was enabled at process startup.
/// @return True when the `ZANNA_DCE_TRACE` environment variable is present.
static bool traceEnabled() {
    static const bool enabled = std::getenv("ZANNA_DCE_TRACE") != nullptr;
    return enabled;
}

/// @brief Check whether an overflow-checked op with constant operands is
///        provably non-overflowing, making it safe to remove when dead.
/// @details If both operands are ConstInt and the operation does not overflow,
///          the instruction's only "side effect" (trapping) provably cannot
///          fire, so removing the instruction is semantics-preserving.
/// @param instr Candidate checked arithmetic instruction.
/// @return True when it is dead-result removable without suppressing a trap.
static bool isDeadOverflowOp(const il::core::Instr &instr) {
    using il::core::Opcode;
    using il::core::Value;

    if (instr.op != Opcode::IAddOvf && instr.op != Opcode::ISubOvf && instr.op != Opcode::IMulOvf)
        return false;
    if (instr.operands.size() < 2)
        return false;
    if (instr.operands[0].kind != Value::Kind::ConstInt ||
        instr.operands[1].kind != Value::Kind::ConstInt)
        return false;

    const long long lhs = instr.operands[0].i64;
    const long long rhs = instr.operands[1].i64;
    long long result{};
    switch (instr.op) {
        case Opcode::IAddOvf:
            return !il::transform::detail::addOverflows(lhs, rhs, result);
        case Opcode::ISubOvf:
            return !il::transform::detail::subOverflows(lhs, rhs, result);
        case Opcode::IMulOvf:
            return !il::transform::detail::mulOverflows(lhs, rhs, result);
        default:
            return false;
    }
}

/// @brief Classify nontrapping value-only opcodes removable when their result is dead.
/// @param op Opcode to classify.
/// @return True for pure arithmetic, comparison, conversion, select, and address
///         construction operations with no observable effect.
static bool isUnconditionallyRemovableValueOp(il::core::Opcode op) {
    using il::core::Opcode;
    switch (op) {
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::And:
        case Opcode::Or:
        case Opcode::Xor:
        case Opcode::Shl:
        case Opcode::LShr:
        case Opcode::AShr:
        case Opcode::FAdd:
        case Opcode::FSub:
        case Opcode::FMul:
        case Opcode::FDiv:
        case Opcode::ICmpEq:
        case Opcode::ICmpNe:
        case Opcode::SCmpLT:
        case Opcode::SCmpLE:
        case Opcode::SCmpGT:
        case Opcode::SCmpGE:
        case Opcode::UCmpLT:
        case Opcode::UCmpLE:
        case Opcode::UCmpGT:
        case Opcode::UCmpGE:
        case Opcode::FCmpEQ:
        case Opcode::FCmpNE:
        case Opcode::FCmpLT:
        case Opcode::FCmpLE:
        case Opcode::FCmpGT:
        case Opcode::FCmpGE:
        case Opcode::FCmpOrd:
        case Opcode::FCmpUno:
        case Opcode::Sitofp:
        case Opcode::CastSiToFp:
        case Opcode::CastUiToFp:
        case Opcode::Zext1:
        case Opcode::Trunc1:
        case Opcode::Select:
        case Opcode::GAddr:
        case Opcode::AddrOf:
        case Opcode::ConstF64:
        case Opcode::ConstNull:
            return true;
        default:
            return false;
    }
}

} // namespace

using namespace il::core;

namespace il::transform {
namespace {
constexpr unsigned kMaxDenseUseVectorTempId = 10'000'000;

/// @brief Use-count table that keeps the normal dense fast path with sparse fallback.
/// @details Well-formed compiler output tends to allocate compact temp ids, where a
///          vector is fastest.  Programmatic or hostile IR can contain a single
///          huge temp id; sparse mode avoids a massive allocation and lets DCE
///          continue conservatively instead of throwing.
struct UseCounts {
    /// @brief Dense count storage indexed directly by temp id.
    std::vector<size_t> dense;

    /// @brief Sparse count storage used when max temp id is too large for dense mode.
    std::unordered_map<unsigned, size_t> sparse;

    /// @brief Highest temp id observed while building the table.
    unsigned maxId = 0;

    /// @brief Whether lookups should consult @ref sparse instead of @ref dense.
    bool sparseMode = false;

    /// @brief Return the logical size used by existing bounds checks.
    /// @return One plus the highest observed temp id, without allocating that range.
    [[nodiscard]] size_t size() const {
        if (sparseMode)
            return static_cast<size_t>(maxId) + 1;
        return dense.size();
    }

    /// @brief Look up a temp's use count.
    /// @param id Temp id to query.
    /// @return Number of uses recorded for @p id, or zero when absent.
    [[nodiscard]] size_t operator[](unsigned id) const {
        if (!sparseMode)
            return id < dense.size() ? dense[id] : 0;
        auto it = sparse.find(id);
        return it == sparse.end() ? 0 : it->second;
    }

    /// @brief Increment the use count for @p id.
    /// @param id Temp id observed as an operand or branch argument.
    void increment(unsigned id) {
        maxId = std::max(maxId, id);
        if (!sparseMode && id > kMaxDenseUseVectorTempId) {
            sparse.reserve(dense.size());
            for (std::size_t denseId = 0; denseId < dense.size(); ++denseId)
                if (dense[denseId] != 0)
                    sparse.emplace(static_cast<unsigned>(denseId), dense[denseId]);
            dense.clear();
            dense.shrink_to_fit();
            sparseMode = true;
        }
        if (sparseMode) {
            ++sparse[id];
            return;
        }
        if (id >= dense.size())
            dense.resize(static_cast<size_t>(id) + 1, 0);
        ++dense[id];
    }


    /// @brief Remove one use and return the updated count.
    /// @param id Temporary whose reference is being removed.
    /// @return Remaining use count, or zero when absent/already unused.
    size_t decrement(unsigned id) {
        if (sparseMode) {
            auto it = sparse.find(id);
            if (it == sparse.end() || it->second == 0)
                return 0;
            if (--it->second == 0) {
                sparse.erase(it);
                return 0;
            }
            return it->second;
        }
        if (id >= dense.size() || dense[id] == 0)
            return 0;
        return --dense[id];
    }
};
} // namespace

/// @brief Count how many times each temporary identifier is referenced.
///
/// @details Determines the maximum SSA id and uses an indexed vector for
///          counts to avoid hashing overhead in large functions. The vector is
///          seeded to include block parameters (zero uses) and incremented for
///          every operand or branch argument referencing a temp id.
///
/// @param F Function whose temporaries are inspected.
/// @return Table indexed by temp id with use counts.
static UseCounts countUses(const Function &F) {
    // Compute maximum SSA id encountered across params, block params, and results
    unsigned maxId = 0;
    for (const auto &p : F.params)
        maxId = std::max(maxId, p.id);
    for (const auto &B : F.blocks) {
        for (const auto &p : B.params)
            maxId = std::max(maxId, p.id);
        for (const auto &I : B.instructions)
            if (I.result)
                maxId = std::max(maxId, static_cast<unsigned>(*I.result));
    }

    UseCounts uses;
    uses.maxId = maxId;
    uses.sparseMode = maxId > kMaxDenseUseVectorTempId;
    if (!uses.sparseMode)
        uses.dense.assign(static_cast<size_t>(maxId) + 1, 0);

    /// Record one temporary operand occurrence.
    auto touch = [&](unsigned id) { uses.increment(id); };

    for (const auto &B : F.blocks) {
        for (const auto &I : B.instructions) {
            for (const auto &Op : I.operands)
                if (Op.kind == Value::Kind::Temp)
                    touch(Op.id);
            for (const auto &ArgList : I.brArgs)
                for (const auto &Arg : ArgList)
                    if (Arg.kind == Value::Kind::Temp)
                        touch(Arg.id);
        }
    }
    return uses;
}

/// @brief Eliminate trivially dead instructions and block parameters.
///
/// @details The transformation iterates over every function in @p M and:
///   1. Builds use counts for temporaries via @ref countUses so later stages can
///      determine which results are never observed.
///   2. Records which @c alloca results are observed by @c load instructions to
///      distinguish genuine stack slots from those that only ever receive
///      stores.
///   3. Deletes dead loads/stores/allocas only when a local proof shows the
///      removed memory operation cannot trap. The instruction traversal keeps
///      indices stable by only advancing when no erasure occurred.
///   4. Walks block parameters in reverse order, removing unused entries and
///      erasing the corresponding operands from predecessor branch argument
///      lists so SSA form remains consistent.
/// Instructions that may observe side effects are generally preserved, but
/// calls to pure runtime helpers (as determined by the runtime signature
/// registry) can be safely eliminated when their results are unused.  This
/// enables more aggressive dead code removal while maintaining correctness.
/// The in-place updates mean callers can reuse existing module objects
/// without re-running expensive analyses.
///
/// @param M Module simplified in place.
void dce(Module &M) {
    for (auto &F : M.functions) {
        if (traceEnabled() && F.name == "main") {
            std::cerr << "[dce] === BEFORE DCE for " << F.name << " ===\n";
            for (auto &B : F.blocks) {
                std::cerr << B.label << ":\n";
                for (auto &I : B.instructions) {
                    std::cerr << "  ";
                    if (I.result)
                        std::cerr << "%" << *I.result << " = ";
                    std::cerr << toString(I.op);
                    for (auto &op : I.operands) {
                        std::cerr << " ";
                        if (op.kind == Value::Kind::Temp)
                            std::cerr << "%t" << op.id;
                        else if (op.kind == Value::Kind::ConstInt)
                            std::cerr << "i64(" << op.i64 << ")";
                        else if (op.kind == Value::Kind::ConstStr)
                            std::cerr << "str(\"" << op.str << "\")";
                        else if (op.kind == Value::Kind::GlobalAddr)
                            std::cerr << "global(@" << op.str << ")";
                        else if (op.kind == Value::Kind::ConstFloat)
                            std::cerr << "f64(" << op.f64 << ")";
                        else if (op.kind == Value::Kind::NullPtr)
                            std::cerr << "null";
                        else
                            std::cerr << "?kind=" << static_cast<int>(op.kind);
                    }
                    if (!I.callee.empty())
                        std::cerr << " " << I.callee;
                    for (size_t li = 0; li < I.labels.size(); ++li)
                        std::cerr << " -> " << I.labels[li];
                    std::cerr << "\n";
                }
            }
            std::cerr << "[dce] === END BEFORE ===\n";
        }
        UseCounts uses;
        bool removedInstruction = false;
        do {
            uses = countUses(F);
            removedInstruction = false;

            // Gather allocas and track if they are "observed" through a read,
            // escaping call argument, returned value, or branch argument.
            // Stores to unobserved alloca-derived addresses can be removed.
            std::unordered_map<unsigned, bool> allocaObserved;
            const auto allocaRootDefs = zanna::analysis::collectAllocaRootDefs(F);
            const auto allocaRoots = zanna::analysis::computeAllocaRoots(F, allocaRootDefs);

            for (auto &B : F.blocks)
                for (auto &I : B.instructions) {
                    if (I.op == Opcode::Alloca && I.result) {
                        allocaObserved[*I.result] = false;
                        if (traceEnabled())
                            std::cerr << "[dce] tracking alloca %" << *I.result << " in " << F.name
                                      << "\n";
                    }
                }

            /// Mark every alloca root reachable from an escaping or reading value.
            auto markObservedAllocaRoots = [&](const Value &value, std::string_view reason) {
                if (value.kind != Value::Kind::Temp)
                    return;
                bool marked = false;
                if (auto direct = allocaObserved.find(value.id); direct != allocaObserved.end()) {
                    direct->second = true;
                    marked = true;
                }
                auto rootsIt = allocaRoots.find(value.id);
                if (rootsIt != allocaRoots.end()) {
                    for (unsigned root : rootsIt->second) {
                        auto obsIt = allocaObserved.find(root);
                        if (obsIt == allocaObserved.end())
                            continue;
                        obsIt->second = true;
                        marked = true;
                    }
                }
                if (marked && traceEnabled())
                    std::cerr << "[dce] marking roots of %" << value.id << " as observed ("
                              << reason << ") in " << F.name << "\n";
            };

            /// Prove that a pointer targets at least one alloca and no observed root.
            auto targetsOnlyUnobservedAllocaRoots = [&](const Value &value) {
                if (value.kind != Value::Kind::Temp)
                    return false;
                bool sawAllocaRoot = false;
                auto direct = allocaObserved.find(value.id);
                if (direct != allocaObserved.end()) {
                    sawAllocaRoot = true;
                    if (direct->second)
                        return false;
                }
                auto rootsIt = allocaRoots.find(value.id);
                if (rootsIt != allocaRoots.end()) {
                    for (unsigned root : rootsIt->second) {
                        auto obsIt = allocaObserved.find(root);
                        if (obsIt == allocaObserved.end())
                            continue;
                        sawAllocaRoot = true;
                        if (obsIt->second)
                            return false;
                    }
                }
                return sawAllocaRoot;
            };

            for (auto &B : F.blocks)
                for (auto &I : B.instructions) {
                    if (I.op == Opcode::Load && !I.operands.empty())
                        markObservedAllocaRoots(I.operands[0], "load");
                    if (I.op == Opcode::Call || I.op == Opcode::CallIndirect)
                        for (auto &op : I.operands)
                            markObservedAllocaRoots(op, "call arg");
                    if (I.op == Opcode::Store && I.operands.size() > 1)
                        markObservedAllocaRoots(I.operands[1], "store value");
                    if (I.op == Opcode::Ret && !I.operands.empty())
                        markObservedAllocaRoots(I.operands[0], "ret");
                    for (const auto &argList : I.brArgs) {
                        for (const auto &v : argList)
                            markObservedAllocaRoots(v, "branch arg");
                    }
                }

            // Prove removals against one stable function snapshot. Deferring
            // erasure keeps the shared definition index valid and permits one
            // linear compaction per block.
            const LoadSafetyContext loadSafety(F);
            std::unordered_map<BasicBlock *, std::vector<std::size_t>> eraseByBlock;
            eraseByBlock.reserve(F.blocks.size());
            for (auto &B : F.blocks) {
                auto &toErase = eraseByBlock[&B];
                for (std::size_t i = 0; i < B.instructions.size(); ++i) {
                    Instr &I = B.instructions[i];
                    if (I.op == Opcode::Load && I.result && uses[*I.result] == 0 &&
                        isLoadKnownNonTrapping(loadSafety, I)) {
                        if (traceEnabled())
                            std::cerr << "[dce] removing dead load %" << *I.result << " in "
                                      << F.name << ":" << B.label << "\n";
                        toErase.push_back(i);
                        continue;
                    }
                    if (I.op == Opcode::Store && !I.operands.empty() &&
                        targetsOnlyUnobservedAllocaRoots(I.operands[0]) &&
                        isStoreKnownNonTrapping(loadSafety, I)) {
                        if (traceEnabled())
                            std::cerr << "[dce] removing dead store to %" << I.operands[0].id
                                      << " in " << F.name << ":" << B.label << "\n";
                        toErase.push_back(i);
                        continue;
                    }
                    if (I.op == Opcode::Alloca && I.result &&
                        allocaObserved.find(*I.result) != allocaObserved.end() &&
                        !allocaObserved[*I.result] && *I.result < uses.size() &&
                        uses[*I.result] == 0 && isAllocaKnownNonTrapping(I)) {
                        if (traceEnabled())
                            std::cerr << "[dce] removing dead alloca %" << *I.result << " in "
                                      << F.name << ":" << B.label << "\n";
                        toErase.push_back(i);
                        continue;
                    }
                    if (I.op == Opcode::Call && I.result && uses[*I.result] == 0) {
                        const CallEffects effects = classifyCallEffects(I, &M);
                        if (effects.canEliminateIfUnused()) {
                            if (traceEnabled())
                                std::cerr << "[dce] removing pure call %" << *I.result << " = "
                                          << I.callee << " in " << F.name << ":" << B.label << "\n";
                            toErase.push_back(i);
                            continue;
                        }
                    }
                    if (I.result && uses[*I.result] == 0 && isDeadOverflowOp(I)) {
                        if (traceEnabled())
                            std::cerr << "[dce] removing dead overflow op %" << *I.result << " = "
                                      << toString(I.op) << " in " << F.name << ":" << B.label
                                      << "\n";
                        toErase.push_back(i);
                        continue;
                    }
                    if (I.result && uses[*I.result] == 0 &&
                        isUnconditionallyRemovableValueOp(I.op)) {
                        toErase.push_back(i);
                        continue;
                    }
                }
            }

            // Propagate deadness through producer chains without rescanning the
            // whole function once per link. Memory-derived decisions remain in
            // the outer fixed point because removing a load can change alloca
            // observability, but ordinary SSA chains collapse in one worklist.
            /// @brief Stable location of a result-producing instruction.
            struct InstrLocation {
                /// Owning basic block.
                BasicBlock *block;
                /// Instruction index used for deferred compaction.
                std::size_t index;
                /// Borrowed instruction pointer valid until compaction.
                Instr *instr;
            };
            std::unordered_map<unsigned, InstrLocation> definitions;
            for (auto &B : F.blocks) {
                for (std::size_t i = 0; i < B.instructions.size(); ++i) {
                    Instr &instr = B.instructions[i];
                    if (instr.result)
                        definitions.emplace(*instr.result, InstrLocation{&B, i, &instr});
                }
            }
            /// Test whether dropping the final use makes an instruction removable.
            auto newlyDead = [&](Instr &instr) {
                if (!instr.result || uses[*instr.result] != 0)
                    return false;
                if (instr.op == Opcode::Load)
                    return isLoadKnownNonTrapping(loadSafety, instr);
                if (instr.op == Opcode::Alloca) {
                    auto observed = allocaObserved.find(*instr.result);
                    return observed != allocaObserved.end() && !observed->second &&
                           isAllocaKnownNonTrapping(instr);
                }
                if (instr.op == Opcode::Call)
                    return classifyCallEffects(instr, &M).canEliminateIfUnused();
                return isDeadOverflowOp(instr) || isUnconditionallyRemovableValueOp(instr.op);
            };

            std::queue<Instr *> deadWorklist;
            std::unordered_set<Instr *> marked;
            for (auto &[block, indices] : eraseByBlock) {
                for (std::size_t index : indices) {
                    Instr *instr = &block->instructions[index];
                    if (marked.insert(instr).second)
                        deadWorklist.push(instr);
                }
            }
            while (!deadWorklist.empty()) {
                Instr *dead = deadWorklist.front();
                deadWorklist.pop();
                /// Decrement an operand and enqueue its newly dead definition.
                auto releaseOperand = [&](const Value &operand) {
                    if (operand.kind != Value::Kind::Temp || uses.decrement(operand.id) != 0)
                        return;
                    auto def = definitions.find(operand.id);
                    if (def == definitions.end() || marked.contains(def->second.instr) ||
                        !newlyDead(*def->second.instr)) {
                        return;
                    }
                    marked.insert(def->second.instr);
                    eraseByBlock[def->second.block].push_back(def->second.index);
                    deadWorklist.push(def->second.instr);
                };
                for (const auto &operand : dead->operands)
                    releaseOperand(operand);
                for (const auto &args : dead->brArgs)
                    for (const auto &operand : args)
                        releaseOperand(operand);
            }
            for (auto &[block, indices] : eraseByBlock) {
                if (indices.empty())
                    continue;
                removedInstruction = true;
                std::vector<bool> remove(block->instructions.size(), false);
                for (std::size_t index : indices)
                    remove[index] = true;
                std::vector<Instr> compacted;
                compacted.reserve(block->instructions.size() - indices.size());
                for (std::size_t index = 0; index < block->instructions.size(); ++index) {
                    if (!remove[index])
                        compacted.push_back(std::move(block->instructions[index]));
                }
                block->instructions = std::move(compacted);
            }
        } while (removedInstruction);
        uses = countUses(F);

        std::unordered_set<unsigned> funcParamIds;
        funcParamIds.reserve(F.params.size());
        for (const auto &fp : F.params)
            funcParamIds.insert(fp.id);

        // Build a predecessor edge index: for each target label, collect
        // (terminator*, successorIndex) pairs.  This must happen AFTER the dead
        // instruction removal phase above, because erase() on the instruction
        // vector invalidates pointers into it.  Building the index earlier would
        // leave dangling Instr* entries for any block that had instructions
        // removed.
        std::unordered_map<std::string, std::vector<std::pair<Instr *, size_t>>> predEdges;
        {
            std::size_t totalLabels = 0;
            for (auto &PB : F.blocks)
                for (auto &I : PB.instructions)
                    totalLabels += I.labels.size();
            if (totalLabels)
                predEdges.reserve(totalLabels);
        }
        for (auto &PB : F.blocks) {
            for (auto &I : PB.instructions) {
                if (I.op != Opcode::Br && I.op != Opcode::CBr && I.op != Opcode::SwitchI32 &&
                    I.op != Opcode::ResumeLabel)
                    continue;
                for (size_t l = 0; l < I.labels.size(); ++l) {
                    predEdges[I.labels[l]].emplace_back(&I, l);
                }
            }
        }

        // Remove unused block params using compaction.
        //
        // The previous implementation iterated over params in reverse and called
        // erase() for each dead param, then iterated over all predecessors to
        // erase the corresponding brArg. This was O(#params * #preds) per block.
        //
        // The new algorithm:
        // 1. Build a vector of indices to keep (live params).
        // 2. Compact B.params in one pass using the keep list.
        // 3. For each predecessor edge, compact brArgs[succIdx] in one pass.
        // This reduces the complexity to O(#params + #preds) per block.
        for (auto &B : F.blocks) {
            const size_t numParams = B.params.size();
            if (numParams == 0)
                continue;

            // Handler blocks have a required ABI: (%err:Error, %tok:ResumeTok).
            // The VM populates both slots on exception dispatch regardless of
            // whether user code references %err.  Never remove params from a
            // block that starts with eh.entry (the handler entry marker).
            if (!B.instructions.empty() && B.instructions.front().op == il::core::Opcode::EhEntry)
                continue;

            // For the entry block, preserve the ABI-visible function-argument
            // prefix. Frontend-built modules may use distinct temp IDs for the
            // function params and the entry-block shadow params until the IL is
            // serialized and parsed again. The calling convention is positional,
            // so DCE must keep the leading entry params even when their IDs do
            // not match F.params.
            size_t entryAbiParamCount = 0;
            if (&B == &F.blocks.front())
                entryAbiParamCount = F.params.size();

            // Identify which param indices to keep.
            std::vector<size_t> keepIndices;
            keepIndices.reserve(numParams);
            for (size_t i = 0; i < numParams; ++i) {
                const unsigned id = B.params[i].id;
                // Always keep the entry-block prefix that corresponds to the
                // function signature. Preserve ID matches too so serialized IL
                // and hand-written modules keep the old fast path.
                if (i < entryAbiParamCount || funcParamIds.count(id)) {
                    keepIndices.push_back(i);
                    continue;
                }
                if (id < uses.size() && uses[id] == 0) {
                    if (traceEnabled())
                        std::cerr << "[dce] removing unused block param %" << id << " from "
                                  << B.label << "\n";
                    continue; // Dead param, skip
                }
                keepIndices.push_back(i);
            }

            // If all params are kept, nothing to do.
            if (keepIndices.size() == numParams)
                continue;

            auto it = predEdges.find(B.label);
            bool canCompactEdges = true;
            if (it != predEdges.end()) {
                for (auto &[term, succIdx] : it->second) {
                    if (term->brArgs.size() <= succIdx) {
                        canCompactEdges = false;
                        break;
                    }
                    auto &args = term->brArgs[succIdx];
                    if (args.size() != numParams) {
                        canCompactEdges = false;
                        break;
                    }
                }
            }
            if (!canCompactEdges)
                continue;

            // Compact B.params: rebuild the vector with only kept params.
            {
                std::vector<Param> compacted;
                compacted.reserve(keepIndices.size());
                for (size_t idx : keepIndices)
                    compacted.push_back(std::move(B.params[idx]));
                B.params = std::move(compacted);
            }

            // Compact predecessor brArgs for each edge targeting this block.
            if (it != predEdges.end()) {
                for (auto &[term, succIdx] : it->second) {
                    if (term->brArgs.size() <= succIdx)
                        continue;
                    auto &args = term->brArgs[succIdx];
                    if (args.size() != numParams)
                        continue;

                    std::vector<Value> compacted;
                    compacted.reserve(keepIndices.size());
                    for (size_t idx : keepIndices)
                        compacted.push_back(std::move(args[idx]));
                    args = std::move(compacted);
                }
            }
        }
    }
    M.internOwnedIdentifiers();
}

} // namespace il::transform
