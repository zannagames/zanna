//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_brdf_lut.h
// Purpose: Shared split-sum environment-BRDF lookup table for image-based
//   lighting. One deterministic CPU precomputation feeds every backend: the
//   GPU backends upload the table as a small RG texture, the software
//   rasterizer samples it directly.
// Key invariants:
//   - The table is a pure function of (NdotV, roughness), built from a fixed
//     Hammersley sample sequence — bit-stable per platform, VM == native.
//   - Lazy construction is protected by a cross-platform atomic once gate;
//     concurrent backend initialization and sampling are safe.
// Ownership/Lifetime:
//   - The table is process-lifetime static storage; callers never free it.
// Links: vgfx3d_backend_sw_raster.inc, vgfx3d_backend_metal.m,
//   vgfx3d_backend_opengl.c, vgfx3d_backend_d3d11.c
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_brdf_lut.h
 * @brief Exposes the shared split-sum environment BRDF lookup table.
 *
 * Graphics3D backends use one deterministic CPU-generated table for specular image-based
 * lighting. GPU backends upload its interleaved Fresnel scale/bias pairs, while the software
 * rasterizer samples the same process-lifetime data directly. Lazy initialization uses the native
 * platform once primitive, and callers never own or free the returned storage.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Table edge size, with NdotV on X, roughness on Y, and two floats per texel.
#define VGFX3D_BRDF_LUT_SIZE 64

/// @brief Build the table if it has not been built yet (thread-safe, exactly once).
/// @details One caller performs the fixed-sample integration while concurrent callers block in the
///          platform once primitive until the immutable process-lifetime table is published.
void vgfx3d_brdf_lut_ensure(void);

/// @brief Borrow the table data: VGFX3D_BRDF_LUT_SIZE^2 texels of (A, B) float
///   pairs, row-major with NdotV along X and roughness along Y. Builds the
///   table on first use.
/// @return Borrowed process-lifetime pointer to the initialized interleaved `(A, B)` pairs.
const float *vgfx3d_brdf_lut_data(void);

/// @brief Bilinear CPU sample of the table; writes scale (A) and bias (B) for
///   the split-sum specular term F0 * A + B.
/// @param ndotv Cosine between the surface normal and view direction, clamped to `[0, 1]`.
/// @param roughness Perceptual surface roughness, clamped to `[0, 1]`.
/// @param out_ab Caller-owned two-float output receiving the interpolated scale and bias; must be
///               non-null.
void vgfx3d_brdf_lut_sample(float ndotv, float roughness, float *out_ab);

#ifdef __cplusplus
}
#endif
