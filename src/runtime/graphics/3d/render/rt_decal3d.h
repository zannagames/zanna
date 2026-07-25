//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_decal3d.h
// Purpose: 3D decals — surface-oriented quads with texture, lifetime, fade.
//   Used for bullet holes, blood splatters, footprints, tire marks.
//
// Key invariants:
//   - Position + normal define the decal plane.
//   - Rendered as a textured quad offset slightly from surface.
//   - Lifetime auto-decrements; alpha fades linearly over last 20%.
//
// Ownership/Lifetime:
//   - Returned decals are GC-managed and retain valid Pixels textures.
//   - Canvas draw calls borrow the decal and its lazily owned mesh/material.
//
// Links: rt_canvas3d.h
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares the GC-managed Decal3D lifecycle and Canvas3D draw interface.
/// @details A decal stores sanitized surface placement, optional texture,
///   lifetime/fade state, and lazily generated render resources. Callers advance
///   time explicitly and may remove objects once expiration is reported.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a surface-aligned projected decal.
/// @param position Vec3 world-space center; required.
/// @param normal Vec3 surface normal, normalized internally with a positive-Y fallback.
/// @param size Positive square width/height in world units; invalid values use one
///   and extreme values are capped.
/// @param texture Optional Pixels image retained when valid; other values produce
///   an untextured decal.
/// @return New GC-managed decal handle, or `NULL` for invalid vector arguments
///   or allocation failure.
void *rt_decal3d_new(void *position, void *normal, double size, void *texture);

/// @brief Set or reset the decal lifetime.
/// @param decal Decal3D receiver; invalid handles are ignored.
/// @param seconds Lifetime in seconds. Negative or nonfinite values make the
///   decal permanent; zero expires immediately.
void rt_decal3d_set_lifetime(void *decal, double seconds);

/// @brief Override the constant depth bias (negative pulls toward camera; 0 = auto).
/// @param decal Decal3D receiver; invalid handles are ignored.
/// @param bias NDC-scale constant bias clamped to `[-0.05, 0.05]`; nonfinite
///   input restores automatic size-scaled bias.
void rt_decal3d_set_depth_bias(void *decal, double bias);

/// @brief Advance the decal's age by @p dt seconds.
/// @param decal Decal3D receiver; invalid handles are ignored.
/// @param dt Positive elapsed seconds. Invalid or nonpositive steps are ignored.
void rt_decal3d_update(void *decal, double dt);

/// @brief Whether the decal has exceeded its lifetime and should be removed.
/// @param decal Decal3D receiver.
/// @return Nonzero for an invalid or expired decal; zero for a live or permanent decal.
int8_t rt_decal3d_is_expired(void *decal);

/// @brief Project and draw @p decal onto the 3D canvas.
/// @details Builds render resources on first use and skips invalid or expired decals.
/// @param canvas Canvas3D receiver used for deferred mesh submission.
/// @param decal Decal3D receiver borrowed for the call.
void rt_canvas3d_draw_decal(void *canvas, void *decal);

/// @brief Shift the decal by the negative floating-origin delta.
/// @details Invalidates cached quad geometry so a subsequent draw rebuilds at
///   the rebased position.
/// @param decal Decal3D receiver; invalid handles are ignored.
/// @param dx World-origin X displacement.
/// @param dy World-origin Y displacement.
/// @param dz World-origin Z displacement.
void rt_decal3d_rebase_origin(void *decal, double dx, double dy, double dz);

/// @brief Copy the sanitized decal world position.
/// @param decal Decal3D receiver; invalid handles produce zeros.
/// @param out Output array of three doubles; `NULL` is ignored.
void rt_decal3d_get_position(void *decal, double out[3]);

#ifdef __cplusplus
}
#endif
