//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/vm/ops/common/Branching.cpp
// Purpose: Implement shared branching helpers used by VM opcode handlers.
// Key invariants: Helpers honour IL semantics by validating branch argument
//                 counts and propagating values before transferring control.
// Ownership/Lifetime: Operates on VM-owned state; no allocations escape the
//                     helper scope.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements shared switch selection and branch transfer mechanics.
/// @details These helpers resolve cached or direct block targets, validate
///          destination arity, retain string parameters, update control state,
///          and produce structured diagnostics for malformed branches.

#include "vm/ops/common/Branching.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "rt.hpp"
#include "vm/DiagFormat.hpp"
#include "vm/OpHandlerAccess.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/VMContext.hpp"

#include <cassert>
#include <cstdlib>
#include <string>
#include <utility>

namespace il::vm::ops::common {
namespace {
/// @brief Abort execution when a branch provides an incorrect number of arguments.
/// @details Formats a descriptive trap message that includes the source and
///          destination labels along with the expected/provided counts, then
///          routes the message through the runtime bridge before exiting. In
///          test mode the trap observer may return after recording the error.
/// @param target Destination block referenced by the branch.
/// @param source Source block issuing the branch (may be null for entry).
/// @param expected Number of parameters declared by the destination block.
/// @param provided Number of arguments supplied by the branch.
/// @param instr Branch instruction that triggered the mismatch.
/// @param frame Current frame supplying context for diagnostics.
void reportBranchArgMismatch(const il::core::BasicBlock &target,
                             const il::core::BasicBlock *source,
                             size_t expected,
                             size_t provided,
                             const il::core::Instr &instr,
                             const Frame &frame) {
    const std::string sourceLabel = source ? source->label : std::string{};
    const std::string functionName = frame.func ? frame.func->name : std::string{};

    RuntimeBridge::trap(
        TrapKind::InvalidOperation,
        diag::formatBranchArgMismatch(target.label, sourceLabel, expected, provided),
        instr.loc,
        functionName,
        sourceLabel);
    return;
}
} // namespace

/// @brief Resolve the target for a SELECT CASE-style dispatch.
/// @details Iterates the ordered case table, checking single-value entries first
///          followed by range entries.  The first match determines the target
///          block.  When no case matches the function returns @p default_tgt,
///          allowing opcode handlers to fall back to the default branch.
/// @param scrutinee Value being matched.
/// @param table Ordered list of case entries (singletons and ranges).
/// @param default_tgt Target describing the default branch.
/// @return Target describing the block that should be executed next.
Target select_case(Scalar scrutinee, std::span<const Case> table, Target default_tgt) {
    for (const Case &entry : table) {
        if (!entry.isRange) {
            if (scrutinee.value == entry.lower.value)
                return entry.target;
            continue;
        }

        if (scrutinee.value >= entry.lower.value && scrutinee.value <= entry.upper.value)
            return entry.target;
    }

    return default_tgt;
}

/// @brief Transfer control to the block described by @p target.
/// @details Validates the branch argument arity, evaluates operands using the
///          VM access helper, and moves the resulting slots into the destination
///          block's parameter array.  String parameters receive retain/release
///          bookkeeping to align with runtime ownership expectations.  Finally,
///          the function updates the caller's current block and instruction
///          pointer so the dispatch loop resumes at the new location.
/// @param frame Active frame that owns the parameter storage.
/// @param target Describes the branch instruction, destination map, and context.
void jump(Frame &frame, Target target) {
    try {
        if (!target.valid()) {
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                "invalid branch target",
                                target.instr ? target.instr->loc : il::support::SourceLoc{},
                                frame.func ? frame.func->name : std::string(),
                                target.currentBlock && *target.currentBlock
                                    ? (*target.currentBlock)->label
                                    : std::string());
            return;
        }

        const il::core::BasicBlock *dest = nullptr;
        if (auto *st = il::vm::detail::VMAccess::currentExecState(*target.vm)) {
            auto &cache = st->branchTargetCache;
            auto &resolved = cache[target.instr];
            if (resolved.size() != target.instr->labels.size()) {
                resolved.resize(target.instr->labels.size());
                for (size_t i = 0; i < target.instr->labels.size(); ++i) {
                    auto it = target.blocks->find(target.instr->labels[i]);
                    if (it == target.blocks->end()) {
                        RuntimeBridge::trap(TrapKind::InvalidOperation,
                                            "branch target label not found",
                                            target.instr->loc,
                                            frame.func ? frame.func->name : std::string(),
                                            *target.currentBlock ? (*target.currentBlock)->label
                                                                 : std::string());
                        return;
                    }
                    resolved[i] = it->second;
                }
            }
            if (target.labelIndex >= resolved.size() || !resolved[target.labelIndex]) {
                RuntimeBridge::trap(TrapKind::InvalidOperation,
                                    "branch target index out of range",
                                    target.instr->loc,
                                    frame.func ? frame.func->name : std::string(),
                                    *target.currentBlock ? (*target.currentBlock)->label
                                                         : std::string());
                return;
            }
            dest = resolved[target.labelIndex];
        } else {
            if (target.labelIndex >= target.instr->labels.size()) {
                RuntimeBridge::trap(TrapKind::InvalidOperation,
                                    "branch target index out of range",
                                    target.instr->loc,
                                    frame.func ? frame.func->name : std::string(),
                                    *target.currentBlock ? (*target.currentBlock)->label
                                                         : std::string());
                return;
            }
            auto it = target.blocks->find(target.instr->labels[target.labelIndex]);
            if (it == target.blocks->end()) {
                RuntimeBridge::trap(TrapKind::InvalidOperation,
                                    "branch target label not found",
                                    target.instr->loc,
                                    frame.func ? frame.func->name : std::string(),
                                    *target.currentBlock ? (*target.currentBlock)->label
                                                         : std::string());
                return;
            }
            dest = it->second;
        }
        const il::core::BasicBlock *sourceBlock = *target.currentBlock;

        const size_t expected = dest->params.size();
        const size_t provided = target.labelIndex < target.instr->brArgs.size()
                                    ? target.instr->brArgs[target.labelIndex].size()
                                    : 0;
        if (provided != expected) {
            reportBranchArgMismatch(*dest, sourceBlock, expected, provided, *target.instr, frame);
            return;
        }

        if (provided > 0) {
            const auto &args = target.instr->brArgs[target.labelIndex];
            for (size_t i = 0; i < provided; ++i) {
                const auto &param = dest->params[i];
                const auto id = param.id;
                if (id >= frame.params.size()) {
                    RuntimeBridge::trap(TrapKind::InvalidOperation,
                                        "block parameter ID out of range",
                                        target.instr->loc,
                                        frame.func ? frame.func->name : std::string(),
                                        dest->label);
                    return;
                }

                Slot incoming = detail::VMAccess::eval(*target.vm, frame, args[i]);

                if (param.type.kind == il::core::Type::Kind::Str) {
                    if (frame.paramsSet[id])
                        rt_str_release_maybe(frame.params[id].str);

                    rt_str_retain_maybe(incoming.str);
                    frame.params[id] = incoming;
                    frame.paramsSet[id] = 1;
                    continue;
                }

                frame.params[id] = incoming;
                frame.paramsSet[id] = 1;
            }
        }

        *target.currentBlock = dest;
        *target.ip = 0;
    } catch (const std::exception &ex) {
        const il::core::Instr *instr = target.instr;
        const std::string fnName = frame.func ? frame.func->name : std::string();
        const std::string blk = (target.currentBlock && *target.currentBlock)
                                    ? (*target.currentBlock)->label
                                    : std::string();
        std::string msg = std::string("branch jump exception: ") + ex.what();
        il::vm::RuntimeBridge::trap(TrapKind::InvalidOperation,
                                    msg,
                                    instr ? instr->loc : il::support::SourceLoc{},
                                    fnName,
                                    blk);
    }
}

/// @brief Evaluate the scrutinee operand for switch-like opcodes.
/// @details Looks up the active VM instance, evaluates the operand using the
///          generic VM access helper, and coerces the result to a 32-bit scalar
///          suitable for table lookups. A missing active VM raises an
///          InvalidOperation trap and returns a zero scalar only if the trap
///          observer permits execution to continue.
/// @param frame Active frame providing operand slots.
/// @param instr Instruction containing the scrutinee operand metadata.
/// @return Scalar representation of the scrutinee value.
Scalar eval_scrutinee(Frame &frame, const il::core::Instr &instr) {
    VM *vm = activeVMInstance();
    if (!vm) {
        RuntimeBridge::trap(TrapKind::InvalidOperation,
                            "active VM instance required to evaluate switch scrutinee",
                            instr.loc,
                            frame.func ? frame.func->name : std::string(),
                            "");
        return {};
    }
    Slot slot = detail::VMAccess::eval(*vm, frame, switchScrutinee(instr));
    Scalar scalar{};
    scalar.value = static_cast<int32_t>(slot.i64);
    return scalar;
}

} // namespace il::vm::ops::common
