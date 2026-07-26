//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// IMPORTANT: This file is auto-generated. Do not edit manually.
//
// File: vm/ops/generated/SwitchDispatchImpl.inc
// Purpose: Switch-based VM dispatch function implementation.
//
// Generator: Manually maintained (synchronized with Opcode enum).
// Regenerate: Update when adding/removing opcodes in il/core/Opcode.hpp.
//
// Description: Provides a portable fallback dispatcher when GCC computed-goto
//              (threaded dispatch) is unavailable. Each case invokes the
//              corresponding inline_handle_<Op> function.
//
// Key Invariants:
//   - Case labels MUST cover all il::core::Opcode values
//   - Case order MUST match Opcode enum declaration order
//
// Links: docs/il/il-guide.md#reference, vm/ops/generated/SwitchDispatchDecl.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements portable opcode dispatch as an exhaustive C++ switch.
/// @details Each known case traces the instruction and invokes the matching
///          inline wrapper; unknown values are routed to the unimplemented
///          opcode trap.
/// @param state Active execution state updated by the selected handler.
/// @param instr Current instruction used for tracing and case selection.
void VM::dispatchOpcodeSwitch(ExecState &state, const il::core::Instr &instr) {
    switch (instr.op) {
        case il::core::Opcode::Add: {
            traceInstruction(instr, state.fr);
            inline_handle_Add(state);
            break;
        }
        case il::core::Opcode::Sub: {
            traceInstruction(instr, state.fr);
            inline_handle_Sub(state);
            break;
        }
        case il::core::Opcode::Mul: {
            traceInstruction(instr, state.fr);
            inline_handle_Mul(state);
            break;
        }
        case il::core::Opcode::IAddOvf: {
            traceInstruction(instr, state.fr);
            inline_handle_IAddOvf(state);
            break;
        }
        case il::core::Opcode::ISubOvf: {
            traceInstruction(instr, state.fr);
            inline_handle_ISubOvf(state);
            break;
        }
        case il::core::Opcode::IMulOvf: {
            traceInstruction(instr, state.fr);
            inline_handle_IMulOvf(state);
            break;
        }
        case il::core::Opcode::SDiv: {
            traceInstruction(instr, state.fr);
            inline_handle_SDiv(state);
            break;
        }
        case il::core::Opcode::UDiv: {
            traceInstruction(instr, state.fr);
            inline_handle_UDiv(state);
            break;
        }
        case il::core::Opcode::SRem: {
            traceInstruction(instr, state.fr);
            inline_handle_SRem(state);
            break;
        }
        case il::core::Opcode::URem: {
            traceInstruction(instr, state.fr);
            inline_handle_URem(state);
            break;
        }
        case il::core::Opcode::SDivChk0: {
            traceInstruction(instr, state.fr);
            inline_handle_SDivChk0(state);
            break;
        }
        case il::core::Opcode::UDivChk0: {
            traceInstruction(instr, state.fr);
            inline_handle_UDivChk0(state);
            break;
        }
        case il::core::Opcode::SRemChk0: {
            traceInstruction(instr, state.fr);
            inline_handle_SRemChk0(state);
            break;
        }
        case il::core::Opcode::URemChk0: {
            traceInstruction(instr, state.fr);
            inline_handle_URemChk0(state);
            break;
        }
        case il::core::Opcode::IdxChk: {
            traceInstruction(instr, state.fr);
            inline_handle_IdxChk(state);
            break;
        }
        case il::core::Opcode::And: {
            traceInstruction(instr, state.fr);
            inline_handle_And(state);
            break;
        }
        case il::core::Opcode::Or: {
            traceInstruction(instr, state.fr);
            inline_handle_Or(state);
            break;
        }
        case il::core::Opcode::Xor: {
            traceInstruction(instr, state.fr);
            inline_handle_Xor(state);
            break;
        }
        case il::core::Opcode::Shl: {
            traceInstruction(instr, state.fr);
            inline_handle_Shl(state);
            break;
        }
        case il::core::Opcode::LShr: {
            traceInstruction(instr, state.fr);
            inline_handle_LShr(state);
            break;
        }
        case il::core::Opcode::AShr: {
            traceInstruction(instr, state.fr);
            inline_handle_AShr(state);
            break;
        }
        case il::core::Opcode::FAdd: {
            traceInstruction(instr, state.fr);
            inline_handle_FAdd(state);
            break;
        }
        case il::core::Opcode::FSub: {
            traceInstruction(instr, state.fr);
            inline_handle_FSub(state);
            break;
        }
        case il::core::Opcode::FMul: {
            traceInstruction(instr, state.fr);
            inline_handle_FMul(state);
            break;
        }
        case il::core::Opcode::FDiv: {
            traceInstruction(instr, state.fr);
            inline_handle_FDiv(state);
            break;
        }
        case il::core::Opcode::ICmpEq: {
            traceInstruction(instr, state.fr);
            inline_handle_ICmpEq(state);
            break;
        }
        case il::core::Opcode::ICmpNe: {
            traceInstruction(instr, state.fr);
            inline_handle_ICmpNe(state);
            break;
        }
        case il::core::Opcode::SCmpLT: {
            traceInstruction(instr, state.fr);
            inline_handle_SCmpLT(state);
            break;
        }
        case il::core::Opcode::SCmpLE: {
            traceInstruction(instr, state.fr);
            inline_handle_SCmpLE(state);
            break;
        }
        case il::core::Opcode::SCmpGT: {
            traceInstruction(instr, state.fr);
            inline_handle_SCmpGT(state);
            break;
        }
        case il::core::Opcode::SCmpGE: {
            traceInstruction(instr, state.fr);
            inline_handle_SCmpGE(state);
            break;
        }
        case il::core::Opcode::UCmpLT: {
            traceInstruction(instr, state.fr);
            inline_handle_UCmpLT(state);
            break;
        }
        case il::core::Opcode::UCmpLE: {
            traceInstruction(instr, state.fr);
            inline_handle_UCmpLE(state);
            break;
        }
        case il::core::Opcode::UCmpGT: {
            traceInstruction(instr, state.fr);
            inline_handle_UCmpGT(state);
            break;
        }
        case il::core::Opcode::UCmpGE: {
            traceInstruction(instr, state.fr);
            inline_handle_UCmpGE(state);
            break;
        }
        case il::core::Opcode::FCmpEQ: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpEQ(state);
            break;
        }
        case il::core::Opcode::FCmpNE: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpNE(state);
            break;
        }
        case il::core::Opcode::FCmpLT: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpLT(state);
            break;
        }
        case il::core::Opcode::FCmpLE: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpLE(state);
            break;
        }
        case il::core::Opcode::FCmpGT: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpGT(state);
            break;
        }
        case il::core::Opcode::FCmpGE: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpGE(state);
            break;
        }
        case il::core::Opcode::FCmpOrd: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpOrd(state);
            break;
        }
        case il::core::Opcode::FCmpUno: {
            traceInstruction(instr, state.fr);
            inline_handle_FCmpUno(state);
            break;
        }
        case il::core::Opcode::Sitofp: {
            traceInstruction(instr, state.fr);
            inline_handle_Sitofp(state);
            break;
        }
        case il::core::Opcode::Fptosi: {
            traceInstruction(instr, state.fr);
            inline_handle_Fptosi(state);
            break;
        }
        case il::core::Opcode::CastFpToSiRteChk: {
            traceInstruction(instr, state.fr);
            inline_handle_CastFpToSiRteChk(state);
            break;
        }
        case il::core::Opcode::CastFpToUiRteChk: {
            traceInstruction(instr, state.fr);
            inline_handle_CastFpToUiRteChk(state);
            break;
        }
        case il::core::Opcode::CastSiNarrowChk: {
            traceInstruction(instr, state.fr);
            inline_handle_CastSiNarrowChk(state);
            break;
        }
        case il::core::Opcode::CastUiNarrowChk: {
            traceInstruction(instr, state.fr);
            inline_handle_CastUiNarrowChk(state);
            break;
        }
        case il::core::Opcode::CastSiToFp: {
            traceInstruction(instr, state.fr);
            inline_handle_CastSiToFp(state);
            break;
        }
        case il::core::Opcode::CastUiToFp: {
            traceInstruction(instr, state.fr);
            inline_handle_CastUiToFp(state);
            break;
        }
        case il::core::Opcode::Zext1: {
            traceInstruction(instr, state.fr);
            inline_handle_Zext1(state);
            break;
        }
        case il::core::Opcode::Trunc1: {
            traceInstruction(instr, state.fr);
            inline_handle_Trunc1(state);
            break;
        }
        case il::core::Opcode::Alloca: {
            traceInstruction(instr, state.fr);
            inline_handle_Alloca(state);
            break;
        }
        case il::core::Opcode::GEP: {
            traceInstruction(instr, state.fr);
            inline_handle_GEP(state);
            break;
        }
        case il::core::Opcode::Load: {
            traceInstruction(instr, state.fr);
            inline_handle_Load(state);
            break;
        }
        case il::core::Opcode::Store: {
            traceInstruction(instr, state.fr);
            inline_handle_Store(state);
            break;
        }
        case il::core::Opcode::AddrOf: {
            traceInstruction(instr, state.fr);
            inline_handle_AddrOf(state);
            break;
        }
        case il::core::Opcode::ConstStr: {
            traceInstruction(instr, state.fr);
            inline_handle_ConstStr(state);
            break;
        }
        case il::core::Opcode::GAddr: {
            traceInstruction(instr, state.fr);
            inline_handle_GAddr(state);
            break;
        }
        case il::core::Opcode::ConstNull: {
            traceInstruction(instr, state.fr);
            inline_handle_ConstNull(state);
            break;
        }
        case il::core::Opcode::ConstF64: {
            traceInstruction(instr, state.fr);
            inline_handle_ConstF64(state);
            break;
        }
        case il::core::Opcode::Select: {
            traceInstruction(instr, state.fr);
            inline_handle_Select(state);
            break;
        }
        case il::core::Opcode::Call: {
            traceInstruction(instr, state.fr);
            inline_handle_Call(state);
            break;
        }
        case il::core::Opcode::CallIndirect: {
            traceInstruction(instr, state.fr);
            inline_handle_CallIndirect(state);
            break;
        }
        case il::core::Opcode::SwitchI32: {
            traceInstruction(instr, state.fr);
            inline_handle_SwitchI32(state);
            break;
        }
        case il::core::Opcode::Br: {
            traceInstruction(instr, state.fr);
            inline_handle_Br(state);
            break;
        }
        case il::core::Opcode::CBr: {
            traceInstruction(instr, state.fr);
            inline_handle_CBr(state);
            break;
        }
        case il::core::Opcode::Ret: {
            traceInstruction(instr, state.fr);
            inline_handle_Ret(state);
            break;
        }
        case il::core::Opcode::TrapKind: {
            traceInstruction(instr, state.fr);
            inline_handle_TrapKind(state);
            break;
        }
        case il::core::Opcode::TrapFromErr: {
            traceInstruction(instr, state.fr);
            inline_handle_TrapFromErr(state);
            break;
        }
        case il::core::Opcode::TrapErr: {
            traceInstruction(instr, state.fr);
            inline_handle_TrapErr(state);
            break;
        }
        case il::core::Opcode::ErrGetKind: {
            traceInstruction(instr, state.fr);
            inline_handle_ErrGetKind(state);
            break;
        }
        case il::core::Opcode::ErrGetCode: {
            traceInstruction(instr, state.fr);
            inline_handle_ErrGetCode(state);
            break;
        }
        case il::core::Opcode::ErrGetIp: {
            traceInstruction(instr, state.fr);
            inline_handle_ErrGetIp(state);
            break;
        }
        case il::core::Opcode::ErrGetLine: {
            traceInstruction(instr, state.fr);
            inline_handle_ErrGetLine(state);
            break;
        }
        case il::core::Opcode::ErrGetMsg: {
            traceInstruction(instr, state.fr);
            inline_handle_ErrGetMsg(state);
            break;
        }
        case il::core::Opcode::EhPush: {
            traceInstruction(instr, state.fr);
            inline_handle_EhPush(state);
            break;
        }
        case il::core::Opcode::EhPop: {
            traceInstruction(instr, state.fr);
            inline_handle_EhPop(state);
            break;
        }
        case il::core::Opcode::ResumeSame: {
            traceInstruction(instr, state.fr);
            inline_handle_ResumeSame(state);
            break;
        }
        case il::core::Opcode::ResumeNext: {
            traceInstruction(instr, state.fr);
            inline_handle_ResumeNext(state);
            break;
        }
        case il::core::Opcode::ResumeLabel: {
            traceInstruction(instr, state.fr);
            inline_handle_ResumeLabel(state);
            break;
        }
        case il::core::Opcode::EhEntry: {
            traceInstruction(instr, state.fr);
            inline_handle_EhEntry(state);
            break;
        }
        case il::core::Opcode::Trap: {
            traceInstruction(instr, state.fr);
            inline_handle_Trap(state);
            break;
        }
        default:
            trapUnimplemented(instr.op);
    }
}
