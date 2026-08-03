//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_reflectionprobe3d.h
// Purpose: Public C ABI for local reflection probes — 6-face scene capture into
//   a CubeMap3D with a box influence volume and scripted re-capture flagging.
// Key invariants:
//   - Capture is explicit/scripted; never runs per frame.
// Ownership/Lifetime:
//   - GC-managed; getters returning objects retain for the caller.
// Links: rt_reflectionprobe3d.c, ADR 0089
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the local ReflectionProbe3D capture and influence-volume C ABI.
/// @details Probes explicitly render six HDR faces into a retained CubeMap3D and expose bounded
///   capture settings plus a scripted dirtiness flag; object-returning getters transfer a retained
///   reference to the caller.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a probe at @p position with a parallax/influence box.
/// @param position Vec3 capture origin.
/// @param box_min Vec3 containing one proxy-box corner.
/// @param box_max Vec3 containing the opposite corner; axis ordering is normalized.
/// @return A new GC-managed ReflectionProbe3D, or `NULL` after trapping on invalid input or
///   allocation failure.
void *rt_reflectionprobe3d_new(void *position, void *box_min, void *box_max);

/// @brief Probe capture position as a fresh Vec3.
/// @param probe ReflectionProbe3D instance.
/// @return A new Vec3 containing the capture origin, or `NULL` for an invalid probe.
void *rt_reflectionprobe3d_get_position(void *probe);

/// @brief Influence multiplier over the proxy box (>= 1, default 1).
/// @param probe ReflectionProbe3D instance.
/// @param scale Finite scale from one through eight; larger values are clamped.
void rt_reflectionprobe3d_set_influence_scale(void *probe, double scale);

/// @brief Return the proxy-box influence multiplier.
/// @param probe ReflectionProbe3D instance.
/// @return The configured scale, or zero for an invalid probe.
double rt_reflectionprobe3d_get_influence_scale(void *probe);

/// @brief Capture face resolution (16..512, default 64); changes request recapture.
/// @param probe ReflectionProbe3D instance.
/// @param resolution Requested square face resolution, clamped from 16 through 512.
void rt_reflectionprobe3d_set_resolution(void *probe, int64_t resolution);

/// @brief Return the configured square cubemap face resolution.
/// @param probe ReflectionProbe3D instance.
/// @return The resolution in pixels, or zero for an invalid probe.
int64_t rt_reflectionprobe3d_get_resolution(void *probe);

/// @brief Scripted re-capture request flag (time-of-day hooks set it).
/// @param probe ReflectionProbe3D instance.
/// @param dirty Non-zero to request recapture, or zero to mark the capture current.
void rt_reflectionprobe3d_set_capture_dirty(void *probe, int8_t dirty);

/// @brief Return whether scripted systems have requested a recapture.
/// @param probe ReflectionProbe3D instance.
/// @return 1 when dirty, or 0 when current or invalid.
int8_t rt_reflectionprobe3d_get_capture_dirty(void *probe);

/// @brief True when @p position lies inside the influence-scaled box.
/// @param probe ReflectionProbe3D instance.
/// @param position Vec3 point tested against the inclusive expanded bounds.
/// @return 1 when the point lies inside the volume, or 0 otherwise.
int8_t rt_reflectionprobe3d_contains(void *probe, void *position);

/// @brief Retained captured CubeMap3D (NULL before the first capture).
/// @param probe ReflectionProbe3D instance.
/// @return The captured cubemap with a new retained reference, or `NULL` before capture.
void *rt_reflectionprobe3d_get_cubemap(void *probe);

/// @brief Render 6 faces of @p scene from the probe through @p canvas; 1 on success.
/// @param probe ReflectionProbe3D receiving the new capture.
/// @param canvas Canvas3D used for off-screen HDR rendering.
/// @param scene Scene3D rendered from each face orientation.
/// @return 1 after replacing the retained cubemap and clearing the dirty flag, or 0 on failure.
int8_t rt_reflectionprobe3d_capture(void *probe, void *canvas, void *scene);

#ifdef __cplusplus
}
#endif
