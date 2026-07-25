//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/InstrLowering.hpp
// Purpose: Opcode-specific lowering handlers for IL->MIR conversion.
// Key invariants:
//   - Each handler returns true on success and false on unrecoverable error.
//   - All emitted MIR is appended to the output block in program order.
//   - Virtual register IDs are allocated monotonically via nextVRegId.
// Ownership/Lifetime:
//   - Handlers are stateless free functions; mutable state is accessed
//     solely through the LoweringContext reference.
// Links: src/codegen/aarch64/InstrLowering.cpp,
//        src/codegen/aarch64/LoweringContext.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares AArch64 IL value materialization and opcode lowering helpers.
 *
 * Handlers append machine IR to a caller-provided block and share allocation,
 * frame, type, and literal metadata through `LoweringContext`. A `false`
 * result means the input shape could not be lowered safely and no caller
 * should continue with assumptions about output operands.
 */

#pragma once

#include "LoweringContext.hpp"
#include "MachineIR.hpp"
#include "il/core/Instr.hpp"

#include <string>

namespace zanna::codegen::aarch64 {

//===----------------------------------------------------------------------===//
// Value Materialization
//===----------------------------------------------------------------------===//

/// @brief Materialize an IL value into a vreg, appending MIR to the output block.
/// @param v The IL value to materialize
/// @param bb The current IL basic block (for parameter lookups)
/// @param ti Target info for ABI register mappings
/// @param fb Frame builder for stack allocation
/// @param out The output MIR basic block
/// @param tempVReg Map from temp ID to vreg ID
/// @param tempRegClass Map from temp ID to register class (GPR/FPR)
/// @param nextVRegId Counter for vreg ID allocation
/// @param outVReg [out] The vreg ID assigned to this value
/// @param outCls [out] The register class of the vreg
/// @param stringLiteralByteLengths Optional literal-label byte-length metadata.
/// @return true if successful, false if the value couldn't be materialized
bool materializeValueToVReg(
    const il::core::Value &v,
    const il::core::BasicBlock &bb,
    const TargetInfo &ti,
    FrameBuilder &fb,
    MBasicBlock &out,
    std::unordered_map<unsigned, uint16_t> &tempVReg,
    std::unordered_map<unsigned, RegClass> &tempRegClass,
    uint16_t &nextVRegId,
    uint16_t &outVReg,
    RegClass &outCls,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths = nullptr);

/// @brief Convenience wrapper that materialises an IL value using a LoweringContext.
/// @param v       The IL value to materialise.
/// @param bb      The current IL basic block (for parameter lookups).
/// @param ctx     Lowering context providing target info, frame builder, and maps.
/// @param out     The output MIR basic block receiving materialisation instructions.
/// @param outVReg [out] The vreg ID assigned to the materialised value.
/// @param outCls  [out] The register class of the assigned vreg.
/// @return True if materialisation succeeded, false otherwise.
inline bool materializeValueToVReg(const il::core::Value &v,
                                   const il::core::BasicBlock &bb,
                                   LoweringContext &ctx,
                                   MBasicBlock &out,
                                   uint16_t &outVReg,
                                   RegClass &outCls) {
    return materializeValueToVReg(v,
                                  bb,
                                  ctx.ti,
                                  ctx.fb,
                                  out,
                                  ctx.tempVReg,
                                  ctx.tempRegClass,
                                  ctx.nextVRegId,
                                  outVReg,
                                  outCls,
                                  ctx.stringLiteralByteLengths);
}

/// @brief Materialize an IL string global into a runtime string handle in `x0`.
/// @details Emits `ADRP` + `ADD` to compute the address of @p sym in `.rodata`,
///          then calls `rt_str_from_const_with_len(addr, len)` to produce a
///          managed runtime string. The result is left in `x0`. Used when a
///          string literal must be passed directly as a call argument.
/// @param sym                       Symbol name of the string literal in `.rodata`.
/// @param stringLiteralByteLengths  Optional map of literal labels to byte lengths
///                                  (passed as the second argument to the runtime).
/// @param out                       Output MIR basic block receiving the sequence.
/// @param nextVRegId                Counter for fresh vreg id allocation (incremented).
void emitConstStrGlobalToX0(
    const std::string &sym,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths,
    MBasicBlock &out,
    uint16_t &nextVRegId);

/// @brief Materialize an IL string global into a runtime string handle in a fresh vreg.
/// @details Same lowering as @ref emitConstStrGlobalToX0 but leaves the runtime
///          string handle in a freshly-allocated virtual register rather than
///          `x0`. Used when the string value feeds into a subsequent operation
///          (e.g. a store, a phi, a comparison) rather than a call.
/// @param sym                       Symbol name of the string literal in `.rodata`.
/// @param stringLiteralByteLengths  Optional map of literal labels to byte lengths.
/// @param out                       Output MIR basic block receiving the sequence.
/// @param nextVRegId                Counter for fresh vreg id allocation (incremented).
/// @return Virtual register id holding the runtime string handle.
uint16_t emitConstStrGlobalToVReg(
    const std::string &sym,
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths,
    MBasicBlock &out,
    uint16_t &nextVRegId);

//===----------------------------------------------------------------------===//
// Call Lowering
//===----------------------------------------------------------------------===//

/// @brief Lower a Call instruction to MIR.
/// @param callI The IL call instruction
/// @param bb The current IL basic block
/// @param ti Target info
/// @param fb Frame builder
/// @param out The output MIR basic block
/// @param seq [out] The lowered call sequence (prefix, call, postfix)
/// @param tempVReg Map from temp ID to vreg ID
/// @param tempRegClass Map from temp ID to register class (GPR/FPR)
/// @param nextVRegId Counter for vreg ID allocation
/// @param knownVarArgNamedArgCounts Optional fixed-prefix arities for variadic callees.
/// @return true if successful
bool lowerCallWithArgs(
    const il::core::Instr &callI,
    const il::core::BasicBlock &bb,
    const TargetInfo &ti,
    FrameBuilder &fb,
    MBasicBlock &out,
    LoweredCall &seq,
    std::unordered_map<unsigned, uint16_t> &tempVReg,
    std::unordered_map<unsigned, RegClass> &tempRegClass,
    uint16_t &nextVRegId,
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts = nullptr);

//===----------------------------------------------------------------------===//
// Integer Arithmetic
//===----------------------------------------------------------------------===//

/// @brief Lower signed remainder with divide-by-zero check (srem.chk0).
/// @details Generates: cbz rhs, trap; sdiv tmp, lhs, rhs; msub dst, tmp, rhs, lhs
/// @param ins The IL srem.chk0 instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerSRemChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out);

/// @brief Lower signed division with divide-by-zero check (sdiv.chk0).
/// @param ins The IL sdiv.chk0 instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerSDivChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out);

/// @brief Lower unsigned division with divide-by-zero check (udiv.chk0).
/// @param ins The IL udiv.chk0 instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerUDivChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out);

/// @brief Lower unsigned remainder with divide-by-zero check (urem.chk0).
/// @param ins The IL urem.chk0 instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerURemChk0(const il::core::Instr &ins,
                   const il::core::BasicBlock &bb,
                   LoweringContext &ctx,
                   MBasicBlock &out);

