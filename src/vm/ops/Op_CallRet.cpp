//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the VM opcode handlers that manage function calls and returns.
// The helpers evaluate operands, interact with the runtime bridge when the VM
// lacks a native implementation, and propagate return values back into the
// interpreter's frame.
//
//===----------------------------------------------------------------------===//

#include "vm/OpHandlers_Control.hpp"

#include "il/runtime/RuntimeSignatures.hpp"
#include "runtime/rt.hpp" // For fast-path runtime function calls
#include "vm/Marshal.hpp"
#include "vm/OpcodeHandlerHelpers.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/ZannaStringHandle.hpp"
#include "vm/tco.hpp"

/// @file
/// @brief Call and return opcode handlers for the VM interpreter.
/// @details These helpers provide the glue between IL call/return instructions
///          and the VM's execution environment.  Calls eagerly evaluate
///          arguments, dispatch to either VM-native functions or the runtime
///          bridge, synchronise mutated arguments back into registers/stack, and
///          write the result slot.  Returns capture the optional operand and
///          signal to the interpreter loop that unwinding should begin.

#include "support/small_vector.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <unordered_map>

// SmallArgBuffer: Uses SmallVector<Slot, 8> to avoid heap allocation for most function
// calls (those with <=8 arguments). The APIs now accept std::span<const Slot>.

