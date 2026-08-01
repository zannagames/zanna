//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_lightbaker3d.h
// Purpose: Public C ABI for baked GI — LightBaker3D (deterministic CPU
//   lightmap baker over static scene geometry) and LightProbeGrid3D (SH-9
//   irradiance probe grid with trilinear sampling and .vlpg serialization).
// Key invariants:
//   - Bakes are deterministic (fixed seeds); BakeStep is chunked and
//     main-thread only.
//   - Bake settings and copied lights freeze at the first scene gather; atlas
//     and UV publication is failure-atomic.
// Ownership/Lifetime:
//   - Objects are GC-managed; object-returning accessors provide caller-owned references.
//   - Bakers retain their scene/atlas but copy explicit light state at registration.
// Links: rt_lightbaker3d.c, misc/plans/thirdpersonupgrade/14-baked-gi.md
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares deterministic baked-lightmap and SH probe-grid C APIs.
/// @details LightBaker3D incrementally bakes static Scene3D geometry into an
///   atlas and UV1 charts. LightProbeGrid3D reuses the same tracer to store and
///   interpolate directional irradiance or serialize its portable binary payload.

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LightBaker3D */
/// @brief Create a baker over @p scene (static-flagged mesh nodes participate).
/// @param scene Live Scene3D retained by the baker.
/// @return New GC-managed baker, or `NULL` for invalid input or allocation failure.
void *rt_lightbaker3d_new(void *scene);

/// @brief Lightmap texel density in texels per world unit (default 8).
/// @param baker LightBaker3D receiver.
/// @param texels Positive density capped at 64; invalid values and changes
///   after gathering starts are ignored.
void rt_lightbaker3d_set_texels_per_unit(void *baker, double texels);

/// @brief Return the configured lightmap texel density.
/// @param baker LightBaker3D receiver.
/// @return Texels per world unit, or zero for an invalid handle.
double rt_lightbaker3d_get_texels_per_unit(void *baker);

/// @brief Hemisphere samples per texel (default 64, clamped to 1024).
/// @param baker LightBaker3D receiver.
/// @param samples Positive sample count; nonpositive values and changes after
///   gathering starts are ignored.
void rt_lightbaker3d_set_samples(void *baker, int64_t samples);

/// @brief Return the configured per-texel sample count.
/// @param baker LightBaker3D receiver.
/// @return Positive sample count, or zero for an invalid handle.
int64_t rt_lightbaker3d_get_samples(void *baker);

/// @brief Indirect bounce count (default 2, clamped to 8).
/// @param baker LightBaker3D receiver.
/// @param bounces Nonnegative recursive depth; negative values and changes
///   after gathering starts are ignored.
void rt_lightbaker3d_set_bounces(void *baker, int64_t bounces);

/// @brief Return the configured indirect bounce depth.
/// @param baker LightBaker3D receiver.
/// @return Bounce count, or zero for an invalid handle.
int64_t rt_lightbaker3d_get_bounces(void *baker);

/// @brief Sky radiance for rays that escape the scene (default black).
/// @param baker LightBaker3D receiver.
/// @param r Nonnegative red radiance; invalid values become zero before a bake starts.
/// @param g Nonnegative green radiance; invalid values become zero before a bake starts.
/// @param b Nonnegative blue radiance; invalid values become zero before a bake starts.
void rt_lightbaker3d_set_sky_color(void *baker, double r, double g, double b);

/// @brief Bake progress in [0, 1].
/// @param baker LightBaker3D receiver.
/// @return Current normalized progress, or zero for an invalid handle.
double rt_lightbaker3d_get_progress(void *baker);

/// @brief Snapshot an explicit enabled non-ambient bake light before gathering.
/// @param baker LightBaker3D receiver.
/// @param light Light3D whose type, pose, radiance, decay/range, cone, and
///   shadow flag are copied, not retained; late inputs and inputs past the
///   fixed sixteen-light cap are ignored.
void rt_lightbaker3d_add_light(void *baker, void *light);

/// @brief Run one deterministic bake slice; returns 1 when the bake is complete.
/// @param baker LightBaker3D receiver.
/// @return Zero while more triangle slices remain; one after completion or a
///   terminal setup/allocation/capacity/source-change failure. Failures leave
///   Atlas null and publish no chart UVs.
int8_t rt_lightbaker3d_bake_step(void *baker);

/// @brief Atomically install the baked atlas on eligible nodes via material instances.
/// @param baker Completed LightBaker3D receiver; incomplete, atlas-less, and
///   repeated applications or changed source nodes are ignored.
void rt_lightbaker3d_apply(void *baker);

/// @brief Retained baked atlas Pixels (NULL before completion).
/// @param baker LightBaker3D receiver.
/// @return Retained Pixels handle that the caller owns, or `NULL` when unavailable.
void *rt_lightbaker3d_get_atlas(void *baker);

/* LightProbeGrid3D */
/// @brief Create a regular probe grid across [min, max] with @p spacing.
/// @param min_v Finite Vec3 minimum grid origin.
/// @param max_v Finite Vec3 maximum extent, component-wise at or above @p min_v.
/// @param spacing Positive world-unit step; invalid or tiny values default to one.
/// @return New GC-managed grid with `ceil(extent / spacing) + 1` probes per
///   axis clamped to two through sixty-four, or `NULL` for invalid bounds or
///   allocation failure.
void *rt_lightprobegrid3d_new(void *min_v, void *max_v, double spacing);

/// @brief Total probe count.
/// @param grid LightProbeGrid3D receiver.
/// @return Product of the three axis counts, or zero for invalid input.
int64_t rt_lightprobegrid3d_get_probe_count(void *grid);

/// @brief Bake probes against @p baker's scene/lights (shares its BVH + tracer).
/// @param grid LightProbeGrid3D receiver whose SH/validity payload is overwritten.
/// @param baker LightBaker3D over the same scene, providing deterministic settings.
void rt_lightprobegrid3d_bake(void *grid, void *baker);

/// @brief Validity-renormalized trilinear SH irradiance sample.
/// @param grid Baked or loaded LightProbeGrid3D receiver.
/// @param position Vec3 world-space position.
/// @param normal Optional Vec3 surface normal; `NULL` uses positive Y.
/// @return Newly allocated irradiance Vec3; unbaked grids, invalid positions,
///   or cells without a valid probe yield a new black Vec3. Invalid/zero
///   normals use positive Y.
void *rt_lightprobegrid3d_sample(void *grid, void *position, void *normal);

/// @brief Serialize a baked grid to the versioned IEEE little-endian `.vlpg` layout.
/// @param grid Baked LightProbeGrid3D receiver.
/// @param path Runtime string naming the output file.
/// @return Nonzero only when the complete file was written.
int8_t rt_lightprobegrid3d_save(void *grid, rt_string path);

/// @brief Strictly load an exact canonical .vlpg file transactionally.
/// @param grid LightProbeGrid3D receiver replaced transactionally after validation.
/// @param path Runtime string naming the input file.
/// @return Nonzero on a complete valid load; zero otherwise.
int8_t rt_lightprobegrid3d_load(void *grid, rt_string path);

#ifdef __cplusplus
}
#endif
