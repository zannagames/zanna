//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/fastpaths/FastPaths_Memory.cpp
// Purpose: Fast-path pattern matching for memory operations.
//          Handles alloca/store/load/ret, load-from-pointer-param/ret, and
//          gep+load/ret patterns — common accessor shapes that map to 1-3 MIR
//          instructions without going through the generic lowering path.
// Key invariants:
//   - Single-block functions only.
//   - Alloca must have been assigned a frame offset by FrameBuilder.
//   - Store/load must target the same alloca for the round-trip pattern.
//   - Return value must come from the load result.
// Ownership/Lifetime:
//   - Stateless free functions; FastPathContext is borrowed for the call duration.
// Links: src/codegen/aarch64/fastpaths/FastPathsInternal.hpp
//
//===----------------------------------------------------------------------===//

#include "FastPathsInternal.hpp"

/**
 * @file
 * @brief Implements simple AArch64 memory-access-and-return fast paths.
 *
 * Width-aware helpers select narrow GPR forms. Recognizers cover a local
 * store/load round trip, dereference of a pointer parameter, and a constant
 * offset GEP followed by a load. String and boolean return semantics remain on
 * the generic path.
 */

namespace zanna::codegen::aarch64::fastpaths {

using il::core::Opcode;

namespace {

/// @brief Select the GPR load MIR opcode for an IL type width.
/// @details Picks the width-correct Ldr{8,16,32,}* opcode; @p frameRelative
///          chooses the FP-relative (FpImm) vs base-register (BaseImm) form.
/// @param kind          IL element type (I1/I16/I32 narrow; else full 64-bit).
/// @param frameRelative True for an x29-relative slot, false for a base reg.
/// @return The matching MOpcode load variant.
MOpcode gprLoadOpcodeForType(il::core::Type::Kind kind, bool frameRelative) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return frameRelative ? MOpcode::Ldr8RegFpImm : MOpcode::Ldr8RegBaseImm;
        case il::core::Type::Kind::I16:
            return frameRelative ? MOpcode::Ldr16RegFpImm : MOpcode::Ldr16RegBaseImm;
        case il::core::Type::Kind::I32:
            return frameRelative ? MOpcode::Ldr32RegFpImm : MOpcode::Ldr32RegBaseImm;
        default:
            return frameRelative ? MOpcode::LdrRegFpImm : MOpcode::LdrRegBaseImm;
    }
}

/// @brief Select the GPR store MIR opcode for an IL type width.
/// @details Mirror of gprLoadOpcodeForType() for the Str{8,16,32,}* family;
///          @p frameRelative selects the FP-relative vs base-register form.
/// @param kind          IL element type (I1/I16/I32 narrow; else full 64-bit).
/// @param frameRelative True for an x29-relative slot, false for a base reg.
/// @return The matching MOpcode store variant.
MOpcode gprStoreOpcodeForType(il::core::Type::Kind kind, bool frameRelative) {
    switch (kind) {
        case il::core::Type::Kind::I1:
            return frameRelative ? MOpcode::Str8RegFpImm : MOpcode::Str8RegBaseImm;
        case il::core::Type::Kind::I16:
            return frameRelative ? MOpcode::Str16RegFpImm : MOpcode::Str16RegBaseImm;
        case il::core::Type::Kind::I32:
            return frameRelative ? MOpcode::Str32RegFpImm : MOpcode::Str32RegBaseImm;
        default:
            return frameRelative ? MOpcode::StrRegFpImm : MOpcode::StrRegBaseImm;
    }
}

} // namespace

/**
 * @brief Attempts one of the supported single-block memory return patterns.
 *
 * Local round trips require an assigned nonzero frame offset and identical
 * store/load allocation IDs. Pointer loads and GEP loads require an entry
 * parameter in the integer argument bank. Loads are emitted at the IL width,
 * with FP results returned through `v0` and other scalars through `x0`.
 *
 * @param[in,out] ctx Fast-path state and output MIR.
 * @return Completed MIR function on a match, otherwise `std::nullopt`.
 */
