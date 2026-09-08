//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/SpillSlots.hpp
// Purpose: The x86-64 spill-slot placeholder encoding and the load/store
//          instruction builders every allocator-side producer of spill
//          traffic shares (register allocation, parallel-copy lowering,
//          tests).
// Key invariants:
//   - A spill slot is an RBP-relative placeholder displacement: slot index
//     `i` of class `cls` encodes as `-(i + base(cls) + 1) * 8` with
//     `base(GPR) = kSpillSlotOffsetGPR` and `base(XMM) = kSpillSlotOffsetXMM`,
//     so frame lowering classifies a slot from its displacement alone and
//     rewrites it to the final offset (FrameLowering.cpp).
//   - Slot indices are zero-based per class.
// Ownership/Lifetime:
//   - Header-only free functions; the returned instructions are values.
// Links: src/codegen/x86_64/FrameLowering.cpp, src/codegen/x86_64/TargetX64.hpp,
//        src/codegen/x86_64/ra/GlobalAllocator.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/TargetX64.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

/// @file
/// @brief Spill-slot placeholder operands and spill load/store builders.

namespace zanna::codegen::x64::ra {

/// @brief Encode class-specific spill slot @p slot as an RBP-relative placeholder.
/// @throws std::out_of_range if @p slot is negative.
/// @throws std::overflow_error if the slot exceeds its class's placeholder
///         range or the encoded displacement cannot fit in int32.
[[nodiscard]] inline Operand makeSpillSlotOperand(RegClass cls, int slot) {
    if (slot < 0)
        throw std::out_of_range("x86 spill slot: negative spill slot index");
    const int base = cls == RegClass::GPR ? kSpillSlotOffsetGPR : kSpillSlotOffsetXMM;
    const int rangeEnd = cls == RegClass::GPR ? kSpillSlotOffsetXMM : kMaxFramePlaceholderIndex;
    if (slot >= rangeEnd - base - 1)
        throw std::overflow_error("x86 spill slot: spill slot index exceeds its placeholder range");
    const int64_t placeholderSlot = static_cast<int64_t>(slot) + static_cast<int64_t>(base) + 1;
    const int64_t offset64 = -placeholderSlot * static_cast<int64_t>(kSlotSizeBytes);
    if (offset64 < std::numeric_limits<int32_t>::min() ||
        offset64 > std::numeric_limits<int32_t>::max())
        throw std::overflow_error("x86 spill slot: spill slot displacement overflows int32");
    const auto baseReg = makePhysReg(RegClass::GPR, static_cast<uint16_t>(PhysReg::RBP));
    return makeMemOperand(baseReg, static_cast<int32_t>(offset64));
}

/// @brief Load spill slot @p slot of class @p cls into @p dst.
[[nodiscard]] inline MInstr makeSpillLoad(RegClass cls, PhysReg dst, int slot) {
    const Operand reg = makePhysRegOperand(cls, static_cast<uint16_t>(dst));
    return MInstr::make(cls == RegClass::GPR ? MOpcode::MOVmr : MOpcode::MOVSDmr,
                        {reg, makeSpillSlotOperand(cls, slot)});
}

/// @brief Store @p src into spill slot @p slot of class @p cls.
[[nodiscard]] inline MInstr makeSpillStore(RegClass cls, int slot, PhysReg src) {
    const Operand reg = makePhysRegOperand(cls, static_cast<uint16_t>(src));
    return MInstr::make(cls == RegClass::GPR ? MOpcode::MOVrm : MOpcode::MOVSDrm,
                        {makeSpillSlotOperand(cls, slot), reg});
}

} // namespace zanna::codegen::x64::ra
