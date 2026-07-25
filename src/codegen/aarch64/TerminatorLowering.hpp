//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/TerminatorLowering.hpp
// Purpose: Terminator instruction lowering for IL→MIR conversion
//          (br, cbr, trap, switch).
//
// Key invariants:
//   - Must be called after all non-terminator instructions are lowered so
//     that branch targets and phi mappings are fully resolved.
//   - Phi parameter spills are emitted into predecessor blocks.
//
// Ownership/Lifetime:
//   - Stateless free function; borrows all maps and builders for the call.
//
// Links: src/codegen/aarch64/TerminatorLowering.cpp,
//        src/codegen/aarch64/LoweringContext.hpp,
//        src/codegen/aarch64/passes/LoweringPass.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "LoweringContext.hpp"
#include "MachineIR.hpp"
#include "il/core/Function.hpp"

#include <unordered_map>
#include <vector>

/**
 * @file
 * @brief Declares the function-wide AArch64 IL terminator-lowering pass.
 *
 * Terminators are handled after ordinary instructions so operand mappings and
 * phi metadata are complete. The pass may append edge blocks, switch dispatch
 * blocks, spill/reload sequences, and terminal runtime calls.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Lowers control-flow terminators for every original IL block.
 *
 * Branch arguments become phi spill stores. Conditional branches may fuse
 * their comparison producer and use edge blocks when arguments differ by
 * successor. Switches select a linear chain, dense jump table, or recursive
 * binary-search tree; traps become non-returning runtime calls.
 *
 * @param fn Source IL function, borrowed without modification.
 * @param[in,out] mf MIR function whose first `fn.blocks.size()` blocks
 *        correspond by index to @p fn; auxiliary blocks may be appended.
 * @param ti Target register/ABI metadata used during value materialization.
 * @param[in,out] fb Frame allocator for phi and switch spill slots.
 * @param phiVregId Phi virtual-register mapping retained for interface
 *        compatibility; the current terminator implementation does not consult it.
 * @param phiRegClass Register class of each phi parameter by destination label.
 * @param phiSpillOffset Frame offset of each phi parameter by destination label.
 * @param[in,out] blockTempVRegSnapshot Per-original-block temporary mappings.
 * @param[in,out] tempRegClass Function-wide temporary register classes.
 * @param[in,out] nextVRegId Ordinary virtual-register allocator state.
 * @pre Ordinary instruction lowering and phi-slot allocation are complete.
 * @pre `mf.blocks.size() >= fn.blocks.size()` and
 *      `blockTempVRegSnapshot.size() >= fn.blocks.size()`.
 * @throws std::runtime_error If a handled terminator is malformed, its values
 *         cannot be materialized, phi metadata disagrees, or ID ranges overflow.
 */
void lowerTerminators(const il::core::Function &fn,
                      MFunction &mf,
                      const TargetInfo &ti,
                      FrameBuilder &fb,
                      const std::unordered_map<std::string, std::vector<uint16_t>> &phiVregId,
                      const std::unordered_map<std::string, std::vector<RegClass>> &phiRegClass,
                      const std::unordered_map<std::string, std::vector<int>> &phiSpillOffset,
                      std::vector<std::unordered_map<unsigned, uint16_t>> &blockTempVRegSnapshot,
                      std::unordered_map<unsigned, RegClass> &tempRegClass,
                      uint16_t &nextVRegId);

} // namespace zanna::codegen::aarch64
