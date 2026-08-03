//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_sky3d.h
// Purpose: Public C ABI for the procedural analytic sky (CPU-generated cubemap
//   installed through the existing skybox + IBL path).
// Key invariants:
//   - Deterministic function of sun direction, turbidity, and ground albedo.
// Ownership/Lifetime:
//   - GC-managed; getters returning objects retain for the caller.
// Links: rt_sky3d.c, ADR 0090
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_sky3d.h
 * @brief Declares the procedural Sky3D runtime C ABI.
 *
 * Sky parameters mark a retained CPU-generated cubemap dirty. An explicit update regenerates that
 * cubemap and can install it on Canvas3D through the standard skybox and IBL path.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a procedural sky with a mid-morning default sun.
/// @return New GC-managed Sky3D handle, or NULL after allocation failure.
void *rt_sky3d_new(void);

/// @brief Set the normalized direction TOWARD the sun; marks dirty only when it changes.
/// @param sky Sky3D handle; invalid handles trap and are ignored.
/// @param direction Vec3 handle containing a finite nonzero direction toward the sun.
void rt_sky3d_set_sun_direction(void *sky, void *direction);

/// @brief Atmospheric haze, 1 (clear) to 10 (hazy); marks dirty on change.
/// @param sky Sky3D handle; invalid handles trap and are ignored.
/// @param turbidity Requested atmospheric turbidity, clamped to `[1, 10]`.
void rt_sky3d_set_turbidity(void *sky, double turbidity);

/// @brief Return the current atmospheric turbidity.
/// @param sky Sky3D handle; invalid handles trap.
/// @return Turbidity in `[1, 10]`, or zero when validation fails.
double rt_sky3d_get_turbidity(void *sky);

/// @brief Ground hemisphere albedo used below the horizon; marks dirty on change.
/// @param sky Sky3D handle; invalid handles trap and are ignored.
/// @param r Red albedo component, clamped to `[0, 1]`.
/// @param g Green albedo component, clamped to `[0, 1]`.
/// @param b Blue albedo component, clamped to `[0, 1]`.
void rt_sky3d_set_ground_albedo(void *sky, double r, double g, double b);

/// @brief Cubemap face resolution (16..256, default 64); marks dirty on change.
/// @param sky Sky3D handle; invalid handles trap and are ignored.
/// @param resolution Requested square face dimension in pixels.
void rt_sky3d_set_resolution(void *sky, int64_t resolution);

/// @brief Return the configured cubemap-face resolution.
/// @param sky Sky3D handle; invalid handles trap.
/// @return Face dimension in pixels, or zero when validation fails.
int64_t rt_sky3d_get_resolution(void *sky);

/// @brief True while the cubemap needs regeneration.
/// @param sky Sky3D handle; invalid handles trap.
/// @return 1 when authored parameters are newer than the cubemap, otherwise 0.
int8_t rt_sky3d_get_dirty(void *sky);

/// @brief Regenerate if dirty and install as @p canvas's skybox (NULL = no install).
/// @param sky Sky3D handle; invalid handles trap.
/// @param canvas Optional Canvas3D handle receiving the generated skybox.
/// @return 1 when a generated cubemap is available, otherwise 0.
int8_t rt_sky3d_update(void *sky, void *canvas);

/// @brief Retained generated CubeMap3D (NULL before the first Update).
/// @param sky Sky3D handle; invalid handles trap.
/// @return Retained CubeMap3D handle, or NULL before generation or on invalid input. The caller
/// owns the returned reference.
void *rt_sky3d_get_cubemap(void *sky);

/// @brief Fresh Vec3 of the normalized sun direction (ADR 0233).
/// @param sky Sky3D handle; invalid handles trap.
/// @return Newly allocated direction snapshot; origin for an invalid handle.
void *rt_sky3d_get_sun_direction(void *sky);

/// @brief Fresh Vec3 of the retained ground albedo (ADR 0233).
/// @param sky Sky3D handle; invalid handles trap.
/// @return Newly allocated albedo snapshot; origin for an invalid handle.
void *rt_sky3d_get_ground_albedo(void *sky);

#ifdef __cplusplus
}
#endif
