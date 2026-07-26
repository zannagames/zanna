//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_3d_world_stubs.c
/// @brief Graphics-disabled navigation, path, instancing, atlas, and world
/// helper stubs.
///
/// @details This split source contains world-organization unavailable-backend
/// symbols that do not belong to Canvas3D, asset loading, physics, or media
/// stubs.
///
// File: src/runtime/graphics/common/rt_3d_world_stubs.c
// Purpose: Graphics-disabled 3D world, navigation, terrain, water, and vegetation entry points.
//
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail with the shared InvalidOperation trap.
//   - Backend-independent query helpers keep their documented fallback values.
//
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/* Graphics-disabled stub contract (intentional, and consistent across the 3D stub files):
 * constructors (*_new) return NULL silently rather than trapping, and every paired setter/getter
 * stub is NULL-tolerant (it ignores its handle), so disabled-build code such as
 *   t = Transform3D.New(); t.SetPosition(...)
 * no-ops safely instead of crashing. This differs deliberately from Canvas3D.New / Camera3D.New
 * (rt_canvas3d_stubs.c), which TRAP on a disabled build to surface an unsupported top-level
 * graphics entry point. Keep new stubs consistent with whichever contract their class follows. */

/* Transform3D stubs */

/// @brief Stub for `Transform3D.New` — would normally allocate an identity
///        TRS transform (zero translation, identity quaternion, unit scale).
///
/// Silent stub returning NULL.
///
/// @return `NULL`.
void *rt_transform3d_new(void) {
    return NULL;
}

/// @brief Stub for `Transform3D.SetPosition` — overwrite the translation
///        component of the transform.
///
/// Silent no-op stub.
///
/// @param x Transform3D handle (ignored).
/// @param a Position x (ignored).
/// @param b Position y (ignored).
/// @param c Position z (ignored).
void rt_transform3d_set_position(void *x, double a, double b, double c) {
    (void)x;
    (void)a;
    (void)b;
    (void)c;
}

/// @brief Stub for `Transform3D.Position` — get the translation component
///        as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param x Transform3D handle (ignored).
///
/// @return `NULL`.
void *rt_transform3d_get_position(void *x) {
    (void)x;
    return NULL;
}

/// @brief Stub for `Transform3D.SetRotation` — overwrite the rotation
///        component using a Quaternion handle.
///
/// Silent no-op stub. Use `SetEuler` for Euler-angle convenience.
///
/// @param x Transform3D handle (ignored).
/// @param q Quaternion handle (ignored).
void rt_transform3d_set_rotation(void *x, void *q) {
    (void)x;
    (void)q;
}

/// @brief Stub for `Transform3D.Rotation` — get the rotation component as
///        a Quaternion handle.
///
/// Silent stub returning NULL.
///
/// @param x Transform3D handle (ignored).
///
/// @return `NULL`.
void *rt_transform3d_get_rotation(void *x) {
    (void)x;
    return NULL;
}

/// @brief Stub for `Transform3D.SetEuler` — overwrite the rotation from
///        intrinsic XYZ Euler angles in radians (pitch, yaw, roll). The
///        real implementation converts to a quaternion internally.
///
/// Silent no-op stub.
///
/// @param x Transform3D handle (ignored).
/// @param p Pitch in radians (rotation around X) (ignored).
/// @param y Yaw in radians (rotation around Y) (ignored).
/// @param r Roll in radians (rotation around Z) (ignored).
void rt_transform3d_set_euler(void *x, double p, double y, double r) {
    (void)x;
    (void)p;
    (void)y;
    (void)r;
}

/// @brief Stub for `Transform3D.SetScale` — overwrite the per-axis scale
///        component.
///
/// Silent no-op stub. `(1, 1, 1)` is no-op (identity scale).
///
/// @param x Transform3D handle (ignored).
/// @param a Scale x (ignored).
/// @param b Scale y (ignored).
/// @param c Scale z (ignored).
void rt_transform3d_set_scale(void *x, double a, double b, double c) {
    (void)x;
    (void)a;
    (void)b;
    (void)c;
}

/// @brief Stub for `Transform3D.Scale` — get the scale component as a
///        Vec3.
///
/// Silent stub returning NULL.
///
/// @param x Transform3D handle (ignored).
///
/// @return `NULL`.
void *rt_transform3d_get_scale(void *x) {
    (void)x;
    return NULL;
}

/// @brief Stub for `Transform3D.Matrix` — get the composed 4x4
///        transformation matrix (TRS in column-major order).
///
/// Silent stub returning NULL.
///
/// @param x Transform3D handle (ignored).
///
/// @return `NULL`.
void *rt_transform3d_get_matrix(void *x) {
    (void)x;
    return NULL;
}

/// @brief Stub for `Transform3D.Translate` — additively translate by a
///        delta Vec3 (in world axes).
///
/// Silent no-op stub.
///
/// @param x Transform3D handle (ignored).
/// @param d Vec3 translation delta (ignored).
void rt_transform3d_translate(void *x, void *d) {
    (void)x;
    (void)d;
}

/// @brief Stub for `Transform3D.Rotate` — additively rotate by `ang`
///        radians around the axis Vec3 `a` (must be normalized).
///
/// Silent no-op stub.
///
/// @param x   Transform3D handle (ignored).
/// @param a   Vec3 rotation axis, normalized (ignored).
/// @param ang Rotation angle in radians (ignored).
void rt_transform3d_rotate(void *x, void *a, double ang) {
    (void)x;
    (void)a;
    (void)ang;
}

/// @brief Stub for `Transform3D.LookAt` — orient the transform so its
///        forward (-Z) axis points at world-space target `t`, with `u`
///        as the up reference.
///
/// Silent no-op stub.
///
/// @param x Transform3D handle (ignored).
/// @param t Vec3 look-at target (ignored).
/// @param u Vec3 up reference, typically `(0, 1, 0)` (ignored).
void rt_transform3d_look_at(void *x, void *t, void *u) {
    (void)x;
    (void)t;
    (void)u;
}

/* Path3D stubs */

/// @brief Stub for `Path3D.New` — would normally allocate an empty path
///        (waypoint list) ready to receive waypoints via `AddPoint`.
///
/// Silent stub returning NULL.
///
/// @return `NULL`.
void *rt_path3d_new(void) {
    return NULL;
}

/// @brief Stub for `Path3D.AddPoint` — append a waypoint to the path.
///
/// Silent no-op stub.
///
/// @param p Path3D handle (ignored).
/// @param v Vec3 waypoint position (ignored).
void rt_path3d_add_point(void *p, void *v) {
    (void)p;
    (void)v;
}

