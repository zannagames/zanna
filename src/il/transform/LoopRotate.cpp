//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/LoopRotate.cpp
// Purpose: Implements loop rotation (while → do-while transformation).
// Key invariants:
//   - Only single-latch, single-exit loops with duplicable headers rotate.
//   - Header parameters used in the body are reconstructed as block params.
//   - Plain signed arithmetic is restored to checked form before CFG changes
//     can invalidate the range proof that allowed its demotion.
// Ownership/Lifetime: Rewrites functions in place; analysis results and IL
//                     values are borrowed for the duration of each attempt.
// Links: il/transform/LoopRotate.hpp, il/transform/analysis/LoopInfo.hpp,
//        il/analysis/IntRangeAnalysis.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Converts while-style loops into do-while form.
/// @details A while-loop has its exit test in the header block:
///
///   ^header(params):
///       %cond = ...
///       cbr %cond, ^body, ^exit
///
/// After rotation, the header becomes a guard that runs once, the body block
/// absorbs the header's parameters, and a copy of the header's instructions
/// (the condition test) is appended to the latch:
///
///   ^guard:
///       [header instrs with initial args]
///       cbr %cond, ^body(initial_args), ^exit
///
///   ^body(params):
///       [original body]
///       [header instrs copy with latch args]
///       cbr %cond', ^body(next_args), ^exit
///
/// This eliminates one branch per iteration and creates a single-entry
/// loop body amenable to LICM and unrolling.

#include "il/transform/LoopRotate.hpp"

#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/SimplifyCFG/Utils.hpp"
#include "il/transform/analysis/LoopInfo.hpp"

#include "il/analysis/Dominators.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"
#include "il/verify/VerifierTable.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {

/// @brief Find a block's current position in a function.
/// @param function Function whose block vector is searched.
/// @param label Label of the desired block.
/// @return Block index, or `SIZE_MAX` when @p label is absent.
size_t findBlockIndex(const Function &function, const std::string &label) {
    for (size_t i = 0; i < function.blocks.size(); ++i) {
        if (function.blocks[i].label == label)
            return i;
    }
    return SIZE_MAX;
}

/// @brief Derive a block label that is unused in a function.
/// @param function Function defining the label namespace.
/// @param base Preferred label before collision suffixes.
/// @return @p base or a dot-suffixed variant unique within @p function.
std::string makeUniqueLabel(const Function &function, const std::string &base) {
    std::string candidate = base;
    unsigned suffix = 0;
    /// Return whether the candidate label is already present.
    auto labelExists = [&](const std::string &label) {
        for (const auto &block : function.blocks) {
            if (block.label == label)
                return true;
        }
        return false;
    };

    while (labelExists(candidate))
        candidate = base + "." + std::to_string(++suffix);
    return candidate;
}

/// @brief Check if header is a simple conditional branch block suitable for rotation.
/// @details The header must contain only non-side-effecting instructions followed
///          by a cbr terminator. All instructions before the cbr must be pure
///          (comparisons, arithmetic, casts) so they can be safely duplicated
///          into the latch block.
/// @param header Candidate loop header.
/// @param loop Loop membership used to distinguish the body and exit successors.
/// @return `true` when the header has one in-loop and one exit edge and all
///         instructions before the branch are duplicable and non-trapping.
bool isRotatableHeader(const BasicBlock &header, const Loop &loop) {
    if (header.instructions.empty())
        return false;

    const Instr &term = header.instructions.back();
    if (term.op != Opcode::CBr)
        return false;

    // Must have exactly two successors: one inside loop (body), one outside (exit)
    if (term.labels.size() != 2)
        return false;

    bool hasInside = false;
    bool hasOutside = false;
    for (const auto &label : term.labels) {
        if (loop.contains(label))
            hasInside = true;
        else
            hasOutside = true;
    }
    if (!hasInside || !hasOutside)
        return false;

    // All non-terminator instructions must be safe to duplicate:
    // pure value-producing instructions with no side effects.
    for (size_t i = 0; i + 1 < header.instructions.size(); ++i) {
        const auto &instr = header.instructions[i];
        if (!instr.result.has_value())
            return false;

        const auto &info = getOpcodeInfo(instr.op);
        // Reject instructions with side effects or that are terminators
        if (info.hasSideEffects || info.isTerminator)
            return false;
        if (const auto props = il::verify::lookup(instr.op); props && props->canTrap)
            return false;
        // Reject memory operations (loads/stores/allocas) — not safe to duplicate
        if (hasMemoryRead(instr.op) || hasMemoryWrite(instr.op))
            return false;
        // Reject calls
        if (instr.op == Opcode::Call || instr.op == Opcode::CallIndirect)
            return false;
    }

    return true;
}