namespace il::vm::detail::control {
namespace {
/// @brief True when @p name consumes the caller-owned string reference directly.
///
/// Helpers such as concat consume retained aliases inside the runtime bridge, so
/// their IL operands remain owned by the VM. Release helpers consume the exact
/// argument they receive, which means an owned temp register must be dismissed
/// after the call.
/// @param name Runtime function name to classify.
/// @return @c true when the callee consumes its exact string argument reference.
bool consumesCallerOwnedStringArg(std::string_view name) {
    return name == "rt_str_release" || name == "rt_str_release_maybe" ||
           name == "rt_memory_release_str" || name == "Zanna.String.ReleaseMaybe" ||
           name == "Zanna.Memory.ReleaseStr";
}
} // namespace

/// @brief Finalise a function by propagating the return value and signalling exit.
///
/// @details Return instructions optionally carry a single operand that is
///          evaluated before the frame unwinds.  The helper extracts that
///          operand, captures the resulting slot on the @ref VM::ExecResult, and
///          flips the @ref VM::ExecResult::returned flag so the dispatch loop can
///          unwind to the caller.  Branch metadata parameters are ignored for
///          this opcode; they are present to satisfy the handler signature.
///
/// @param vm Active virtual machine used to evaluate an optional result operand.
/// @param fr Frame owning the registers and temporary storage for the call.
/// @param in IL instruction describing the return operation.
/// @param blocks Map of block labels to basic block pointers (unused).
/// @param bb Reference to the current block pointer (unused).
/// @param ip Instruction pointer within @p bb (unused).
/// @return Execution result populated with the returned slot when present.
VM::ExecResult handleRet(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip) {
    (void)vm;
    (void)blocks;
    (void)bb;
    (void)ip;
    VM::ExecResult result{};
    if (!in.operands.empty())
        result.value = VMAccess::eval(vm, fr, in.operands[0]);
    result.returned = true;
    return result;
}

/// @brief Invoke a callee and write the result back into the destination register.
///
/// @details The handler performs the following sequence:
///          1. Evaluate all operand expressions eagerly so argument side effects
///             occur before dispatch.  This mirrors the IL semantics and keeps
///             runtime bridges deterministic.
///          2. Look up the callee within the VM's direct function map.  When a
///             match is found the VM-specific implementation executes via
///             @ref VMAccess::callFunction.
///          3. Fall back to @ref RuntimeBridge::call when the VM lacks a native
///             implementation, thereby delegating to the runtime library.
///          4. Persist the returned slot using @ref ops::storeResult so that
///             register lifetime management is centralised.
///          The handler never manipulates control-flow metadata directly; the
///          interpreter loop continues execution in the current block after the
///          call completes.
///
/// @param vm Virtual machine coordinating the call.
/// @param fr Active frame whose registers supply arguments and receive results.
/// @param in Instruction describing the call site and callee symbol.
/// @param blocks Map of block labels to block pointers (unused).
/// @param bb Pointer to the current block, used for TCO and diagnostics.
/// @param ip Instruction pointer used to inspect a following return for TCO.
/// @return Result structure with @ref VM::ExecResult::returned left false.
VM::ExecResult handleCall(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip) {
    (void)blocks;
    (void)ip;

    // Evaluate operands up front so argument propagation is explicit and
    // deterministic before dispatch.  This mirrors the IL semantics and avoids
    // leaking partially evaluated slots if a bridge call traps.
    il::support::SmallVector<Slot, 8> args;
    args.reserve(in.operands.size());
    for (const auto &op : in.operands)
        args.push_back(VMAccess::eval(vm, fr, op));

    Slot out{};
    // Guard to release any string result if not consumed (e.g., on exception or unused result)
    const bool isStringResult = (in.type.kind == il::core::Type::Kind::Str);
    ScopedSlotStringGuard outGuard(out.str, isStringResult);
    const std::string functionName = fr.func ? fr.func->name : std::string{};
    const std::string blockLabel = bb ? bb->label : std::string{};

    const auto &fnMap = VMAccess::functionMap(vm);
    auto it = fnMap.find(in.callee);
    if (it != fnMap.end()) {
        // Tail-call optimisation: call immediately followed by ret
#if defined(ZANNA_VM_TAILCALL) && ZANNA_VM_TAILCALL
        if (bb && (ip + 1) < bb->instructions.size()) {
            const auto &nextInstr = bb->instructions[ip + 1];
            if (nextInstr.op == il::core::Opcode::Ret) {
                // Only apply TCO if return types are compatible:
                // - If ret has no operand, callee must return void
                // - If ret has operand, callee must return non-void (value will be propagated)
                const bool retHasOperand = !nextInstr.operands.empty();
                const bool calleeReturnsVoid =
                    (it->second->retType.kind == il::core::Type::Kind::Void);
                const bool compatibleReturn = (retHasOperand == !calleeReturnsVoid);

                if (compatibleReturn &&
                    il::vm::tryTailCall(vm, it->second, std::span<const Slot>{args})) {
                    VM::ExecResult r{};
                    r.jumped = true; // prevent ip++ so entry ip=0 is executed
                    return r;
                }
            }
        }
#endif
        out = VMAccess::callFunction(vm, *it->second, args);
    } else {
        // =========================================================================
        // FAST PATH: Direct calls for hot runtime functions
        // =========================================================================
        // These functions are called frequently in game loops. Bypassing the
        // RuntimeBridge eliminates descriptor lookup, argument marshalling, and
        // context guard overhead - typically ~10-20x faster.
        // Uses a static hash map for O(1) lookup instead of sequential string
        // comparisons.

        /// @brief Identifier for runtime calls supported by the direct fast path.
        enum class FastPathId : uint8_t {
            InkeyStr,          ///< Read a pending key as a runtime string.
            TermLocate,        ///< Move the terminal cursor.
            TermColor,         ///< Set terminal foreground/background colors.
            TermCls,           ///< Clear the terminal.
            TimerMs,           ///< Read the millisecond timer.
            SleepMs,           ///< Sleep for a millisecond duration.
            Keypressed,        ///< Query whether keyboard input is pending.
            TermAltScreen,     ///< Toggle the alternate terminal screen.
            TermCursorVisible ///< Toggle terminal cursor visibility.
        };

        /// @brief Hash transparent string-view keys in the runtime fast-path table.
        struct SvHash {
            /// @brief Hash a runtime function name.
            /// @param sv Function name to hash.
            /// @return Hash value compatible with string-view equality.
            size_t operator()(std::string_view sv) const {
                return std::hash<std::string_view>{}(sv);
            }
        };

        static const std::unordered_map<std::string_view, FastPathId> kFastPathMap = {
            {"rt_inkey_str", FastPathId::InkeyStr},
            {"rt_term_locate_i32", FastPathId::TermLocate},
            {"rt_term_color_i32", FastPathId::TermColor},
            {"rt_term_cls", FastPathId::TermCls},
            {"rt_timer_ms", FastPathId::TimerMs},
            {"rt_sleep_ms", FastPathId::SleepMs},
            {"rt_keypressed", FastPathId::Keypressed},
            {"rt_term_alt_screen_i32", FastPathId::TermAltScreen},
            {"rt_term_cursor_visible_i32", FastPathId::TermCursorVisible},
        };

        auto fpIt = kFastPathMap.find(std::string_view(in.callee));
        if (fpIt != kFastPathMap.end()) {
            /// @brief Raise a stable argument-count trap for a runtime fast path.
            /// @param expected Required argument count.
            /// @return Returned execution result after recording the trap.
            auto trapFastPathArity = [&](size_t expected) -> VM::ExecResult {
                RuntimeBridge::trap(TrapKind::DomainError,
                                    il::vm::detail::formatArgumentCountError(
                                        std::string_view(in.callee), expected, args.size()),
                                    in.loc,
                                    functionName,
                                    blockLabel);
                VM::ExecResult result{};
                result.returned = true;
                return result;
            };
            switch (fpIt->second) {
                case FastPathId::InkeyStr:
                    if (args.size() != 0)
                        return trapFastPathArity(0);
                    out.str = rt_inkey_str();
                    break;
                case FastPathId::TermLocate:
                    if (args.size() != 2)
                        return trapFastPathArity(2);
                    rt_term_locate_i32(static_cast<int32_t>(args[0].i64),
                                       static_cast<int32_t>(args[1].i64));
                    break;
                case FastPathId::TermColor:
                    if (args.size() != 2)
                        return trapFastPathArity(2);
                    rt_term_color_i32(static_cast<int32_t>(args[0].i64),
                                      static_cast<int32_t>(args[1].i64));
                    break;
                case FastPathId::TermCls:
                    if (args.size() != 0)
                        return trapFastPathArity(0);
                    rt_term_cls();
                    break;
                case FastPathId::TimerMs:
                    if (args.size() != 0)
                        return trapFastPathArity(0);
                    out.i64 = rt_timer_ms();
                    break;
                case FastPathId::SleepMs:
                    if (args.size() != 1)
                        return trapFastPathArity(1);
                    rt_sleep_ms(static_cast<int32_t>(args[0].i64));
                    break;
                case FastPathId::Keypressed:
                    if (args.size() != 0)
                        return trapFastPathArity(0);
                    out.i64 = rt_keypressed();
                    break;
                case FastPathId::TermAltScreen:
                    if (args.size() != 1)
                        return trapFastPathArity(1);
                    rt_term_alt_screen_i32(static_cast<int32_t>(args[0].i64));
                    break;
                case FastPathId::TermCursorVisible:
                    if (args.size() != 1)
                        return trapFastPathArity(1);
                    rt_term_cursor_visible_i32(static_cast<int32_t>(args[0].i64));
                    break;
            }
            ops::storeResult(fr, in, out);
            return {};
        }

        // End of fast path - fall through to generic RuntimeBridge
        // =========================================================================

        // Build bindings and original values lazily only for runtime calls
        // Use SmallVector to avoid heap allocation for typical argument counts
        /// @brief Writable caller locations associated with a marshalled argument.
        struct ArgBinding {
            Slot *reg = nullptr;       ///< Register to synchronize after a mutable call.
            uint8_t *stackPtr = nullptr; ///< Frame-stack address to synchronize.
        };

        il::support::SmallVector<ArgBinding, 8> bindings;
        bindings.reserve(in.operands.size());
        il::support::SmallVector<Slot, 8> originalArgs;
        originalArgs.reserve(in.operands.size());

        uint8_t *const stackBegin = fr.stack.data();
        uint8_t *const stackEnd = stackBegin + fr.stack.size();

        for (size_t i = 0; i < in.operands.size(); ++i) {
            const auto &op = in.operands[i];
            ArgBinding binding{};
            if (op.kind == il::core::Value::Kind::Temp && op.id < fr.regs.size())
                binding.reg = &fr.regs[op.id];
            if (args[i].ptr) {
                auto *ptr = static_cast<uint8_t *>(args[i].ptr);
                if (ptr >= stackBegin && ptr < stackEnd)
                    binding.stackPtr = ptr;
            }
            bindings.push_back(binding);
            originalArgs.push_back(args[i]);
        }

        const auto *signature = il::runtime::findRuntimeSignature(in.callee);
        il::support::SmallVector<uint8_t, 4> consumedOwnedStringArgs;
        consumedOwnedStringArgs.resize(args.size(), 0);
        if (signature && consumesCallerOwnedStringArg(in.callee)) {
            const size_t paramCount = std::min(args.size(), signature->paramTypes.size());
            for (size_t index = 0; index < paramCount; ++index) {
                if (signature->paramTypes[index].kind != il::core::Type::Kind::Str)
                    continue;

                const auto &op = in.operands[index];
                const bool ownedTemp = op.kind == il::core::Value::Kind::Temp &&
                                       op.id < fr.regIsStr.size() && fr.regIsStr[op.id] != 0;
                if (ownedTemp) {
                    consumedOwnedStringArgs[index] = 1;
                } else {
                    // The release helper consumes its argument. If the source is
                    // a literal/cache/borrowed value, pass it an extra reference
                    // so the source remains valid after the helper returns.
                    rt_str_retain_maybe(args[index].str);
                }
            }
        }

        out = RuntimeBridge::callMutable(VMAccess::runtimeContext(vm),
                                         std::string_view(in.callee),
                                         std::span<Slot>{args.data(), args.size()},
                                         in.loc,
                                         functionName,
                                         blockLabel);

        for (size_t index = 0; index < consumedOwnedStringArgs.size(); ++index) {
            if (!consumedOwnedStringArgs[index])
                continue;
            const auto &op = in.operands[index];
            if (op.kind != il::core::Value::Kind::Temp || op.id >= fr.regIsStr.size())
                continue;
            fr.regIsStr[op.id] = 0;
            if (op.id < fr.regs.size())
                fr.regs[op.id].str = nullptr;
        }

        if (signature) {
            const size_t paramCount = std::min(args.size(), signature->paramTypes.size());
            for (size_t index = 0; index < paramCount; ++index) {
                const bool forcedOutCopy =
                    index < 64 && (signature->ownedOutArgMask & (std::uint64_t{1} << index)) != 0;
                const bool argUnchanged = args[index].bitwiseEquals(originalArgs[index]);
                // Compare slots using bitwise equality (safe for all types)
                if (!forcedOutCopy && argUnchanged)
                    continue;

                const auto kind = signature->paramTypes[index].kind;
                const ArgBinding &binding = bindings[index];
                const bool releaseTempStr = (kind == il::core::Type::Kind::Str);

                /// @brief Copy the current out-argument value into one bound register.
                /// @param destination Destination slot, or `nullptr` when unbound.
                auto assignRegister = [&](Slot *destination) {
                    if (!destination)
                        return;
                    if (kind == il::core::Type::Kind::Str) {
                        rt_str_release_maybe(destination->str);
                        Slot stored = args[index];
                        rt_str_retain_maybe(stored.str);
                        *destination = stored;
                    } else {
                        *destination = args[index];
                    }
                };

                assignRegister(binding.reg);

                if (binding.stackPtr && !(forcedOutCopy && argUnchanged)) {
                    /// @brief Return the ABI copy width for one IL value kind.
                    /// @param k Value kind to classify.
                    /// @return Number of bytes copied to stack-backed argument storage.
                    auto copyWidthForKind = [](il::core::Type::Kind k) -> size_t {
                        switch (k) {
                            case il::core::Type::Kind::I1:
                                return sizeof(uint8_t);
                            case il::core::Type::Kind::I16:
                                return sizeof(int16_t);
                            case il::core::Type::Kind::I32:
                                return sizeof(int32_t);
                            case il::core::Type::Kind::I64:
                                return sizeof(int64_t);
                            case il::core::Type::Kind::F64:
                                return sizeof(double);
                            case il::core::Type::Kind::Ptr:
                            case il::core::Type::Kind::Error:
                            case il::core::Type::Kind::ResumeTok:
                                return sizeof(void *);
                            case il::core::Type::Kind::Str:
                                return sizeof(rt_string);
                            case il::core::Type::Kind::Void:
                                return 0;
                        }
                        return 0;
                    };

                    const size_t width = copyWidthForKind(kind);
                    if (width != 0 && binding.stackPtr >= stackBegin &&
                        binding.stackPtr < stackEnd) {
                        const auto stackPtrAddr =
                            reinterpret_cast<std::uintptr_t>(binding.stackPtr);
                        const auto stackEndAddr = reinterpret_cast<std::uintptr_t>(stackEnd);
                        if (stackEndAddr - stackPtrAddr >= width) {
                            Slot &mutated = args[index];
                            if (kind == il::core::Type::Kind::Str) {
                                // Avoid strict-aliasing UB: copy via memcpy
                                rt_string current{};
                                std::memcpy(&current, binding.stackPtr, sizeof(current));
                                rt_str_release_maybe(current);
                                rt_string incoming = mutated.str;
                                rt_str_retain_maybe(incoming);
                                std::memcpy(binding.stackPtr, &incoming, sizeof(incoming));
                            } else if (void *src = il::vm::slotToArgPointer(mutated, kind)) {
                                std::memcpy(binding.stackPtr, src, width);
                            }
                        }
                    }
                }

                if (releaseTempStr) {
                    rt_str_release_maybe(args[index].str);
                    args[index].str = nullptr;
                }
            }
        }
    }
    // storeResult retains string destinations; the guard releases the transient
    // call result reference after that retain succeeds.
    ops::storeResult(fr, in, out);
    return {};
}

/// @brief Indirect call via a callee operand (global name or function pointer).
///
/// @details The first operand supplies either a GlobalAddr (callee identifier)
///          or a pointer value produced by loads (e.g., from an itable). When a
///          name is provided, behaviour mirrors direct calls. When a function
///          pointer is provided, the pointer must reference an IL function
///          instance and the VM invokes it directly. Remaining operands (if
///          any) are treated as arguments.
/// @param vm Virtual machine used for lookup, operand evaluation, and invocation.
/// @param fr Active frame supplying operands and receiving the result.
/// @param in Indirect-call instruction; operand zero identifies the callee.
/// @param blocks Block map required by the common handler signature; unused.
/// @param bb Current block pointer used for trap diagnostics.
/// @param ip Current instruction index; unused by indirect calls.
/// @return Normal continuation result, or a returned result after an invalid callee trap.
VM::ExecResult handleCallIndirect(VM &vm,
                                  Frame &fr,
                                  const il::core::Instr &in,
                                  const VM::BlockMap &blocks,
                                  const il::core::BasicBlock *&bb,
                                  size_t &ip) {
    (void)blocks;
    (void)ip;

    VM::ExecResult result{};
    if (in.operands.empty())
        return result;

    // Operand 0 may be a GlobalAddr or a function pointer value.
    const il::core::Value &calleeVal = in.operands[0];
    Slot out{};
    // Guard to release any string result if not consumed (e.g., on exception or unused result)
    const bool isStringResult = (in.type.kind == il::core::Type::Kind::Str);
    ScopedSlotStringGuard outGuard(out.str, isStringResult);

    if (calleeVal.kind == il::core::Value::Kind::GlobalAddr) {
        std::string calleeName = calleeVal.str;
        il::support::SmallVector<Slot, 8> args;
        if (in.operands.size() > 1) {
            args.reserve(in.operands.size() - 1);
            for (size_t i = 1; i < in.operands.size(); ++i)
                args.push_back(VMAccess::eval(vm, fr, in.operands[i]));
        }

        const auto &fnMap = VMAccess::functionMap(vm);
        auto it = fnMap.find(calleeName);
        if (it != fnMap.end()) {
            out = VMAccess::callFunction(vm, *it->second, args);
        } else {
            const std::string functionName = fr.func ? fr.func->name : std::string{};
            const std::string blockLabel = bb ? bb->label : std::string{};
            out = RuntimeBridge::callMutable(VMAccess::runtimeContext(vm),
                                             std::string_view(calleeName),
                                             std::span<Slot>{args.data(), args.size()},
                                             in.loc,
                                             functionName,
                                             blockLabel);
        }
    } else {
        // Pointer-based indirect call
        Slot callee = VMAccess::eval(vm, fr, calleeVal);
        if (!callee.ptr) {
            const std::string blockLabel = bb ? bb->label : std::string{};
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                "null indirect callee",
                                in.loc,
                                fr.func ? fr.func->name : std::string(),
                                blockLabel);
        }
        const auto *fn = reinterpret_cast<const il::core::Function *>(callee.ptr);
        if (!VMAccess::isKnownFunctionPointer(vm, fn)) {
            const std::string blockLabel = bb ? bb->label : std::string{};
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                "invalid indirect callee",
                                in.loc,
                                fr.func ? fr.func->name : std::string(),
                                blockLabel);
            VM::ExecResult trapped{};
            trapped.returned = true;
            return trapped;
        }
        il::support::SmallVector<Slot, 8> args;
        if (in.operands.size() > 1) {
            args.reserve(in.operands.size() - 1);
            for (size_t i = 1; i < in.operands.size(); ++i)
                args.push_back(VMAccess::eval(vm, fr, in.operands[i]));
        }
        out = VMAccess::callFunction(vm, *fn, args);
    }

    // storeResult retains string destinations; the guard releases the transient
    // call result reference after that retain succeeds.
    ops::storeResult(fr, in, out);
    return {};
}

} // namespace il::vm::detail::control