/// @brief Stub for `Path3D.PositionAt(t)` — sample the world-space
///        position along the path at parametric distance `t` (0 = start,
///        1 = end). Used by `PathFollower` and AI navigation.
///
/// Silent stub returning NULL.
///
/// @param p Path3D handle (ignored).
/// @param t Parametric distance, 0..1 (ignored).
///
/// @return `NULL`.
void *rt_path3d_get_position_at(void *p, double t) {
    (void)p;
    (void)t;
    return NULL;
}

/// @brief Stub for `Path3D.DirectionAt(t)` — sample the unit tangent
///        (forward direction) along the path at parametric distance `t`.
///
/// Silent stub returning NULL.
///
/// @param p Path3D handle (ignored).
/// @param t Parametric distance, 0..1 (ignored).
///
/// @return `NULL`.
void *rt_path3d_get_direction_at(void *p, double t) {
    (void)p;
    (void)t;
    return NULL;
}

/// @brief Stub for `Path3D.Length` — total arc length of the path,
///        computed by summing distances between consecutive waypoints.
///
/// Silent stub returning `0.0`.
///
/// @param p Path3D handle (ignored).
///
/// @return `0.0`.
double rt_path3d_get_length(void *p) {
    (void)p;
    return 0.0;
}

/// @brief Stub for `Path3D.PointCount` — number of waypoints in the path.
///
/// Silent stub returning `0`.
///
/// @param p Path3D handle (ignored).
///
/// @return `0`.
int64_t rt_path3d_get_point_count(void *p) {
    (void)p;
    return 0;
}

/// @brief Stub for `Path3D.SetLooping` — when enabled, sampling at
///        `t > 1.0` wraps around to the start of the path.
///
/// Silent no-op stub.
///
/// @param p Path3D handle (ignored).
/// @param l Non-zero to enable looping (ignored).
void rt_path3d_set_looping(void *p, int8_t l) {
    (void)p;
    (void)l;
}

/// @brief Remove all entries from the path3d.
///
/// @param p Path3D handle (ignored).
void rt_path3d_clear(void *p) {
    (void)p;
}

/* Terrain3D stubs */

