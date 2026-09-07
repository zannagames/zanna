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
//   - Coalescing preserves parallel copy semantics by deterministic ordering:
//     the ordering and cycle breaking come from the shared
//     codegen/common/ra/ParallelCopy.hpp sequentializer; this class only
//     resolves operands to locations and supplies x86-64 moves and scratch.
//   - GPR cycles break through the fixed scratch R10 and memory-to-memory
//     copies go through R11; XMM scratch is borrowed from the allocator and
//     returned as soon as the sequencer is done with it.
// Ownership/Lifetime:
//   - Borrows non-owning references to the allocator and spiller.
// Links: src/codegen/x86_64/ra/Coalescer.cpp,
//        src/codegen/x86_64/ra/Allocator.hpp,
//        src/codegen/common/ra/ParallelCopy.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "../MachineIR.hpp"
#include "codegen/common/ra/ParallelCopy.hpp"

#include <optional>
#include <vector>

/// @file
/// @brief Declares the x86-64 PX_COPY lowering helper.

namespace zanna::codegen::x64::ra {

class LinearScanAllocator;
class Spiller;

/// @brief Source side of one copy: a physical register or a spill slot.
struct CopySource {
    /// @brief Storage category of the source.
    enum class Kind { Reg, Mem };

    Kind kind{Kind::Reg};
    PhysReg reg{PhysReg::RAX};
    int slot{-1};
};

/// @brief One `dst <- src` pair of a PX_COPY after operand resolution.
struct CopyTask {
    /// @brief Storage category of the destination.
    enum class DestKind { Reg, Mem };

    DestKind destKind{DestKind::Reg};
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
/// @details Resolves every operand to a register or spill slot (materializing
///          unmapped virtual registers through the allocator), then hands the
///          location pairs to the shared sequentializer, emitting x86-64
///          moves, loads and stores on its behalf.
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
    /// @brief Emitter adapter driven by the shared sequentializer.
    struct CopyEmitter;

    /// @brief Borrowed allocator supplying virtual and physical state.
    LinearScanAllocator &allocator_;
    /// @brief Borrowed spill-slot and memory-transfer helper.
    Spiller &spiller_;

    /// @brief Append the x86-64 instruction for one `dst <- src` location move.
    void emitMove(const codegen::ra::CopyLoc &dst,
                  const codegen::ra::CopyLoc &src,
                  std::vector<MInstr> &generated);

    /// @brief Borrow a register of class @p cls from the allocator.
    [[nodiscard]] PhysReg borrowRegister(RegClass cls, std::vector<MInstr> &generated);

    /// @brief Return a borrowed register to the allocator.
    void returnRegister(PhysReg reg, RegClass cls);
};

} // namespace zanna::codegen::x64::ra