std::optional<MFunction> tryMemoryFastPaths(FastPathContext &ctx) {
    if (ctx.fn.blocks.empty())
        return std::nullopt;
    if (ctx.fn.retType.kind == il::core::Type::Kind::I1 ||
        ctx.fn.retType.kind == il::core::Type::Kind::Str)
        return std::nullopt;

    const auto &bb = ctx.fn.blocks.front();
    auto &bbMir = ctx.bbOut(0);

    // =========================================================================
    // alloca/store/load/ret pattern
    // =========================================================================
    // Pattern: %local = alloca i64; store %param0, %local; %val = load %local; ret %val
    // This matches simple functions that spill a parameter and reload it.
    // Emits: str srcReg, [x29, #offset]; ldr x0, [x29, #offset]; ret
    if (!ctx.mf.frame.locals.empty() && ctx.fn.blocks.size() == 1 && bb.instructions.size() >= 4) {
        const auto *allocaI = &bb.instructions[bb.instructions.size() - 4];
        const auto *storeI = &bb.instructions[bb.instructions.size() - 3];
        const auto *loadI = &bb.instructions[bb.instructions.size() - 2];
        const auto *retI = &bb.instructions[bb.instructions.size() - 1];

        if (allocaI->op == Opcode::Alloca && allocaI->result && storeI->op == Opcode::Store &&
            storeI->operands.size() == 2 && loadI->op == Opcode::Load && loadI->result &&
            loadI->operands.size() == 1 && retI->op == Opcode::Ret && !retI->operands.empty()) {
            const unsigned allocaId = *allocaI->result;
            const auto &storePtr = storeI->operands[0]; // pointer is operand 0
            const auto &storeVal = storeI->operands[1]; // value is operand 1
            const auto &loadPtr = loadI->operands[0];
            const auto &retVal = retI->operands[0];

            // Check that store and load both target the same alloca
            if (storePtr.kind == il::core::Value::Kind::Temp && storePtr.id == allocaId &&
                loadPtr.kind == il::core::Value::Kind::Temp && loadPtr.id == allocaId &&
                retVal.kind == il::core::Value::Kind::Temp && retVal.id == *loadI->result) {
                // Get offset for this alloca from frame builder
                const int offset = ctx.fb.localOffset(allocaId);
                if (offset != 0) {
                    // Get register holding the value to store
                    auto srcReg = ctx.getValueReg(bb, storeVal);
                    if (srcReg) {
                        const bool isF64 = loadI->type.kind == il::core::Type::Kind::F64 ||
                                           storeI->type.kind == il::core::Type::Kind::F64 ||
                                           ctx.fn.retType.kind == il::core::Type::Kind::F64;
                        const MOpcode storeOpc =
                            isF64 ? MOpcode::StrFprFpImm
                                  : gprStoreOpcodeForType(storeI->type.kind,
                                                          /*frameRelative=*/true);
                        const MOpcode loadOpc = isF64
                                                    ? MOpcode::LdrFprFpImm
                                                    : gprLoadOpcodeForType(loadI->type.kind,
                                                                           /*frameRelative=*/true);
                        bbMir.instrs.push_back(
                            MInstr{storeOpc, {MOperand::regOp(*srcReg), MOperand::immOp(offset)}});
                        const PhysReg retReg = isF64 ? PhysReg::V0 : PhysReg::X0;
                        bbMir.instrs.push_back(
                            MInstr{loadOpc, {MOperand::regOp(retReg), MOperand::immOp(offset)}});
                        // ret
                        bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                        ctx.fb.finalize();
                        return ctx.mf;
                    }
                }
            }
        }
    }

    // =========================================================================
    // load-from-param/ret fast-path
    // =========================================================================
    // Pattern: %v = load type, %param0; ret %v
    // Simple accessor that loads from a pointer parameter and returns.
    // Emits: ldr x0, [x0] (or appropriate variant); ret
    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() == 2 && !bb.params.empty()) {
        const auto &loadI = bb.instructions[0];
        const auto &retI = bb.instructions[1];

        if (loadI.op == Opcode::Load && loadI.result && !loadI.operands.empty() &&
            retI.op == Opcode::Ret && !retI.operands.empty()) {
            const auto &loadPtr = loadI.operands[0];
            const auto &retVal = retI.operands[0];

            // Check that we're loading from a param and returning the loaded value
            if (loadPtr.kind == il::core::Value::Kind::Temp &&
                retVal.kind == il::core::Value::Kind::Temp && retVal.id == *loadI.result) {
                int pIdx = indexOfParam(bb, loadPtr.id);
                if (pIdx >= 0 && static_cast<std::size_t>(pIdx) < kMaxGPRArgs) {
                    const PhysReg ptrReg = ctx.argOrder[static_cast<std::size_t>(pIdx)];
                    const bool isF64 = (loadI.type.kind == il::core::Type::Kind::F64) ||
                                       (ctx.fn.retType.kind == il::core::Type::Kind::F64);

                    if (isF64) {
                        // ldr d0, [ptrReg]
                        bbMir.instrs.push_back(MInstr{MOpcode::LdrFprBaseImm,
                                                      {MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(ptrReg),
                                                       MOperand::immOp(0)}});
                    } else {
                        const MOpcode loadOpc =
                            gprLoadOpcodeForType(loadI.type.kind, /*frameRelative=*/false);
                        bbMir.instrs.push_back(MInstr{loadOpc,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(ptrReg),
                                                       MOperand::immOp(0)}});
                    }
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                    ctx.fb.finalize();
                    return ctx.mf;
                }
            }
        }
    }

    // =========================================================================
    // gep+load/ret fast-path
    // =========================================================================
    // Pattern: %p = gep %param0, offset; %v = load type, %p; ret %v
    // Simple field accessor that computes GEP then loads and returns.
    // Emits: ldr x0, [x0, #offset]; ret
    if (ctx.fn.blocks.size() == 1 && bb.instructions.size() == 3 && !bb.params.empty()) {
        const auto &gepI = bb.instructions[0];
        const auto &loadI = bb.instructions[1];
        const auto &retI = bb.instructions[2];

        if (gepI.op == Opcode::GEP && gepI.result && gepI.operands.size() == 2 &&
            loadI.op == Opcode::Load && loadI.result && !loadI.operands.empty() &&
            retI.op == Opcode::Ret && !retI.operands.empty()) {
            const auto &gepBase = gepI.operands[0];
            const auto &gepOffset = gepI.operands[1];
            const auto &loadPtr = loadI.operands[0];
            const auto &retVal = retI.operands[0];

            // Check pattern: gep from param, load from gep result, return loaded value
            if (gepBase.kind == il::core::Value::Kind::Temp &&
                gepOffset.kind == il::core::Value::Kind::ConstInt &&
                loadPtr.kind == il::core::Value::Kind::Temp && loadPtr.id == *gepI.result &&
                retVal.kind == il::core::Value::Kind::Temp && retVal.id == *loadI.result) {
                int pIdx = indexOfParam(bb, gepBase.id);
                if (pIdx >= 0 && static_cast<std::size_t>(pIdx) < kMaxGPRArgs) {
                    const PhysReg baseReg = ctx.argOrder[static_cast<std::size_t>(pIdx)];
                    const long long offset = gepOffset.i64;
                    const bool isF64 = (loadI.type.kind == il::core::Type::Kind::F64) ||
                                       (ctx.fn.retType.kind == il::core::Type::Kind::F64);

                    if (isF64) {
                        // ldr d0, [baseReg, #offset]
                        bbMir.instrs.push_back(MInstr{MOpcode::LdrFprBaseImm,
                                                      {MOperand::regOp(PhysReg::V0),
                                                       MOperand::regOp(baseReg),
                                                       MOperand::immOp(offset)}});
                    } else {
                        const MOpcode loadOpc =
                            gprLoadOpcodeForType(loadI.type.kind, /*frameRelative=*/false);
                        bbMir.instrs.push_back(MInstr{loadOpc,
                                                      {MOperand::regOp(PhysReg::X0),
                                                       MOperand::regOp(baseReg),
                                                       MOperand::immOp(offset)}});
                    }
                    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
                    ctx.fb.finalize();
                    return ctx.mf;
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace zanna::codegen::aarch64::fastpaths
