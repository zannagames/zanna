//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// IMPORTANT: This file is auto-generated. Do not edit manually.
//
// File: vm/ops/generated/ThreadedLabels.inc
// Purpose: GCC computed-goto label address table for threaded VM dispatch.
//
// Generator: Manually maintained (synchronized with Opcode enum).
// Regenerate: Update when adding/removing opcodes in il/core/Opcode.hpp.
//
// Description: Each entry is &&LBL_<OpName> for the corresponding opcode handler.
//              Included directly in VM.cpp within a label address array initializer.
//
// Key Invariants:
//   - Entry count MUST equal il::core::kNumOpcodes
//   - Entry order MUST match Opcode enum declaration order exactly
//   - Requires GCC/Clang computed-goto extension (&&label address syntax)
//
// Links: docs/il/il-guide.md#reference, vm/ops/generated/ThreadedCases.inc
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Lists computed-goto label addresses in exact opcode order.
/// @details The initializer is consumed inside the threaded dispatch table;
///          the final unimplemented label provides a defensive fallback.

&&LBL_Add, &&LBL_Sub, &&LBL_Mul, &&LBL_IAddOvf, &&LBL_ISubOvf, &&LBL_IMulOvf, &&LBL_SDiv,
    &&LBL_UDiv, &&LBL_SRem, &&LBL_URem, &&LBL_SDivChk0, &&LBL_UDivChk0, &&LBL_SRemChk0,
    &&LBL_URemChk0, &&LBL_IdxChk, &&LBL_And, &&LBL_Or, &&LBL_Xor, &&LBL_Shl, &&LBL_LShr, &&LBL_AShr,
    &&LBL_FAdd, &&LBL_FSub, &&LBL_FMul, &&LBL_FDiv, &&LBL_ICmpEq, &&LBL_ICmpNe, &&LBL_SCmpLT,
    &&LBL_SCmpLE, &&LBL_SCmpGT, &&LBL_SCmpGE, &&LBL_UCmpLT, &&LBL_UCmpLE, &&LBL_UCmpGT,
    &&LBL_UCmpGE, &&LBL_FCmpEQ, &&LBL_FCmpNE, &&LBL_FCmpLT, &&LBL_FCmpLE, &&LBL_FCmpGT,
    &&LBL_FCmpGE, &&LBL_FCmpOrd, &&LBL_FCmpUno, &&LBL_Sitofp, &&LBL_Fptosi, &&LBL_CastFpToSiRteChk,
    &&LBL_CastFpToUiRteChk, &&LBL_CastSiNarrowChk, &&LBL_CastUiNarrowChk, &&LBL_CastSiToFp,
    &&LBL_CastUiToFp, &&LBL_Zext1, &&LBL_Trunc1, &&LBL_Alloca, &&LBL_GEP, &&LBL_Load, &&LBL_Store,
    &&LBL_AddrOf, &&LBL_ConstStr, &&LBL_GAddr, &&LBL_ConstNull, &&LBL_ConstF64, &&LBL_Call,
    &&LBL_CallIndirect, &&LBL_SwitchI32, &&LBL_Br, &&LBL_CBr, &&LBL_Ret, &&LBL_TrapKind,
    &&LBL_TrapFromErr, &&LBL_TrapErr, &&LBL_ErrGetKind, &&LBL_ErrGetCode, &&LBL_ErrGetIp,
    &&LBL_ErrGetLine, &&LBL_ErrGetMsg, &&LBL_EhPush, &&LBL_EhPop, &&LBL_ResumeSame,
    &&LBL_ResumeNext, &&LBL_ResumeLabel, &&LBL_EhEntry, &&LBL_Trap, &&LBL_Select, &&LBL_UNIMPL,
