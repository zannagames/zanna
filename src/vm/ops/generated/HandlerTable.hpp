//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/ops/generated/HandlerTable.hpp
// Purpose: Function table mapping opcodes to their VM handler functions.
// Key invariants: Table size MUST equal il::core::kNumOpcodes. Each entry
//                 corresponds to an Opcode enum value in declaration order.
// Ownership/Lifetime: Static table shared across all VM instances.
// Links: docs/internals/architecture.md, vm/DispatchMacros.hpp
//
// IMPORTANT: When adding a new opcode, add its handler at the correct position
// matching the opcode's enum value. See vm/DispatchMacros.hpp for full guide.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the process-wide opcode-to-handler function table.
/// @details The generated ordering mirrors Opcode.def exactly and is checked at
///          compile time so every opcode index selects its canonical VM handler.

#pragma once

#include "vm/DispatchMacros.hpp"
#include "vm/OpHandlers.hpp"
#include "vm/VM.hpp"

namespace il::vm::generated {

/// @brief Static handler table indexed by opcode enum value.
/// @details Each entry is a function pointer to the handler for the
///          corresponding opcode. The table order MUST match the opcode
///          declaration order in Opcode.def.
/// @return Immutable process-lifetime handler table indexed by opcode value.
inline const VM::OpcodeHandlerTable &opcodeHandlers() {
    static const VM::OpcodeHandlerTable table = {
        // =================================================================
        // Arithmetic Operations (Opcode::Add through Opcode::IdxChk)
        // =================================================================
        &il::vm::detail::handleAdd,
        &il::vm::detail::handleSub,
        &il::vm::detail::handleMul,
        &il::vm::detail::handleIAddOvf,
        &il::vm::detail::handleISubOvf,
        &il::vm::detail::handleIMulOvf,
        &il::vm::detail::handleSDiv,
        &il::vm::detail::handleUDiv,
        &il::vm::detail::handleSRem,
        &il::vm::detail::handleURem,
        &il::vm::detail::handleSDivChk0,
        &il::vm::detail::handleUDivChk0,
        &il::vm::detail::handleSRemChk0,
        &il::vm::detail::handleURemChk0,
        &il::vm::detail::handleIdxChk,
        &il::vm::detail::handleAnd,
        &il::vm::detail::handleOr,
        &il::vm::detail::handleXor,
        &il::vm::detail::handleShl,
        &il::vm::detail::handleLShr,
        &il::vm::detail::handleAShr,
        &il::vm::detail::handleFAdd,
        &il::vm::detail::handleFSub,
        &il::vm::detail::handleFMul,
        &il::vm::detail::handleFDiv,
        &il::vm::detail::handleICmpEq,
        &il::vm::detail::handleICmpNe,
        &il::vm::detail::handleSCmpLT,
        &il::vm::detail::handleSCmpLE,
        &il::vm::detail::handleSCmpGT,
        &il::vm::detail::handleSCmpGE,
        &il::vm::detail::handleUCmpLT,
        &il::vm::detail::handleUCmpLE,
        &il::vm::detail::handleUCmpGT,
        &il::vm::detail::handleUCmpGE,
        &il::vm::detail::handleFCmpEQ,
        &il::vm::detail::handleFCmpNE,
        &il::vm::detail::handleFCmpLT,
        &il::vm::detail::handleFCmpLE,
        &il::vm::detail::handleFCmpGT,
        &il::vm::detail::handleFCmpGE,
        &il::vm::detail::handleFCmpOrd,
        &il::vm::detail::handleFCmpUno,
        &il::vm::detail::handleSitofp,
        &il::vm::detail::handleFptosi,
        &il::vm::detail::handleCastFpToSiRteChk,
        &il::vm::detail::handleCastFpToUiRteChk,
        &il::vm::detail::handleCastSiNarrowChk,
        &il::vm::detail::handleCastUiNarrowChk,
        &il::vm::detail::handleCastSiToFp,
        &il::vm::detail::handleCastUiToFp,
        &il::vm::detail::handleTruncOrZext1,
        &il::vm::detail::handleTruncOrZext1,
        &il::vm::detail::handleAlloca,
        &il::vm::detail::handleGEP,
        &il::vm::detail::handleLoad,
        &il::vm::detail::handleStore,
        &il::vm::detail::handleAddrOf,
        &il::vm::detail::handleConstStr,
        &il::vm::detail::handleGAddr,
        &il::vm::detail::handleConstNull,
        &il::vm::detail::handleConstF64,
        &il::vm::detail::handleCall,
        &il::vm::detail::handleCallIndirect,
        &il::vm::detail::handleSwitchI32,
        &il::vm::detail::handleBr,
        &il::vm::detail::handleCBr,
        &il::vm::detail::handleRet,
        &il::vm::detail::handleTrapKind,
        &il::vm::detail::handleTrap,
        &il::vm::detail::handleTrapErr,
        &il::vm::detail::handleErrGet,
        &il::vm::detail::handleErrGet,
        &il::vm::detail::handleErrGet,
        &il::vm::detail::handleErrGet,
        &il::vm::detail::handleErrGet,
        &il::vm::detail::handleEhPush,
        &il::vm::detail::handleEhPop,
        &il::vm::detail::handleResumeSame,
        &il::vm::detail::handleResumeNext,
        &il::vm::detail::handleResumeLabel,
        &il::vm::detail::handleEhEntry,
        &il::vm::detail::handleTrap,
        &il::vm::detail::handleSelect,
    };

    // Compile-time verification that handler table covers all opcodes.
    // If this fails, an opcode was added to Opcode.def without updating this table.
    ZANNA_ASSERT_HANDLER_TABLE_SIZE(table);

    return table;
}

} // namespace il::vm::generated
