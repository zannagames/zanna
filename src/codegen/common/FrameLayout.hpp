//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/FrameLayout.hpp
// Purpose: Abstract interface for stack frame layout management shared between
//          backend register allocators. Both x86-64 (FrameInfo + assignSpillSlots)
//          and AArch64 (FrameBuilder) implement this interface to provide
//          spill slot allocation and frame size computation.
//
// Key invariants:
//   - addLocal() and ensureSpill() may be called in any order before finalize().
//   - finalize() computes the total frame size with proper alignment.
//   - totalBytes() is only valid after finalize() has been called.
//   - Offsets are signed (negative on frame-pointer-relative architectures).
//
// Ownership/Lifetime: Interface only; implementations borrow MFunction references.
// Links: codegen/aarch64/FrameBuilder.hpp, codegen/x86_64/FrameLowering.hpp,
//        plans/audit-01-backend-abstraction.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

/// @file
/// @brief Declares the backend-neutral stack-frame allocation interface.

namespace zanna::codegen::common {

/// @brief Abstract interface for stack frame layout management.
///
/// @details Provides the common operations that register allocators need from
///          the frame layout system: allocating local variable slots, allocating
///          spill slots for evicted virtual registers, reserving space for outgoing
///          call arguments, and finalizing the frame size.
///
///          Both backends implement this interface differently:
///          - AArch64's FrameBuilder uses incremental allocation with slot reuse
///          - x86-64's FrameInfo + assignSpillSlots uses post-allocation assignment
///
///          The shared interface enables future shared register allocator code
///          to manage spill slots without knowing which backend is active.
class FrameLayout {
  public:
    /// @brief Destroy a frame-layout implementation through the common interface.
    virtual ~FrameLayout() = default;

    /// @brief Allocate a local variable slot for an IL temporary.
    /// @param tempId The IL temporary identifier.
    /// @param sizeBytes Size of the slot in bytes.
    /// @param alignBytes Alignment requirement (typically 8 bytes).
    virtual void addLocal(unsigned tempId, int sizeBytes, int alignBytes) = 0;

    /// @brief Get the frame-relative offset for a local variable.
    /// @param tempId The IL temporary identifier.
    /// @return Signed offset from the frame pointer (negative on most targets).
    virtual int localOffset(unsigned tempId) const = 0;

    /// @brief Ensure a spill slot exists for a virtual register.
    /// @param vreg Virtual register identifier.
    /// @param sizeBytes Size of the spill slot (typically 8 bytes).
    /// @param alignBytes Alignment requirement (typically 8 bytes).
    /// @return Frame-relative offset of the spill slot.
    virtual int ensureSpill(uint32_t vreg, int sizeBytes, int alignBytes) = 0;

    /// @brief Reserve space for outgoing arguments passed on the stack.
    /// @param bytes Maximum bytes needed for any call's stack arguments.
    /// @post A later smaller request must not reduce the effective reservation.
    virtual void setMaxOutgoing(int bytes) = 0;

    /// @brief Finalize frame layout and compute the total frame size.
    /// @details Must be called once after all locals and spills are declared.
    virtual void finalize() = 0;

    /// @brief Get the total frame size in bytes (valid only after finalize()).
    /// @return Final aligned frame byte count.
    virtual int totalBytes() const = 0;
};

} // namespace zanna::codegen::common