/// @brief Clone an instruction, remapping temporary IDs according to a mapping.
/// @param src Source instruction to clone.
/// @param remap Map from old temp IDs to replacement values.
/// @param nextId Counter for allocating new IDs.
/// @return Cloned instruction with remapped operands and result.
Instr cloneInstr(const Instr &src,
                 const std::unordered_map<unsigned, Value> &remap,
                 unsigned &nextId) {
    Instr clone = src;

    // Remap result
    if (clone.result.has_value()) {
        unsigned newId = nextId++;
        clone.result = newId;
    }

    // Remap operands
    for (auto &op : clone.operands) {
        if (op.kind == Value::Kind::Temp) {
            auto it = remap.find(op.id);
            if (it != remap.end())
                op = it->second;
        }
    }

    for (auto &bundle : clone.brArgs) {
        for (auto &arg : bundle) {
            if (arg.kind == Value::Kind::Temp) {
                auto it = remap.find(arg.id);
                if (it != remap.end())
                    arg = it->second;
            }
        }
    }

    return clone;
}

/// @brief Record a name for a newly allocated SSA id.
/// @param function Function whose value-name table is extended.
/// @param id Temporary or block-parameter id to name.
/// @param name Name moved into the table entry.
void setValueName(Function &function, unsigned id, std::string name) {
    if (function.valueNames.size() <= id)
        function.valueNames.resize(id + 1);
    function.valueNames[id] = std::move(name);
}