/// @brief Lower index bounds check (idx.chk).
/// @details Generates: cmp idx, lo; b.lt trap; cmp idx, hi; b.ge trap; mov dst, idx
/// @param ins The IL idx.chk instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerIdxChk(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out);

/// @brief Lower the ternary `select` opcode to a conditional select.
/// @details Generates: cmp cond, #0 followed by csel (integer) or fcsel (f64).
/// @param ins The IL select instruction to lower.
/// @param bb The IL basic block containing the instruction.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerSelect(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out);

/// @brief Lower signed remainder (srem) without zero-check.
/// @details Generates: sdiv tmp, lhs, rhs; msub dst, tmp, rhs, lhs
/// @param ins The IL srem instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerSRem(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out);

/// @brief Lower unsigned remainder (urem) without zero-check.
/// @details Generates: udiv tmp, lhs, rhs; msub dst, tmp, rhs, lhs
/// @param ins The IL urem instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerURem(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out);

//===----------------------------------------------------------------------===//
// Type Conversions
//===----------------------------------------------------------------------===//

/// @brief Lower zext1/trunc1 (boolean extension/truncation).
/// @param ins The IL zext1 or trunc1 instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
[[maybe_unused]] bool lowerZext1Trunc1(const il::core::Instr &ins,
                                       const il::core::BasicBlock &bb,
                                       LoweringContext &ctx,
                                       MBasicBlock &out);

