//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// IMPORTANT: This file is auto-generated. Do not edit manually.
//
// File: vm/ops/generated/InlineHandlersDecl.inc
// Purpose: Forward declarations of inline handler functions for all opcodes.
//
// Generator: Manually maintained (synchronized with Opcode enum).
// Regenerate: Update when adding/removing opcodes in il/core/Opcode.hpp.
//
// Key Invariants:
//   - One declaration per opcode in enum order
//   - Handler signature: void inline_handle_<OpName>(ExecState &st)
//
// Links: docs/il/il-guide.md#reference, vm/ops/generated/InlineHandlersImpl.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares one inline-dispatch wrapper for every IL opcode.
/// @details Every wrapper requires @c st.currentInstr and @c st.blocks to be
///          valid, delegates to the opcode's canonical handler, and feeds the
///          resulting control-flow state through @c handleInlineResult.
///
/// @name Inline opcode dispatch wrappers
/// @{
/// @brief Dispatch the `Add` opcode through its canonical handler and apply the result.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Add(ExecState &st);
/// @brief Dispatch the `Sub` opcode through its canonical handler and apply the result.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Sub(ExecState &st);
/// @brief Dispatch the `Mul` opcode through its canonical handler and apply the result.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Mul(ExecState &st);
/// @brief Dispatch checked signed addition and apply its result or overflow trap.
/// @param st Active execution state providing instruction and control context.
void inline_handle_IAddOvf(ExecState &st);
/// @brief Dispatch checked signed subtraction and apply its result or overflow trap.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ISubOvf(ExecState &st);
/// @brief Dispatch checked signed multiplication and apply its result or overflow trap.
/// @param st Active execution state providing instruction and control context.
void inline_handle_IMulOvf(ExecState &st);
/// @brief Dispatch signed integer division through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SDiv(ExecState &st);
/// @brief Dispatch unsigned integer division through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_UDiv(ExecState &st);
/// @brief Dispatch signed integer remainder through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SRem(ExecState &st);
/// @brief Dispatch unsigned integer remainder through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_URem(ExecState &st);
/// @brief Dispatch checked signed division and apply a divide-by-zero trap when required.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SDivChk0(ExecState &st);
/// @brief Dispatch checked unsigned division and apply a divide-by-zero trap when required.
/// @param st Active execution state providing instruction and control context.
void inline_handle_UDivChk0(ExecState &st);
/// @brief Dispatch checked signed remainder and apply a divide-by-zero trap when required.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SRemChk0(ExecState &st);
/// @brief Dispatch checked unsigned remainder and apply a divide-by-zero trap when required.
/// @param st Active execution state providing instruction and control context.
void inline_handle_URemChk0(ExecState &st);
/// @brief Dispatch an index bounds check and apply its continuation or trap result.
/// @param st Active execution state providing instruction and control context.
void inline_handle_IdxChk(ExecState &st);
/// @brief Dispatch bitwise conjunction through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_And(ExecState &st);
/// @brief Dispatch bitwise disjunction through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Or(ExecState &st);
/// @brief Dispatch bitwise exclusive-or through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Xor(ExecState &st);
/// @brief Dispatch a logical left shift through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Shl(ExecState &st);
/// @brief Dispatch a logical right shift through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_LShr(ExecState &st);
/// @brief Dispatch an arithmetic right shift through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_AShr(ExecState &st);
/// @brief Dispatch floating-point addition through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FAdd(ExecState &st);
/// @brief Dispatch floating-point subtraction through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FSub(ExecState &st);
/// @brief Dispatch floating-point multiplication through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FMul(ExecState &st);
/// @brief Dispatch floating-point division through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FDiv(ExecState &st);
/// @brief Dispatch integer equality comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ICmpEq(ExecState &st);
/// @brief Dispatch integer inequality comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ICmpNe(ExecState &st);
/// @brief Dispatch signed less-than comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SCmpLT(ExecState &st);
/// @brief Dispatch signed less-than-or-equal comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SCmpLE(ExecState &st);
/// @brief Dispatch signed greater-than comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SCmpGT(ExecState &st);
/// @brief Dispatch signed greater-than-or-equal comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SCmpGE(ExecState &st);
/// @brief Dispatch unsigned less-than comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_UCmpLT(ExecState &st);
/// @brief Dispatch unsigned less-than-or-equal comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_UCmpLE(ExecState &st);
/// @brief Dispatch unsigned greater-than comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_UCmpGT(ExecState &st);
/// @brief Dispatch unsigned greater-than-or-equal comparison through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_UCmpGE(ExecState &st);
/// @brief Dispatch ordered floating-point equality comparison.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpEQ(ExecState &st);
/// @brief Dispatch ordered floating-point inequality comparison.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpNE(ExecState &st);
/// @brief Dispatch ordered floating-point less-than comparison.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpLT(ExecState &st);
/// @brief Dispatch ordered floating-point less-than-or-equal comparison.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpLE(ExecState &st);
/// @brief Dispatch ordered floating-point greater-than comparison.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpGT(ExecState &st);
/// @brief Dispatch ordered floating-point greater-than-or-equal comparison.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpGE(ExecState &st);
/// @brief Dispatch the floating-point ordered predicate.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpOrd(ExecState &st);
/// @brief Dispatch the floating-point unordered predicate.
/// @param st Active execution state providing instruction and control context.
void inline_handle_FCmpUno(ExecState &st);
/// @brief Dispatch signed-integer-to-floating-point conversion.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Sitofp(ExecState &st);
/// @brief Dispatch floating-point-to-signed-integer conversion.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Fptosi(ExecState &st);
/// @brief Dispatch checked round-to-even conversion from floating point to signed integer.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CastFpToSiRteChk(ExecState &st);
/// @brief Dispatch checked round-to-even conversion from floating point to unsigned integer.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CastFpToUiRteChk(ExecState &st);
/// @brief Dispatch checked narrowing of a signed integer.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CastSiNarrowChk(ExecState &st);
/// @brief Dispatch checked narrowing of an unsigned integer.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CastUiNarrowChk(ExecState &st);
/// @brief Dispatch signed-integer-to-floating-point casting.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CastSiToFp(ExecState &st);
/// @brief Dispatch unsigned-integer-to-floating-point casting.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CastUiToFp(ExecState &st);
/// @brief Dispatch zero-extension of a one-bit value.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Zext1(ExecState &st);
/// @brief Dispatch truncation to a one-bit value.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Trunc1(ExecState &st);
/// @brief Dispatch stack allocation through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Alloca(ExecState &st);
/// @brief Dispatch effective-address calculation through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_GEP(ExecState &st);
/// @brief Dispatch a memory load through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Load(ExecState &st);
/// @brief Dispatch a memory store through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Store(ExecState &st);
/// @brief Dispatch symbol address materialization through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_AddrOf(ExecState &st);
/// @brief Dispatch string-constant materialization through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ConstStr(ExecState &st);
/// @brief Dispatch global-address materialization through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_GAddr(ExecState &st);
/// @brief Dispatch null-pointer constant materialization.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ConstNull(ExecState &st);
/// @brief Dispatch double-precision constant materialization.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ConstF64(ExecState &st);
/// @brief Dispatch conditional value selection through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Select(ExecState &st);
/// @brief Dispatch a direct function call and apply its control-flow result.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Call(ExecState &st);
/// @brief Dispatch an indirect function call and apply its control-flow result.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CallIndirect(ExecState &st);
/// @brief Dispatch integer switch control flow through its canonical handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_SwitchI32(ExecState &st);
/// @brief Dispatch an unconditional branch and apply its target state.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Br(ExecState &st);
/// @brief Dispatch a conditional branch and apply its selected target state.
/// @param st Active execution state providing instruction and control context.
void inline_handle_CBr(ExecState &st);
/// @brief Dispatch function return and apply the resulting frame transition.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Ret(ExecState &st);
/// @brief Dispatch creation of a trap carrying an explicit trap kind.
/// @param st Active execution state providing instruction and control context.
void inline_handle_TrapKind(ExecState &st);
/// @brief Dispatch conversion of the active runtime error into a trap.
/// @param st Active execution state providing instruction and control context.
void inline_handle_TrapFromErr(ExecState &st);
/// @brief Dispatch creation of a trap from explicit runtime-error data.
/// @param st Active execution state providing instruction and control context.
void inline_handle_TrapErr(ExecState &st);
/// @brief Dispatch extraction of the active error kind.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ErrGetKind(ExecState &st);
/// @brief Dispatch extraction of the active error code.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ErrGetCode(ExecState &st);
/// @brief Dispatch extraction of the active error instruction pointer.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ErrGetIp(ExecState &st);
/// @brief Dispatch extraction of the active error source line.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ErrGetLine(ExecState &st);
/// @brief Dispatch extraction of the active error message.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ErrGetMsg(ExecState &st);
/// @brief Dispatch installation of an exception-handling scope.
/// @param st Active execution state providing instruction and control context.
void inline_handle_EhPush(ExecState &st);
/// @brief Dispatch removal of the current exception-handling scope.
/// @param st Active execution state providing instruction and control context.
void inline_handle_EhPop(ExecState &st);
/// @brief Dispatch resumption at the instruction that raised the active error.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ResumeSame(ExecState &st);
/// @brief Dispatch resumption after the instruction that raised the active error.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ResumeNext(ExecState &st);
/// @brief Dispatch resumption at an explicit recovery label.
/// @param st Active execution state providing instruction and control context.
void inline_handle_ResumeLabel(ExecState &st);
/// @brief Dispatch entry into an exception handler.
/// @param st Active execution state providing instruction and control context.
void inline_handle_EhEntry(ExecState &st);
/// @brief Dispatch an unconditional trap and apply the terminal result.
/// @param st Active execution state providing instruction and control context.
void inline_handle_Trap(ExecState &st);
/// @}