/// @brief Stub for `Terrain3D.New` — would normally allocate a `(w x d)`
///        terrain grid. Subsequent calls to `SetHeightmap` /
///        `GeneratePerlin` populate the heights; without those, the
///        terrain is flat at y=0.
///
/// Trapping stub: terrain is referenced by Canvas3D draw calls and
/// vegetation systems — a NULL return would crash later.
///
/// @param w Grid width in vertices (ignored).
/// @param d Grid depth in vertices (ignored).
///
/// @return Never returns normally.
void *rt_terrain3d_new(int64_t w, int64_t d) {
    (void)w;
    (void)d;
    rt_graphics_unavailable_("Terrain3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Terrain3D.GeneratePerlin` — would normally fill the
///        heightmap from a PerlinNoise object with the given scale,
///        octave count, and persistence (per-octave amplitude falloff).
///        Native fast path that bypasses the Pixels intermediate.
///
/// Silent no-op stub.
///
/// @param t  Terrain3D handle (ignored).
/// @param p  PerlinNoise handle (ignored).
/// @param s  World-space scale of the noise lookup (ignored).
/// @param o  Octave count (ignored).
/// @param pe Per-octave persistence, 0..1 (ignored).
void rt_terrain3d_generate_perlin(void *t, void *p, double s, int64_t o, double pe) {
    (void)t;
    (void)p;
    (void)s;
    (void)o;
    (void)pe;
}

/// @brief Stub for `Terrain3D.SetLODDistances` — configure the near/far
///        distance thresholds for terrain chunk LOD switching. Chunks
///        within `n` use full resolution; chunks beyond `f` use the
///        coarsest resolution; chunks in between scale linearly.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param n Near LOD distance (ignored).
/// @param f Far LOD distance (ignored).
void rt_terrain3d_set_lod_distances(void *t, double n, double f) {
    (void)t;
    (void)n;
    (void)f;
}

/// @brief Stub for `Terrain3D.SetLODHysteresis` — would normally configure the stable distance
///        band used to prevent chunk LOD threshold flicker.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param d Hysteresis distance in world units (ignored).
void rt_terrain3d_set_lod_hysteresis(void *t, double d) {
    (void)t;
    (void)d;
}

/// @brief Stub for `Terrain3D.SetSkirtDepth` — height of the downward-
///        facing skirt triangles inserted at chunk edges to hide
///        T-junction cracks at LOD boundaries.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param d Skirt depth in world units below the lowest edge vertex (ignored).
void rt_terrain3d_set_skirt_depth(void *t, double d) {
    (void)t;
    (void)d;
}

/// @brief Stub for `Terrain3D.SetCpuOcclusion` — opt-in terrain CPU occlusion participation.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param enabled Requested state (ignored).
void rt_terrain3d_set_cpu_occlusion(void *t, int8_t enabled) {
    (void)t;
    (void)enabled;
}

/// @brief Stub for `Terrain3D.CpuOcclusion` — returns whether CPU occlusion is enabled.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`.
int8_t rt_terrain3d_get_cpu_occlusion(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no chunks were considered.
int64_t rt_terrain3d_get_last_chunk_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no chunks were drawn.
int64_t rt_terrain3d_get_last_drawn_chunk_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no chunks were rejected by the camera frustum.
int64_t rt_terrain3d_get_last_frustum_culled_chunk_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no chunk requested an unavailable LOD mesh.
int64_t rt_terrain3d_get_last_missing_lod_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no chunk LOD selection was clamped.
int64_t rt_terrain3d_get_last_lod_clamped_chunk_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no LOD-zero chunks were drawn.
int64_t rt_terrain3d_get_last_lod0_chunk_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no LOD-one chunks were drawn.
int64_t rt_terrain3d_get_last_lod1_chunk_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for Terrain3D draw diagnostics; returns `0`.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`, because no LOD-two chunks were drawn.
int64_t rt_terrain3d_get_last_lod2_chunk_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Stub for `Terrain3D.SetHeightmap` — would normally upload a
///        Pixels surface as the source heightmap. R+G channels combine
///        for 16-bit precision; the heightmap is sampled per vertex.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param p Pixels handle for the heightmap (ignored).
void rt_terrain3d_set_heightmap(void *t, void *p) {
    (void)t;
    (void)p;
}

/// @brief Stub for `Terrain3D.SetMaterial` — bind a Material3D used
///        when rendering the terrain. Often paired with a splat-map for
///        per-pixel layer blending.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param m Material3D handle (ignored).
void rt_terrain3d_set_material(void *t, void *m) {
    (void)t;
    (void)m;
}

/// @brief Stub for `Terrain3D.SetScale` — world-space scale factors
///        along each axis. `sy` controls the height range; `sx` and `sz`
///        control the planar footprint.
///
/// Silent no-op stub.
///
/// @param t  Terrain3D handle (ignored).
/// @param sx Scale along X (ignored).
/// @param sy Scale along Y (ignored).
/// @param sz Scale along Z (ignored).
void rt_terrain3d_set_scale(void *t, double sx, double sy, double sz) {
    (void)t;
    (void)sx;
    (void)sy;
    (void)sz;
}

/// @brief Stub for `Terrain3D.SetSplatMap` — bind a 4-channel RGBA
///        Pixels surface that controls per-pixel blending between up to
///        4 layer textures. R = layer 0 weight, G = layer 1, etc.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param p Pixels handle for the splat map (ignored).
void rt_terrain3d_set_splat_map(void *t, void *p) {
    (void)t;
    (void)p;
}

/// @brief Stub for `Terrain3D.SetSplatMapAt` — indexed splat map bind. Silent no-op stub.
///
/// @param t     Terrain3D handle (ignored).
/// @param index Splat-map index (ignored).
/// @param p     Pixels handle containing four layer weights (ignored).
void rt_terrain3d_set_splat_map_at(void *t, int64_t index, void *p) {
    (void)t;
    (void)index;
    (void)p;
}

/// @brief Silent stub for `Terrain3D.SetHole` — no-op; returns -1 (no hole).
///
/// @param t     Terrain3D handle (ignored).
/// @param x     World-space hole center or origin along X (ignored).
/// @param z     World-space hole center or origin along Z (ignored).
/// @param width Hole width along X (ignored).
/// @param depth Hole depth along Z (ignored).
///
/// @return `-1`, the invalid hole index.
int64_t rt_terrain3d_set_hole(void *t, double x, double z, double width, double depth) {
    (void)t;
    (void)x;
    (void)z;
    (void)width;
    (void)depth;
    return -1;
}

/// @brief Silent stub for `Terrain3D.RemoveHole` — no-op; returns 0.
///
/// @param t     Terrain3D handle (ignored).
/// @param index Hole index to remove (ignored).
///
/// @return `0`, indicating that no hole was removed.
int8_t rt_terrain3d_remove_hole(void *t, int64_t index) {
    (void)t;
    (void)index;
    return 0;
}

/// @brief Stub for `Terrain3D.ClearHoles`. Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
void rt_terrain3d_clear_holes(void *t) {
    (void)t;
}

/// @brief Silent stub for `Terrain3D.get_HoleCount` — no-op; returns 0.
///
/// @param t Terrain3D handle (ignored).
///
/// @return `0`.
int64_t rt_terrain3d_get_hole_count(void *t) {
    (void)t;
    return 0;
}

/// @brief Silent stub for the internal hole-mask handoff — no-op; returns NULL.
///
/// Initializes each supplied cell-count output to zero.
///
/// @param t           Terrain3D handle (ignored).
/// @param out_cells_x Optional output receiving the mask width in cells.
/// @param out_cells_z Optional output receiving the mask depth in cells.
///
/// @return `NULL`.
const uint8_t *rt_terrain3d_get_hole_mask_raw(void *t, int32_t *out_cells_x, int32_t *out_cells_z) {
    (void)t;
    if (out_cells_x)
        *out_cells_x = 0;
    if (out_cells_z)
        *out_cells_z = 0;
    return 0;
}

/// @brief Stub for `Terrain3D.SetSlopeLayer`. Silent no-op stub.
///
/// @param t             Terrain3D handle (ignored).
/// @param layer         Material-layer index (ignored).
/// @param min_slope_deg Lower slope angle in degrees (ignored).
/// @param max_slope_deg Upper slope angle in degrees (ignored).
/// @param sharpness     Transition sharpness at the range boundaries (ignored).
void rt_terrain3d_set_slope_layer(
    void *t, int64_t layer, double min_slope_deg, double max_slope_deg, double sharpness) {
    (void)t;
    (void)layer;
    (void)min_slope_deg;
    (void)max_slope_deg;
    (void)sharpness;
}

/// @brief Stub for `Terrain3D.SetHeightLayer`. Silent no-op stub.
///
/// @param t         Terrain3D handle (ignored).
/// @param layer     Material-layer index (ignored).
/// @param min_y     Minimum world-space height for the layer (ignored).
/// @param max_y     Maximum world-space height for the layer (ignored).
/// @param sharpness Transition sharpness at the range boundaries (ignored).
void rt_terrain3d_set_height_layer(
    void *t, int64_t layer, double min_y, double max_y, double sharpness) {
    (void)t;
    (void)layer;
    (void)min_y;
    (void)max_y;
    (void)sharpness;
}

/// @brief Stub for `Terrain3D.RebuildSplatWeights`. Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
void rt_terrain3d_rebuild_splat_weights(void *t) {
    (void)t;
}

/// @brief Silent stub for the internal splat-map inspection hook — no-op; returns NULL.
///
/// @param t     Terrain3D handle (ignored).
/// @param index Splat-map index (ignored).
///
/// @return `NULL`.
void *rt_terrain3d_get_splat_map_raw(void *t, int64_t index) {
    (void)t;
    (void)index;
    return 0;
}

/// @brief Stub for `Terrain3D.SetLayerTexture` — bind a Pixels texture
///        as one of the 4 splat-map layers (`l = 0..3`). Each layer is
///        sampled independently with its own UV scale.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param l Layer index 0..3 (ignored).
/// @param p Pixels handle for the layer texture (ignored).
void rt_terrain3d_set_layer_texture(void *t, int64_t l, void *p) {
    (void)t;
    (void)l;
    (void)p;
}

/// @brief Stub for `Terrain3D.SetLayerScale` — UV tiling scale for a
///        splat layer. Higher = the layer texture tiles more times across
///        the terrain footprint.
///
/// Silent no-op stub.
///
/// @param t Terrain3D handle (ignored).
/// @param l Layer index 0..3 (ignored).
/// @param s UV tiling factor (ignored).
void rt_terrain3d_set_layer_scale(void *t, int64_t l, double s) {
    (void)t;
    (void)l;
    (void)s;
}

/// @brief Stub for `Terrain3D.HeightAt` — sample the terrain surface
///        height at world-space `(x, z)`. Bilinear interpolation between
///        the 4 nearest grid vertices.
///
/// Silent stub returning `0.0` (flat ground default).
///
/// @param t Terrain3D handle (ignored).
/// @param x World-space X (ignored).
/// @param z World-space Z (ignored).
///
/// @return `0.0`.
double rt_terrain3d_get_height_at(void *t, double x, double z) {
    (void)t;
    (void)x;
    (void)z;
    return 0.0;
}

/// @brief Stub for `Terrain3D.NormalAt` — sample the terrain surface
///        normal at world-space `(x, z)` as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param t Terrain3D handle (ignored).
/// @param x World-space X (ignored).
/// @param z World-space Z (ignored).
///
/// @return `NULL`.
void *rt_terrain3d_get_normal_at(void *t, double x, double z) {
    (void)t;
    (void)x;
    (void)z;
    return NULL;
}

/* NavMesh3D stubs */

/// @brief Stub for `NavMesh3D.Build` — would normally bake a navmesh
///        from the given Mesh3D, accounting for the agent's collision
///        cylinder (radius `r`, height `h`) so all walkable polygons can
///        actually fit the agent.
///
/// Silent stub returning NULL.
///
/// @param m Source Mesh3D representing the world geometry (ignored).
/// @param r Agent collision radius (ignored).
/// @param h Agent collision height (ignored).
///
/// @return `NULL`.
void *rt_navmesh3d_build(void *m, double r, double h) {
    (void)m;
    (void)r;
    (void)h;
    return NULL;
}

/// @brief Stub for `NavMesh3D.Bake` — would normally gather Scene3D mesh geometry.
///
/// Silent stub returning NULL.
///
/// @param s     Scene3D containing source geometry (ignored).
/// @param r     Agent radius (ignored).
/// @param h     Agent height (ignored).
/// @param slope Maximum walkable slope (ignored).
/// @param cell  Navmesh rasterization cell size (ignored).
///
/// @return `NULL`.
void *rt_navmesh3d_bake(void *s, double r, double h, double slope, double cell) {
    (void)s;
    (void)r;
    (void)h;
    (void)slope;
    (void)cell;
    return NULL;
}

/// @brief Stub for `NavMesh3D.BakeTiled` — tiled scene bake entry point.
///
/// Silent stub returning NULL.
///
/// @param s     Scene3D containing source geometry (ignored).
/// @param tile  Tile edge length in world units (ignored).
/// @param r     Agent radius (ignored).
/// @param h     Agent height (ignored).
/// @param slope Maximum walkable slope (ignored).
/// @param cell  Navmesh rasterization cell size (ignored).
///
/// @return `NULL`.
void *rt_navmesh3d_bake_tiled(void *s, double tile, double r, double h, double slope, double cell) {
    (void)s;
    (void)tile;
    (void)r;
    (void)h;
    (void)slope;
    (void)cell;
    return NULL;
}

/// @brief Stub for `NavMesh3D.FindPath` — would normally run an A*
///        path query from world position `f` to world position `t`,
///        returning a Path3D-like object representing the corridor.
///
/// Silent stub returning NULL.
///
/// @param n NavMesh3D handle (ignored).
/// @param f Vec3 path start (ignored).
/// @param t Vec3 path target (ignored).
///
/// @return `NULL`.
void *rt_navmesh3d_find_path(void *n, void *f, void *t) {
    (void)n;
    (void)f;
    (void)t;
    return NULL;
}

/// @brief Stub for `NavMesh3D.FindPathOption` — A* path lookup as Option.
/// @details Graphics-disabled builds have no baked navmesh to query, so the
///          modern absence-aware API returns `None`.
/// @param n NavMesh3D handle (ignored).
/// @param f Vec3 path start (ignored).
/// @param t Vec3 path target (ignored).
/// @return `None`.
void *rt_navmesh3d_find_path_option(void *n, void *f, void *t) {
    (void)n;
    (void)f;
    (void)t;
    return rt_option_none();
}

/// @brief Stub for `NavMesh3D.SamplePosition` — would normally project
///        an arbitrary world-space point onto the nearest walkable navmesh
///        polygon, returning the snapped position as a Vec3 (or NULL when
///        no polygon is within the search radius).
///
/// Silent stub returning NULL.
///
/// @param n NavMesh3D handle (ignored).
/// @param p Vec3 query position (ignored).
///
/// @return `NULL`.
void *rt_navmesh3d_sample_position(void *n, void *p) {
    (void)n;
    (void)p;
    return NULL;
}

/// @brief Stub for `NavMesh3D.IsWalkable` — boolean test for whether the
///        given position lies on a walkable polygon (within tolerance).
///
/// Silent stub returning `0`.
///
/// @param n NavMesh3D handle (ignored).
/// @param p Vec3 query position (ignored).
///
/// @return `0`.
int8_t rt_navmesh3d_is_walkable(void *n, void *p) {
    (void)n;
    (void)p;
    return 0;
}

/// @brief Stub for `NavMesh3D.TriangleCount` — number of walkable
///        triangles in the baked navmesh.
///
/// Silent stub returning `0`.
///
/// @param n NavMesh3D handle (ignored).
///
/// @return `0`.
int64_t rt_navmesh3d_get_triangle_count(void *n) {
    (void)n;
    return 0;
}

/// @brief Return the fallback cost of the most recent absent navmesh path.
///
/// @param n NavMesh3D handle (ignored).
///
/// @return `0.0`.
double rt_navmesh3d_get_last_path_cost(void *n) {
    (void)n;
    return 0.0;
}

/// @brief Stub for `NavMesh3D.AddOffMeshLink` — would normally add an
///        authored traversal edge such as a jump, ladder, or drop-down between
///        two walkable navmesh points.
///
/// Silent stub returning `0`.
///
/// @param n NavMesh3D handle (ignored).
/// @param f Vec3 link start (ignored).
/// @param t Vec3 link end (ignored).
/// @param b Whether traversal is bidirectional (ignored).
///
/// @return `0`.
int8_t rt_navmesh3d_add_offmesh_link(void *n, void *f, void *t, int8_t b) {
    (void)n;
    (void)f;
    (void)t;
    (void)b;
    return 0;
}

/// @brief Stub for `NavMesh3D.OffMeshLinkCount` — number of authored traversal links.
///
/// Silent stub returning `0`.
///
/// @param n NavMesh3D handle (ignored).
///
/// @return `0`.
int64_t rt_navmesh3d_get_offmesh_link_count(void *n) {
    (void)n;
    return 0;
}

/// @brief Report that off-mesh link metadata was not updated.
///
/// @param n              NavMesh3D handle (ignored).
/// @param index          Off-mesh link index (ignored).
/// @param kind           Traversal-kind name, such as jump or ladder (ignored).
/// @param traversal_cost Authored traversal cost (ignored).
/// @param state_flags    Application-defined availability flags (ignored).
///
/// @return `0`.
int8_t rt_navmesh3d_set_offmesh_link_metadata(
    void *n, int64_t index, rt_string kind, double traversal_cost, int64_t state_flags) {
    (void)n;
    (void)index;
    (void)kind;
    (void)traversal_cost;
    (void)state_flags;
    return 0;
}

/// @brief Return the fallback traversal kind for an unavailable off-mesh link.
///
/// @param n     NavMesh3D handle (ignored).
/// @param index Off-mesh link index (ignored).
///
/// @return An empty owned runtime string.
rt_string rt_navmesh3d_get_offmesh_link_kind(void *n, int64_t index) {
    (void)n;
    (void)index;
    return rt_string_from_bytes("", 0);
}

/// @brief Return the fallback traversal cost for an unavailable off-mesh link.
///
/// @param n     NavMesh3D handle (ignored).
/// @param index Off-mesh link index (ignored).
///
/// @return `0.0`.
double rt_navmesh3d_get_offmesh_link_traversal_cost(void *n, int64_t index) {
    (void)n;
    (void)index;
    return 0.0;
}

/// @brief Return the fallback state flags for an unavailable off-mesh link.
///
/// @param n     NavMesh3D handle (ignored).
/// @param index Off-mesh link index (ignored).
///
/// @return `0`.
int64_t rt_navmesh3d_get_offmesh_link_state(void *n, int64_t index) {
    (void)n;
    (void)index;
    return 0;
}

/// @brief Stub for `NavMesh3D.AddObstacle` — would normally add an AABB carving obstacle.
///
/// Silent stub returning `0`.
///
/// @param n   NavMesh3D handle (ignored).
/// @param min Vec3 obstacle minimum corner (ignored).
/// @param max Vec3 obstacle maximum corner (ignored).
///
/// @return `0`, indicating that no obstacle was added.
int8_t rt_navmesh3d_add_obstacle(void *n, void *min, void *max) {
    (void)n;
    (void)min;
    (void)max;
    return 0;
}

/// @brief Stub for `NavMesh3D.RemoveObstacle` — would normally remove an authored obstacle.
///
/// Silent stub returning `0`.
///
/// @param n     NavMesh3D handle (ignored).
/// @param index Obstacle index (ignored).
///
/// @return `0`, indicating that no obstacle was removed.
int8_t rt_navmesh3d_remove_obstacle(void *n, int64_t index) {
    (void)n;
    (void)index;
    return 0;
}

/// @brief Stub for `NavMesh3D.UpdateObstacle` — would normally edit an authored obstacle.
///
/// Silent stub returning `0`.
///
/// @param n     NavMesh3D handle (ignored).
/// @param index Obstacle index (ignored).
/// @param min   Vec3 updated minimum corner (ignored).
/// @param max   Vec3 updated maximum corner (ignored).
///
/// @return `0`, indicating that no obstacle was updated.
int8_t rt_navmesh3d_update_obstacle(void *n, int64_t index, void *min, void *max) {
    (void)n;
    (void)index;
    (void)min;
    (void)max;
    return 0;
}

/// @brief Stub for `NavMesh3D.ObstacleCount` — number of authored coarse obstacles.
///
/// Silent stub returning `0`.
///
/// @param n NavMesh3D handle (ignored).
///
/// @return `0`.
int64_t rt_navmesh3d_get_obstacle_count(void *n) {
    (void)n;
    return 0;
}

/// @brief Report that a named navigation area was not assigned to an AABB.
///
/// @param n    NavMesh3D handle (ignored).
/// @param min  Vec3 area minimum corner (ignored).
/// @param max  Vec3 area maximum corner (ignored).
/// @param area Area-kind name (ignored).
/// @param cost Traversal-cost multiplier (ignored).
///
/// @return `0`.
int8_t rt_navmesh3d_set_area(void *n, void *min, void *max, rt_string area, double cost) {
    (void)n;
    (void)min;
    (void)max;
    (void)area;
    (void)cost;
    return 0;
}

/// @brief Return the fallback area kind at a navmesh point.
///
/// @param n     NavMesh3D handle (ignored).
/// @param point Vec3 world-space query point (ignored).
///
/// @return An empty owned runtime string.
rt_string rt_navmesh3d_get_area(void *n, void *point) {
    (void)n;
    (void)point;
    return rt_string_from_bytes("", 0);
}

/// @brief Return the fallback traversal cost at a navmesh point.
///
/// @param n     NavMesh3D handle (ignored).
/// @param point Vec3 world-space query point (ignored).
///
/// @return `0.0`.
double rt_navmesh3d_get_traversal_cost(void *n, void *point) {
    (void)n;
    (void)point;
    return 0.0;
}

/// @brief Stub for `NavMesh3D.RebuildTile` — tile rebuild entry point.
///
/// Silent stub returning `0`.
///
/// @param n      NavMesh3D handle (ignored).
/// @param tile_x Tile coordinate along X (ignored).
/// @param tile_z Tile coordinate along Z (ignored).
///
/// @return `0`, indicating that no tile was rebuilt.
int8_t rt_navmesh3d_rebuild_tile(void *n, int64_t tile_x, int64_t tile_z) {
    (void)n;
    (void)tile_x;
    (void)tile_z;
    return 0;
}

/// @brief Stub for `NavMesh3D.SetMaxSlope` — maximum slope (in radians)
///        a triangle can have and still be considered walkable. Steeper
///        triangles are excluded from the navmesh during bake.
///
/// Silent no-op stub.
///
/// @param n NavMesh3D handle (ignored).
/// @param d Max slope in radians (ignored).
void rt_navmesh3d_set_max_slope(void *n, double d) {
    (void)n;
    (void)d;
}

/// @brief Stub for `NavMesh3D.DebugDraw` — would normally render the
///        navmesh as a wireframe overlay on the given Canvas3D for visual
///        debugging.
///
/// Silent no-op stub.
///
/// @param n NavMesh3D handle (ignored).
/// @param c Canvas3D handle (ignored).
void rt_navmesh3d_debug_draw(void *n, void *c) {
    (void)n;
    (void)c;
}

/// @brief Stub for `NavMesh3D.CopyPathPoints` — internal helper for
///        path-finding that copies the smoothed path from `f` to `t` into
///        a freshly malloc'd `(x, y, z)` array. Returns waypoint count.
///
/// Silent stub: writes NULL to `*out_points_xyz` and returns `0` (no path).
///
/// @param n              NavMesh3D handle (ignored).
/// @param f              Vec3 path start (ignored).
/// @param t              Vec3 path target (ignored).
/// @param out_points_xyz Out-param receiving a malloc'd array; set to NULL.
///
/// @return `0`.
int64_t rt_navmesh3d_copy_path_points(void *n, void *f, void *t, double **out_points_xyz) {
    (void)n;
    (void)f;
    (void)t;
    if (out_points_xyz)
        *out_points_xyz = NULL;
    return 0;
}

/* NavAgent3D stubs */

/// @brief Stub for `NavAgent3D.New` — would normally create an
///        autonomous pathfinding agent with the given collision cylinder
///        (radius `r`, height `h`) bound to the given NavMesh3D.
///
/// Silent stub returning NULL.
///
/// @param n NavMesh3D handle (ignored).
/// @param r Agent collision radius in world units (ignored).
/// @param h Agent collision height in world units (ignored).
///
/// @return `NULL`.
void *rt_navagent3d_new(void *n, double r, double h) {
    (void)n;
    (void)r;
    (void)h;
    return NULL;
}

/// @brief Stub for `NavAgent3D.SetTarget` — request a new path to the
///        given world-space destination. Triggers an A* query at the next
///        `Update` tick.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param p Vec3 destination (ignored).
void rt_navagent3d_set_target(void *a, void *p) {
    (void)a;
    (void)p;
}

/// @brief Stub for `NavAgent3D.ClearTarget` — abandon the current
///        destination. The agent stops at its current position.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
void rt_navagent3d_clear_target(void *a) {
    (void)a;
}

/// @brief Stub for `NavAgent3D.Update` — advance the agent by `dt`
///        seconds: re-evaluate steering toward the next corridor waypoint,
///        integrate motion, and detect arrival at the destination.
///
/// Silent no-op stub.
///
/// @param a  NavAgent3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_navagent3d_update(void *a, double dt) {
    (void)a;
    (void)dt;
}

/// @brief Graphics-disabled silent fallback for deterministic NavAgent3D batch updates.
/// @details No navigation state exists without graphics support, so the borrowed handle array and
///   delta time are ignored and no entries are reported as processed.
/// @param agents Borrowed array of NavAgent3D handles (ignored).
/// @param agent_count Number of entries in @p agents (ignored).
/// @param dt Tick duration in seconds (ignored).
/// @return Always zero.
int64_t rt_navagent3d_update_batch(void *const *agents, int64_t agent_count, double dt) {
    (void)agents;
    (void)agent_count;
    (void)dt;
    return 0;
}

/// @brief Stub for `NavAgent3D.Warp` — teleport the agent to `p` without
///        running steering / collision response. Use for spawn / respawn.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param p Vec3 destination (ignored).
void rt_navagent3d_warp(void *a, void *p) {
    (void)a;
    (void)p;
}

/// @brief Stub for `NavAgent3D.Position` — get the agent's current
///        world-space position as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `NULL`.
void *rt_navagent3d_get_position(void *a) {
    (void)a;
    return NULL;
}

/// @brief Stub for `NavAgent3D.Velocity` — get the agent's current
///        velocity vector as a Vec3 (post-steering, post-clamp).
///
/// Silent stub returning NULL.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `NULL`.
void *rt_navagent3d_get_velocity(void *a) {
    (void)a;
    return NULL;
}

/// @brief Stub for `NavAgent3D.DesiredVelocity` — get the velocity the
///        steering controller wants this tick, before clamping by max-speed
///        / collision response. Useful for animation blending.
///
/// Silent stub returning NULL.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `NULL`.
void *rt_navagent3d_get_desired_velocity(void *a) {
    (void)a;
    return NULL;
}

/// @brief Stub for `NavAgent3D.HasPath` — true while the agent has a
///        valid path to its target and is steering along it.
///
/// Silent stub returning `0`.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `0`.
int8_t rt_navagent3d_get_has_path(void *a) {
    (void)a;
    return 0;
}

/// @brief Stub for `NavAgent3D.RemainingDistance` — distance along the
///        current corridor from the agent's position to the target.
///
/// Silent stub returning `0.0`.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `0.0`.
double rt_navagent3d_get_remaining_distance(void *a) {
    (void)a;
    return 0.0;
}

/// @brief Stub for `NavAgent3D.StoppingDistance` — get the radius around
///        the destination at which the agent declares itself "arrived"
///        and stops steering.
///
/// Silent stub returning `0.0`.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `0.0`.
double rt_navagent3d_get_stopping_distance(void *a) {
    (void)a;
    return 0.0;
}

/// @brief Stub for `NavAgent3D.SetStoppingDistance` — adjust the arrival
///        radius.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param d Stopping distance in world units (ignored).
void rt_navagent3d_set_stopping_distance(void *a, double d) {
    (void)a;
    (void)d;
}

/// @brief Stub for `NavAgent3D.DesiredSpeed` — get the agent's
///        nominal-cruise speed in world units per second. Steering targets
///        this magnitude when far from the destination.
///
/// Silent stub returning `0.0`.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `0.0`.
double rt_navagent3d_get_desired_speed(void *a) {
    (void)a;
    return 0.0;
}

/// @brief Stub for `NavAgent3D.SetDesiredSpeed` — adjust the nominal
///        cruise speed.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param s Speed in world units per second (ignored).
void rt_navagent3d_set_desired_speed(void *a, double s) {
    (void)a;
    (void)s;
}

/// @brief Stub for `NavAgent3D.AutoRepath` — get the auto-repath flag.
///        When enabled, the agent re-runs A* if the current path becomes
///        invalid (e.g. dynamic obstacle blocked it).
///
/// Silent stub returning `0`.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `0`.
int8_t rt_navagent3d_get_auto_repath(void *a) {
    (void)a;
    return 0;
}

/// @brief Stub for `NavAgent3D.SetAutoRepath` — enable or disable
///        automatic path recomputation on path invalidation.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param e Non-zero to enable auto-repath (ignored).
void rt_navagent3d_set_auto_repath(void *a, int8_t e) {
    (void)a;
    (void)e;
}

/// @brief Stub for `NavAgent3D.AvoidanceEnabled` — get the local
///        same-NavMesh separation steering flag.
///
/// Silent stub returning `0`.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `0`.
int8_t rt_navagent3d_get_avoidance_enabled(void *a) {
    (void)a;
    return 0;
}

/// @brief Stub for `NavAgent3D.SetAvoidanceEnabled` — enable or disable
///        local same-NavMesh separation steering.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param e Non-zero to enable avoidance (ignored).
void rt_navagent3d_set_avoidance_enabled(void *a, int8_t e) {
    (void)a;
    (void)e;
}

/// @brief Stub for `NavAgent3D.AvoidanceRadius` — get the local
///        separation radius.
///
/// Silent stub returning `0.0`.
///
/// @param a NavAgent3D handle (ignored).
///
/// @return `0.0`.
double rt_navagent3d_get_avoidance_radius(void *a) {
    (void)a;
    return 0.0;
}

/// @brief Stub for `NavAgent3D.SetAvoidanceRadius` — adjust the local
///        separation radius.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param r Radius in world units (ignored).
void rt_navagent3d_set_avoidance_radius(void *a, double r) {
    (void)a;
    (void)r;
}

/// @brief Stub for `NavAgent3D.BindCharacter` — attach a CharacterController
///        so the agent's steering output drives the character's locomotion
///        (recommended for production over raw `Update` polling).
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param c CharacterController3D handle (ignored).
void rt_navagent3d_bind_character(void *a, void *c) {
    (void)a;
    (void)c;
}

/// @brief Stub for `NavAgent3D.BindNode` — attach a SceneNode3D so the
///        agent's position drives the node's transform each frame.
///        Convenience binding that avoids manual `Position`/`SetPosition`
///        calls every tick.
///
/// Silent no-op stub.
///
/// @param a NavAgent3D handle (ignored).
/// @param n SceneNode3D handle, or NULL to detach (ignored).
void rt_navagent3d_bind_node(void *a, void *n) {
    (void)a;
    (void)n;
}

/* Water3D stubs */

/// @brief Stub for `Water3D.New` — would normally create a horizontal
///        water plane of the given world dimensions.
///
/// Silent stub returning NULL.
///
/// @param w Width along X in world units (ignored).
/// @param d Depth along Z in world units (ignored).
///
/// @return `NULL`.
void *rt_water3d_new(double w, double d) {
    (void)w;
    (void)d;
    return NULL;
}

/// @brief Stub for `Water3D.SetHeight` — set the water plane's Y position
///        (world-space sea level).
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param y World-space Y for the water surface (ignored).
void rt_water3d_set_height(void *w, double y) {
    (void)w;
    (void)y;
}

/// @brief Stub for `Water3D.SetPosition` — set the water plane's world XZ
///        centre and base Y (aligns the origin-built grid over an off-origin
///        area).
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param x World-space X centre (ignored).
/// @param y World-space Y for the surface (ignored).
/// @param z World-space Z centre (ignored).
void rt_water3d_set_position(void *w, double x, double y, double z) {
    (void)w;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `Water3D.SetWaveParams` — legacy single-wave control.
///        Use `Water3D.AddWave` for the modern Gerstner multi-wave system.
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param s Wave speed (ignored).
/// @param a Wave amplitude (ignored).
/// @param f Wave frequency (ignored).
void rt_water3d_set_wave_params(void *w, double s, double a, double f) {
    (void)w;
    (void)s;
    (void)a;
    (void)f;
}

/// @brief Stub for `Water3D.SetColor` — base water tint applied as a
///        multiplier over the lit surface.
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param r Tint red, 0..1 (ignored).
/// @param g Tint green, 0..1 (ignored).
/// @param b Tint blue, 0..1 (ignored).
/// @param a Overall opacity, 0..1 (ignored).
void rt_water3d_set_color(void *w, double r, double g, double b, double a) {
    (void)w;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
}

/// @brief Stub for `Water3D.SetTexture` — bind a Pixels surface as the
///        water's diffuse texture (typically tiled small ripples).
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param p Pixels handle for the diffuse texture, or NULL (ignored).
void rt_water3d_set_texture(void *w, void *p) {
    (void)w;
    (void)p;
}

/// @brief Stub for `Water3D.SetNormalMap` — bind a tangent-space normal
///        map for water surface micro-perturbation. Combined with the
///        Gerstner deformation this gives detail at multiple scales.
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param p Pixels handle for the normal map, or NULL (ignored).
void rt_water3d_set_normal_map(void *w, void *p) {
    (void)w;
    (void)p;
}

/// @brief Stub for `Water3D.SetEnvMap` — bind a CubeMap3D as the
///        environment map sampled by the water's reflection contribution.
///
/// Silent no-op stub. Combined with `SetReflectivity` to control mix.
///
/// @param w Water3D handle (ignored).
/// @param c CubeMap3D handle, or NULL (ignored).
void rt_water3d_set_env_map(void *w, void *c) {
    (void)w;
    (void)c;
}

/// @brief Stub for `Water3D.SetReflectivity` — fraction of the
///        environment map color blended into the surface (0 = no
///        reflection, 1 = mirror-like).
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param r Reflectivity, 0..1 (ignored).
void rt_water3d_set_reflectivity(void *w, double r) {
    (void)w;
    (void)r;
}

/// @brief Stub for `Water3D.SetResolution` — grid density of the water
///        surface mesh. Higher values give finer wave detail at higher
///        rendering cost. Default 64; range 8..256.
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
/// @param r Grid density per side (ignored).
void rt_water3d_set_resolution(void *w, int64_t r) {
    (void)w;
    (void)r;
}

/// @brief Stub for `Water3D.AddWave` — add a directional Gerstner wave
///        to the water simulation. Up to 8 waves can be summed; each
///        contributes per-direction sinusoidal displacement and
///        derivative-based normal perturbation.
///
/// Silent no-op stub.
///
/// @param w  Water3D handle (ignored).
/// @param dx Wave direction x component (ignored).
/// @param dz Wave direction z component (ignored).
/// @param s  Wave speed in world units / second (ignored).
/// @param a  Wave amplitude (peak-to-trough/2) (ignored).
/// @param wl Wavelength in world units (ignored).
void rt_water3d_add_wave(void *w, double dx, double dz, double s, double a, double wl) {
    (void)w;
    (void)dx;
    (void)dz;
    (void)s;
    (void)a;
    (void)wl;
}

/// @brief Stub for `Water3D.ClearWaves` — remove all Gerstner waves
///        previously added via `AddWave`. The surface returns to a
///        flat plane.
///
/// Silent no-op stub.
///
/// @param w Water3D handle (ignored).
void rt_water3d_clear_waves(void *w) {
    (void)w;
}

/// @brief Stub for `Water3D.Update` — advance the water simulation by
///        `dt` seconds: increment wave phase and rebuild the surface
///        mesh / normals.
///
/// Silent no-op stub.
///
/// @param w  Water3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_water3d_update(void *w, double dt) {
    (void)w;
    (void)dt;
}

/* Vegetation3D stubs */

/// @brief Stub for `Vegetation3D.New` — would normally create an
///        instanced grass/foliage system bound to the given Pixels
///        texture (the per-blade billboard).
///
/// Silent stub returning NULL.
///
/// @param t Pixels handle for the blade texture (ignored).
///
/// @return `NULL`.
void *rt_vegetation3d_new(void *t) {
    (void)t;
    return NULL;
}

/// @brief Stub for `Vegetation3D.SetDensityMap` — bind a Pixels surface
///        whose red channel modulates spawn probability. Lets users paint
///        vegetation density (e.g., dense in valleys, sparse on hills).
///
/// Silent no-op stub.
///
/// @param v Vegetation3D handle (ignored).
/// @param p Pixels handle for the density map, or NULL for uniform (ignored).
void rt_vegetation3d_set_density_map(void *v, void *p) {
    (void)v;
    (void)p;
}

/// @brief Stub for `Vegetation3D.SetWindParams` — configure wind
///        animation: per-blade Y-axis shear via `sin(position + time)`.
///        Speed `s`, strength `st`, and turbulence `t` shape the motion.
///
/// Silent no-op stub.
///
/// @param v  Vegetation3D handle (ignored).
/// @param s  Wind speed (ignored).
/// @param st Wind strength (sway amplitude) (ignored).
/// @param t  Turbulence factor (ignored).
void rt_vegetation3d_set_wind_params(void *v, double s, double st, double t) {
    (void)v;
    (void)s;
    (void)st;
    (void)t;
}

/// @brief Stub for `Vegetation3D.SetLODDistances` — configure progressive
///        thinning between near distance `n` and far distance `f`. Beyond
///        `f` blades are hard-culled. The thinning is randomized per blade
///        so the visible density falls off smoothly.
///
/// Silent no-op stub.
///
/// @param v Vegetation3D handle (ignored).
/// @param n Near LOD distance (full density before this) (ignored).
/// @param f Far LOD distance (zero density beyond this) (ignored).
void rt_vegetation3d_set_lod_distances(void *v, double n, double f) {
    (void)v;
    (void)n;
    (void)f;
}

/// @brief Stub for `Vegetation3D.SetBladeSize` — per-blade quad
///        dimensions and randomized variance. Each blade is a
///        cross-billboard of two perpendicular `(w x h)` quads.
///
/// Silent no-op stub.
///
/// @param v  Vegetation3D handle (ignored).
/// @param w  Blade width in world units (ignored).
/// @param h  Blade height in world units (ignored).
/// @param va Variance fraction applied to size at spawn (ignored).
void rt_vegetation3d_set_blade_size(void *v, double w, double h, double va) {
    (void)v;
    (void)w;
    (void)h;
    (void)va;
}

/// @brief Stub for `Vegetation3D.SetSeed` — configure the deterministic
///        scatter seed used by later Populate calls.
///
/// Silent no-op stub.
///
/// @param v Vegetation3D handle (ignored).
/// @param seed Scatter seed (ignored).
void rt_vegetation3d_set_seed(void *v, int64_t seed) {
    (void)v;
    (void)seed;
}

/// @brief Stub for `Vegetation3D.Populate` — scatter `c` blade
///        instances across the surface of the given Terrain3D using LCG
///        random sampling, optionally filtered by the bound density map.
///
/// Silent no-op stub. Idempotent: re-running clears the previous
/// population first.
///
/// @param v Vegetation3D handle (ignored).
/// @param t Terrain3D handle providing the surface (ignored).
/// @param c Target instance count (ignored).
void rt_vegetation3d_populate(void *v, void *t, int64_t c) {
    (void)v;
    (void)t;
    (void)c;
}

/// @brief Stub for `Vegetation3D.Update` — per-frame update: advance
///        the wind animation phase by `dt` and update LOD selection
///        based on camera position `(cx, cy, cz)`.
///
/// Silent no-op stub.
///
/// @param v  Vegetation3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
/// @param cx Camera x (ignored).
/// @param cy Camera y (ignored).
/// @param cz Camera z (ignored).
void rt_vegetation3d_update(void *v, double dt, double cx, double cy, double cz) {
    (void)v;
    (void)dt;
    (void)cx;
    (void)cy;
    (void)cz;
}

/// @brief Stub for `Water3D.set_SimDistance`. Silent no-op stub.
///
/// @param water    Water3D handle (ignored).
/// @param distance Maximum camera distance for water simulation (ignored).
void rt_water3d_set_sim_distance(void *water, double distance) {
    (void)water;
    (void)distance;
}

/// @brief Stub for `Water3D.get_SimDistance`. Silent stub returning 0.
///
/// @param water Water3D handle (ignored).
///
/// @return `0.0`.
double rt_water3d_get_sim_distance(void *water) {
    (void)water;
    return 0.0;
}

/* Stub-parity audit additions: entry points registered in the 3D runtime
 * defs whose implementations compile only in graphics-enabled builds. */

/// @brief Silent fallback stub for `NavAgent3D.get_LinkKind` (graphics-disabled build).
///
/// @param o NavAgent3D handle (ignored).
///
/// @return An empty owned runtime string.
rt_string rt_navagent3d_get_link_kind(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("NavAgent3D.get_LinkKind: graphics support not compiled in",
                                  rt_string_from_bytes("", 0));
}

/// @brief Silent fallback stub for `NavAgent3D.get_OnOffMeshLink` (graphics-disabled build).
///
/// @param o NavAgent3D handle (ignored).
///
/// @return `0`, indicating that the agent is not traversing an off-mesh link.
int8_t rt_navagent3d_get_on_offmesh_link(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("NavAgent3D.get_OnOffMeshLink: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `NavMesh3D.get_HeuristicMode` (graphics-disabled build).
///
/// @param o NavMesh3D handle (ignored).
///
/// @return `0`, the fallback pathfinding heuristic mode.
int64_t rt_navmesh3d_get_heuristic_mode(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("NavMesh3D.get_HeuristicMode: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `NavMesh3D.get_TileSize` (graphics-disabled build).
///
/// @param o NavMesh3D handle (ignored).
///
/// @return `0.0`.
double rt_navmesh3d_get_tile_size(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("NavMesh3D.get_TileSize: graphics support not compiled in", 0.0);
}

/// @brief Trapping stub for `NavMesh3D.SetHeuristicMode` (graphics-disabled build).
///
/// @param o  NavMesh3D handle (ignored before trapping).
/// @param a1 Pathfinding heuristic-mode value (ignored before trapping).
void rt_navmesh3d_set_heuristic_mode(void *o, int64_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("NavMesh3D.SetHeuristicMode: graphics support not compiled in");
}