/// @brief Lower cast.si_narrow.chk / cast.ui_narrow.chk.
/// @param ins The IL narrowing cast instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
[[maybe_unused]] bool lowerNarrowingCast(const il::core::Instr &ins,
                                         const il::core::BasicBlock &bb,
                                         LoweringContext &ctx,
                                         MBasicBlock &out);

/// @brief Lower sitofp (signed int to float).
/// @param ins The IL sitofp instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerSitofp(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out);

/// @brief Lower fptosi (float to signed int).
/// @param ins The IL fptosi instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerFptosi(const il::core::Instr &ins,
                 const il::core::BasicBlock &bb,
                 LoweringContext &ctx,
                 MBasicBlock &out);

//===----------------------------------------------------------------------===//
// Floating-Point Operations
//===----------------------------------------------------------------------===//

/// @brief Lower FP arithmetic (fadd, fsub, fmul, fdiv).
/// @param ins The IL floating-point arithmetic instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerFpArithmetic(const il::core::Instr &ins,
                       const il::core::BasicBlock &bb,
                       LoweringContext &ctx,
                       MBasicBlock &out);

/// @brief Lower FP comparisons (fcmp.eq, fcmp.ne, etc.).
/// @param ins The IL floating-point comparison instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerFpCompare(const il::core::Instr &ins,
                    const il::core::BasicBlock &bb,
                    LoweringContext &ctx,
                    MBasicBlock &out);

//===----------------------------------------------------------------------===//
// Memory Operations
//===----------------------------------------------------------------------===//

/// @brief Lower a Store instruction to MIR.
/// @param ins The IL store instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerStore(const il::core::Instr &ins,
                const il::core::BasicBlock &bb,
                LoweringContext &ctx,
                MBasicBlock &out);

/// @brief Lower a Load instruction to MIR.
/// @param ins The IL load instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerLoad(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out);

/// @brief Lower a GEP (get element pointer) instruction to MIR.
/// @param ins The IL GEP instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerGEP(const il::core::Instr &ins,
              const il::core::BasicBlock &bb,
              LoweringContext &ctx,
              MBasicBlock &out);

//===----------------------------------------------------------------------===//
// Call & Return Operations
//===----------------------------------------------------------------------===//

/// @brief Lower a Call instruction to MIR (full argument passing + result handling).
/// @param ins The IL call instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerCall(const il::core::Instr &ins,
               const il::core::BasicBlock &bb,
               LoweringContext &ctx,
               MBasicBlock &out);

/// @brief Lower a CallIndirect instruction to MIR.
/// @param ins The IL indirect call instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerCallIndirect(const il::core::Instr &ins,
                       const il::core::BasicBlock &bb,
                       LoweringContext &ctx,
                       MBasicBlock &out);

/// @brief Lower a Ret instruction to MIR.
/// @param ins The IL return instruction to lower.
/// @param bb  The IL basic block containing @p ins.
/// @param ctx Lowering context for register allocation and frame state.
/// @param out The output MIR basic block receiving the lowered sequence.
/// @return True on success, false on lowering failure.
bool lowerRet(const il::core::Instr &ins,
              const il::core::BasicBlock &bb,
              LoweringContext &ctx,
              MBasicBlock &out);

} // namespace zanna::codegen::aarch64
