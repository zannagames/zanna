//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_disabled_runtime_stubs.c
/// @brief Supplies deterministic fallback symbols for graphics-disabled runtime builds.
///
/// @details
/// This translation unit preserves ABI compatibility for GUI progress-bar,
/// rendering, scene, asset, physics, and navigation helpers that are normally
/// provided by graphics-enabled components. Mutators retain no state, queries
/// return explicit empty defaults, and mutation probes report failure. Commands,
/// virtual lists and trees, accessibility helpers, the GUI test harness, and
/// widget sizing keep their real backend-free implementations from
/// rt_gui_ide.cpp and rt_gui_widgets.c, so they are deliberately absent here.
///
// File: src/runtime/graphics/common/rt_disabled_runtime_stubs.c
// Purpose: Supplemental exported runtime stubs for graphics-disabled builds.
// Key invariants:
//   - Every supplemental public symbol remains link-compatible with the full runtime.
//   - Every declaring header is included, so a stub whose signature drifts from its
//     header contract fails to compile instead of silently mismatching the ABI.
//   - Scene mutation probes return failure when no graphics scene graph exists.
// Ownership/Lifetime:
//   - Stubs retain no graphics handles and allocate only documented fallback values.
//   - Stubs for consuming entry points release the buffers their contracts transfer.
// Links: src/runtime/graphics/3d/scene/rt_scene3d.h,
//   docs/adr/0162-exact-preserve-world-scenenode-reparenting.md,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md
//
//===----------------------------------------------------------------------===//

#include "rt_animcontroller3d.h"
#include "rt_canvas3d.h"
#include "rt_decal3d.h"
#include "rt_game3d_internal.h"
#include "rt_gltf.h"
#include "rt_gui.h"
#include "rt_iksolver3d.h"
#include "rt_model3d.h"
#include "rt_navmesh3d.h"
#include "rt_particles3d.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"
#include "rt_sprite3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/// @brief Create an owned empty runtime string for disabled-feature fallbacks.
///
/// @return A newly created empty runtime string.
static rt_string disabled_empty_string(void) {
    return rt_string_from_bytes("", 0);
}

/// @brief Ignore a progress-bar style update in a disabled GUI build.
/// @param progress ProgressBar handle (ignored).
/// @param style Style identifier (ignored).
void rt_progressbar_set_style(void *progress, int64_t style) {
    (void)progress;
    (void)style;
}

/// @brief Ignore a progress-bar percentage-visibility update.
/// @param progress ProgressBar handle (ignored).
/// @param show Non-zero to show the percentage label (ignored).
void rt_progressbar_show_percentage(void *progress, int64_t show) {
    (void)progress;
    (void)show;
}

/// @brief Ignore entry into a Canvas3D overlay pass.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_begin_overlay(void *obj) {
    (void)obj;
}

/// @brief Ignore completion of a Canvas3D overlay pass.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_end_overlay(void *obj) {
    (void)obj;
}

/// @brief Ignore clearing a disabled Canvas3D overlay.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_clear_overlay(void *obj) {
    (void)obj;
}

/// @brief Ignore finalization of a disabled Canvas3D frame.
/// @param obj Canvas3D handle (ignored).
void rt_canvas3d_finalize_frame(void *obj) {
    (void)obj;
}

