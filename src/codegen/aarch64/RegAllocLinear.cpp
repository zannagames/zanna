//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/RegAllocLinear.cpp
// Purpose: Thin adapter that drives the AArch64 linear-scan register allocator.
//          Delegates all work to ra::LinearAllocator.
//
// Key invariants:
//   - After allocation all MReg operands have isPhys=true.
//   - Callee-saved register usage is recorded in fn.savedGPRs/savedFPRs.
//
// Ownership/Lifetime:
//   - Modifies MFunction in place; caller retains ownership.
//
// Links: src/codegen/aarch64/RegAllocLinear.hpp,
//        src/codegen/aarch64/ra/Allocator.hpp
//
//===----------------------------------------------------------------------===//

#include "RegAllocLinear.hpp"

#include "ra/Allocator.hpp"

/**
 * @file
 * @brief Implements the public adapter to the AArch64 linear allocator.
 *
 * Allocation policy and rewriting live in `ra::LinearAllocator`; this
 * translation unit keeps that internal type out of higher-level pipeline
 * interfaces.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Constructs and runs a linear allocator for one MIR function.
 *
 * @param[in,out] fn Function whose virtual registers are resolved in place.
 * @param ti Target register inventory and ABI rules.
 * @return Result object returned by the allocator.
 * @post Register operands in @p fn are physical and saved-register/frame
 *       metadata has been updated.
 */
AllocationResult allocate(MFunction &fn, const TargetInfo &ti) {
    ra::LinearAllocator allocator(fn, ti);
    return allocator.run();
}

} // namespace zanna::codegen::aarch64
