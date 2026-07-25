//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_skinning_scratch.h
// Purpose: Grow-only CPU skinning scratch storage shared by canvas-frame draw paths.
// Key invariants:
//   - Scratch buffers are caller-owned and never static/global.
//   - Capacity only grows; callers decide when to release storage.
// Ownership/Lifetime:
//   - The owner initializes the struct to zero and frees it with
//     vgfx3d_skinning_scratch_free().
//   - Pointers are invalidated only by scratch growth or explicit free.
// Links: vgfx3d_skinning.h, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_skinning_scratch.h
 * @brief Defines caller-owned, grow-only temporary storage for CPU skinning.
 *
 * Reusing one zero-initialized instance across draws avoids recomputing per-bone normal matrices
 * and matrix-validity flags for every vertex. Capacity grows on demand and never shrinks
 * implicitly; pointers may change during growth and remain valid until the next growth operation
 * or vgfx3d_skinning_scratch_free().
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reusable buffers that cache per-palette CPU skinning data.
 *
 * Initialize every field to zero before first use. The structure owns both buffer pointers but is
 * itself owned by the caller.
 */
typedef struct {
    /// Grow-only array containing one 16-float normal matrix per cached bone.
    float *normal_palette;
    /// Number of bone matrices currently representable by @ref normal_palette.
    int32_t normal_palette_capacity;
    /// Diagnostic count of successful @ref normal_palette growth operations.
    uint64_t normal_palette_grow_count;
    /* Per-bone finiteness flags, computed once per palette. Matrix validity
     * is a bone property, so hoisting it out of the vertex loop removes up
     * to ~128 isfinite() calls per vertex from the CPU-skinning inner loop. */
    /// Grow-only array containing one matrix-validity flag per cached bone.
    uint8_t *bone_valid;
    /// Number of bone flags currently representable by @ref bone_valid.
    int32_t bone_valid_capacity;
} vgfx3d_skinning_scratch_t;

/// @brief Release all owned CPU skinning scratch buffers and reset counters.
/// @param scratch Mutable caller-owned scratch state; null is a no-op. The structure remains
///                zeroed and reusable after the call.
void vgfx3d_skinning_scratch_free(vgfx3d_skinning_scratch_t *scratch);

#ifdef __cplusplus
}
#endif
