//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_water3d.h
// Purpose: Water plane with animated sine-based waves, transparency,
//   and optional tinted color. Rendered as a dynamic mesh updated each frame.
//
// Key invariants:
//   - Wave height = amplitude * sin(frequency * (x + z) - time * speed).
//   - Mesh regenerated each Update() call with new vertex positions.
//   - Transparent material (alpha blend) with configurable color.
//
// Links: rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_water3d.h
 * @brief Declares dynamic Water3D wave simulation and Canvas3D rendering.
 *
 * Water surfaces support a legacy sine wave or up to eight directional Gerstner waves, retained
 * texture/normal/environment maps, configurable tessellation, and camera-distance simulation
 * gating.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a water plane sized (@p width × @p depth) world units centered at the origin.
/// @param width Positive world-space extent along x.
/// @param depth Positive world-space extent along z.
/// @return New GC-managed Water3D handle, or NULL on allocation failure.
void *rt_water3d_new(double width, double depth);

/// @brief Set the world Y-coordinate of the water surface.
/// @param water Water3D handle; invalid handles are ignored.
/// @param y Finite base height, clamped to the supported world range.
void rt_water3d_set_height(void *water, double y);

/// @brief Set the water plane's world XZ centre and base Y (aligns the origin-built
///        grid over an off-origin terrain).
/// @param water Water3D handle; invalid handles are ignored.
/// @param x World-space center x coordinate.
/// @param y World-space base height.
/// @param z World-space center z coordinate.
void rt_water3d_set_position(void *water, double x, double y, double z);

/// @brief Configure the legacy single-sine-wave parameters (speed, amplitude, frequency).
/// @param water Water3D handle; invalid handles are ignored.
/// @param speed Signed temporal phase speed.
/// @param amplitude Nonnegative vertical displacement amplitude.
/// @param frequency Nonnegative spatial angular frequency.
void rt_water3d_set_wave_params(void *water, double speed, double amplitude, double frequency);

/// @brief Set the water's tint color and per-pixel alpha (0–1).
/// @param water Water3D handle; invalid handles are ignored.
/// @param r Red tint component, clamped to `[0, 1]`.
/// @param g Green tint component, clamped to `[0, 1]`.
/// @param b Blue tint component, clamped to `[0, 1]`.
/// @param alpha Surface alpha, clamped to `[0, 1]`.
void rt_water3d_set_color(void *water, double r, double g, double b, double alpha);

/// @brief Bind a Pixels surface texture (tiled across the water plane).
/// @param water Water3D handle; invalid handles are ignored.
/// @param pixels Pixels texture to retain, or NULL to clear the binding.
void rt_water3d_set_texture(void *water, void *pixels);

/// @brief Bind a tangent-space normal map for additional wave detail.
/// @param water Water3D handle; invalid handles are ignored.
/// @param pixels Pixels normal map to retain, or NULL to clear the binding.
void rt_water3d_set_normal_map(void *water, void *pixels);

/// @brief Bind a CubeMap3D for environment reflections on the water surface.
/// @param water Water3D handle; invalid handles are ignored.
/// @param cubemap Complete CubeMap3D handle to retain, or NULL to clear reflections.
void rt_water3d_set_env_map(void *water, void *cubemap);

/// @brief Set how strongly the env map reflects (0–1).
/// @param water Water3D handle; invalid handles are ignored.
/// @param r Reflectivity clamped to `[0, 1]`.
void rt_water3d_set_reflectivity(void *water, double r);

/// @brief Set the mesh tessellation resolution (clamped to [8, 256]).
/// @param water Water3D handle; invalid handles are ignored.
/// @param resolution Number of grid quads per axis.
void rt_water3d_set_resolution(void *water, int64_t resolution);

/// @brief Distance-gate the CPU wave rebuild around the last drawing camera (0 = off).
/// @param water Water3D handle; invalid handles are ignored.
/// @param distance Nonnegative camera-to-surface distance in world units.
void rt_water3d_set_sim_distance(void *water, double distance);

/// @brief Current simulation gate distance (0 = off).
/// @param water Water3D handle.
/// @return Configured nonnegative distance, or zero for an invalid handle.
double rt_water3d_get_sim_distance(void *water);

/// @brief Add a Gerstner wave (direction, speed, amplitude, wavelength). Up to 8 waves total.
/// @param water Water3D handle; invalid handles are ignored.
/// @param dirX Propagation-direction x component.
/// @param dirZ Propagation-direction z component.
/// @param speed Signed temporal phase speed.
/// @param amplitude Nonnegative vertical displacement amplitude.
/// @param wavelength Positive wavelength in world units.
void rt_water3d_add_wave(
    void *water, double dirX, double dirZ, double speed, double amplitude, double wavelength);

/// @brief Remove all Gerstner waves (reverts to the legacy single-sine wave).
/// @param water Water3D handle; invalid handles are ignored.
void rt_water3d_clear_waves(void *water);

/// @brief Tick the water clock by @p dt seconds and rebuild the deformed mesh.
/// @param water Water3D handle; invalid handles are ignored.
/// @param dt Nonnegative finite simulation delta, clamped to one second.
void rt_water3d_update(void *water, double dt);

/// @brief Render the water surface onto @p canvas (drawn with backface culling disabled).
/// @param canvas Canvas3D handle receiving the draw.
/// @param water Water3D handle supplying the current mesh and material.
/// @param camera Optional Camera3D handle used by distance-gated simulation.
void rt_canvas3d_draw_water(void *canvas, void *water, void *camera);

#ifdef __cplusplus
}
#endif
