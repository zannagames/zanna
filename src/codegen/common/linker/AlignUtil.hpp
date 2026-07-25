//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/AlignUtil.hpp
// Purpose: Shared alignment utility for the native linker subsystem.
// Key invariants:
//   - align must be 0 or a power of two
//   - align=0 is treated as no-op (returns val unchanged)
// Links: codegen/common/linker/SectionMerger.cpp,
//        codegen/common/linker/ElfExeWriter.cpp,
//        codegen/common/linker/MachOExeWriter.cpp,
//        codegen/common/linker/PeExeWriter.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file AlignUtil.hpp
 * @brief Provides checked power-of-two alignment for native linker layouts.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace zanna::codegen::linker {

/// @brief Rounds a size upward to a requested power-of-two alignment.
/// @param val Value to align.
/// @param align Required alignment, or zero to leave @p val unchanged.
/// @return The smallest multiple of @p align greater than or equal to
///         @p val, or @p val when @p align is zero.
/// @throws std::invalid_argument If a nonzero alignment is not a power of two.
/// @throws std::length_error If rounding would overflow `size_t`.
inline size_t alignUp(size_t val, size_t align) {
    if (align == 0)
        return val;
    if ((align & (align - 1)) != 0)
        throw std::invalid_argument("alignUp: alignment must be a power of two");
    if (val > std::numeric_limits<size_t>::max() - (align - 1))
        throw std::length_error("alignUp: aligned value exceeds addressable size");
    assert((align & (align - 1)) == 0 && "alignUp: alignment must be a power of two");
    return (val + align - 1) & ~(align - 1);
}

} // namespace zanna::codegen::linker
