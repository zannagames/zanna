//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/common/rt_3d_render_stubs.c
// Purpose: Graphics-disabled Canvas3D, Camera3D, PostFX3D, InstanceBatch3D,
//          Mesh3D, Material3D, Light3D, SceneAsset, and glTF entry points that
//          no longer fit the size-limited rt_canvas3d_stubs.c and
//          rt_3d_asset_stubs.c translation units.
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Constructors and stateful Canvas3D, Camera3D, PostFX3D, and
//     InstanceBatch3D operations trap through rt_graphics_unavailable_().
//   - Queries and mutations on handles that cannot exist without graphics
//     return documented fallbacks; ZANNA_GRAPHICS_STUBS_STRICT makes them trap.
//   - PostFXEffectKind constants keep their graphics-build values.
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//   - Result-returning loaders return caller-owned Err(String) results.
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h,
//        src/runtime/graphics/common/rt_canvas3d_stubs.c,
//        src/runtime/graphics/common/rt_3d_asset_stubs.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Graphics-disabled render-surface stubs added after the original stub split.
/// @details Mirrors the trap and fallback contracts of rt_canvas3d_stubs.c (Canvas3D,
///          Camera3D, PostFX3D, InstanceBatch3D) and rt_3d_asset_stubs.c (Mesh3D,
///          Material3D, Light3D, SceneAsset, glTF) for their newer members.

#include "rt_graphics_stubs_internal.h"

/* Gltf stubs */

