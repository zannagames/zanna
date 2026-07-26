//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/LivenessAnalysis.cpp
// Purpose: Cross-block liveness analysis for IL→MIR lowering.
//          Identifies IL temps whose values must survive across basic block
//          boundaries. Such temps are assigned dedicated stack spill slots so
//          the lowering pass can store them before block exits and reload them
//          at uses in successor blocks.
// Key invariants:
//   - Alloca temps are excluded (frame addresses, not values).
//   - Block parameters count as definitions by their own block.
//   - Each cross-block temp receives exactly one spill slot.
// Ownership/Lifetime:
//   - Returns LivenessInfo by value; borrows fn and fb only during the call.
// Links: src/codegen/aarch64/LivenessAnalysis.hpp,
//        src/codegen/aarch64/LowerILToMIR.cpp (usage),
//        src/codegen/aarch64/FrameBuilder.cpp (spill allocation)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements cross-block temporary discovery and spill reservation.
 *
 * Definitions are collected first, then every instruction operand and branch
 * argument is compared with its use block. Cross-block values receive stable
 * frame offsets through `FrameBuilder`; alloca temporaries are omitted because
 * their addresses are recomputed from the frame pointer.
 */

#include "LivenessAnalysis.hpp"
#include "LoweringContext.hpp"
#include "il/core/Instr.hpp"

namespace zanna::codegen::aarch64 {

/// @brief Analyze cross-block IL temporary uses and allocate their spill slots.
/// @param fn Function whose block-local definitions and uses are scanned.
/// @param allocaTemps Recomputable frame-address temporaries to exclude.
/// @param fb Frame builder that owns resulting spill assignments.
/// @return Self-contained definition, cross-block, and spill-offset maps.
LivenessInfo analyzeCrossBlockLiveness(const il::core::Function &fn,
                                       const std::unordered_set<unsigned> &allocaTemps,
                                       FrameBuilder &fb) {
    LivenessInfo info;

    // ===========================================================================
    // Step 1: Build map of tempId -> defining block index
    // ===========================================================================
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        // Block parameters are "defined" by their block
        for (const auto &param : bb.params) {
            info.tempDefBlock[param.id] = bi;
        }
        // Instructions that produce a result
        for (const auto &instr : bb.instructions) {
            if (instr.result) {
                info.tempDefBlock[*instr.result] = bi;
            }
        }
    }

    // ===========================================================================
    // Step 2: Find temps used in different blocks than their definition
    // ===========================================================================
    // Exclude alloca temps since they don't hold values - they represent stack addresses
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];
        /// @brief Records a temporary used outside its defining block.
        /// @param v Operand value to inspect.
        auto checkValue = [&](const il::core::Value &v) {
            if (v.kind == il::core::Value::Kind::Temp) {
                // Skip alloca temps - they don't need cross-block spilling
                // Their address is computed from the frame pointer when needed
                if (allocaTemps.contains(v.id))
                    return;
                auto it = info.tempDefBlock.find(v.id);
                if (it != info.tempDefBlock.end() && it->second != bi) {
                    // This temp is used in block bi but defined in a different block
                    info.crossBlockTemps.insert(v.id);
                }
            }
        };

        for (const auto &instr : bb.instructions) {
            for (const auto &op : instr.operands) {
                checkValue(op);
            }
        }

        // Check terminator operands (branch conditions and arguments)
        // The terminator is the last instruction in the block
        if (!bb.instructions.empty()) {
            const auto &term = bb.instructions.back();
            // Check condition operand for CBr
            if (term.op == il::core::Opcode::CBr && !term.operands.empty()) {
                checkValue(term.operands[0]); // condition
            }
            // Check return value for Ret
            if (term.op == il::core::Opcode::Ret && !term.operands.empty()) {
                checkValue(term.operands[0]);
            }
            // Check branch arguments (phi values)
            for (const auto &argList : term.brArgs) {
                for (const auto &arg : argList) {
                    checkValue(arg);
                }
            }
        }
    }

    // ===========================================================================
    // Step 3: Allocate spill slots for cross-block temps
    // ===========================================================================
    for (unsigned tempId : info.crossBlockTemps) {
        int offset = ensureCrossBlockSpill(fb, tempId);
        info.crossBlockSpillOffset[tempId] = offset;
    }

    return info;
}

} // namespace zanna::codegen::aarch64