/// @brief Return the fallback GPU frame time.
/// @param obj Canvas3D handle (ignored).
/// @return `0` microseconds.
int64_t rt_canvas3d_get_frame_gpu_time_us(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the number of draws submitted by the disabled renderer.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_draws_submitted(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the disabled renderer's AABB-transform count.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_aabb_transforms(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the disabled renderer's sort-pass count.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_sort_passes(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return the disabled renderer's backend state-change count.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_backend_state_changes(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Return no finalized-frame screenshot from a disabled renderer.
/// @param obj Canvas3D handle (ignored).
/// @return `NULL`.
void *rt_canvas3d_screenshot_final(void *obj) {
    (void)obj;
    return NULL;
}

/// @brief Report that no Canvas3D frame has been finalized.
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_frame_finalized(void *obj) {
    (void)obj;
    return 0;
}

/// @brief Ignore a Mesh3D capacity reservation without graphics support.
/// @param obj Mesh3D handle (ignored).
/// @param vertex_count Requested vertex capacity (ignored).
/// @param triangle_count Requested triangle capacity (ignored).
void rt_mesh3d_reserve(void *obj, int64_t vertex_count, int64_t triangle_count) {
    (void)obj;
    (void)vertex_count;
    (void)triangle_count;
}

/// @brief Ignore a Material3D anisotropy update.
/// @param obj Material3D handle (ignored).
/// @param anisotropy Requested anisotropy level (ignored).
void rt_material3d_set_anisotropy(void *obj, int64_t anisotropy) {
    (void)obj;
    (void)anisotropy;
}

/// @brief Return the default material anisotropy level.
/// @param obj Material3D handle (ignored).
/// @return `1`.
int64_t rt_material3d_get_anisotropy(void *obj) {
    (void)obj;
    return 1;
}

/// @brief Ignore rebasing the origin of a disabled scene.
/// @param scene Scene3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_scene3d_rebase_origin(void *scene, double dx, double dy, double dz) {
    (void)scene;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Report that a child was not added to a disabled scene node.
/// @param node Parent SceneNode3D handle (ignored).
/// @param child Child SceneNode3D handle (ignored).
/// @return `0`.
int8_t rt_scene_node3d_try_add_child(void *node, void *child) {
    (void)node;
    (void)child;
    return 0;
}

/// @brief Report that preserve-world reparenting was not performed.
/// @param node Parent SceneNode3D handle (ignored).
/// @param child Child SceneNode3D handle (ignored).
/// @return `0`.
int8_t rt_scene_node3d_try_add_child_preserve_world(void *node, void *child) {
    (void)node;
    (void)child;
    return 0;
}

/// @brief Report that a world matrix was not assigned to a disabled scene node.
/// @param node SceneNode3D handle (ignored).
/// @param world_matrix Matrix4 world transform (ignored).
/// @return `0`.
int8_t rt_scene_node3d_try_set_world_matrix(void *node, void *world_matrix) {
    (void)node;
    (void)world_matrix;
    return 0;
}

/// @brief Initialize world-position outputs for a disabled scene node.
/// @param node SceneNode3D handle (ignored).
/// @param x Optional X output receiving `0.0`.
/// @param y Optional Y output receiving `0.0`.
/// @param z Optional Z output receiving `0.0`.
/// @return `0`, indicating that no world position was available.
int8_t rt_scene_node3d_get_world_position_components(void *node, double *x, double *y, double *z) {
    (void)node;
    if (x)
        *x = 0.0;
    if (y)
        *y = 0.0;
    if (z)
        *z = 0.0;
    return 0;
}

/// @brief Return the scene count of an unavailable model.
/// @param model Model3D handle (ignored).
/// @return `0`.
int64_t rt_model3d_get_scene_count(void *model) {
    (void)model;
    return 0;
}

/// @brief Return the camera count of an unavailable model.
/// @param model Model3D handle (ignored).
/// @param scene_index Scene index (ignored).
/// @return `0`.
int64_t rt_model3d_get_camera_count(void *model, int64_t scene_index) {
    (void)model;
    (void)scene_index;
    return 0;
}

/// @brief Return no camera from an unavailable model.
/// @param model Model3D handle (ignored).
/// @param scene_index Scene index (ignored).
/// @param index Camera index (ignored).
/// @return `NULL`.
void *rt_model3d_get_camera(void *model, int64_t scene_index, int64_t index) {
    (void)model;
    (void)scene_index;
    (void)index;
    return NULL;
}

/// @brief Return the empty scene name from an unavailable model.
/// @param model Model3D handle (ignored).
/// @param index Scene index (ignored).
/// @return An owned empty runtime string.
rt_string rt_model3d_get_scene_name(void *model, int64_t index) {
    (void)model;
    (void)index;
    return disabled_empty_string();
}

/// @brief Return no instantiated scene from an unavailable model.
/// @param model Model3D handle (ignored).
/// @param index Scene index (ignored).
/// @return `NULL`.
void *rt_model3d_instantiate_scene_at(void *model, int64_t index) {
    (void)model;
    (void)index;
    return NULL;
}

/// @brief Return no model from a preloaded glTF bundle in a disabled build.
/// @details The contract transfers @p bundle to this call on every path, so it is released.
/// @param path Logical source path (ignored).
/// @param bundle Owned staged glTF bundle, released here.
/// @param load_assets Nonzero for asset-manager resolution semantics (ignored).
/// @return `NULL`.
void *rt_model3d_load_preloaded_gltf_bundle(rt_string path,
                                            struct rt_gltf_preload_bundle *bundle,
                                            int load_assets) {
    (void)path;
    (void)load_assets;
    rt_gltf_preload_bundle_free(bundle);
    return NULL;
}

/// @brief Return no model from preloaded FBX bytes in a disabled build.
/// @details The contract transfers @p preloaded_data to this call on every path, so it is freed.
/// @param path Logical source path (ignored).
/// @param preloaded_data Owned FBX byte buffer, freed here.
/// @param preloaded_size Buffer length in bytes (ignored).
/// @param load_assets Nonzero for asset-manager dependency behavior (ignored).
/// @return `NULL`.
void *rt_model3d_load_preloaded_fbx(rt_string path,
                                    uint8_t *preloaded_data,
                                    size_t preloaded_size,
                                    int load_assets) {
    (void)path;
    (void)preloaded_size;
    (void)load_assets;
    free(preloaded_data);
    return NULL;
}

/// @brief Report that a disabled physics world contains no body.
/// @param world Physics3DWorld handle (ignored).
/// @param body Body3D handle (ignored).
/// @return `0`.
int8_t rt_world3d_contains_body(void *world, void *body) {
    (void)world;
    (void)body;
    return 0;
}

/// @brief Return the disabled world's broadphase fallback count.
/// @param world Physics3DWorld handle (ignored).
/// @return `0`.
int64_t rt_world3d_get_broadphase_fallback_count(void *world) {
    (void)world;
    return 0;
}

/// @brief Return the disabled world's query broadphase rebuild count.
/// @param world Physics3DWorld handle (ignored).
/// @return `0`.
int64_t rt_world3d_get_query_broadphase_rebuild_count(void *world) {
    (void)world;
    return 0;
}

/// @brief Ignore rebasing the origin of a disabled physics world.
/// @param world Physics3DWorld handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_world3d_rebase_origin(void *world, double dx, double dy, double dz) {
    (void)world;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Report that a disabled navmesh was not exported.
/// @param navmesh NavMesh3D handle (ignored).
/// @param path Destination path (ignored).
/// @return `0`.
int8_t rt_navmesh3d_export(void *navmesh, rt_string path) {
    (void)navmesh;
    (void)path;
    return 0;
}

/// @brief Return no imported navmesh when graphics support is disabled.
/// @param path Source path (ignored).
/// @return `NULL`.
void *rt_navmesh3d_import(rt_string path) {
    (void)path;
    return NULL;
}

/// @brief Ignore the ground normal assigned to a disabled IK solver.
/// @param solver IKSolver3D handle (ignored).
/// @param normal Vec3 ground normal (ignored).
void rt_ik_solver3d_set_ground_normal(void *solver, void *normal) {
    (void)solver;
    (void)normal;
}

/// @brief Ignore the end-bone orientation goal assigned to a disabled IK solver.
/// @param solver IKSolver3D handle (ignored).
/// @param rotation Quat orientation goal (ignored).
void rt_ik_solver3d_set_target_rotation(void *solver, void *rotation) {
    (void)solver;
    (void)rotation;
}

/// @brief Ignore clearing the end-bone orientation goal on a disabled IK solver.
/// @param solver IKSolver3D handle (ignored).
void rt_ik_solver3d_clear_target_rotation(void *solver) {
    (void)solver;
}

/// @brief Return the fallback playback time for a disabled animation controller.
/// @param controller AnimController3D handle (ignored).
/// @return `0.0` seconds.
double rt_anim_controller3d_get_state_time(void *controller) {
    (void)controller;
    return 0.0;
}

/// @brief Report that a disabled animation controller is not playing a state.
/// @param controller AnimController3D handle (ignored).
/// @param state_name State name (ignored).
/// @return `0`.
int8_t rt_anim_controller3d_is_state_playing(void *controller, rt_string state_name) {
    (void)controller;
    (void)state_name;
    return 0;
}

/// @brief Ignore a bone-LOD update on a disabled animation controller.
/// @param controller AnimController3D handle (ignored).
/// @param lod Bone LOD level (ignored).
void rt_anim_controller3d_set_bone_lod(void *controller, int64_t lod) {
    (void)controller;
    (void)lod;
}

/// @brief Ignore rebasing a disabled particle system.
/// @param particles Particles3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_particles3d_rebase_origin(void *particles, double dx, double dy, double dz) {
    (void)particles;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Ignore rebasing a disabled decal.
/// @param decal Decal3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_decal3d_rebase_origin(void *decal, double dx, double dy, double dz) {
    (void)decal;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Ignore rebasing a disabled 3D sprite.
/// @param sprite Sprite3D handle (ignored).
/// @param dx Origin shift along X (ignored).
/// @param dy Origin shift along Y (ignored).
/// @param dz Origin shift along Z (ignored).
void rt_sprite3d_rebase_origin(void *sprite, double dx, double dy, double dz) {
    (void)sprite;
    (void)dx;
    (void)dy;
    (void)dz;
}

/// @brief Return no synthesized heightmap Pixels from a disabled terrain.
/// @param terrain Terrain3D handle (ignored).
/// @return `NULL`.
void *rt_terrain3d_build_heightmap_pixels(void *terrain) {
    (void)terrain;
    return NULL;
}

/// @brief Report that two disabled terrain edges were not stitched.
/// @param terrain Terrain3D handle (ignored).
/// @param edge Local edge identifier (ignored).
/// @param neighbor Neighbor Terrain3D handle (ignored).
/// @param neighbor_edge Neighbor edge identifier (ignored).
/// @return `0`.
int64_t rt_terrain3d_stitch_edge(void *terrain,
                                 int64_t edge,
                                 void *neighbor,
                                 int64_t neighbor_edge) {
    (void)terrain;
    (void)edge;
    (void)neighbor;
    (void)neighbor_edge;
    return 0;
}

/// @brief Return no navigation mesh built from a disabled terrain.
/// @param terrain Terrain3D handle (ignored).
/// @param step Heightmap sampling stride (ignored).
/// @return `NULL`.
void *rt_terrain3d_build_nav_mesh(void *terrain, int64_t step) {
    (void)terrain;
    (void)step;
    return NULL;
}

/// @brief Ignore drawing a disabled terrain at a world-space offset.
/// @param canvas Canvas3D handle (ignored).
/// @param terrain Terrain3D handle (ignored).
/// @param x World-space X position (ignored).
/// @param y World-space Y position (ignored).
/// @param z World-space Z position (ignored).
void rt_canvas3d_draw_terrain_at(void *canvas, void *terrain, double x, double y, double z) {
    (void)canvas;
    (void)terrain;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Return no glTF preload bundle in a graphics-disabled build.
/// @details The contract transfers @p root_data to this call, so it is freed; the diagnostic
///          buffer receives the unavailable-graphics reason like any other preload failure.
/// @param path Null-terminated source path (ignored).
/// @param root_data Malloc-owned root bytes, freed here.
/// @param root_size Number of readable root bytes (ignored).
/// @param load_assets Non-zero for asset-manager resolution (ignored).
/// @param error Optional diagnostic buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return `NULL`.
rt_gltf_preload_bundle *rt_gltf_preload_bundle_create_cstr(const char *path,
                                                           uint8_t *root_data,
                                                           size_t root_size,
                                                           int load_assets,
                                                           char *error,
                                                           size_t error_cap) {
    (void)path;
    (void)root_size;
    (void)load_assets;
    free(root_data);
    if (error && error_cap > 0)
        snprintf(error, error_cap, "graphics support not compiled in");
    return NULL;
}

/// @brief Ignore release of an absent glTF preload bundle.
/// @param bundle Preload-bundle pointer (ignored).
void rt_gltf_preload_bundle_free(rt_gltf_preload_bundle *bundle) {
    (void)bundle;
}

/// @brief Return the decoded-image footprint of an absent preload bundle.
/// @param bundle Preload-bundle pointer (ignored).
/// @return `0` bytes.
size_t rt_gltf_preload_bundle_decoded_image_bytes(const rt_gltf_preload_bundle *bundle) {
    (void)bundle;
    return 0;
}

/// @brief Return the next decoded-image slice size for an absent preload bundle.
/// @param bundle Preload-bundle pointer (ignored).
/// @param max_bytes Requested maximum slice size (ignored).
/// @return `0` bytes.
size_t rt_gltf_preload_bundle_next_decoded_image_slice_bytes(const rt_gltf_preload_bundle *bundle,
                                                             size_t max_bytes) {
    (void)bundle;
    (void)max_bytes;
    return 0;
}

/// @brief Report that no decoded-image slice was prepared.
/// @param bundle Preload-bundle pointer (ignored).
/// @param max_bytes Requested maximum slice size (ignored).
/// @return `0` bytes.
size_t rt_gltf_preload_bundle_prepare_decoded_image_slice(rt_gltf_preload_bundle *bundle,
                                                          size_t max_bytes) {
    (void)bundle;
    (void)max_bytes;
    return 0;
}

/// @brief Initialize camera-position outputs when no camera is available.
/// @param camera Camera3D handle (ignored).
/// @param x Optional X output receiving `0.0`.
/// @param y Optional Y output receiving `0.0`.
/// @param z Optional Z output receiving `0.0`.
/// @return `0`, indicating that no position was read.
int8_t rt_camera3d_get_position_components(void *camera, double *x, double *y, double *z) {
    (void)camera;
    if (x)
        *x = 0.0;
    if (y)
        *y = 0.0;
    if (z)
        *z = 0.0;
    return 0;
}

/// @brief Ignore a component-wise look-at update on a disabled camera.
/// @param camera Camera3D handle (ignored).
/// @param eye_x Eye X coordinate (ignored).
/// @param eye_y Eye Y coordinate (ignored).
/// @param eye_z Eye Z coordinate (ignored).
/// @param target_x Target X coordinate (ignored).
/// @param target_y Target Y coordinate (ignored).
/// @param target_z Target Z coordinate (ignored).
/// @param up_x Up-vector X component (ignored).
/// @param up_y Up-vector Y component (ignored).
/// @param up_z Up-vector Z component (ignored).
void rt_camera3d_look_at_components(void *camera,
                                    double eye_x,
                                    double eye_y,
                                    double eye_z,
                                    double target_x,
                                    double target_y,
                                    double target_z,
                                    double up_x,
                                    double up_y,
                                    double up_z) {
    (void)camera;
    (void)eye_x;
    (void)eye_y;
    (void)eye_z;
    (void)target_x;
    (void)target_y;
    (void)target_z;
    (void)up_x;
    (void)up_y;
    (void)up_z;
}

/// @brief Ignore a component-wise orbit update on a disabled camera.
/// @param camera Camera3D handle (ignored).
/// @param target_x Orbit-target X coordinate (ignored).
/// @param target_y Orbit-target Y coordinate (ignored).
/// @param target_z Orbit-target Z coordinate (ignored).
/// @param yaw Orbit yaw (ignored).
/// @param pitch Orbit pitch (ignored).
/// @param distance Orbit distance (ignored).
void rt_camera3d_orbit_components(void *camera,
                                  double target_x,
                                  double target_y,
                                  double target_z,
                                  double yaw,
                                  double pitch,
                                  double distance) {
    (void)camera;
    (void)target_x;
    (void)target_y;
    (void)target_z;
    (void)yaw;
    (void)pitch;
    (void)distance;
}

/// @brief Ignore a request to show the cursor without a graphics backend.
void vgfx_show_cursor(void) {}

/// @brief Ignore a request to hide the cursor without a graphics backend.
void vgfx_hide_cursor(void) {}
