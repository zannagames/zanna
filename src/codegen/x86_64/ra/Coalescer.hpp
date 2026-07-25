//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Coalescer.hpp
// Purpose: Declare the PX_COPY lowering helper used by the linear-scan
//          allocator to coalesce parallel move bundles into executable
//          instruction sequences.
// Key invariants:
//   - Coalescing preserves parallel copy semantics by deterministic ordering.
//   - Cycle detection prevents infinite loops when copies form a permutation.
// Ownership/Lifetime:
//   - Borrows non-owning references to the allocator and spiller.
// Links: src/codegen/x86_64/ra/Coalescer.cpp,
//        src/codegen/x86_64/ra/Allocator.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"

#include <optional>
#include <vector>

/// @file
/// @brief Declares allocator-integrated x86-64 parallel-copy lowering.

namespace zanna::codegen::x64::ra {

class LinearScanAllocator;
class Spiller;

/// @brief Describes the source of a parallel copy operand.
struct CopySource {
    /// @brief Storage form supplying the copied value.
    enum class Kind { Reg, Mem };

    /// @brief Whether the source is @c reg or spill @c slot.
    Kind kind{Kind::Reg};
    /// @brief Physical source when @c kind is @c Reg.
    PhysReg reg{PhysReg::RAX};
    /// @brief Spill-slot index when @c kind is @c Mem.
    int slot{-1};
};

/// @brief Represents a single PX_COPY transfer lowered by the coalescer.
struct CopyTask {
    /// @brief Storage form receiving the copied value.
    enum class DestKind { Reg, Mem };

    /// @brief Whether the destination is @c destReg or @c destSlot.
    DestKind destKind{DestKind::Reg};
    /// @brief Register class shared by source and destination.
    RegClass cls{RegClass::GPR};
    /// @brief Physical destination when @c destKind is @c Reg.
    PhysReg destReg{PhysReg::RAX};
    /// @brief Spill-slot index when @c destKind is @c Mem.
    int destSlot{-1};
    /// @brief Materialized source location.
    CopySource src{};
    /// @brief Original virtual destination id, when one existed.
    std::optional<uint16_t> destVReg{};
};

/// @brief Handles lowering of PX_COPY instructions using allocator facilities.
/// @details Builds explicit storage-location transfers, schedules acyclic tasks
///          in dependency-safe order, and breaks cycles with fixed GPR or
///          borrowed XMM scratch registers.
class Coalescer {
  public:
    /// @brief Construct a coalescer using the given allocator and spiller.
    /// @param allocator Borrowed allocator providing state, pools, and move construction.
    /// @param spiller Borrowed spiller providing stack slots and memory transfers.
    /// @pre Both collaborators outlive this coalescer.
    Coalescer(LinearScanAllocator &allocator, Spiller &spiller);

    /// @brief Lower a PX_COPY bundle into concrete move instructions.
    /// @details Accepts alternating destination/source register operands,
    ///          materializes virtual locations, emits dependency-safe copies,
    ///          and appends them without clearing existing output.
    /// @param instr PX_COPY pseudo-instruction containing register pairs.
    /// @param out Output vector extended with prefix and copy instructions.
    /// @throws std::runtime_error If pairs are malformed, classes disagree,
    ///         fixed scratch registers occur explicitly, or allocation fails.
    void lower(const MInstr &instr, std::vector<MInstr> &out);

  private:
    /// @brief Borrowed allocator supplying virtual and physical state.
    LinearScanAllocator &allocator_;
    /// @brief Borrowed spill-slot and memory-transfer helper.
    Spiller &spiller_;

    /// @brief Emit a single copy task as one or more concrete machine instructions.
    /// @param task The copy task describing source and destination (reg or mem).
    /// @param generated Output vector to append the generated instructions to.
    /// @throws std::runtime_error If an XMM scratch register cannot be allocated.
    void emitCopyTask(const CopyTask &task, std::vector<MInstr> &generated);
};

} // namespace zanna::codegen::x64::ra
