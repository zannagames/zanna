//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/BytecodeVM_threaded.cpp
// Purpose: Computed-goto threaded interpreter dispatch for BytecodeVM.
// Key invariants:
//   - Only compiled on GCC/Clang (requires labels-as-values extension).
//   - Dispatch table covers all 256 opcodes; unhandled entries go to L_DEFAULT.
//   - Local copies of pc/sp/locals are synced back via SYNC_STATE before
//     any call that may observe or modify VM state.
// Ownership/Lifetime:
//   - Part of BytecodeVM; operates on member state via `this`.
// Links: BytecodeVM.hpp, BytecodeVM.cpp, Bytecode.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements the GCC/Clang computed-goto dispatch loop for `BytecodeVM`.
 *
 * Opcode behavior deliberately mirrors the portable switch loop in
 * `BytecodeVM.cpp`. Register-local copies of the program counter, stack
 * pointer, and locals pointer are synchronized before helpers that can trap,
 * re-enter bytecode, change frames, or expose debugger state. The dispatch
 * table is derived from `Bytecode.def`, with every unsupported byte value
 * routed to the invalid-opcode handler.
 */

#include "bytecode/BytecodeVM.hpp"
#include "bytecode/BytecodeSemantics.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/VMContext.hpp"
#include "vm/err_bridge.hpp"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace zanna {
namespace bytecode {

#if defined(__GNUC__) || defined(__clang__)

/// @brief Execute the loaded function using computed-goto ("threaded")
///        dispatch — the fast interpreter path on GCC/Clang.
/// @details Builds a 256-entry table of label addresses (labels-as-values
///          GNU extension) and jumps directly opcode→handler→next-opcode,
///          eliminating the switch range check and branch-predictor penalty
///          of the portable loop. The table is a local (not static): GCC's
///          C++ frontend rejects label addresses in designated initializers
///          and a static table would race on first-call init. Working copies
///          of pc/sp/locals are kept in registers and flushed back to member
///          state via SYNC_STATE before any call that may observe or unwind
///          the VM. Unmapped opcodes fall through to L_DEFAULT (trap).
/// @pre A validated function frame is active in `fp_`.
/// @post Execution leaves the VM halted, trapped, paused, or at a re-entrant
///       callback boundary, with register-local state flushed to VM members.
void BytecodeVM::runThreaded() {
// Label addresses (`&&L_x`) and computed gotos (`goto *p`) are GNU extensions
// that ISO C++ does not define. This dispatcher exists to use them, so the
// pedantic diagnostics are suppressed for its body only.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
    // Dispatch table for computed goto. Keep it local: GCC C++ rejects label
    // addresses in designated initializers, and static post-init mutation races.
    //
    // The opcode -> handler-label mapping is generated from bytecode/Bytecode.def
    // (single source of truth). Every slot defaults to &&L_DEFAULT; opcodes with a
    // real handler override it. BC_OPCODE_TRAP opcodes (TAIL_CALL, MAKE_ERROR,
    // OPCODE_COUNT) have no handler label and stay
    // routed to L_DEFAULT. The compiler enforces this classification: a missing
    // label triggers an "undefined label" error and an over-marked one triggers
    // -Wunused-label -Werror, so the table can never silently drift from the enum.
    void *dispatchTable[256];
    for (auto &slot : dispatchTable)
        slot = &&L_DEFAULT;
#define BC_OPCODE(name, value) dispatchTable[(value)] = &&L_##name;
#define BC_OPCODE_TRAP(name, value) /* no handler label: stays L_DEFAULT */
#include "bytecode/Bytecode.def"
#undef BC_OPCODE
#undef BC_OPCODE_TRAP

    state_ = VMState::Running;

    // Local copies for performance
    const uint32_t *code = fp_->func->code.data();
    uint32_t pc = fp_->pc;
    BCSlot *sp = sp_;
    BCSlot *locals = fp_->locals;

    uint32_t instr;

#define DISPATCH()                                                                                 \
    do {                                                                                           \
        if (!fp_ || !fp_->func) {                                                                  \
            SYNC_STATE();                                                                          \
            return;                                                                                \
        }                                                                                          \
        code = fp_->func->code.data();                                                             \
        if (!trustedDispatch_) {                                                                   \
            if (!ensurePcInRange(*fp_->func, pc, "BytecodeVM::runThreaded(fetch)")) {              \
                SYNC_STATE();                                                                      \
                return;                                                                            \
            }                                                                                      \
        }                                                                                          \
        SYNC_STATE();                                                                              \
        if (checkBreakpoint()) {                                                                   \
            state_ = VMState::Halted;                                                              \
            return;                                                                                \
        }                                                                                          \
        if (!trustedDispatch_) {                                                                   \
            instr = code[pc];                                                                      \
            if (!ensureStackForInstruction(*fp_, sp, instr, "BytecodeVM::runThreaded")) {          \
                SYNC_STATE();                                                                      \
                return;                                                                            \
            }                                                                                      \
        } else {                                                                                   \
            instr = code[pc];                                                                      \
        }                                                                                          \
        ++pc;                                                                                      \
        ++instrCount_;                                                                             \
        if (maxInstrCount_ != 0 && instrCount_ > maxInstrCount_) {                                  \
            SYNC_STATE();                                                                          \
            trap(TrapKind::Interrupt, "VM: step limit exceeded");                                  \
            return;                                                                                \
        }                                                                                          \
        /* Every slot is default-filled to &&L_DEFAULT (see runThreaded), so no */                 \
        /* per-dispatch null-guard is needed — the jump target is always valid. */                 \
        goto *dispatchTable[instr & 0xFF];                                                         \
    } while (0)

#define SYNC_STATE()                                                                               \
    do {                                                                                           \
        if (fp_)                                                                                   \
            fp_->pc = pc;                                                                          \
        sp_ = sp;                                                                                  \
    } while (0)

#define RELOAD_STATE()                                                                             \
    do {                                                                                           \
        code = fp_->func->code.data();                                                             \
        pc = fp_->pc;                                                                              \
        sp = sp_;                                                                                  \
        locals = fp_->locals;                                                                      \
    } while (0)

#define RETURN_OR_DISPATCH_TRAP()                                                                  \
    do {                                                                                           \
        if (state_ == VMState::Running) {                                                          \
            RELOAD_STATE();                                                                        \
            DISPATCH();                                                                            \
        }                                                                                          \
        return;                                                                                    \
    } while (0)

#define THREAD_TRAP_OR_DISPATCH(kind, message)                                                     \
    do {                                                                                           \
        SYNC_STATE();                                                                              \
        sp_ = sp;                                                                                  \
        trapOrDispatch((kind), (message));                                                         \
        RETURN_OR_DISPATCH_TRAP();                                                                 \
    } while (0)

    DISPATCH();

    // Stack Operations
L_NOP:
    DISPATCH();

L_DUP:
    SYNC_STATE();
    sp_ = sp;
    if (!duplicateTopSlot("BytecodeVM::DUP(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();

L_DUP2:
    SYNC_STATE();
    sp_ = sp;
    if (!duplicateTopTwoSlots("BytecodeVM::DUP2(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();

L_POP:
    SYNC_STATE();
    sp_ = sp;
    popOwnedSlots(1);
    RELOAD_STATE();
    DISPATCH();

L_POP2:
    SYNC_STATE();
    sp_ = sp;
    popOwnedSlots(2);
    RELOAD_STATE();
    DISPATCH();

L_SWAP: {
    SYNC_STATE();
    sp_ = sp;
    swapTopTwoSlots();
    RELOAD_STATE();
    DISPATCH();
}

L_ROT3: {
    SYNC_STATE();
    sp_ = sp;
    rotateTopThreeSlots();
    RELOAD_STATE();
    DISPATCH();
}

    // Local Variable Operations
L_LOAD_LOCAL: {
    uint8_t slot = decodeArg8_0(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!ensureLocalIndex(slot, "BytecodeVM::LOAD_LOCAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    if (!pushLocal(slot, "BytecodeVM::LOAD_LOCAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();
}

L_STORE_LOCAL: {
    uint8_t slot = decodeArg8_0(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!ensureLocalIndex(slot, "BytecodeVM::STORE_LOCAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    if (!storeLocal(slot, "BytecodeVM::STORE_LOCAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();
}

L_LOAD_LOCAL_W: {
    uint16_t slot = decodeArg16(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!ensureLocalIndex(slot, "BytecodeVM::LOAD_LOCAL_W(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    if (!pushLocal(slot, "BytecodeVM::LOAD_LOCAL_W(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();
}

L_STORE_LOCAL_W: {
    uint16_t slot = decodeArg16(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!ensureLocalIndex(slot, "BytecodeVM::STORE_LOCAL_W(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    if (!storeLocal(slot, "BytecodeVM::STORE_LOCAL_W(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();
}

L_INC_LOCAL: {
    uint8_t slot = decodeArg8_0(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!ensureLocalIndex(slot, "BytecodeVM::INC_LOCAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    locals[slot].i64 = wrappingAdd(locals[slot].i64, 1);
    DISPATCH();
}

L_DEC_LOCAL: {
    uint8_t slot = decodeArg8_0(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!ensureLocalIndex(slot, "BytecodeVM::DEC_LOCAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    locals[slot].i64 = wrappingSub(locals[slot].i64, 1);
    DISPATCH();
}

    // Constant Loading
L_LOAD_I8:
    sp->i64 = decodeArgI8_0(instr);
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();

L_LOAD_I16:
    sp->i64 = decodeArgI16(instr);
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();

L_LOAD_I32: {
    // Extended format: the 32-bit value is stored in the next code word.
    if (!ensureWordsAvailable(*fp_->func, pc, 1, "BytecodeVM::LOAD_I32(threaded)")) {
        SYNC_STATE();
        return;
    }
    int32_t val = static_cast<int32_t>(code[pc++]);
    sp->i64 = val;
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();
}

L_LOAD_I64:
    if (decodeArg16(instr) >= module_->i64Pool.size()) {
        SYNC_STATE();
        trap(TrapKind::InvalidOpcode, "LOAD_I64 constant index out of range");
        return;
    }
    sp->i64 = module_->i64Pool[decodeArg16(instr)];
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();

L_LOAD_F64:
    if (decodeArg16(instr) >= module_->f64Pool.size()) {
        SYNC_STATE();
        trap(TrapKind::InvalidOpcode, "LOAD_F64 constant index out of range");
        return;
    }
    sp->f64 = module_->f64Pool[decodeArg16(instr)];
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();

L_LOAD_STR: {
    uint16_t idx = decodeArg16(instr);
    if (!module_ || idx >= module_->stringPool.size()) {
        SYNC_STATE();
        trap(TrapKind::InvalidOpcode, "LOAD_STR constant index out of range");
        return;
    }
    sp->ptr = getStringLiteral(idx);
    if (sp->ptr) {
        if (!validateStringHandle(sp->ptr, "BytecodeVM::LOAD_STR(threaded)")) {
            SYNC_STATE();
            return;
        }
        rt_str_retain_maybe(static_cast<rt_string>(sp->ptr));
        setSlotOwnsString(sp, true);
    } else {
        setSlotOwnsString(sp, false);
    }
    sp++;
    DISPATCH();
}

L_LOAD_NULL:
    sp->ptr = nullptr;
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();

L_LOAD_ZERO:
    sp->i64 = 0;
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();

L_LOAD_ONE:
    sp->i64 = 1;
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();

L_LOAD_GLOBAL: {
    uint16_t gIdx = decodeArg16(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!loadGlobal(gIdx, "BytecodeVM::LOAD_GLOBAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();
}

L_STORE_GLOBAL: {
    uint16_t gIdx = decodeArg16(instr);
    SYNC_STATE();
    sp_ = sp;
    if (!storeGlobal(gIdx, "BytecodeVM::STORE_GLOBAL(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    RELOAD_STATE();
    DISPATCH();
}

L_LOAD_GLOBAL_ADDR: {
    uint16_t gIdx = decodeArg16(instr);
    if (gIdx >= globals_.size()) {
        SYNC_STATE();
        trap(TrapKind::InvalidOpcode, "LOAD_GLOBAL_ADDR index out of range");
        return;
    }
    sp->ptr = &globals_[gIdx];
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();
}

    // Integer Arithmetic
L_ADD_I64:
    sp[-2].i64 = wrappingAdd(sp[-2].i64, sp[-1].i64);
    sp--;
    DISPATCH();

L_SUB_I64:
    sp[-2].i64 = wrappingSub(sp[-2].i64, sp[-1].i64);
    sp--;
    DISPATCH();

L_MUL_I64:
    sp[-2].i64 = wrappingMul(sp[-2].i64, sp[-1].i64);
    sp--;
    DISPATCH();

L_SDIV_I64: {
    int64_t result = 0;
    TrapKind fault = TrapKind::None;
    if (!safeSignedDiv(sp[-2].i64, sp[-1].i64, result, fault)) {
        SYNC_STATE();
        sp_ = sp;
        if (!dispatchTrap(fault)) {
            trap(fault,
                 fault == TrapKind::DivideByZero ? "division by zero"
                                                 : "Overflow: integer division overflow");
            return;
        }
        RELOAD_STATE();
        DISPATCH();
    }
    sp[-2].i64 = result;
}
    sp--;
    DISPATCH();

L_UDIV_I64: {
    int64_t result = 0;
    TrapKind fault = TrapKind::None;
    if (!safeUnsignedDiv(sp[-2].i64, sp[-1].i64, result, fault)) {
        SYNC_STATE();
        sp_ = sp;
        if (!dispatchTrap(fault)) {
            trap(fault, "division by zero");
            return;
        }
        RELOAD_STATE();
        DISPATCH();
    }
    sp[-2].i64 = result;
}
    sp--;
    DISPATCH();

L_SREM_I64: {
    int64_t result = 0;
    TrapKind fault = TrapKind::None;
    if (!safeSignedRem(sp[-2].i64, sp[-1].i64, result, fault)) {
        SYNC_STATE();
        sp_ = sp;
        if (!dispatchTrap(fault)) {
            trap(fault,
                 fault == TrapKind::DivideByZero ? "division by zero"
                                                 : "Overflow: integer remainder overflow");
            return;
        }
        RELOAD_STATE();
        DISPATCH();
    }
    sp[-2].i64 = result;
}
    sp--;
    DISPATCH();

L_UREM_I64: {
    int64_t result = 0;
    TrapKind fault = TrapKind::None;
    if (!safeUnsignedRem(sp[-2].i64, sp[-1].i64, result, fault)) {
        SYNC_STATE();
        sp_ = sp;
        if (!dispatchTrap(fault)) {
            trap(fault, "division by zero");
            return;
        }
        RELOAD_STATE();
        DISPATCH();
    }
    sp[-2].i64 = result;
}
    sp--;
    DISPATCH();

L_NEG_I64: {
    int64_t result = 0;
    TrapKind fault = TrapKind::None;
    if (!safeNegate(sp[-1].i64, result, fault)) {
        SYNC_STATE();
        sp_ = sp;
        if (!dispatchTrap(fault)) {
            trap(fault, "Overflow: integer negation overflow");
            return;
        }
        RELOAD_STATE();
        DISPATCH();
    }
    sp[-1].i64 = result;
}
    DISPATCH();

L_ADD_I64_OVF: {
    const auto result = il::semantics::checkedAdd(
        sp[-2].i64,
        sp[-1].i64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        THREAD_TRAP_OR_DISPATCH(detail::toBytecodeTrap(result.trap),
                                "Overflow: integer overflow in add");
    }
    sp[-2].i64 = result.value;
    sp--;
    DISPATCH();
}

L_SUB_I64_OVF: {
    const auto result = il::semantics::checkedSub(
        sp[-2].i64,
        sp[-1].i64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        THREAD_TRAP_OR_DISPATCH(detail::toBytecodeTrap(result.trap),
                                "Overflow: integer overflow in sub");
    }
    sp[-2].i64 = result.value;
    sp--;
    DISPATCH();
}

L_MUL_I64_OVF: {
    const auto result = il::semantics::checkedMul(
        sp[-2].i64,
        sp[-1].i64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        THREAD_TRAP_OR_DISPATCH(detail::toBytecodeTrap(result.trap),
                                "Overflow: integer overflow in mul");
    }
    sp[-2].i64 = result.value;
    sp--;
    DISPATCH();
}

L_SDIV_I64_CHK: {
    const auto result = il::semantics::signedDiv(
        sp[-2].i64,
        sp[-1].i64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        const TrapKind fault = detail::toBytecodeTrap(result.trap);
        THREAD_TRAP_OR_DISPATCH(fault,
                                fault == TrapKind::DivideByZero
                                    ? "division by zero"
                                    : "Overflow: integer division overflow");
    }
    sp[-2].i64 = result.value;
}
    sp--;
    DISPATCH();

L_UDIV_I64_CHK: {
    int64_t result = 0;
    TrapKind fault = TrapKind::None;
    if (!safeUnsignedDiv(sp[-2].i64, sp[-1].i64, result, fault)) {
        SYNC_STATE();
        sp_ = sp;
        if (!dispatchTrap(fault)) {
            trap(fault, "division by zero");
            return;
        }
        RELOAD_STATE();
        DISPATCH();
    }
    sp[-2].i64 = result;
}
    sp--;
    DISPATCH();

L_SREM_I64_CHK: {
    const auto result = il::semantics::signedRem(
        sp[-2].i64,
        sp[-1].i64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        const TrapKind fault = detail::toBytecodeTrap(result.trap);
        THREAD_TRAP_OR_DISPATCH(fault,
                                fault == TrapKind::DivideByZero
                                    ? "division by zero"
                                    : "Overflow: integer remainder overflow");
    }
    sp[-2].i64 = result.value;
}
    sp--;
    DISPATCH();

L_UREM_I64_CHK: {
    int64_t result = 0;
    TrapKind fault = TrapKind::None;
    if (!safeUnsignedRem(sp[-2].i64, sp[-1].i64, result, fault)) {
        SYNC_STATE();
        sp_ = sp;
        if (!dispatchTrap(fault)) {
            trap(fault, "division by zero");
            return;
        }
        RELOAD_STATE();
        DISPATCH();
    }
    sp[-2].i64 = result;
}
    sp--;
    DISPATCH();

L_IDX_CHK: {
    const auto result = il::semantics::boundsCheck(
        sp[-3].i64,
        sp[-2].i64,
        sp[-1].i64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        THREAD_TRAP_OR_DISPATCH(detail::toBytecodeTrap(result.trap), "index out of bounds");
    }
    sp[-3].i64 = result.value;
    sp -= 2;
    DISPATCH();
}

L_SELECT:
    // Whole-slot copy so integer and floating payloads both pass.
    sp[-3] = (sp[-3].i64 != 0) ? sp[-2] : sp[-1];
    sp -= 2;
    DISPATCH();

    // Float Arithmetic
L_ADD_F64:
    sp[-2].f64 += sp[-1].f64;
    sp--;
    DISPATCH();

L_SUB_F64:
    sp[-2].f64 -= sp[-1].f64;
    sp--;
    DISPATCH();

L_MUL_F64:
    sp[-2].f64 *= sp[-1].f64;
    sp--;
    DISPATCH();

L_DIV_F64:
    sp[-2].f64 /= sp[-1].f64;
    sp--;
    DISPATCH();

L_NEG_F64:
    sp[-1].f64 = -sp[-1].f64;
    DISPATCH();

    // Bitwise Operations
L_AND_I64:
    sp[-2].i64 &= sp[-1].i64;
    sp--;
    DISPATCH();

L_OR_I64:
    sp[-2].i64 |= sp[-1].i64;
    sp--;
    DISPATCH();

L_XOR_I64:
    sp[-2].i64 ^= sp[-1].i64;
    sp--;
    DISPATCH();

L_NOT_I64:
    sp[-1].i64 = ~sp[-1].i64;
    DISPATCH();

L_SHL_I64:
    sp[-2].i64 = wrappingShl(sp[-2].i64, sp[-1].i64);
    sp--;
    DISPATCH();

L_LSHR_I64:
    sp[-2].i64 = il::semantics::logicalShiftRight(sp[-2].i64, sp[-1].i64);
    sp--;
    DISPATCH();

L_ASHR_I64:
    sp[-2].i64 = arithmeticShr(sp[-2].i64, sp[-1].i64);
    sp--;
    DISPATCH();

    // Integer Comparisons
L_CMP_EQ_I64:
    sp[-2].i64 = (sp[-2].i64 == sp[-1].i64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_NE_I64:
    sp[-2].i64 = (sp[-2].i64 != sp[-1].i64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_SLT_I64:
    sp[-2].i64 = (sp[-2].i64 < sp[-1].i64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_SLE_I64:
    sp[-2].i64 = (sp[-2].i64 <= sp[-1].i64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_SGT_I64:
    sp[-2].i64 = (sp[-2].i64 > sp[-1].i64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_SGE_I64:
    sp[-2].i64 = (sp[-2].i64 >= sp[-1].i64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_ULT_I64:
    sp[-2].i64 = (static_cast<uint64_t>(sp[-2].i64) < static_cast<uint64_t>(sp[-1].i64)) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_ULE_I64:
    sp[-2].i64 = (static_cast<uint64_t>(sp[-2].i64) <= static_cast<uint64_t>(sp[-1].i64)) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_UGT_I64:
    sp[-2].i64 = (static_cast<uint64_t>(sp[-2].i64) > static_cast<uint64_t>(sp[-1].i64)) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_UGE_I64:
    sp[-2].i64 = (static_cast<uint64_t>(sp[-2].i64) >= static_cast<uint64_t>(sp[-1].i64)) ? 1 : 0;
    sp--;
    DISPATCH();

    // Float Comparisons
L_CMP_EQ_F64:
    sp[-2].i64 = (sp[-2].f64 == sp[-1].f64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_NE_F64:
    sp[-2].i64 = (sp[-2].f64 != sp[-1].f64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_LT_F64:
    sp[-2].i64 = (sp[-2].f64 < sp[-1].f64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_LE_F64:
    sp[-2].i64 = (sp[-2].f64 <= sp[-1].f64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_GT_F64:
    sp[-2].i64 = (sp[-2].f64 > sp[-1].f64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_GE_F64:
    sp[-2].i64 = (sp[-2].f64 >= sp[-1].f64) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_ORD_F64:
    sp[-2].i64 = (!std::isnan(sp[-2].f64) && !std::isnan(sp[-1].f64)) ? 1 : 0;
    sp--;
    DISPATCH();

L_CMP_UNO_F64:
    sp[-2].i64 = (std::isnan(sp[-2].f64) || std::isnan(sp[-1].f64)) ? 1 : 0;
    sp--;
    DISPATCH();

    // Type Conversions
L_I64_TO_F64:
    sp[-1].f64 = static_cast<double>(sp[-1].i64);
    DISPATCH();

L_U64_TO_F64:
    sp[-1].f64 = static_cast<double>(static_cast<uint64_t>(sp[-1].i64));
    DISPATCH();

L_F64_TO_I64: {
    int64_t converted = 0;
    TrapKind fault = TrapKind::None;
    if (!truncF64ToI64(sp[-1].f64, converted, fault))
        THREAD_TRAP_OR_DISPATCH(fault,
                                fault == TrapKind::InvalidCast
                                    ? "InvalidCast: invalid float to int conversion"
                                    : "Overflow: float to int conversion overflow");
    sp[-1].i64 = converted;
    DISPATCH();
}

L_BOOL_TO_I64:
    DISPATCH(); // No-op, already i64

L_I64_TO_BOOL:
    sp[-1].i64 = (sp[-1].i64 != 0) ? 1 : 0;
    DISPATCH();

L_I64_NARROW_CHK: {
    const auto result = il::semantics::signedNarrow(
        sp[-1].i64,
        detail::decodeNarrowWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        THREAD_TRAP_OR_DISPATCH(detail::toBytecodeTrap(result.trap),
                                "InvalidCast: signed narrow conversion out of range");
    }
    sp[-1].i64 = result.value;
    DISPATCH();
}

L_U64_NARROW_CHK: {
    const auto result = il::semantics::unsignedNarrow(
        static_cast<uint64_t>(sp[-1].i64),
        detail::decodeNarrowWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        THREAD_TRAP_OR_DISPATCH(detail::toBytecodeTrap(result.trap),
                                "InvalidCast: unsigned narrow conversion out of range");
    }
    sp[-1].i64 = result.value;
    DISPATCH();
}

L_F64_TO_I64_CHK: {
    const auto result = il::semantics::fpToSiRte(
        sp[-1].f64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        const TrapKind fault = detail::toBytecodeTrap(result.trap);
        THREAD_TRAP_OR_DISPATCH(fault,
                                fault == TrapKind::InvalidCast
                                    ? "InvalidCast: invalid float to int conversion"
                                    : "Overflow: float to int conversion overflow");
    }
    sp[-1].i64 = result.value;
    DISPATCH();
}

L_F64_TO_U64_CHK: {
    const auto result = il::semantics::fpToUiRte(
        sp[-1].f64,
        detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
    if (!result.ok()) {
        const TrapKind fault = detail::toBytecodeTrap(result.trap);
        THREAD_TRAP_OR_DISPATCH(fault,
                                fault == TrapKind::InvalidCast
                                    ? "InvalidCast: invalid float to uint conversion"
                                    : "Overflow: float to uint conversion overflow");
    }
    sp[-1].i64 = result.value;
    DISPATCH();
}

    // Memory Operations
L_ALLOCA: {
    // Allocate from the separate alloca buffer (not operand stack)
    const int64_t size = (--sp)->i64;
    void *ptr = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!allocateAlloca(size, ptr, "BytecodeVM::ALLOCA(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    sp->ptr = ptr;
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();
}

L_GEP: {
    int64_t offset = (--sp)->i64;
    void *adjusted = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!addPointerOffset(sp[-1].ptr, offset, adjusted, "BytecodeVM::GEP(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    sp[-1].ptr = adjusted;
    DISPATCH();
}

L_LOAD_I8_MEM: {
    void *ptr = sp[-1].ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(int8_t), "BytecodeVM::LOAD_I8_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    int8_t val;
    std::memcpy(&val, ptr, sizeof(val));
    sp[-1].i64 = val;
    DISPATCH();
}

L_LOAD_I16_MEM: {
    void *ptr = sp[-1].ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(int16_t), "BytecodeVM::LOAD_I16_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    int16_t val;
    std::memcpy(&val, ptr, sizeof(val));
    sp[-1].i64 = val;
    DISPATCH();
}

L_LOAD_I32_MEM: {
    void *ptr = sp[-1].ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(int32_t), "BytecodeVM::LOAD_I32_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    int32_t val;
    std::memcpy(&val, ptr, sizeof(val));
    sp[-1].i64 = val;
    DISPATCH();
}

L_LOAD_I64_MEM: {
    void *addr = sp[-1].ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(addr, sizeof(int64_t), "BytecodeVM::LOAD_I64_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    int64_t val;
    std::memcpy(&val, addr, sizeof(val));
    sp[-1].i64 = val;
    DISPATCH();
}

L_LOAD_F64_MEM: {
    void *ptr = sp[-1].ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(double), "BytecodeVM::LOAD_F64_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    double val;
    std::memcpy(&val, ptr, sizeof(val));
    sp[-1].f64 = val;
    DISPATCH();
}

L_LOAD_PTR_MEM: {
    void *addr = sp[-1].ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(addr, sizeof(void *), "BytecodeVM::LOAD_PTR_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    void *val;
    std::memcpy(&val, addr, sizeof(val));
    sp[-1].ptr = val;
    setSlotOwnsString(sp - 1, false);
    DISPATCH();
}

L_STORE_I8_MEM: {
    int8_t val = static_cast<int8_t>((--sp)->i64);
    void *ptr = (--sp)->ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(int8_t), "BytecodeVM::STORE_I8_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    clearGlobalStringOwnershipForRawStore(ptr, sizeof(int8_t));
    std::memcpy(ptr, &val, sizeof(val));
    DISPATCH();
}

L_STORE_I16_MEM: {
    int16_t val = static_cast<int16_t>((--sp)->i64);
    void *ptr = (--sp)->ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(int16_t), "BytecodeVM::STORE_I16_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    clearGlobalStringOwnershipForRawStore(ptr, sizeof(int16_t));
    std::memcpy(ptr, &val, sizeof(val));
    DISPATCH();
}

L_STORE_I32_MEM: {
    int32_t val = static_cast<int32_t>((--sp)->i64);
    void *ptr = (--sp)->ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(int32_t), "BytecodeVM::STORE_I32_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    clearGlobalStringOwnershipForRawStore(ptr, sizeof(int32_t));
    std::memcpy(ptr, &val, sizeof(val));
    DISPATCH();
}

L_STORE_I64_MEM: {
    int64_t val = (--sp)->i64;
    void *ptr = (--sp)->ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(int64_t), "BytecodeVM::STORE_I64_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    clearGlobalStringOwnershipForRawStore(ptr, sizeof(int64_t));
    std::memcpy(ptr, &val, sizeof(val));
    DISPATCH();
}

L_STORE_F64_MEM: {
    double val = (--sp)->f64;
    void *ptr = (--sp)->ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(double), "BytecodeVM::STORE_F64_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    clearGlobalStringOwnershipForRawStore(ptr, sizeof(double));
    std::memcpy(ptr, &val, sizeof(val));
    DISPATCH();
}

L_STORE_PTR_MEM: {
    void *val = (--sp)->ptr;
    void *ptr = (--sp)->ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(void *), "BytecodeVM::STORE_PTR_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    clearGlobalStringOwnershipForRawStore(ptr, sizeof(void *));
    std::memcpy(ptr, &val, sizeof(val));
    setSlotOwnsString(sp, false);
    setSlotOwnsString(sp + 1, false);
    DISPATCH();
}

L_LOAD_STR_MEM: {
    rt_string val = nullptr;
    void *ptr = sp[-1].ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(rt_string), "BytecodeVM::LOAD_STR_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    std::memcpy(&val, ptr, sizeof(val));
    sp[-1].ptr = val;
    if (val) {
        if (!validateStringHandle(val, "BytecodeVM::LOAD_STR_MEM(threaded)")) {
            SYNC_STATE();
            return;
        }
        rt_str_retain_maybe(val);
        setSlotOwnsString(sp - 1, true);
    } else {
        setSlotOwnsString(sp - 1, false);
    }
    DISPATCH();
}

L_STORE_STR_MEM: {
    BCSlot *valueSlot = --sp;
    rt_string incoming = static_cast<rt_string>(valueSlot->ptr);
    const bool incomingOwns = slotOwnsString(valueSlot);
    void *ptr = (--sp)->ptr;
    SYNC_STATE();
    sp_ = sp;
    if (!ensureMemoryAccess(ptr, sizeof(rt_string), "BytecodeVM::STORE_STR_MEM(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    rt_string current = nullptr;
    std::memcpy(&current, ptr, sizeof(current));
    if (current && !validateStringHandle(current, "BytecodeVM::STORE_STR_MEM(current, threaded)")) {
        SYNC_STATE();
        return;
    }
    if (incoming && !validateStringHandle(incoming, "BytecodeVM::STORE_STR_MEM(threaded)")) {
        SYNC_STATE();
        return;
    }
    if (incoming && (!incomingOwns || incoming == current))
        rt_str_retain_maybe(incoming);
    rt_str_release_maybe(current);
    std::memcpy(ptr, &incoming, sizeof(incoming));
    setGlobalStringOwnershipForAddress(ptr, incoming != nullptr);
    setSlotOwnsString(valueSlot, false);
    setSlotOwnsString(sp, false);
    DISPATCH();
}

    // Control Flow
L_JUMP:
    if (!computeRelativeTarget(*fp_->func, pc, decodeArgI16(instr), pc, "BytecodeVM::JUMP(threaded)")) {
        SYNC_STATE();
        return;
    }
    DISPATCH();

L_JUMP_IF_TRUE:
    if ((--sp)->i64 != 0) {
        if (!computeRelativeTarget(
                *fp_->func, pc, decodeArgI16(instr), pc, "BytecodeVM::JUMP_IF_TRUE(threaded)")) {
            SYNC_STATE();
            return;
        }
    }
    DISPATCH();

L_JUMP_IF_FALSE:
    if ((--sp)->i64 == 0) {
        if (!computeRelativeTarget(
                *fp_->func, pc, decodeArgI16(instr), pc, "BytecodeVM::JUMP_IF_FALSE(threaded)")) {
            SYNC_STATE();
            return;
        }
    }
    DISPATCH();

L_JUMP_LONG:
    if (!computeRelativeTarget(
            *fp_->func, pc, decodeArgI24(instr), pc, "BytecodeVM::JUMP_LONG(threaded)")) {
        SYNC_STATE();
        return;
    }
    DISPATCH();

L_SWITCH: {
    // Format: SWITCH [numCases:u32] [defaultOffset:i32] [caseVal:i32 caseOffset:i32]...
    // Pop scrutinee from stack
    int32_t scrutinee = static_cast<int32_t>((--sp)->i64);

    // pc currently points to the word after SWITCH opcode (numCases)
    if (!ensureWordsAvailable(*fp_->func, pc, 2, "BytecodeVM::SWITCH(header, threaded)")) {
        SYNC_STATE();
        return;
    }
    uint32_t numCases = code[pc++];

    // Position of default offset word
    uint32_t defaultOffsetPos = pc++;
    const uint64_t caseWords = static_cast<uint64_t>(numCases) * 2u;
    if (caseWords > std::numeric_limits<uint32_t>::max() ||
        !ensureWordsAvailable(*fp_->func,
                              pc,
                              static_cast<uint32_t>(caseWords),
                              "BytecodeVM::SWITCH(cases, threaded)")) {
        SYNC_STATE();
        return;
    }

    // Search for matching case
    bool found = false;
    for (uint32_t i = 0; i < numCases; ++i) {
        int32_t caseVal = static_cast<int32_t>(code[pc++]);
        uint32_t caseOffsetPos = pc++;

        if (caseVal == scrutinee) {
            // Found matching case - jump to its target
            // Offset is relative to the offset word position (same as EH_PUSH)
            int32_t caseOffset = static_cast<int32_t>(code[caseOffsetPos]);
            if (!computeRelativeTarget(*fp_->func,
                                       caseOffsetPos,
                                       caseOffset,
                                       pc,
                                       "BytecodeVM::SWITCH(case, threaded)")) {
                SYNC_STATE();
                return;
            }
            found = true;
            break;
        }
    }

    if (!found) {
        // No match - use default offset
        int32_t defaultOffset = static_cast<int32_t>(code[defaultOffsetPos]);
        if (!computeRelativeTarget(*fp_->func,
                                   defaultOffsetPos,
                                   defaultOffset,
                                   pc,
                                   "BytecodeVM::SWITCH(default, threaded)")) {
            SYNC_STATE();
            return;
        }
    }

    DISPATCH();
}

L_CALL: {
    uint16_t funcIdx = decodeArg16(instr);
    if (funcIdx >= module_->functions.size()) {
        SYNC_STATE();
        trap(TrapKind::RuntimeError, "Invalid function index");
        return;
    }
    SYNC_STATE();
    sp_ = sp;
    fp_->pc = pc;
    call(&module_->functions[funcIdx]);
    if (state_ != VMState::Running || !fp_)
        return;
    RELOAD_STATE();
    DISPATCH();
}

L_CALL_NATIVE: {
    uint8_t argCount = decodeArg8_0(instr);
    uint16_t nativeIdx = decodeArg16_1(instr);

    if (nativeIdx >= module_->nativeFuncs.size()) {
        SYNC_STATE();
        trap(TrapKind::RuntimeError, "Invalid native function index");
        return;
    }

    const NativeFuncRef &ref = module_->nativeFuncs[nativeIdx];
    if (argCount != ref.paramCount) {
        SYNC_STATE();
        trap(TrapKind::RuntimeError, "CALL_NATIVE encoded arity mismatch");
        return;
    }
    BCSlot *args = sp - argCount;
    BCSlot result{};

    if (runtimeBridgeEnabled_) {
        SYNC_STATE();
        if (!invokeRuntimeBridgeNative(ref, args, argCount, result)) {
            if (state_ == VMState::Trapped)
                return;
            RELOAD_STATE();
            DISPATCH();
        }
        RELOAD_STATE();
        args = sp - argCount;
    } else {
        auto it = nativeHandlers_.find(ref.name);
        if (it == nativeHandlers_.end()) {
            SYNC_STATE();
            trap(TrapKind::RuntimeError, "Native function not registered");
            return;
        }
        SYNC_STATE();
        sp_ = sp;
        it->second(args, argCount, &result);
        if (state_ != VMState::Running || !fp_)
            return;
        RELOAD_STATE();
        args = sp - argCount;
    }

    dismissConsumedStringArgs(ref, args, argCount);
    releaseCallArgs(args, argCount);

    sp -= argCount;
    if (ref.hasReturn) {
        *sp++ = result;
        if (ref.returnsString && result.ptr) {
            setSlotOwnsString(sp - 1, true);
        } else {
            setSlotOwnsString(sp - 1, false);
        }
    }
    DISPATCH();
}

L_ARR_I32_GET_FAST: {
    BCSlot *arrSlot = sp - 2;
    if (!arrSlot->ptr)
        THREAD_TRAP_OR_DISPATCH(TrapKind::NullPointer, "ARR_I32_GET_FAST on null array");
    BCSlot *idxSlot = --sp;
    const size_t idx = static_cast<size_t>(idxSlot->i64);
    setSlotOwnsString(idxSlot, false);
    int32_t *element = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!resolveArrayFastElement<int32_t>(arrSlot->ptr,
                                          idx,
                                          element,
                                          "ARR_I32_GET_FAST index address overflow",
                                          "BytecodeVM::ARR_I32_GET_FAST(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    arrSlot->i64 = static_cast<int64_t>(*element);
    setSlotOwnsString(arrSlot, false);
    DISPATCH();
}

L_ARR_I32_SET_FAST: {
    BCSlot *arrSlot = sp - 3;
    if (!arrSlot->ptr)
        THREAD_TRAP_OR_DISPATCH(TrapKind::NullPointer, "ARR_I32_SET_FAST on null array");
    BCSlot *valueSlot = --sp;
    const int32_t value = static_cast<int32_t>(valueSlot->i64);
    setSlotOwnsString(valueSlot, false);
    BCSlot *idxSlot = --sp;
    const size_t idx = static_cast<size_t>(idxSlot->i64);
    setSlotOwnsString(idxSlot, false);
    --sp;
    int32_t *element = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!resolveArrayFastElement<int32_t>(arrSlot->ptr,
                                          idx,
                                          element,
                                          "ARR_I32_SET_FAST index address overflow",
                                          "BytecodeVM::ARR_I32_SET_FAST(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    setSlotOwnsString(arrSlot, false);
    *element = value;
    DISPATCH();
}

L_ARR_I64_GET_FAST: {
    BCSlot *arrSlot = sp - 2;
    if (!arrSlot->ptr)
        THREAD_TRAP_OR_DISPATCH(TrapKind::NullPointer, "ARR_I64_GET_FAST on null array");
    BCSlot *idxSlot = --sp;
    const size_t idx = static_cast<size_t>(idxSlot->i64);
    setSlotOwnsString(idxSlot, false);
    int64_t *element = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!resolveArrayFastElement<int64_t>(arrSlot->ptr,
                                          idx,
                                          element,
                                          "ARR_I64_GET_FAST index address overflow",
                                          "BytecodeVM::ARR_I64_GET_FAST(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    arrSlot->i64 = *element;
    setSlotOwnsString(arrSlot, false);
    DISPATCH();
}

L_ARR_I64_SET_FAST: {
    BCSlot *arrSlot = sp - 3;
    if (!arrSlot->ptr)
        THREAD_TRAP_OR_DISPATCH(TrapKind::NullPointer, "ARR_I64_SET_FAST on null array");
    BCSlot *valueSlot = --sp;
    const int64_t value = valueSlot->i64;
    setSlotOwnsString(valueSlot, false);
    BCSlot *idxSlot = --sp;
    const size_t idx = static_cast<size_t>(idxSlot->i64);
    setSlotOwnsString(idxSlot, false);
    --sp;
    int64_t *element = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!resolveArrayFastElement<int64_t>(arrSlot->ptr,
                                          idx,
                                          element,
                                          "ARR_I64_SET_FAST index address overflow",
                                          "BytecodeVM::ARR_I64_SET_FAST(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    setSlotOwnsString(arrSlot, false);
    *element = value;
    DISPATCH();
}

L_ARR_F64_GET_FAST: {
    BCSlot *arrSlot = sp - 2;
    if (!arrSlot->ptr)
        THREAD_TRAP_OR_DISPATCH(TrapKind::NullPointer, "ARR_F64_GET_FAST on null array");
    BCSlot *idxSlot = --sp;
    const size_t idx = static_cast<size_t>(idxSlot->i64);
    setSlotOwnsString(idxSlot, false);
    double *element = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!resolveArrayFastElement<double>(arrSlot->ptr,
                                         idx,
                                         element,
                                         "ARR_F64_GET_FAST index address overflow",
                                         "BytecodeVM::ARR_F64_GET_FAST(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    arrSlot->f64 = *element;
    setSlotOwnsString(arrSlot, false);
    DISPATCH();
}

L_ARR_F64_SET_FAST: {
    BCSlot *arrSlot = sp - 3;
    if (!arrSlot->ptr)
        THREAD_TRAP_OR_DISPATCH(TrapKind::NullPointer, "ARR_F64_SET_FAST on null array");
    BCSlot *valueSlot = --sp;
    const double value = valueSlot->f64;
    setSlotOwnsString(valueSlot, false);
    BCSlot *idxSlot = --sp;
    const size_t idx = static_cast<size_t>(idxSlot->i64);
    setSlotOwnsString(idxSlot, false);
    --sp;
    double *element = nullptr;
    SYNC_STATE();
    sp_ = sp;
    if (!resolveArrayFastElement<double>(arrSlot->ptr,
                                         idx,
                                         element,
                                         "ARR_F64_SET_FAST index address overflow",
                                         "BytecodeVM::ARR_F64_SET_FAST(threaded)"))
        RETURN_OR_DISPATCH_TRAP();
    setSlotOwnsString(arrSlot, false);
    *element = value;
    DISPATCH();
}

L_CALL_INDIRECT: {
    // Indirect call through function pointer
    // Stack layout: [callee][arg0][arg1]...[argN] <- sp
    uint8_t argCount = decodeArg8_0(instr);

    // Get callee from below the arguments
    BCSlot *callee = sp - argCount - 1;
    BCSlot *args = sp - argCount;

    // Check if callee is a tagged function pointer (high bit set)
    constexpr uint64_t kFuncPtrTag = 0x8000000000000000ULL;
    uint64_t calleeVal = static_cast<uint64_t>(callee->i64);

    if (calleeVal & kFuncPtrTag) {
        // Tagged function index - extract and call
        uint32_t funcIdx = static_cast<uint32_t>(calleeVal & 0x7FFFFFFFULL);
        if (funcIdx >= module_->functions.size()) {
            SYNC_STATE();
            trap(TrapKind::RuntimeError, "Invalid indirect function index");
            return;
        }
        const BytecodeFunction &targetFunc = module_->functions[funcIdx];
        if (argCount != targetFunc.numParams) {
            SYNC_STATE();
            trap(TrapKind::RuntimeError, "Indirect call arity mismatch");
            return;
        }

        // Shift arguments down to overwrite the callee slot
        // This is needed because call() expects args at the top of stack
        for (int i = 0; i < argCount; ++i) {
            callee[i] = args[i];
            setSlotOwnsString(callee + i, slotOwnsString(args + i));
            if (callee + i != args + i)
                setSlotOwnsString(args + i, false);
        }
        sp = callee + argCount; // Adjust stack pointer

        SYNC_STATE();
        sp_ = sp;
        fp_->pc = pc;
        call(&targetFunc);
        if (state_ != VMState::Running || !fp_)
            return;
        RELOAD_STATE();
    } else if (calleeVal == 0) {
        // Null function pointer
        SYNC_STATE();
        trap(TrapKind::NullPointer, "Null indirect callee");
        return;
    } else {
        // Unknown pointer format - try runtime bridge
        // (This handles cases where the function pointer came from runtime)
        SYNC_STATE();
        trap(TrapKind::RuntimeError, "Invalid indirect call target");
        return;
    }
    DISPATCH();
}

L_RETURN: {
    SYNC_STATE();
    sp_ = sp;
    if (!returnValueFromFrame())
        return;
    RELOAD_STATE();
    DISPATCH();
}

L_RETURN_VOID: {
    SYNC_STATE();
    sp_ = sp;
    if (!returnVoidFromFrame())
        return;
    RELOAD_STATE();
    DISPATCH();
}

L_STR_RETAIN:
    if (!retainStringSlot(sp - 1, "BytecodeVM::STR_RETAIN(threaded)")) {
        SYNC_STATE();
        return;
    }
    DISPATCH();

L_STR_RELEASE:
    releaseOwnedString(sp - 1);
    sp--;
    DISPATCH();

    //==========================================================================
    // Exception Handling
    //==========================================================================

L_EH_PUSH: {
    // Handler offset is in the next code word (raw i32 offset)
    if (!ensureWordsAvailable(*fp_->func, pc, 1, "BytecodeVM::EH_PUSH(threaded)")) {
        SYNC_STATE();
        return;
    }
    const uint32_t offsetPc = pc;
    int32_t offset = static_cast<int32_t>(code[pc++]);
    uint32_t handlerPc = 0;
    if (!computeRelativeTarget(
            *fp_->func, offsetPc, offset, handlerPc, "BytecodeVM::EH_PUSH(threaded)")) {
        SYNC_STATE();
        return;
    }
    SYNC_STATE();
    sp_ = sp;
    pushExceptionHandler(handlerPc);
    DISPATCH();
}

L_EH_POP: {
    SYNC_STATE();
    popExceptionHandler();
    DISPATCH();
}

L_EH_ENTRY: {
    // Handler entry marker - no-op
    // The error and resume token are already on the stack from dispatchTrap
    DISPATCH();
}

L_ERR_GET_KIND: {
    // The error token already carries the trap discriminator; replace-in-place
    // is therefore a no-op with the correct consume-one / produce-one shape.
    setSlotOwnsString(sp - 1, false);
    DISPATCH();
}

L_ERR_GET_CODE: {
    // Replace the error token with the extracted code.
    sp[-1].i64 = static_cast<int64_t>(currentErrorCode_);
    DISPATCH();
}

L_ERR_GET_IP: {
    sp[-1].i64 = trapRecord_.valid ? static_cast<int64_t>(trapRecord_.faultPc)
                                   : static_cast<int64_t>(fp_ ? fp_->pc : 0);
    DISPATCH();
}

L_ERR_GET_LINE: {
    sp[-1].i64 = trapRecord_.valid ? static_cast<int64_t>(trapRecord_.faultLine) : -1;
    DISPATCH();
}

L_ERR_GET_MSG: {
    // Owned string from the current trap message (parity with the tree-walking VM).
    const std::string &msg = trapMessage_;
    sp[-1].ptr = rt_string_from_bytes(msg.data(), msg.size());
    setSlotOwnsString(sp - 1, true);
    DISPATCH();
}

L_TRAP: {
    uint8_t kind = decodeArg8_0(instr);
    TrapKind trapKind = static_cast<TrapKind>(kind);
    SYNC_STATE();
    sp_ = sp;
    if (!dispatchTrap(trapKind)) {
        // Include trap kind name in message
        const char *msg =
            (trapKind == TrapKind::Overflow) ? "Overflow: unhandled trap" : "Trap: unhandled trap";
        trap(trapKind, msg);
        return;
    }
    RELOAD_STATE();
    DISPATCH();
}

L_TRAP_FROM_ERR: {
    int64_t errCode = (--sp)->i64;
    const il::vm::TrapKind vmTrapKind = il::vm::map_err_to_trap(static_cast<int32_t>(errCode));
    TrapKind trapKind = static_cast<TrapKind>(static_cast<int32_t>(vmTrapKind));
    SYNC_STATE();
    sp_ = sp;
    if (!dispatchTrap(trapKind, static_cast<int32_t>(errCode))) {
        trap(trapKind, "Unhandled trap from error");
        return;
    }
    RELOAD_STATE();
    DISPATCH();
}

L_RESUME_SAME: {
    SYNC_STATE();
    sp_ = sp;
    if (!resumeTrap(false)) {
        trap(TrapKind::InvalidOperation, "resume.same: invalid resume token");
        return;
    }
    RELOAD_STATE();
    DISPATCH();
}

L_RESUME_NEXT: {
    SYNC_STATE();
    sp_ = sp;
    if (!resumeTrap(true)) {
        trap(TrapKind::InvalidOperation, "resume.next: invalid resume token");
        return;
    }
    RELOAD_STATE();
    DISPATCH();
}

L_RESUME_LABEL: {
    BCSlot token = *--sp;
    setSlotOwnsString(sp, false);
    if (token.ptr != &trapRecord_ || !trapRecord_.valid) {
        SYNC_STATE();
        sp_ = sp;
        trap(TrapKind::InvalidOperation, "resume.label: invalid resume token");
        return;
    }
    clearTrapRecord();
    // Target offset is in the next code word (raw i32 offset)
    if (!ensureWordsAvailable(*fp_->func, pc, 1, "BytecodeVM::RESUME_LABEL(threaded)")) {
        SYNC_STATE();
        return;
    }
    const uint32_t offsetPc = pc;
    int32_t offset = static_cast<int32_t>(code[pc++]);
    if (!computeRelativeTarget(
            *fp_->func, offsetPc, offset, pc, "BytecodeVM::RESUME_LABEL(threaded)")) {
        SYNC_STATE();
        return;
    }
    DISPATCH();
}

L_TRAP_KIND: {
    // Push the current trap kind as an I64 for typed-catch comparison.
    // Values 0-11 are aligned with il::vm::TrapKind (vm/Trap.hpp).
    // BC-specific kinds (100+) map to RuntimeError(9) as catch-all.
    uint8_t raw = static_cast<uint8_t>(trapKind_);
    int64_t ilKind = (raw <= 11) ? static_cast<int64_t>(raw) : 9;
    sp->i64 = ilKind;
    setSlotOwnsString(sp, false);
    sp++;
    DISPATCH();
}

L_LINE:
    DISPATCH();

L_WATCH_VAR:
    DISPATCH();

L_BREAKPOINT: {
    SYNC_STATE();
    const uint32_t breakpointPc = pc > 0 ? pc - 1 : 0;
    if (requestDebugPause(true, breakpointPc)) {
        state_ = VMState::Halted;
        return;
    }
    DISPATCH();
}

L_DEFAULT:
    SYNC_STATE();
    trap(TrapKind::InvalidOpcode, "Unknown opcode");
    return;

#undef DISPATCH
#undef SYNC_STATE
#undef RELOAD_STATE
#undef RETURN_OR_DISPATCH_TRAP
#undef THREAD_TRAP_OR_DISPATCH
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

#endif // __GNUC__ || __clang__

} // namespace bytecode
} // namespace zanna
