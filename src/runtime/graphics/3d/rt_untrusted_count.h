//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_untrusted_count.h
// Purpose: Shared validation for counts read from untrusted 3D asset files.
//
// Key invariants:
//   - Negative counts are always invalid.
//   - count * elem_min_bytes must fit in size_t and stay within the available byte budget.
//
// Ownership/Lifetime:
//   - Header-only helper; owns no memory and performs no allocation.
//
// Links: assets/rt_gltf.c, assets/rt_fbx_loader.c, render/rt_mesh3d.c,
//        scene/rt_scene3d_vscn_load.c, rt_game3d.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides overflow-safe validation for untrusted serialized counts.
/// @details The helper proves that a signed count is non-negative, its minimum
/// byte requirement is representable by `size_t`, and the corresponding source
/// range fits inside the caller-supplied byte budget before allocation or parsing.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Validate an untrusted element count against the bytes available for its source data.
/// @details A zero minimum element size is valid only for a zero count, avoiding
///          ambiguous acceptance of arbitrary counts with no byte bound.
/// @param count Signed element count read from an untrusted source.
/// @param elem_min_bytes Minimum encoded bytes required per element.
/// @param available_bytes Number of source bytes available for all elements.
/// @return Nonzero when the count and minimum byte product are safe and in bounds.
static inline int rt_untrusted_count_ok(int64_t count,
                                        size_t elem_min_bytes,
                                        size_t available_bytes) {
    uint64_t unsigned_count;

    if (count < 0)
        return 0;
    if (elem_min_bytes == 0u)
        return count == 0;
    unsigned_count = (uint64_t)count;
    if (unsigned_count > (uint64_t)(SIZE_MAX / elem_min_bytes))
        return 0;
    return (size_t)unsigned_count <= available_bytes / elem_min_bytes;
}

#ifdef __cplusplus
}
#endif
