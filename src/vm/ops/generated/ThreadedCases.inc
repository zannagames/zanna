//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// IMPORTANT: This file is auto-generated. Do not edit manually.
//
// File: vm/ops/generated/ThreadedCases.inc
// Purpose: GCC computed-goto case labels and handler bodies for threaded dispatch.
//
// Generator: Manually maintained (synchronized with Opcode enum).
// Regenerate: Update when adding/removing opcodes in il/core/Opcode.hpp.
//
// Key Invariants:
//   - One LBL_<OpName>: case per opcode in enum order
//   - Each case invokes inline_handle_<Op> and dispatches via DISPATCH_NEXT_FAST()
//   - Tracing is handled by DISPATCH_NEXT_FAST() or the slow path
//   - Requires GCC/Clang computed-goto extension (&&label address syntax)
//
// Links: docs/il/il-guide.md#reference, vm/ops/generated/ThreadedLabels.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines computed-goto handler labels for every IL opcode.
/// @details Each label invokes its inline opcode wrapper and either continues
///          through the fast dispatch macro or returns to the slow path when
///          control flow or exit state requires centralized bookkeeping.

LBL_Add : {
    vm.inline_handle_Add(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Sub : {
    vm.inline_handle_Sub(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Mul : {
    vm.inline_handle_Mul(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_IAddOvf : {
    vm.inline_handle_IAddOvf(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ISubOvf : {
    vm.inline_handle_ISubOvf(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_IMulOvf : {
    vm.inline_handle_IMulOvf(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SDiv : {
    vm.inline_handle_SDiv(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_UDiv : {
    vm.inline_handle_UDiv(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SRem : {
    vm.inline_handle_SRem(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_URem : {
    vm.inline_handle_URem(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SDivChk0 : {
    vm.inline_handle_SDivChk0(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_UDivChk0 : {
    vm.inline_handle_UDivChk0(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SRemChk0 : {
    vm.inline_handle_SRemChk0(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_URemChk0 : {
    vm.inline_handle_URemChk0(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_IdxChk : {
    vm.inline_handle_IdxChk(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_And : {
    vm.inline_handle_And(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Or : {
    vm.inline_handle_Or(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Xor : {
    vm.inline_handle_Xor(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Shl : {
    vm.inline_handle_Shl(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_LShr : {
    vm.inline_handle_LShr(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_AShr : {
    vm.inline_handle_AShr(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FAdd : {
    vm.inline_handle_FAdd(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FSub : {
    vm.inline_handle_FSub(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FMul : {
    vm.inline_handle_FMul(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FDiv : {
    vm.inline_handle_FDiv(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ICmpEq : {
    vm.inline_handle_ICmpEq(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ICmpNe : {
    vm.inline_handle_ICmpNe(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SCmpLT : {
    vm.inline_handle_SCmpLT(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SCmpLE : {
    vm.inline_handle_SCmpLE(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SCmpGT : {
    vm.inline_handle_SCmpGT(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SCmpGE : {
    vm.inline_handle_SCmpGE(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_UCmpLT : {
    vm.inline_handle_UCmpLT(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_UCmpLE : {
    vm.inline_handle_UCmpLE(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_UCmpGT : {
    vm.inline_handle_UCmpGT(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_UCmpGE : {
    vm.inline_handle_UCmpGE(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpEQ : {
    vm.inline_handle_FCmpEQ(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpNE : {
    vm.inline_handle_FCmpNE(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpLT : {
    vm.inline_handle_FCmpLT(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpLE : {
    vm.inline_handle_FCmpLE(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpGT : {
    vm.inline_handle_FCmpGT(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpGE : {
    vm.inline_handle_FCmpGE(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpOrd : {
    vm.inline_handle_FCmpOrd(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_FCmpUno : {
    vm.inline_handle_FCmpUno(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Sitofp : {
    vm.inline_handle_Sitofp(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Fptosi : {
    vm.inline_handle_Fptosi(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_CastFpToSiRteChk : {
    vm.inline_handle_CastFpToSiRteChk(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_CastFpToUiRteChk : {
    vm.inline_handle_CastFpToUiRteChk(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_CastSiNarrowChk : {
    vm.inline_handle_CastSiNarrowChk(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_CastUiNarrowChk : {
    vm.inline_handle_CastUiNarrowChk(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_CastSiToFp : {
    vm.inline_handle_CastSiToFp(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_CastUiToFp : {
    vm.inline_handle_CastUiToFp(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Zext1 : {
    vm.inline_handle_Zext1(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Trunc1 : {
    vm.inline_handle_Trunc1(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Alloca : {
    vm.inline_handle_Alloca(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_GEP : {
    vm.inline_handle_GEP(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Load : {
    vm.inline_handle_Load(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Store : {
    vm.inline_handle_Store(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_AddrOf : {
    vm.inline_handle_AddrOf(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ConstStr : {
    vm.inline_handle_ConstStr(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_GAddr : {
    auto exec =
        il::vm::detail::handleGAddr(vm, state.fr, *currentInstr, *state.blocks, state.bb, state.ip);
    vm.handleInlineResult(state, exec);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ConstNull : {
    vm.inline_handle_ConstNull(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ConstF64 : {
    vm.inline_handle_ConstF64(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Select : {
    vm.inline_handle_Select(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Call : {
    vm.inline_handle_Call(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_CallIndirect : {
    vm.inline_handle_CallIndirect(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_SwitchI32 : {
    vm.inline_handle_SwitchI32(state);
    // SwitchI32 modifies ip/bb, use slow path to handle the jump
    goto LBL_SLOW_PATH;
}

LBL_Br : {
    vm.inline_handle_Br(state);
    // Br modifies ip/bb, use slow path to handle the jump
    goto LBL_SLOW_PATH;
}

LBL_CBr : {
    vm.inline_handle_CBr(state);
    // CBr modifies ip/bb, use slow path to handle the jump
    goto LBL_SLOW_PATH;
}

LBL_Ret : {
    vm.inline_handle_Ret(state);
    // Ret sets exitRequested, go to slow path to return
    goto LBL_SLOW_PATH;
}

LBL_TrapKind : {
    vm.inline_handle_TrapKind(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_TrapFromErr : {
    vm.inline_handle_TrapFromErr(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_TrapErr : {
    vm.inline_handle_TrapErr(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ErrGetKind : {
    vm.inline_handle_ErrGetKind(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ErrGetCode : {
    vm.inline_handle_ErrGetCode(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ErrGetIp : {
    vm.inline_handle_ErrGetIp(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ErrGetLine : {
    vm.inline_handle_ErrGetLine(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ErrGetMsg : {
    vm.inline_handle_ErrGetMsg(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_EhPush : {
    vm.inline_handle_EhPush(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_EhPop : {
    vm.inline_handle_EhPop(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_ResumeSame : {
    vm.inline_handle_ResumeSame(state);
    // ResumeSame modifies ip, use slow path
    goto LBL_SLOW_PATH;
}

LBL_ResumeNext : {
    vm.inline_handle_ResumeNext(state);
    // ResumeNext modifies ip, use slow path
    goto LBL_SLOW_PATH;
}

LBL_ResumeLabel : {
    vm.inline_handle_ResumeLabel(state);
    // ResumeLabel modifies ip/bb, use slow path
    goto LBL_SLOW_PATH;
}

LBL_EhEntry : {
    vm.inline_handle_EhEntry(state);
    if (state.exitRequested) [[unlikely]]
        goto LBL_SLOW_PATH;
    DISPATCH_NEXT_FAST();
}

LBL_Trap : {
    vm.inline_handle_Trap(state);
    // Trap may modify control flow, use slow path
    goto LBL_SLOW_PATH;
}
