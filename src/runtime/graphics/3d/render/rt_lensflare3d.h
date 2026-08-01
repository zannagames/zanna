//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_lensflare3d.h
// Purpose: LensFlare3D — occlusion-aware ghost-chain lens flares bound to a
//   Light3D and drawn in overlay space along the light->screen-center axis.
// Key invariants:
//   - Elements hold pre-tinted procedural ghost sprites (radial-falloff discs).
//   - Occlusion uses CPU depth when available, otherwise asynchronous backend
//     probes; backends without either depth source draw unoccluded.
// Ownership/Lifetime:
//   - LensFlare3D is GC-managed; finalizer releases the bound light and every
//     element's ghost Pixels.
// Links: rt_canvas3d.h, rt_light3d.c, misc/plans/fps/07-visual-polish.md
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares Light3D-bound, occlusion-aware lens flare overlays.
/// @details A flare retains one light and up to sixteen pre-tinted ghost
///   elements. Drawing projects the light, smooths sampled visibility, and
///   queues each ghost after scene rendering but before presentation. Mutable
///   counts, ghost handles, numeric element state, and smoothing state are
///   bounded or repaired before use.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a lens flare bound to @p light (retained).
/// @param light Live Light3D retained for the flare lifetime.
/// @return New GC-managed empty flare, or `NULL` for invalid input or allocation failure.
void *rt_lensflare3d_new(void *light);

/// @brief Add a procedural ghost along the light-to-screen-center axis.
/// @param obj LensFlare3D receiver; invalid handles and full element lists are ignored.
/// @param axis_offset Axis position clamped to `[-1, 2]`: zero is the light,
///   one is screen center, and two mirrors through center.
/// @param size Base output-pixel dimension, defaulted to 32 when invalid and capped at 1024.
/// @param color_rgb Packed `0xRRGGBB` sprite tint.
/// @param rotation Reserved for API stability; circular ghost sprites ignore it.
void rt_lensflare3d_add_element(
    void *obj, double axis_offset, double size, int64_t color_rgb, double rotation);

/// @brief Draw the flare into the canvas overlay (call after End, before Flip).
/// @param canvas Canvas3D receiver supplying completed scene depth and overlay queue.
/// @param flare LensFlare3D receiver borrowed for the call.
void rt_canvas3d_draw_lens_flare(void *canvas, void *flare);

#ifdef __cplusplus
}
#endif
