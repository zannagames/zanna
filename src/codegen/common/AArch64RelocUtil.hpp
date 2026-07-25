//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/AArch64RelocUtil.hpp
// Purpose: Shared AArch64 instruction-shape helpers for relocation validation.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

/// @file
/// @brief Defines bit-level AArch64 instruction checks shared by relocators.

namespace zanna::codegen {

/// @brief Test whether an encoded instruction belongs to AArch64 ADD-immediate.
/// @param insn Little-endian-decoded 32-bit instruction word.
/// @return `true` for the ADD-immediate encoding class, independent of operand
///         fields and immediate value.
inline bool isA64AddImmediate(uint32_t insn) {
    return (insn & 0x7F000000u) == 0x11000000u;
}

/// @brief Decode the element-size shift of an unsigned-offset load/store.
///
/// Recognizes the unsigned-immediate scalar and SIMD load/store class while
/// excluding unscaled, pre/post-indexed, literal, and pair encodings.
///
/// @param insn Little-endian-decoded 32-bit instruction word.
/// @param[out] shift Receives the scale exponent used by the 12-bit offset;
///                   128-bit vector accesses report four.
/// @return `true` when @p insn belongs to the recognized encoding class.
inline bool a64UnsignedLdStOffsetShift(uint32_t insn, uint32_t &shift) {
    // Unsigned-immediate load/store class. This excludes pre/post-indexed,
    // unscaled, literal, and pair encodings; the scale lives in bits [31:30],
    // with 128-bit SIMD/vector memory operations using the architectural
    // 16-byte scale.
    if ((insn & 0x3B000000u) != 0x39000000u)
        return false;
    shift = insn >> 30;
    if ((insn & 0x04800000u) == 0x04800000u)
        shift = 4;
    return true;
}

/// @brief Test an unsigned-offset load/store for an exact element-size shift.
/// @param insn Little-endian-decoded 32-bit instruction word.
/// @param expectedShift Required scale exponent for the encoded offset.
/// @return `true` when @p insn is a recognized unsigned-offset access and its
///         decoded shift equals @p expectedShift.
inline bool isA64UnsignedLdStOffsetWithShift(uint32_t insn, uint32_t expectedShift) {
    uint32_t shift = 0;
    return a64UnsignedLdStOffsetShift(insn, shift) && shift == expectedShift;
}

} // namespace zanna::codegen