/// @brief Attempt to rotate a single loop.
/// @param function Function containing the loop and receiving all CFG/SSA edits.
/// @param loop Loop description obtained before this rewrite attempt.
/// @return `true` if the loop was rotated.
/// @details Validates the complete loop shape before mutation, restores checked
///          arithmetic whose range proof could be invalidated, clones the header
///          test into a new guard and the latch, and threads any live header
///          parameters through the rotated body.
bool rotateLoop(Function &function, const Loop &loop) {
    // Require single latch and single exit for safety
    if (loop.latchLabels.size() != 1)
        return false;
    if (loop.exits.size() != 1)
        return false;

    size_t headerIdx = findBlockIndex(function, loop.headerLabel);
    if (headerIdx == SIZE_MAX)
        return false;

    // Check if the header is a simple rotatable conditional branch
    if (!isRotatableHeader(function.blocks[headerIdx], loop))
        return false;

    // Capture header state before modifications
    const std::string headerLabel = function.blocks[headerIdx].label;
    const std::vector<Param> headerParams = function.blocks[headerIdx].params;
    const auto &headerBlockInstrs = function.blocks[headerIdx].instructions;
    std::vector<Instr> headerInstrs(headerBlockInstrs.begin(), headerBlockInstrs.end());
    const Instr &headerTerm = headerInstrs.back();

    // Identify the body successor (inside loop) and exit successor (outside loop)
    std::string bodySuccLabel;
    std::string exitLabel;

    for (size_t i = 0; i < headerTerm.labels.size(); ++i) {
        if (loop.contains(headerTerm.labels[i])) {
            bodySuccLabel = headerTerm.labels[i];
        } else {
            exitLabel = headerTerm.labels[i];
        }
    }

    if (bodySuccLabel.empty() || exitLabel.empty())
        return false;

    // Don't rotate if the body successor is the header itself (while(true) {})
    if (bodySuccLabel == headerLabel)
        return false;

    // Find the latch block
    size_t latchIdx = findBlockIndex(function, loop.latchLabels[0]);
    if (latchIdx == SIZE_MAX)
        return false;

    // The latch must end with br ^header(args)
    if (function.blocks[latchIdx].instructions.empty())
        return false;
    const Instr &latchTerm = function.blocks[latchIdx].instructions.back();
    if (latchTerm.op != Opcode::Br)
        return false;
    if (latchTerm.labels.size() != 1 || latchTerm.labels[0] != headerLabel)
        return false;

    // Collect the latch's branch arguments to the header
    std::vector<Value> latchArgs;
    if (!latchTerm.brArgs.empty())
        latchArgs = latchTerm.brArgs[0];
    if (latchArgs.size() != headerParams.size())
        return false;

    // Collect outside-loop predecessors of the header (entry edges)
    /// @brief Stable indices identifying an external CFG edge into the header.
    struct EntryEdge {
        /// Index of the predecessor block.
        size_t blockIdx;
        /// Index of its terminator instruction.
        size_t instrIdx;
        /// Successor slot that names the old header.
        size_t labelIdx;
    };

    std::vector<EntryEdge> entryEdges;

    for (size_t bi = 0; bi < function.blocks.size(); ++bi) {
        for (size_t ii = 0; ii < function.blocks[bi].instructions.size(); ++ii) {
            auto &instr = function.blocks[bi].instructions[ii];
            if (!zanna::il::isTerminator(instr))
                continue;
            for (size_t li = 0; li < instr.labels.size(); ++li) {
                if (instr.labels[li] == headerLabel) {
                    if (loop.contains(function.blocks[bi].label)) {
                        if (bi != latchIdx)
                            return false;
                        continue;
                    }
                    entryEdges.push_back({bi, ii, li});
                }
            }
            break;
        }
    }

    if (entryEdges.empty())
        return false;

    // === SSA-reconstruction analysis (must run before any mutation) ===
    // After rotation the header no longer dominates the loop body, so any header
    // *parameter* that the body uses must be threaded into the body as a new
    // block parameter (fed by the guard on entry and the latch on the back-edge).
    // We only thread header params — they always carry a Type. To stay safe we
    // conservatively decline to rotate when a header instruction result is live
    // outside the header, when a header param is used outside the loop body
    // region, or when the body successor has an unexpected predecessor.
    /// Return whether a value directly refers to the requested temporary.
    auto valueRefs = [](const Value &v, unsigned id) {
        return v.kind == Value::Kind::Temp && v.id == id;
    };
    /// Return whether an instruction operand or branch argument refers to an id.
    auto instrRefs = [&](const Instr &ins, unsigned id) {
        for (const auto &op : ins.operands)
            if (valueRefs(op, id))
                return true;
        for (const auto &bundle : ins.brArgs)
            for (const auto &arg : bundle)
                if (valueRefs(arg, id))
                    return true;
        return false;
    };
    std::unordered_set<std::string> loopLabels(loop.blockLabels.begin(), loop.blockLabels.end());

    // Bail if any header instruction result is referenced outside the header.
    for (size_t i = 0; i + 1 < headerInstrs.size(); ++i) {
        if (!headerInstrs[i].result.has_value())
            continue;
        unsigned rid = *headerInstrs[i].result;
        for (const auto &bb : function.blocks) {
            if (bb.label == headerLabel)
                continue;
            for (const auto &ins : bb.instructions) {
                if (instrRefs(ins, rid))
                    return false;
            }
        }
    }

    // Determine which header params are used in the loop body region; bail if any
    // header param is referenced directly outside that region.
    std::vector<size_t> usedParamIdx;
    for (size_t pi = 0; pi < headerParams.size(); ++pi) {
        unsigned pid = headerParams[pi].id;
        bool usedInBody = false;
        for (const auto &bb : function.blocks) {
            if (bb.label == headerLabel)
                continue;
            bool used = false;
            for (const auto &ins : bb.instructions) {
                if (instrRefs(ins, pid)) {
                    used = true;
                    break;
                }
            }
            if (!used)
                continue;
            if (loopLabels.count(bb.label) != 0)
                usedInBody = true;
            else
                return false; // header param escapes the loop body region
        }
        if (usedInBody)
            usedParamIdx.push_back(pi);
    }

    // Bail if the body successor is entered from any block other than the header
    // (post-rotation its predecessors must be exactly the guard and the latch).
    for (const auto &bb : function.blocks) {
        if (bb.label == headerLabel || bb.instructions.empty())
            continue;
        const Instr &term = bb.instructions.back();
        if (!zanna::il::isTerminator(term))
            continue;
        for (const auto &lbl : term.labels) {
            if (lbl == bodySuccLabel)
                return false;
        }
    }

    // Plain add/sub/mul instructions are accepted only while range analysis
    // can prove that their checked counterparts cannot overflow. Rotation
    // changes the control-flow facts reaching the body and latch, so that
    // proof may no longer be reconstructible even though runtime semantics
    // are unchanged. Restore checked form before committing the CFG rewrite;
    // a later CheckOpt pass may demote instructions whose proof survives the
    // rotated shape.
    /// Restore trapping signed arithmetic before discarding its edge-range proof.
    const auto restoreCheckedArithmetic = [](Instr &instr) {
        switch (instr.op) {
            case Opcode::Add:
                instr.op = Opcode::IAddOvf;
                break;
            case Opcode::Sub:
                instr.op = Opcode::ISubOvf;
                break;
            case Opcode::Mul:
                instr.op = Opcode::IMulOvf;
                break;
            default:
                break;
        }
    };
    for (auto &block : function.blocks) {
        if (loopLabels.count(block.label) == 0)
            continue;
        for (auto &instr : block.instructions)
            restoreCheckedArithmetic(instr);
    }
    for (auto &instr : headerInstrs)
        restoreCheckedArithmetic(instr);

    unsigned nextId = zanna::il::nextTempId(function);

    // === Step 1: Turn the header into a guard block ===
    // The guard block contains the header's instructions with the original entry
    // arguments, branching to the body on true or the exit on false.
    // We keep the header block as-is but redirect its body-successor to point
    // to a new rotated body block.

    // Create new guard label
    std::string guardLabel = makeUniqueLabel(function, headerLabel + ".guard");

    // Build guard block: clone of header instructions
    BasicBlock guard;
    guard.label = guardLabel;
    guard.params = headerParams; // Same params as original header

    // Re-ID the guard's params
    std::unordered_map<unsigned, Value> guardRemap;
    for (auto &param : guard.params) {
        unsigned newId = nextId++;
        guardRemap[param.id] = Value::temp(newId);
        param.id = newId;
        setValueName(function, param.id, param.name);
    }

    // Clone header instructions into guard, remapping temp references
    for (const auto &instr : headerInstrs) {
        Instr clone = cloneInstr(instr, guardRemap, nextId);

        // Update remap with new result ID
        if (instr.result.has_value() && clone.result.has_value()) {
            guardRemap[*instr.result] = Value::temp(*clone.result);
            if (*instr.result < function.valueNames.size())
                setValueName(function, *clone.result, function.valueNames[*instr.result]);
        }

        guard.instructions.push_back(std::move(clone));
    }
    guard.terminated = true;

    // === Step 2: Modify the latch to include header condition ===
    // Remove the latch's unconditional br ^header and replace with:
    //   [cloned header instructions using latch args]
    //   cbr %cond, ^bodySucc(next_args), ^exit(exit_args)

    // Build remap from header params to latch args
    std::unordered_map<unsigned, Value> latchRemap;
    for (size_t pi = 0; pi < headerParams.size(); ++pi)
        latchRemap[headerParams[pi].id] = latchArgs[pi];

    // Remove the old latch terminator
    auto &latchInstrs = function.blocks[latchIdx].instructions;
    if (!latchInstrs.empty())
        latchInstrs.pop_back();

    // Clone header instructions into latch
    for (const auto &instr : headerInstrs) {
        Instr clone = cloneInstr(instr, latchRemap, nextId);

        if (instr.result.has_value() && clone.result.has_value()) {
            latchRemap[*instr.result] = Value::temp(*clone.result);
            if (*instr.result < function.valueNames.size())
                setValueName(function, *clone.result, function.valueNames[*instr.result]);
        }

        latchInstrs.push_back(std::move(clone));
    }
    function.blocks[latchIdx].terminated = true;

    // === Step 3: Redirect entry edges to the guard block ===
    for (const auto &edge : entryEdges) {
        auto &instr = function.blocks[edge.blockIdx].instructions[edge.instrIdx];
        instr.labels[edge.labelIdx] = guardLabel;
    }

    // === Step 4: SSA reconstruction — thread header params used in the body ===
    // Add a new parameter on the body-successor block for each header param the
    // body uses, rewrite the (now-undominated) uses to it, and feed it the guard
    // value on entry and the latch value on the back-edge.
    if (!usedParamIdx.empty()) {
        size_t bodyIdx = findBlockIndex(function, bodySuccLabel);
        if (bodyIdx == SIZE_MAX)
            return false;

        std::unordered_map<unsigned, unsigned> oldToNew;
        for (size_t pi : usedParamIdx) {
            const Param &hp = headerParams[pi];
            Param np;
            np.name = hp.name;
            np.type = hp.type;
            np.id = nextId++;
            setValueName(function, np.id, hp.name);
            function.blocks[bodyIdx].params.push_back(np);
            oldToNew[hp.id] = np.id;
        }

        // Rewrite the body's (and latch's) live uses of the header params. The
        // freshly cloned guard/latch condition references remapped ids, not the
        // original header param ids, so it is untouched. The dead header keeps
        // its internal references (harmless; SimplifyCFG removes it).
        for (const auto &kv : oldToNew)
            zanna::il::replaceAllUses(function, kv.first, Value::temp(kv.second));

        // Append incoming values for the new params on the guard and latch edges
        // to the body. Map any header-param-valued arg through oldToNew so the
        // back-edge passes the body's current value, not the now-dead header id.
        /// Append reconstructed parameter values to every edge targeting the body.
        auto threadArgs = [&](Instr &term, const std::unordered_map<unsigned, Value> &remap) {
            for (size_t li = 0; li < term.labels.size(); ++li) {
                if (term.labels[li] != bodySuccLabel)
                    continue;
                if (term.brArgs.size() < term.labels.size())
                    term.brArgs.resize(term.labels.size());
                for (size_t pi : usedParamIdx) {
                    unsigned origId = headerParams[pi].id;
                    auto it = remap.find(origId);
                    Value v = (it != remap.end()) ? it->second : Value::temp(origId);
                    if (v.kind == Value::Kind::Temp) {
                        auto rn = oldToNew.find(v.id);
                        if (rn != oldToNew.end())
                            v = Value::temp(rn->second);
                    }
                    term.brArgs[li].push_back(v);
                }
            }
        };
        threadArgs(guard.instructions.back(), guardRemap);
        threadArgs(function.blocks[latchIdx].instructions.back(), latchRemap);
    }

    // Add the guard block to the function.
    function.blocks.push_back(std::move(guard));

    // The original header now has no incoming labels (entry edges go to the
    // guard; the latch branches straight to the body). When we extended the
    // body's parameter list, the dead header still carries a stale branch to the
    // body with the old argument count, which the verifier rejects even though
    // the block is unreachable — so remove it here. (SimplifyCFG would otherwise
    // drop it on the next pass.) It is never the entry block: a loop header
    // always has an out-of-loop predecessor.
    if (!usedParamIdx.empty()) {
        size_t deadHeaderIdx = findBlockIndex(function, headerLabel);
        if (deadHeaderIdx != SIZE_MAX && deadHeaderIdx != 0)
            function.blocks.erase(function.blocks.begin() + static_cast<long>(deadHeaderIdx));
    }

    return true;
}

} // namespace

/// @copydoc LoopRotate::id()
std::string_view LoopRotate::id() const {
    return "loop-rotate";
}

/// @copydoc LoopRotate::run()
PreservedAnalyses LoopRotate::run(Function &function, AnalysisManager &analysis) {
    bool changed = false;
    for (;;) {
        LoopInfo loopInfo = computeLoopInfo(analysis.module(), function);
        bool changedThisIteration = false;

        for (const Loop &loop : loopInfo.loops()) {
            // Skip nested loops — only rotate outermost
            if (!loop.parentHeader.empty())
                continue;
            if (rotateLoop(function, loop)) {
                changed = true;
                changedThisIteration = true;
                break;
            }
        }

        if (!changedThisIteration)
            break;
    }

    if (!changed)
        return PreservedAnalyses::all();

    PreservedAnalyses preserved;
    preserved.preserveAllModules();
    return preserved;
}

} // namespace il::transform