/// @brief Silent fallback stub for `Gltf.get_SkeletonCount` (graphics-disabled build).
/// @param asset Candidate glTF asset handle (ignored).
/// @return `0`.
int64_t rt_gltf_skeleton_count(void *asset) {
    (void)asset;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Gltf.get_SkeletonCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Gltf.GetSkeleton` (graphics-disabled build).
/// @param asset Candidate glTF asset handle (ignored).
/// @param index Zero-based skeleton index (ignored).
/// @return `NULL`.
void *rt_gltf_get_skeleton(void *asset, int64_t index) {
    (void)asset;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Gltf.GetSkeleton: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Gltf.get_AnimationCount` (graphics-disabled build).
/// @param asset Candidate glTF asset handle (ignored).
/// @return `0`.
int64_t rt_gltf_animation_count(void *asset) {
    (void)asset;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Gltf.get_AnimationCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Gltf.GetAnimation` (graphics-disabled build).
/// @param asset Candidate glTF asset handle (ignored).
/// @param index Zero-based animation index (ignored).
/// @return `NULL`.
void *rt_gltf_get_animation(void *asset, int64_t index) {
    (void)asset;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Gltf.GetAnimation: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Gltf.get_NodeAnimationCount` (graphics-disabled build).
/// @param asset Candidate glTF asset handle (ignored).
/// @return `0`.
int64_t rt_gltf_node_animation_count(void *asset) {
    (void)asset;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Gltf.get_NodeAnimationCount: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Gltf.GetNodeAnimation` (graphics-disabled build).
/// @param asset Candidate glTF asset handle (ignored).
/// @param index Zero-based node-animation index (ignored).
/// @return `NULL`.
void *rt_gltf_get_node_animation(void *asset, int64_t index) {
    (void)asset;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Gltf.GetNodeAnimation: graphics support not compiled in", NULL);
}

/* Canvas3D stubs */

/// @brief Trapping stub for `Canvas3D.NewFullscreen` (graphics-disabled build).
/// @param title Borrowed runtime string used as the platform-window title (ignored before
/// trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_canvas3d_new_fullscreen(rt_string title) {
    (void)title;
    RT_GRAPHICS_TRAP_RET("Canvas3D.NewFullscreen: graphics support not compiled in", NULL);
}

/// @brief Trapping stub for `Canvas3D.NewOffscreenAccelerated` (graphics-disabled build).
/// @param target Live RenderTarget3D retained as the initial output (ignored before trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_canvas3d_new_offscreen_accelerated(void *target) {
    (void)target;
    RT_GRAPHICS_TRAP_RET("Canvas3D.NewOffscreenAccelerated: graphics support not compiled in",
                         NULL);
}

/// @brief Trapping stub for `Canvas3D.SetIcon` (graphics-disabled build).
/// @param obj Canvas3D handle (ignored before trapping).
/// @param pixels Pixels (ignored before trapping).
void rt_canvas3d_set_icon(void *obj, void *pixels) {
    (void)obj;
    (void)pixels;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.SetIcon: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Canvas3D.get_Wireframe` (graphics-disabled build).
/// @param obj Canvas3D handle or approved stack fixture (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_wireframe(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_Wireframe: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `Canvas3D.set_IblEnabled` (graphics-disabled build).
/// @param obj Borrowed Canvas3D handle (ignored before trapping).
/// @param enabled Non-zero to enable skybox-based diffuse and specular environment lighting
/// (ignored before trapping).
void rt_canvas3d_set_ibl_enabled(void *obj, int8_t enabled) {
    (void)obj;
    (void)enabled;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.set_IblEnabled: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Canvas3D.get_IblEnabled` (graphics-disabled build).
/// @param obj Borrowed Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_ibl_enabled(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_IblEnabled: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `Canvas3D.set_IblIntensity` (graphics-disabled build).
/// @param obj Borrowed Canvas3D handle (ignored before trapping).
/// @param intensity Requested non-negative environment multiplier (ignored before trapping).
void rt_canvas3d_set_ibl_intensity(void *obj, double intensity) {
    (void)obj;
    (void)intensity;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.set_IblIntensity: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Canvas3D.get_IblIntensity` (graphics-disabled build).
/// @param obj Borrowed Canvas3D handle (ignored).
/// @return `0.0`.
double rt_canvas3d_get_ibl_intensity(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_IblIntensity: graphics support not compiled in",
                                  0.0);
}

/// @brief Trapping stub for `Canvas3D.SetHeightFog` (graphics-disabled build).
/// @param obj Borrowed Canvas3D handle (ignored before trapping).
/// @param base_height World-space reference height (ignored before trapping).
/// @param falloff Non-negative exponential thinning rate (ignored before trapping).
/// @param density Non-negative density at the reference height (ignored before trapping).
/// @param blend Normalized contribution weight (ignored before trapping).
void rt_canvas3d_set_height_fog(
    void *obj, double base_height, double falloff, double density, double blend) {
    (void)obj;
    (void)base_height;
    (void)falloff;
    (void)density;
    (void)blend;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.SetHeightFog: graphics support not compiled in");
}

/// @brief Trapping stub for `Canvas3D.DrawText2DTtf` (graphics-disabled build).
/// @param obj Borrowed Canvas3D handle (ignored before trapping).
/// @param font Borrowed live TtfFont handle (ignored before trapping).
/// @param x Left origin in logical pixels (ignored before trapping).
/// @param y Top origin in logical pixels (top of the text box, not baseline) (ignored before
/// trapping).
/// @param text Borrowed runtime string (ignored before trapping).
/// @param size_px Font pixel size (clamped to the TtfFont range) (ignored before trapping).
/// @param color Packed 0xRRGGBB color (ignored before trapping).
void rt_canvas3d_draw_text2d_ttf(
    void *obj, void *font, int64_t x, int64_t y, rt_string text, double size_px, int64_t color) {
    (void)obj;
    (void)font;
    (void)x;
    (void)y;
    (void)text;
    (void)size_px;
    (void)color;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.DrawText2DTtf: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Canvas3D.MeasureText2DTtf` (graphics-disabled build).
/// @param obj Borrowed Canvas3D handle (ignored).
/// @param font Borrowed live TtfFont handle (ignored).
/// @param text Borrowed runtime string (ignored).
/// @param size_px Font pixel size (clamped to the TtfFont range) (ignored).
/// @return `0`.
int64_t rt_canvas3d_measure_text2d_ttf(void *obj, void *font, rt_string text, double size_px) {
    (void)obj;
    (void)font;
    (void)text;
    (void)size_px;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.MeasureText2DTtf: graphics support not compiled in", 0);
}

/// @brief No-op stub for internal `rt_canvas3d_release_adopted_window` (graphics-disabled build).
/// @param canvas3d Borrowed Canvas3D handle (invalid handles are ignored) (ignored).
void rt_canvas3d_release_adopted_window(void *canvas3d) {
    (void)canvas3d;
}

/// @brief Trapping stub for `Canvas3D.DrawRect2DAlpha` (graphics-disabled build).
/// @param canvas Borrowed Canvas3D handle (ignored before trapping).
/// @param x Destination left edge in logical pixels (ignored before trapping).
/// @param y Destination top edge in logical pixels (ignored before trapping).
/// @param w Destination width in logical pixels (ignored before trapping).
/// @param h Destination height in logical pixels (ignored before trapping).
/// @param color Packed runtime color value (ignored before trapping).
/// @param alpha Opacity clamped to the inclusive normalized range (ignored before trapping).
void rt_canvas3d_draw_rect2d_alpha(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, double alpha) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
    (void)alpha;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.DrawRect2DAlpha: graphics support not compiled in");
}

/// @brief Trapping stub for `Canvas3D.SetShadowAtlasResolution` (graphics-disabled build).
/// @param obj Canvas handle (ignored before trapping).
/// @param resolution Negative values become zero (ignored before trapping).
void rt_canvas3d_set_shadow_atlas_resolution(void *obj, int64_t resolution) {
    (void)obj;
    (void)resolution;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.SetShadowAtlasResolution: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Canvas3D.get_ShadowAtlasResolution` (graphics-disabled build).
/// @param obj Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_shadow_atlas_resolution(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Canvas3D.get_ShadowAtlasResolution: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `Canvas3D.SetCaptureAfterPresent` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored before trapping).
/// @param enabled Non-zero to capture every presented frame (one blit/frame on GPU present paths)
/// (ignored before trapping).
void rt_canvas3d_set_capture_after_present(void *canvas, int8_t enabled) {
    (void)canvas;
    (void)enabled;
    RT_GRAPHICS_TRAP_VOID("Canvas3D.SetCaptureAfterPresent: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Canvas3D.get_CaptureAfterPresent` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_capture_after_present(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Canvas3D.get_CaptureAfterPresent: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ShadowBias` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0.0`.
double rt_canvas3d_get_shadow_bias(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ShadowBias: graphics support not compiled in", 0.0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ShadowSlopeBias` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0.0`.
double rt_canvas3d_get_shadow_slope_bias(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ShadowSlopeBias: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ShadowStrength` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0.0`.
double rt_canvas3d_get_shadow_strength(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ShadowStrength: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ShadowQuality` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_shadow_quality(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ShadowQuality: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ShadowCascades` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_shadow_cascades(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ShadowCascades: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ShadowBudget` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_shadow_budget(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ShadowBudget: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ClusterLightBudget` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_cluster_light_budget(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Canvas3D.get_ClusterLightBudget: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_BackfaceCull` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_backface_cull(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_BackfaceCull: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_FogEnabled` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_fog_enabled(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_FogEnabled: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_FogNear` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0.0`.
double rt_canvas3d_get_fog_near(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_FogNear: graphics support not compiled in", 0.0);
}

/// @brief Silent fallback stub for `Canvas3D.get_FogFar` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0.0`.
double rt_canvas3d_get_fog_far(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_FogFar: graphics support not compiled in", 0.0);
}

/// @brief Silent fallback stub for `Canvas3D.get_FogColor` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `NULL`.
void *rt_canvas3d_get_fog_color(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_FogColor: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Canvas3D.get_AmbientColor` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `NULL`.
void *rt_canvas3d_get_ambient_color(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_AmbientColor: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Canvas3D.get_Skybox` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `NULL`.
void *rt_canvas3d_get_skybox(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_Skybox: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Canvas3D.get_RenderTarget` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `NULL`.
void *rt_canvas3d_get_render_target(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_RenderTarget: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Canvas3D.get_PostFX` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `NULL`.
void *rt_canvas3d_get_post_fx(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_PostFX: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Canvas3D.get_FrustumCulling` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_frustum_culling(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_FrustumCulling: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_OcclusionCulling` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_occlusion_culling(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_OcclusionCulling: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_TextureStreaming` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_texture_streaming(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_TextureStreaming: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_TextureStreamingBias` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0.0`.
double rt_canvas3d_get_texture_streaming_bias(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Canvas3D.get_TextureStreamingBias: graphics support not compiled in", 0.0);
}

/// @brief Silent fallback stub for `Canvas3D.get_MaxDeltaTime` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_dt_max(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_MaxDeltaTime: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ForceCpuSkinning` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_force_cpu_skinning(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ForceCpuSkinning: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ClipRectActive` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int8_t rt_canvas3d_get_clip_rect_active(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ClipRectActive: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ClipRectX` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_clip_rect_x(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ClipRectX: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ClipRectY` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_clip_rect_y(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ClipRectY: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ClipRectWidth` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_clip_rect_width(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ClipRectWidth: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `Canvas3D.get_ClipRectHeight` (graphics-disabled build).
/// @param canvas Canvas3D handle (ignored).
/// @return `0`.
int64_t rt_canvas3d_get_clip_rect_height(void *canvas) {
    (void)canvas;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Canvas3D.get_ClipRectHeight: graphics support not compiled in",
                                  0);
}

/* Mesh3D stubs */

/// @brief Silent fallback stub for `Mesh3D.VertexNormal` (graphics-disabled build).
/// @param obj Mesh3D handle (ignored).
/// @param index Index (ignored).
/// @return `NULL`.
void *rt_mesh3d_get_vertex_normal(void *obj, int64_t index) {
    (void)obj;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Mesh3D.VertexNormal: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `Mesh3D.Append` (graphics-disabled build).
/// @param obj Destination mesh, mutated in place (ignored).
/// @param src_obj Source mesh (ignored).
void rt_mesh3d_append(void *obj, void *src_obj) {
    (void)obj;
    (void)src_obj;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Mesh3D.Append: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Mesh3D.get_BoundsMin` (graphics-disabled build).
/// @param obj Mesh3D handle (ignored).
/// @return `NULL`.
void *rt_mesh3d_get_bounds_min(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Mesh3D.get_BoundsMin: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Mesh3D.get_BoundsMax` (graphics-disabled build).
/// @param obj Mesh3D receiver (ignored).
/// @return `NULL`.
void *rt_mesh3d_get_bounds_max(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Mesh3D.get_BoundsMax: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Mesh3D.get_BoundsCenter` (graphics-disabled build).
/// @param obj Mesh3D receiver (ignored).
/// @return `NULL`.
void *rt_mesh3d_get_bounds_center(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Mesh3D.get_BoundsCenter: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Mesh3D.get_BoundsSize` (graphics-disabled build).
/// @param obj Mesh3D receiver (ignored).
/// @return `NULL`.
void *rt_mesh3d_get_bounds_size(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Mesh3D.get_BoundsSize: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Mesh3D.get_BoundsRadius` (graphics-disabled build).
/// @param obj Mesh3D receiver (ignored).
/// @return `0.0`.
double rt_mesh3d_get_bounds_radius(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Mesh3D.get_BoundsRadius: graphics support not compiled in", 0.0);
}

/// @brief Silent fallback stub for internal `rt_mesh3d_get_vertex_raw` (graphics-disabled build).
/// @param obj Borrowed Mesh3D handle (ignored).
/// @param index Zero-based vertex index (ignored).
/// @param out_pos Writable three-double position array (ignored).
/// @param out_normal Writable three-double normal array (ignored).
/// @param out_uv Writable two-double texture-coordinate array (ignored).
/// @return `0`.
int8_t rt_mesh3d_get_vertex_raw(
    void *obj, int64_t index, double out_pos[3], double out_normal[3], double out_uv[2]) {
    (void)obj;
    (void)index;
    (void)out_pos;
    (void)out_normal;
    (void)out_uv;
    return 0;
}

/// @brief Silent fallback stub for internal `rt_mesh3d_get_triangle_raw` (graphics-disabled build).
/// @param obj Borrowed Mesh3D handle (ignored).
/// @param triangle Zero-based triangle index (ignored).
/// @param out_indices Writable three-element index array (ignored).
/// @return `0`.
int8_t rt_mesh3d_get_triangle_raw(void *obj, int64_t triangle, int64_t out_indices[3]) {
    (void)obj;
    (void)triangle;
    (void)out_indices;
    return 0;
}

/// @brief Silent no-op stub for `Mesh3D.RasterizeUvMaskY` (graphics-disabled build).
/// @param obj Borrowed Mesh3D handle (ignored).
/// @param mask_pixels Borrowed mutable Pixels handle receiving the coverage (ignored).
/// @param y_min Inclusive lower object-space Y bound (ignored).
/// @param y_max Inclusive upper object-space Y bound (ignored).
void rt_mesh3d_rasterize_uv_mask_y(void *obj, void *mask_pixels, double y_min, double y_max) {
    (void)obj;
    (void)mask_pixels;
    (void)y_min;
    (void)y_max;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Mesh3D.RasterizeUvMaskY: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Mesh3D.RasterizeUvHeight` (graphics-disabled build).
/// @param obj Mesh3D receiver (ignored).
/// @param height_pixels Pixels handle receiving the height map (any size) (ignored).
/// @param y_min Object-space Y mapping to luminance 1 (ignored).
/// @param y_max Object-space Y mapping to luminance 255 (> y_min) (ignored).
void rt_mesh3d_rasterize_uv_height(void *obj, void *height_pixels, double y_min, double y_max) {
    (void)obj;
    (void)height_pixels;
    (void)y_min;
    (void)y_max;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Mesh3D.RasterizeUvHeight: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Mesh3D.RasterizeUvAxis` (graphics-disabled build).
/// @param obj Mesh3D receiver (ignored).
/// @param height_pixels Pixels handle receiving the map (any size) (ignored).
/// @param axis 0 = X, 1 = Y, 2 = Z (ignored).
/// @param lo Object-space coordinate mapping to luminance 1 (ignored).
/// @param hi Object-space coordinate mapping to luminance 255 (> lo) (ignored).
void rt_mesh3d_rasterize_uv_axis(
    void *obj, void *height_pixels, int64_t axis, double lo, double hi) {
    (void)obj;
    (void)height_pixels;
    (void)axis;
    (void)lo;
    (void)hi;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Mesh3D.RasterizeUvAxis: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Mesh3D.Mirror` (graphics-disabled build).
/// @param obj Borrowed source Mesh3D handle (ignored).
/// @param skeleton Borrowed Skeleton3D used for the partner resolution (ignored).
/// @return `NULL`.
void *rt_mesh3d_mirror(void *obj, void *skeleton) {
    (void)obj;
    (void)skeleton;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Mesh3D.Mirror: graphics support not compiled in", NULL);
}

/* Camera3D stubs */

/// @brief Silent fallback stub for `Camera3D.get_Up` (graphics-disabled build).
/// @param obj Borrowed Camera3D handle (ignored).
/// @return `NULL`.
void *rt_camera3d_get_up(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Camera3D.get_Up: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Camera3D.get_ViewMatrix` (graphics-disabled build).
/// @param obj Borrowed Camera3D handle (ignored).
/// @return `NULL`.
void *rt_camera3d_get_view_matrix(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Camera3D.get_ViewMatrix: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Camera3D.get_ProjectionMatrix` (graphics-disabled build).
/// @param obj Borrowed Camera3D handle (ignored).
/// @return `NULL`.
void *rt_camera3d_get_projection_matrix(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Camera3D.get_ProjectionMatrix: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Camera3D.get_AspectRatio` (graphics-disabled build).
/// @param obj Borrowed Camera3D handle (ignored).
/// @return `0.0`.
double rt_camera3d_get_aspect(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Camera3D.get_AspectRatio: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent fallback stub for internal `rt_camera3d_world_to_screen` (graphics-disabled
/// build).
/// @param obj Borrowed Camera3D handle (ignored).
/// @param x World-space X coordinate (ignored).
/// @param y World-space Y coordinate (ignored).
/// @param z World-space Z coordinate (ignored).
/// @param sw Positive viewport width in pixels (ignored).
/// @param sh Positive viewport height in pixels (ignored).
/// @param out_sx Optional destination for horizontal screen pixels (ignored).
/// @param out_sy Optional destination for vertical screen pixels (ignored).
/// @return `0`.
int8_t rt_camera3d_world_to_screen(void *obj,
                                   double x,
                                   double y,
                                   double z,
                                   int64_t sw,
                                   int64_t sh,
                                   double *out_sx,
                                   double *out_sy) {
    (void)obj;
    (void)x;
    (void)y;
    (void)z;
    (void)sw;
    (void)sh;
    (void)out_sx;
    (void)out_sy;
    return 0;
}

/* Material3D stubs */

/// @brief Silent no-op stub for `Material3D.set_TemporalWeight` (graphics-disabled build).
/// @param obj Borrowed Material3D handle (ignored).
/// @param weight Zero rejects history, one preserves the configured contribution (ignored).
void rt_material3d_set_temporal_weight(void *obj, double weight) {
    (void)obj;
    (void)weight;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Material3D.set_TemporalWeight: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Material3D.get_TemporalWeight` (graphics-disabled build).
/// @param obj Borrowed Material3D handle (ignored).
/// @return `0.0`.
double rt_material3d_get_temporal_weight(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_TemporalWeight: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent no-op stub for `Material3D.SetTextureFilters` (graphics-disabled build).
/// @param obj Borrowed Material3D handle (ignored).
/// @param min_filter Minification: 0=Linear, 1=Nearest (out of range = Linear) (ignored).
/// @param mag_filter Magnification: 0=Linear, 1=Nearest (out of range = Linear) (ignored).
/// @param mip_filter Mip selection: 0=None, 1=Nearest, 2=Linear (trilinear) (ignored).
void rt_material3d_set_texture_filters(void *obj,
                                       int64_t min_filter,
                                       int64_t mag_filter,
                                       int64_t mip_filter) {
    (void)obj;
    (void)min_filter;
    (void)mag_filter;
    (void)mip_filter;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Material3D.SetTextureFilters: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Material3D.get_Texture` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_texture(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_Texture: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Material3D.get_NormalMap` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_normal_map(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_NormalMap: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Material3D.get_SpecularMap` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_specular_map(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_SpecularMap: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Material3D.get_EmissiveMap` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_emissive_map(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_EmissiveMap: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Material3D.get_MetallicRoughnessMap` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_metallic_roughness_map(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Material3D.get_MetallicRoughnessMap: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Material3D.get_AmbientOcclusionMap` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_ao_map(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Material3D.get_AmbientOcclusionMap: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Material3D.get_Lightmap` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_lightmap(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_Lightmap: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Material3D.get_EnvMap` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_env_map(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_EnvMap: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Material3D.get_EmissiveColor` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `NULL`.
void *rt_material3d_get_emissive_color(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_EmissiveColor: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Material3D.get_Shininess` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `0.0`.
double rt_material3d_get_shininess(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_Shininess: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent fallback stub for `Material3D.get_DepthBias` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `0.0`.
double rt_material3d_get_depth_bias(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_DepthBias: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent fallback stub for `Material3D.get_DepthSlopeBias` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @return `0.0`.
double rt_material3d_get_depth_slope_bias(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.get_DepthSlopeBias: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent fallback stub for `Material3D.GetCustomParam` (graphics-disabled build).
/// @param obj Material3D handle (ignored).
/// @param index Zero-based parameter index (ignored).
/// @return `0.0`.
double rt_material3d_get_custom_param(void *obj, int64_t index) {
    (void)obj;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Material3D.GetCustomParam: graphics support not compiled in",
                                  0.0);
}

/* Light3D stubs */

/// @brief Silent fallback stub for `Light3D.get_InnerConeDegrees` (graphics-disabled build).
/// @param obj Borrowed Light3D handle (ignored).
/// @return `0.0`.
double rt_light3d_get_inner_cone_degrees(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Light3D.get_InnerConeDegrees: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent fallback stub for `Light3D.get_OuterConeDegrees` (graphics-disabled build).
/// @param obj Borrowed Light3D handle (ignored).
/// @return `0.0`.
double rt_light3d_get_outer_cone_degrees(void *obj) {
    (void)obj;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Light3D.get_OuterConeDegrees: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent no-op stub for `Light3D.SetSpotCone` (graphics-disabled build).
/// @param obj Borrowed spot Light3D handle (ignored).
/// @param inner_angle Requested inner half-angle in degrees (ignored).
/// @param outer_angle Requested outer half-angle in degrees (ignored).
void rt_light3d_set_spot_cone(void *obj, double inner_angle, double outer_angle) {
    (void)obj;
    (void)inner_angle;
    (void)outer_angle;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Light3D.SetSpotCone: graphics support not compiled in");
}

/* InstanceBatch3D stubs */

/// @brief Silent fallback stub for `InstanceBatch3D.GetTransform` (graphics-disabled build).
/// @param batch InstanceBatch3D receiver (ignored).
/// @param index Zero-based instance index (ignored).
/// @return `NULL`.
void *rt_instbatch3d_get(void *batch, int64_t index) {
    (void)batch;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("InstanceBatch3D.GetTransform: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `InstanceBatch3D.get_Mesh` (graphics-disabled build).
/// @param batch InstanceBatch3D receiver (ignored).
/// @return `NULL`.
void *rt_instbatch3d_borrow_mesh(void *batch) {
    (void)batch;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("InstanceBatch3D.get_Mesh: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `InstanceBatch3D.get_Material` (graphics-disabled build).
/// @param batch InstanceBatch3D receiver (ignored).
/// @return `NULL`.
void *rt_instbatch3d_borrow_material(void *batch) {
    (void)batch;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("InstanceBatch3D.get_Material: graphics support not compiled in",
                                  NULL);
}

/* SceneAsset stubs */

/// @brief Fallback stub for `SceneAsset.LoadTextResult` (graphics-disabled build).
/// @details Result-returning APIs report unavailable graphics support as `Err(String)`
///          instead of trapping.
/// @param path Logical VSCN path used for extension validation, dependency bases, and diagnostics
/// (ignored).
/// @param text Complete VSCN source text (ignored).
/// @return `Err("SceneAsset.LoadTextResult: graphics support not compiled in")`.
void *rt_model3d_load_text_result(rt_string path, rt_string text) {
    (void)path;
    (void)text;
    return rt_graphics_unavailable_result_(
        "SceneAsset.LoadTextResult: graphics support not compiled in");
}

/// @brief Silent fallback stub for internal `rt_model3d_load_preloaded_vscn` (graphics-disabled
/// build).
/// @param path Logical .scene3d/.vscn source path (ignored).
/// @param preloaded_text Owned document bytes, released by this stub as the contract requires.
/// @param preloaded_len Number of bytes in @p preloaded_text (ignored).
/// @param load_assets Nonzero to retain asset-manager dependency behavior (ignored).
/// @return `NULL`.
void *rt_model3d_load_preloaded_vscn(rt_string path,
                                     char *preloaded_text,
                                     size_t preloaded_len,
                                     int load_assets) {
    (void)path;
    (void)preloaded_len;
    (void)load_assets;
    free(preloaded_text); /* the buffer is consumed on every path */
    return NULL;
}

/* PostFX3D stubs */

/// @brief Silent fallback stub for `PostFX3D.GetEffectKind` (graphics-disabled build).
/// @param obj Candidate PostFX3D chain (ignored).
/// @param index Zero-based position in the chain (ignored).
/// @return `0`.
int64_t rt_postfx3d_get_effect_kind(void *obj, int64_t index) {
    (void)obj;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("PostFX3D.GetEffectKind: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `PostFX3D.RemoveEffectAt` (graphics-disabled build).
/// @param obj Candidate PostFX3D chain (ignored before trapping).
/// @param index Zero-based position in the chain (ignored before trapping).
/// @return `0` after raising the graphics-unavailable trap.
int8_t rt_postfx3d_remove_effect_at(void *obj, int64_t index) {
    (void)obj;
    (void)index;
    RT_GRAPHICS_TRAP_RET("PostFX3D.RemoveEffectAt: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `PostFX3D.SetDofFocus` (graphics-disabled build).
/// @param obj PostFX3D chain containing the DOF entry (ignored before trapping).
/// @param distance New non-negative focus distance (ignored before trapping).
/// @return `0` after raising the graphics-unavailable trap.
int8_t rt_postfx3d_set_dof_focus(void *obj, double distance) {
    (void)obj;
    (void)distance;
    RT_GRAPHICS_TRAP_RET("PostFX3D.SetDofFocus: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `PostFX3D.AddSharpen` (graphics-disabled build).
/// @param obj PostFX3D chain receiving the effect (ignored before trapping).
/// @param amount Edge gain clamped to `[0, 1]` (0 is identity) (ignored before trapping).
void rt_postfx3d_add_sharpen(void *obj, double amount) {
    (void)obj;
    (void)amount;
    RT_GRAPHICS_TRAP_VOID("PostFX3D.AddSharpen: graphics support not compiled in");
}

/* PostFXEffectKind stubs */

/// @brief Constant stub for `PostFXEffectKind.get_Bloom` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_BLOOM`.
int64_t rt_postfx3d_effect_kind_bloom(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_BLOOM;
}

/// @brief Constant stub for `PostFXEffectKind.get_Tonemap` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_TONEMAP`.
int64_t rt_postfx3d_effect_kind_tonemap(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_TONEMAP;
}

/// @brief Constant stub for `PostFXEffectKind.get_Fxaa` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_FXAA`.
int64_t rt_postfx3d_effect_kind_fxaa(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_FXAA;
}

/// @brief Constant stub for `PostFXEffectKind.get_ColorGrade` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_COLOR_GRADE`.
int64_t rt_postfx3d_effect_kind_color_grade(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_COLOR_GRADE;
}

/// @brief Constant stub for `PostFXEffectKind.get_Vignette` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_VIGNETTE`.
int64_t rt_postfx3d_effect_kind_vignette(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_VIGNETTE;
}

/// @brief Constant stub for `PostFXEffectKind.get_Ssao` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_SSAO`.
int64_t rt_postfx3d_effect_kind_ssao(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_SSAO;
}

/// @brief Constant stub for `PostFXEffectKind.get_Dof` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_DOF`.
int64_t rt_postfx3d_effect_kind_dof(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_DOF;
}

/// @brief Constant stub for `PostFXEffectKind.get_MotionBlur` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_MOTION_BLUR`.
int64_t rt_postfx3d_effect_kind_motion_blur(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_MOTION_BLUR;
}

/// @brief Constant stub for `PostFXEffectKind.get_Taa` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_TAA`.
int64_t rt_postfx3d_effect_kind_taa(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_TAA;
}

/// @brief Constant stub for `PostFXEffectKind.get_Ssr` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_SSR`.
int64_t rt_postfx3d_effect_kind_ssr(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_SSR;
}

/// @brief Constant stub for `PostFXEffectKind.get_AutoExposure` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_AUTO_EXPOSURE`.
int64_t rt_postfx3d_effect_kind_auto_exposure(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_AUTO_EXPOSURE;
}

/// @brief Constant stub for `PostFXEffectKind.get_ColorLut` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_COLOR_LUT`.
int64_t rt_postfx3d_effect_kind_color_lut(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_COLOR_LUT;
}

/// @brief Constant stub for `PostFXEffectKind.get_SunShafts` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_SUN_SHAFTS`.
int64_t rt_postfx3d_effect_kind_sun_shafts(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_SUN_SHAFTS;
}

/// @brief Constant stub for `PostFXEffectKind.get_Sharpen` (graphics-disabled build).
/// @details Effect kinds are backend-independent, so this returns the graphics-build
///          value rather than a fallback.
/// @return `VGFX3D_POSTFX_EFFECT_SHARPEN`.
int64_t rt_postfx3d_effect_kind_sharpen(void) {
    return (int64_t)VGFX3D_POSTFX_EFFECT_SHARPEN;
}
