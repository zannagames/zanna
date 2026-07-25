//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/FramePlan.hpp
// Purpose: Describe a minimal frame save/restore plan for AArch64 functions.
// Key invariants:
//   - Frame sizes are always 16-byte aligned per AArch64 ABI.
//   - saveGPRs/saveFPRs do not include X29/X30; those are handled in prologue.
// Ownership/Lifetime:
//   - FramePlan owns its register-list storage and contains no borrowed pointers.
// Links: src/codegen/aarch64/AsmEmitter.hpp,
//        src/codegen/aarch64/binenc/A64BinaryEncoder.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Defines the self-contained callee-save and local-frame plan shared by
 *        AArch64 text and binary emitters.
 */

#pragma once

#include <vector>

#include "codegen/aarch64/TargetAArch64.hpp"

namespace zanna::codegen::aarch64 {

/// @brief Describes the stack frame layout and callee-saved register plan for a function.
///
/// The FramePlan captures all information needed to generate the function prologue
/// (stack allocation and register saves) and epilogue (register restores and stack
/// deallocation). It follows the AAPCS64 (ARM 64-bit Procedure Call Standard).
///
/// ## AArch64 Stack Frame Layout
///
/// A typical AArch64 stack frame looks like this (growing downward):
///
/// ```
///     Higher addresses
///     +---------------------------+
///     | Caller's frame            |
///     +---------------------------+
///     | Return address (LR/X30)   | <- Pushed by callee if non-leaf
///     | Frame pointer (FP/X29)    | <- Pushed by callee if using FP
///     +---------------------------+
///     | Saved GPRs (X19-X28)      | <- Callee-saved, pushed in pairs
///     +---------------------------+
///     | Saved FPRs (D8-D15)       | <- Callee-saved, pushed in pairs
///     +---------------------------+
///     | Local variables           | <- localFrameSize bytes
///     | Spill slots               |
///     +---------------------------+
///     | Outgoing arguments        | <- For calls with >8 args
///     +---------------------------+ <- SP (16-byte aligned)
///     Lower addresses
/// ```
///
/// ## Callee-Saved Registers (AArch64 AAPCS64)
///
/// - **GPRs (X19-X28)**: Must be preserved across calls. X29 (FP) and X30 (LR)
///   are handled specially in the prologue/epilogue.
/// - **FPRs (D8-D15)**: The lower 64 bits of V8-V15 must be preserved.
///   Only the D (double) portion is callee-saved, not the full 128-bit Q register.
///
/// ## Alignment Requirements
///
/// The stack pointer must always be 16-byte aligned. When saving an odd number
/// of 8-byte registers, padding is added to maintain alignment.
///
/// @see PrologueEpiloguePass for prologue/epilogue code generation
/// @see RegAllocLinear for determining which callee-saved registers are used
/// @invariant @ref localFrameSize is nonnegative and 16-byte aligned.
/// @invariant Save lists exclude frame pointer `x29` and link register `x30`.
struct FramePlan {
    /// @brief List of general-purpose registers that must be saved in the prologue.
    ///
    /// Contains only those X19-X28 registers that are actually used by the function.
    /// The prologue generator saves these in pairs using STP instructions for
    /// efficiency. An odd final register occupies its own padded stack slot.
    ///
    /// @note X29 (FP) and X30 (LR) are handled separately, not included in this list.
    std::vector<PhysReg> saveGPRs;

    /// @brief List of floating-point registers that must be saved in the prologue.
    ///
    /// Contains only those D8-D15 registers that are actually used by the function.
    /// These are saved as 64-bit double-precision values (D registers), even though
    /// the hardware registers are 128-bit SIMD registers (V/Q registers).
    ///
    /// @note Only the lower 64 bits (D portion) are callee-saved per AAPCS64.
    std::vector<PhysReg> saveFPRs;

    /// @brief Size in bytes reserved for local variables and spill slots.
    ///
    /// This is the space between the saved registers and the stack pointer,
    /// used for:
    /// - Stack-allocated local variables (alloca in IL)
    /// - Register spill slots during register allocation
    /// - Temporary storage for complex operations
    ///
    /// Always rounded up to a multiple of 16 bytes for AArch64 SP alignment.
    /// A value of 0 indicates a leaf function with no local stack usage.
    int localFrameSize{0};
};

} // namespace zanna::codegen::aarch64
