//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d.h
// Purpose: Runtime bridge for Zanna.Graphics3D — Canvas3D, Mesh3D, Camera3D,
//   Material3D, and Light3D types. Provides 3D rendering via a software
//   rasterizer (Phase 1) with GPU backend abstraction (Phase 2+).
//
// Key invariants:
//   - All object pointers are opaque handles returned by *_new functions.
//   - Canvas3D.Begin/End must bracket all DrawMesh calls (no nesting).
//   - All math stays double precision at API boundary; rasterizer uses float.
//   - Counter-clockwise (CCW) winding is front-facing.
//
// Ownership/Lifetime:
//   - Canvas3D owns the 3D rendering context; GC finalizer cleans up.
//   - Mesh3D owns vertex/index arrays; GC finalizer frees them.
//   - Camera3D and Light3D contain scalar fields; Material3D retains texture/env references and
//     releases them from its GC finalizer.
//
// Links: plans/3d/01-software-renderer.md, src/runtime/graphics/common/rt_graphics.h,
//        docs/adr/0168-windowless-canvas3d-rendering.md,
//        docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"
#include "rt_textureasset3d.h"

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Canvas3D — 3D rendering surface
//=========================================================================

/** @name Canvas3D submission diagnostic status codes
 *
 * These values are returned by @ref rt_canvas3d_get_last_submission_status.
 * The status is sticky until @ref rt_canvas3d_reset_submission_diagnostics so
 * a later successful legacy `void` draw cannot erase evidence of a failure.
 * @{ */
#define RT_CANVAS3D_SUBMISSION_OK 0LL
#define RT_CANVAS3D_SUBMISSION_QUEUE_FAILURE 1LL
#define RT_CANVAS3D_SUBMISSION_SNAPSHOT_FAILURE 2LL
#define RT_CANVAS3D_SUBMISSION_RESOURCE_FAILURE 3LL
#define RT_CANVAS3D_SUBMISSION_INSTANCE_FAILURE 4LL
/** @} */

/// @brief Report whether the Canvas3D runtime is usable in this build.
/// @details Graphics-enabled builds return 1. Graphics-disabled stub builds return 0 so
///          programs can branch before calling stateful 3D APIs that would trap or no-op.
int8_t rt_canvas3d_is_available(void);
/// @brief Create a new 3D canvas window with the given title and pixel dimensions.
void *rt_canvas3d_new(rt_string title, int64_t w, int64_t h);
/// @brief Create a fullscreen 3D canvas at desktop resolution (no windowed flash).
void *rt_canvas3d_new_fullscreen(rt_string title);
/// @brief Create a windowless software Canvas3D bound to an explicit RenderTarget3D.
void *rt_canvas3d_new_offscreen(void *target);
/// @brief Windowless constructor that requests the platform GPU backend with software
///        fallback (ADR 0191). Not byte-deterministic across backends; probes and bakes
///        must keep using rt_canvas3d_new_offscreen.
void *rt_canvas3d_new_offscreen_accelerated(void *target);
/// @brief Report whether the canvas was created without a platform window.
int8_t rt_canvas3d_get_is_offscreen(void *obj);
/// @brief Resize the canvas and active backend output targets.
void rt_canvas3d_resize(void *obj, int64_t w, int64_t h);
/// @brief Clear the back buffer to the given RGB color (each channel 0.0–1.0).
void rt_canvas3d_clear(void *obj, double r, double g, double b);
/// @brief Begin a 3D draw pass with the given camera (must be paired with `_end`).
void rt_canvas3d_begin(void *obj, void *camera);
/// @brief Begin a view-model pass over the finished scene: keeps color, clears depth,
///        renders with an independent FOV (<= 0 keeps the camera FOV). Pair with `_end`.
void rt_canvas3d_begin_view_model(void *obj, void *camera, double fov_y_degrees);
/// @brief Begin a 2D screen-space overlay pass (orthographic, no depth test).
void rt_canvas3d_begin_2d(void *obj);
/// @brief Draw a screen-space filled rectangle inside the current 2D pass.
void rt_canvas3d_draw_rect_3d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);
/// @brief Draw screen-space text inside the current 2D pass.
void rt_canvas3d_draw_text_3d(void *canvas, int64_t x, int64_t y, rt_string text, int64_t color);
/// @brief Submit a mesh+transform+material draw inside the active 3D pass.
void rt_canvas3d_draw_mesh(void *obj, void *mesh, void *transform, void *material);
/// @brief Draw a mesh with a height-weighted per-vertex wind sway (foliage/canopies).
void rt_canvas3d_draw_mesh_wind(void *obj,
                                void *mesh,
                                void *transform,
                                void *material,
                                double dir_x,
                                double dir_z,
                                double strength,
                                double phase);
/// @brief End the current 3D or 2D pass and flush queued draws.
void rt_canvas3d_end(void *obj);
/// @brief Present the back buffer to the window (vsync-controlled by backend).
void rt_canvas3d_flip(void *obj);
/// @brief Pump events and update per-frame input state.
/// @return 1 while the canvas remains open, 0 after a close request or invalid handle.
int64_t rt_canvas3d_poll(void *obj);
/// @brief Dequeue one event type captured by the most recent Poll call, or 0 if none.
int64_t rt_canvas3d_poll_event(void *obj);
/// @brief True when the user has requested window close (clicked X or Alt-F4).
int8_t rt_canvas3d_should_close(void *obj);
/// @brief Toggle wireframe rendering for subsequent mesh draws.
void rt_canvas3d_set_wireframe(void *obj, int8_t enabled);
/// @brief Toggle backface culling (CCW = front-facing).
void rt_canvas3d_set_backface_cull(void *obj, int8_t enabled);
/// @brief Set a slope-scaled shadow bias used to reduce acne on glancing-angle casters.
/// @details The value is applied while rendering caster geometry into shadow maps. It complements
///   `rt_canvas3d_set_shadow_bias`, which biases the later shadow comparison. Higher values improve
///   stability for shallow slopes and coplanar caster triangles, while excessive values can visibly
///   detach shadows from geometry.
void rt_canvas3d_set_shadow_slope_bias(void *obj, double bias);
/// @brief Set how dark fully-occluded texels get (0 = disabled, 1 = fully black; default 0.85).
void rt_canvas3d_set_shadow_strength(void *obj, double strength);
/// @brief Select the shadow PCF tier (0 = 4 taps, 1 = 8, 2 = 16 rotated-Poisson taps).
void rt_canvas3d_set_shadow_quality(void *obj, int64_t quality);
/// @brief Get the canvas width in pixels.
int64_t rt_canvas3d_get_width(void *obj);
/// @brief Get the canvas height in pixels.
int64_t rt_canvas3d_get_height(void *obj);
/// @brief Get the backing window width, ignoring any bound render target.
int64_t rt_canvas3d_get_window_width(void *obj);
/// @brief Get the backing window height, ignoring any bound render target.
int64_t rt_canvas3d_get_window_height(void *obj);
/// @brief Switch the canvas window between fullscreen and windowed mode (non-zero = fullscreen).
void rt_canvas3d_set_fullscreen(void *obj, int8_t enabled);
/// @brief Report whether the canvas window is currently fullscreen (1) or windowed (0).
int8_t rt_canvas3d_is_fullscreen(void *obj);
/// @brief Flip the canvas window between fullscreen and windowed mode.
void rt_canvas3d_toggle_fullscreen(void *obj);
/// @brief Get the active output width (window, or current render target when bound).
int64_t rt_canvas3d_get_active_output_width(void *obj);
/// @brief Get the active output height (window, or current render target when bound).
int64_t rt_canvas3d_get_active_output_height(void *obj);
/// @brief Get the rolling-average FPS measured over recent Poll/Flip/synthetic timing samples.
int64_t rt_canvas3d_get_fps(void *obj);
/// @brief Get milliseconds since the latest timing sample; Poll normally samples live clocks.
int64_t rt_canvas3d_get_delta_time(void *obj);
/// @brief Get seconds since the latest timing sample; Poll normally samples live clocks.
double rt_canvas3d_get_delta_time_sec(void *obj);
/// @brief Cap the per-frame delta time (smooths spikes after pause/breakpoint).
void rt_canvas3d_set_dt_max(void *obj, int64_t max_ms);
/// @brief Apply a backend-safe quality profile (0 performance, 1 balanced, 2 cinematic).
void rt_canvas3d_set_quality(void *obj, int64_t quality);
/// @brief Last quality profile requested through SetQuality.
int64_t rt_canvas3d_get_quality_requested(void *obj);
/// @brief Quality profile actually active after backend fallback.
int64_t rt_canvas3d_get_quality_active(void *obj);
/// @brief True when the active quality profile was degraded for backend safety.
int8_t rt_canvas3d_get_quality_fallback(void *obj);
/// @brief Reason for the last quality fallback, or an empty string.
rt_string rt_canvas3d_get_quality_fallback_reason(void *obj);
/// @brief Select live, synthetic, or live+synthetic input for this canvas (0, 1, or 2).
void rt_canvas3d_set_input_source(void *obj, int64_t mode);
/// @brief Queue a synthetic keyboard transition for the next synthetic input frame.
void rt_canvas3d_push_synthetic_key(void *obj, int64_t key, int8_t down);
/// @brief Queue a synthetic mouse sample for the next synthetic input frame.
void rt_canvas3d_push_synthetic_mouse(
    void *obj, double dx, double dy, int64_t buttons, double wheel);
/// @brief Drop queued synthetic input and release keys/buttons held by the synthetic source.
void rt_canvas3d_clear_synthetic_input(void *obj);
/// @brief Select live wall-clock or fixed synthetic delta-time source (0 or 1).
void rt_canvas3d_set_clock_source(void *obj, int64_t mode);
/// @brief Set the fixed synthetic delta time in seconds.
void rt_canvas3d_set_synthetic_delta_time_sec(void *obj, double dt);
/// @brief Advance one deterministic input/timing frame without pumping platform events.
void rt_canvas3d_advance_synthetic_frame(void *obj);
/// @brief Bind or clear a Light3D slot; the canvas retains the assigned light until replaced.
void rt_canvas3d_set_light(void *obj, int64_t index, void *light);
/// @brief Clear all retained canvas light slots.
void rt_canvas3d_clear_lights(void *obj);
/// @brief Install a conservative key/fill/ambient setup for readable default scenes.
void rt_canvas3d_set_default_lighting(void *obj);
/// @brief Count currently assigned canvas light slots.
int64_t rt_canvas3d_get_light_count(void *obj);
/// @brief Try to set clustered/forward+ lighting without trapping.
///
/// @return 1 when the requested state is applied, or 0 when enabling is blocked
///         by the active backend, an environment kill switch, or an invalid canvas.
int8_t rt_canvas3d_try_set_clustered_lighting(void *obj, int8_t enabled);
/// @brief Enable clustered/forward+ lighting when the backend advertises support.
///
/// Unlike rt_canvas3d_try_set_clustered_lighting(), this strict property setter
/// traps when enabling is requested on an unsupported backend.
void rt_canvas3d_set_clustered_lighting(void *obj, int8_t enabled);
/// @brief Report whether clustered/forward+ lighting is currently enabled.
int8_t rt_canvas3d_get_clustered_lighting(void *obj);
/// @brief Current maximum active light count for the selected lighting path.
int64_t rt_canvas3d_get_max_active_lights(void *obj);
/// @brief Set the ambient light color applied to all lit materials.
void rt_canvas3d_set_ambient(void *obj, double r, double g, double b);
/// @brief Enable/disable image-based lighting from the canvas skybox (PBR draws
///   without an explicit material env map use SH irradiance + prefiltered specular).
void rt_canvas3d_set_ibl_enabled(void *obj, int8_t enabled);
/// @brief True when image-based lighting is enabled for this canvas.
int8_t rt_canvas3d_get_ibl_enabled(void *obj);
/// @brief Scale the environment lighting contribution (default 1.0, clamped to [0, 8]).
void rt_canvas3d_set_ibl_intensity(void *obj, double intensity);
/// @brief Current environment lighting intensity scale.
double rt_canvas3d_get_ibl_intensity(void *obj);
/// @brief Draw a debug 3D line between two Vec3 points.
void rt_canvas3d_draw_line3d(void *obj, void *from, void *to, int64_t color);
/// @brief Raw-array form of rt_canvas3d_draw_line3d: @p from and @p to are
///   double[3] world-space coordinates instead of boxed Vec3 objects.
void rt_canvas3d_draw_line3d_raw(void *obj, const double *from, const double *to, int64_t color);
/// @brief Draw a debug 3D point (square) at the given position with pixel size.
void rt_canvas3d_draw_point3d(void *obj, void *pos, int64_t color, int64_t size);
/// @brief Get the active backend name ("d3d11", "metal", "opengl", or "software").
rt_string rt_canvas3d_get_backend(void *obj);
/// @brief Return true when Canvas3D fell back from the selected GPU backend to software.
int8_t rt_canvas3d_get_backend_fallback(void *obj);
/// @brief Human-readable reason for BackendFallback, or an empty string when no fallback happened.
rt_string rt_canvas3d_get_backend_fallback_reason(void *obj);

#define RT_CANVAS3D_BACKEND_CAP_SOFTWARE 0x0001LL
#define RT_CANVAS3D_BACKEND_CAP_GPU 0x0002LL
#define RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET 0x0004LL
#define RT_CANVAS3D_BACKEND_CAP_WINDOW_READBACK 0x0008LL
#define RT_CANVAS3D_BACKEND_CAP_SHADOWS 0x0010LL
#define RT_CANVAS3D_BACKEND_CAP_SKYBOX 0x0020LL
#define RT_CANVAS3D_BACKEND_CAP_HARDWARE_INSTANCING 0x0040LL
#define RT_CANVAS3D_BACKEND_CAP_POSTFX 0x0080LL
#define RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX 0x0100LL
#define RT_CANVAS3D_BACKEND_CAP_POSTFX_OVERLAY 0x0200LL
#define RT_CANVAS3D_BACKEND_CAP_FINAL_SCREENSHOT 0x0400LL
#define RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX_OVERLAY 0x0800LL
#define RT_CANVAS3D_BACKEND_CAP_CLUSTERED_LIGHTING 0x1000LL
#define RT_CANVAS3D_BACKEND_CAP_SHADOW_CSM 0x2000LL
#define RT_CANVAS3D_BACKEND_CAP_OCCLUSION 0x4000LL
#define RT_CANVAS3D_BACKEND_CAP_HLOD 0x8000LL
#define RT_CANVAS3D_BACKEND_CAP_BC7 RT_TEXTUREASSET3D_BACKEND_CAP_BC7
#define RT_CANVAS3D_BACKEND_CAP_ASTC RT_TEXTUREASSET3D_BACKEND_CAP_ASTC
#define RT_CANVAS3D_BACKEND_CAP_ETC2 RT_TEXTUREASSET3D_BACKEND_CAP_ETC2
#define RT_CANVAS3D_BACKEND_CAP_ANISOTROPY 0x80000LL
#define RT_CANVAS3D_BACKEND_CAP_PBR 0x100000LL
#define RT_CANVAS3D_BACKEND_CAP_NORMAL_MAPS 0x200000LL
#define RT_CANVAS3D_BACKEND_CAP_ALPHA_MASK 0x400000LL
#define RT_CANVAS3D_BACKEND_CAP_MORPH_TARGETS 0x800000LL
#define RT_CANVAS3D_BACKEND_CAP_SKINNING 0x1000000LL
#define RT_CANVAS3D_BACKEND_CAP_TERRAIN_SPLAT 0x2000000LL
#define RT_CANVAS3D_BACKEND_CAP_BC1 RT_TEXTUREASSET3D_BACKEND_CAP_BC1
#define RT_CANVAS3D_BACKEND_CAP_BC3 RT_TEXTUREASSET3D_BACKEND_CAP_BC3
#define RT_CANVAS3D_BACKEND_CAP_BC4 RT_TEXTUREASSET3D_BACKEND_CAP_BC4
#define RT_CANVAS3D_BACKEND_CAP_BC5 RT_TEXTUREASSET3D_BACKEND_CAP_BC5
#define RT_CANVAS3D_BACKEND_CAP_HDR_SCENE 0x40000000LL
#define RT_CANVAS3D_BACKEND_CAP_TAA 0x80000000LL
#define RT_CANVAS3D_BACKEND_CAP_SOFT_PARTICLES 0x100000000LL
#define RT_CANVAS3D_BACKEND_CAP_SSR 0x200000000LL
/* Backend implements the instanced-draw hook (any backend, incl. software).
 * HARDWARE_INSTANCING additionally requires a GPU backend. */
#define RT_CANVAS3D_BACKEND_CAP_INSTANCING 0x400000000LL
/* Omnidirectional point-light shadows (requires the extended shadow slots:
 * a cube light consumes six tiles beyond the four classic slots). */
#define RT_CANVAS3D_BACKEND_CAP_SHADOW_POINT 0x800000000LL
/* The full post-FX chain (incl. SSAO/DOF/MotionBlur/TAA/SSR) executes on this
 * backend — GPU-accelerated or via the CPU parity implementations. */
#define RT_CANVAS3D_BACKEND_CAP_POSTFX_FULL 0x1000000000LL

/// @brief Return a 64-bit RT_CANVAS3D_BACKEND_CAP_* bitmask for the active backend.
int64_t rt_canvas3d_get_backend_capabilities(void *obj);
/// @brief Return whether the active backend supports a named capability.
int8_t rt_canvas3d_backend_supports(void *obj, rt_string capability);
/// @brief Number of main 3D draw submissions queued by the latest ended frame.
/// @brief Force all skinned draws through the CPU skinning path (debug/bisection).
void rt_canvas3d_set_force_cpu_skinning(void *obj, int8_t enabled);
/// @brief Lifetime count of skinned draws routed to GPU vertex-shader skinning.
int64_t rt_canvas3d_get_gpu_skinned_draw_count(void *obj);
/// @brief Lifetime bone-palette bytes handed to the backend for GPU skinning.
int64_t rt_canvas3d_get_skinning_upload_bytes(void *obj);
int64_t rt_canvas3d_get_draw_count(void *obj);
/// @brief Per-pass draw submissions for the latest frame (Game3D.RenderPass ids).
int64_t rt_canvas3d_pass_draw_count(void *obj, int64_t pass);
/// @brief Per-pass instance tally for the latest frame (instanced draws expanded).
int64_t rt_canvas3d_pass_instance_count(void *obj, int64_t pass);
/// @brief Number of draw submissions rejected by visibility culling in the latest scene draw.
int64_t rt_canvas3d_get_occluded_draw_count(void *obj);
/// @brief Enable exponential height fog (pools below baseHeight; combines with distance fog).
/// @brief Configure height-fog sun inscattering (amount 0 disables; default off).
void rt_canvas3d_set_height_fog_sun(
    void *obj, double r, double g, double b, double power, double amount);
/// @brief Disable height fog only (distance fog remains).
void rt_canvas3d_clear_height_fog(void *obj);
/// @brief True while exponential height fog is enabled.
int8_t rt_canvas3d_get_height_fog_enabled(void *obj);
void rt_canvas3d_set_height_fog(
    void *obj, double base_height, double falloff, double density, double blend);
/// @brief Draw anti-aliased screen-space text at an arbitrary scale.
void rt_canvas3d_draw_text2d_aa(
    void *obj, int64_t x, int64_t y, rt_string text, int64_t color, double scale);
/// @brief Width in pixels of DrawText2DAA output for @p text at @p scale.
int64_t rt_canvas3d_measure_text2d_aa(void *obj, rt_string text, double scale);
/// @brief Draw a 9-slice image: corners unscaled, edges axis-stretched, center stretched.
void rt_canvas3d_draw_image2d_nine_slice(void *obj,
                                         int64_t x,
                                         int64_t y,
                                         int64_t w,
                                         int64_t h,
                                         void *pixels,
                                         int64_t inset_l,
                                         int64_t inset_t,
                                         int64_t inset_r,
                                         int64_t inset_b);
/// @brief Instances drawn via the per-draw instanced fallback this frame.
int64_t rt_canvas3d_get_instanced_fallback_count(void *obj);
/// @brief Instances skipped because a chunked fallback queue reservation actually failed.
int64_t rt_canvas3d_get_instanced_fallback_dropped_count(void *obj);
/// @brief Return the sticky status code for the most recently recorded draw-submission failure.
/// @details A zero result means no failure has been recorded since construction or the latest
///   diagnostic reset. Successful submissions do not clear a non-zero status; see the
///   `RT_CANVAS3D_SUBMISSION_*` constants for failure classes.
/// @param obj Canvas3D handle. Invalid handles return @ref RT_CANVAS3D_SUBMISSION_OK.
/// @return One of the `RT_CANVAS3D_SUBMISSION_*` status constants.
int64_t rt_canvas3d_get_last_submission_status(void *obj);
/// @brief Return the saturating number of submission failures recorded since construction/reset.
/// @param obj Canvas3D handle. Invalid handles return zero.
/// @return Non-negative failure count, saturated at `INT64_MAX`.
int64_t rt_canvas3d_get_submission_failure_count(void *obj);
/// @brief Clear both sticky Canvas3D submission diagnostics.
/// @details This resets `LastSubmissionStatus` to zero and `SubmissionFailureCount` to zero without
///   changing queued commands, frame state, or backend resources.
/// @param obj Canvas3D handle; invalid handles are ignored.
void rt_canvas3d_reset_submission_diagnostics(void *obj);
/// @brief Window/input events dropped from Canvas3D's public PollEvent ring since creation.
int64_t rt_canvas3d_get_event_drop_count(void *obj);
/// @brief Mesh snapshot bytes copied by the current/latest frame.
int64_t rt_canvas3d_get_mesh_snapshot_bytes(void *obj);
/// @brief Mesh snapshot allocation or budget denials in the current/latest frame.
int64_t rt_canvas3d_get_mesh_snapshot_drop_count(void *obj);
/// @brief Requested mesh snapshot bytes denied in the current/latest frame.
int64_t rt_canvas3d_get_mesh_snapshot_dropped_bytes(void *obj);
/// @brief Per-frame mesh snapshot byte budget used by deferred geometry copies.
int64_t rt_canvas3d_get_mesh_snapshot_budget_bytes(void *obj);
/// @brief Set the shadow-light slot budget (1..max slots).
void rt_canvas3d_set_shadow_budget(void *obj, int64_t budget);
/// @brief Shadow slots rendered in the latest frame (cascades included).
int64_t rt_canvas3d_get_shadow_slots_used(void *obj);
/// @brief Shadow-requesting lights denied a slot in the latest frame.
int64_t rt_canvas3d_get_shadow_requests_dropped(void *obj);
/// @brief Set the per-cluster light-index capacity (8..64, default 64).
void rt_canvas3d_set_cluster_light_budget(void *obj, int64_t budget);
/// @brief Lifetime count of cluster light-index entries truncated by capacity.
int64_t rt_canvas3d_get_cluster_overflow_count(void *obj);
/// @brief Enabled lights truncated by the forward-path light limit this frame.
int64_t rt_canvas3d_get_dropped_light_count(void *obj);
/// @brief Number of draw submissions rejected by CPU frustum culling in the latest ended frame.
int64_t rt_canvas3d_get_frustum_culled_draw_count(void *obj);
/// @brief Number of opaque draw submissions rejected by the CPU occlusion grid in the latest frame.
int64_t rt_canvas3d_get_cpu_occluded_draw_count(void *obj);
/// @brief Number of opaque draw candidates tested by the CPU occlusion grid in the latest frame.
int64_t rt_canvas3d_get_occlusion_candidate_count(void *obj);
/// @brief Texture payload bytes uploaded to backend storage in the latest ended frame.
int64_t rt_canvas3d_get_texture_upload_bytes(void *obj);
/// @brief Latest completed backend GPU frame time in microseconds, or 0 when unsupported.
int64_t rt_canvas3d_get_frame_gpu_time_us(void *obj);
/// @brief CPU milliseconds one render stage took last frame (0=shadow, 1=main,
///   2=overlay, 3=backend end-of-frame).
double rt_canvas3d_get_pass_cpu_ms(void *obj, int64_t pass);
/// @brief Number of PassCpuMs stages.
int64_t rt_canvas3d_get_pass_count(void *obj);
/// @brief Backend draw submissions issued since the latest public Begin/Begin2D.
int64_t rt_canvas3d_get_draws_submitted(void *obj);
/// @brief World-AABB transform computations performed since the latest public Begin/Begin2D.
int64_t rt_canvas3d_get_aabb_transforms(void *obj);
/// @brief Stable deferred sort passes run since the latest public Begin/Begin2D.
int64_t rt_canvas3d_get_sort_passes(void *obj);
/// @brief Material/backend state-group transitions observed during backend submission.
int64_t rt_canvas3d_get_backend_state_changes(void *obj);
/// @brief Set the backend texture upload byte budget for each frame; negative disables the budget.
void rt_canvas3d_set_texture_upload_budget(void *obj, int64_t bytes);
/// @brief Texture payload bytes still waiting for backend texture upload budget.
int64_t rt_canvas3d_get_texture_upload_pending_bytes(void *obj);
/// @brief Enable or disable automatic TextureAsset3D mip-residency streaming (default off).
void rt_canvas3d_set_texture_streaming(void *obj, int8_t enabled);
/// @brief Bias streaming's desired mip level; positive drops more detail. Clamped to [-16, 16].
void rt_canvas3d_set_texture_streaming_bias(void *obj, double bias);
/// @brief Lifetime count of resident-window demotions applied by texture streaming.
int64_t rt_canvas3d_get_texture_streaming_demotions(void *obj);
/// @brief Successful draw calls emitted by the active backend since canvas creation.
int64_t rt_canvas3d_get_backend_draw_calls(void *obj);
/// @brief Draw commands rejected inside the active backend since canvas creation.
int64_t rt_canvas3d_get_backend_dropped_draws(void *obj);
/// @brief Static mesh cache hits observed by the active backend since canvas creation.
int64_t rt_canvas3d_get_backend_mesh_cache_hits(void *obj);
/// @brief Static mesh cache misses observed by the active backend since canvas creation.
int64_t rt_canvas3d_get_backend_mesh_cache_misses(void *obj);
/// @brief Transient mesh uploads performed by the active backend since canvas creation.
int64_t rt_canvas3d_get_backend_mesh_stream_uploads(void *obj);
/// @brief Fallback texture binds observed by the active backend since canvas creation.
int64_t rt_canvas3d_get_backend_texture_fallback_binds(void *obj);
/// @brief Active backend present path: 0 unknown, 1 direct GPU drawable, 2 offscreen resolve.
int64_t rt_canvas3d_get_backend_present_path(void *obj);
/// @brief Capture the current back-buffer contents into a fresh Pixels object.
void *rt_canvas3d_screenshot(void *obj);
/// @brief Try to copy the current back buffer into an existing same-size Pixels object.
/// @details Reuses canvas-owned GPU staging storage after warm-up and bumps Pixels generation.
int8_t rt_canvas3d_try_copy_screenshot_to(void *obj, void *pixels);
/// @brief Begin recording a final overlay pass composited after post-FX during finalization.
void rt_canvas3d_begin_overlay(void *obj);
/// @brief Finish recording the final overlay pass started by BeginOverlay.
void rt_canvas3d_end_overlay(void *obj);
/// @brief Discard any recorded final overlay commands for the current frame.
void rt_canvas3d_clear_overlay(void *obj);
/// @brief Apply post-FX and final overlay exactly once, without presenting.
void rt_canvas3d_finalize_frame(void *obj);
/// @brief Capture finalized pixels, finalizing first if needed.
void *rt_canvas3d_screenshot_final(void *obj);
/// @brief Finalize if needed, then try to copy into an existing same-size Pixels object.
int8_t rt_canvas3d_try_copy_screenshot_final_to(void *obj, void *pixels);
/// @brief Return whether the current frame has already been finalized.
int8_t rt_canvas3d_get_frame_finalized(void *obj);

//=========================================================================
// CubeMap3D — 6-face cube map for skybox + reflections
//=========================================================================

/// @brief Build a cubemap from six Pixels faces (px = +X, nx = -X, etc.).
void *rt_cubemap3d_new(void *px, void *nx, void *py, void *ny, void *pz, void *nz);
/// @brief Load a Radiance .hdr equirectangular panorama as a CubeMap3D
///   (linear decode, engine face projection, Reinhard range compression at
///   @p exposure; exposure <= 0 defaults to 1).
void *rt_cubemap3d_load_hdr_panorama(rt_string path, double exposure);
/// @brief Bind a cubemap as the scene skybox (rendered behind opaque geometry).
void rt_canvas3d_set_skybox(void *canvas, void *cubemap);
/// @brief Remove the skybox; subsequent clears use the regular clear color.
void rt_canvas3d_clear_skybox(void *canvas);
/// @brief Bind a cubemap as a material's environment reflection map.
void rt_material3d_set_env_map(void *obj, void *cubemap);
/// @brief Set how strongly the material reflects its env map (0.0–1.0).
void rt_material3d_set_reflectivity(void *obj, double r);
/// @brief Read the current reflectivity scalar.
double rt_material3d_get_reflectivity(void *obj);

//=========================================================================
// RenderTarget3D — offscreen rendering target
//=========================================================================

/// @brief Allocate an offscreen render target of the given pixel dimensions.
void *rt_rendertarget3d_new(int64_t width, int64_t height);
/// @brief Allocate an HDR offscreen render target with RGBA16F color storage.
void *rt_rendertarget3d_new_hdr(int64_t width, int64_t height);
/// @brief Width of the render target in pixels.
int64_t rt_rendertarget3d_get_width(void *obj);
/// @brief Height of the render target in pixels.
int64_t rt_rendertarget3d_get_height(void *obj);
/// @brief Return non-zero when the render target stores HDR color internally.
int32_t rt_rendertarget3d_get_is_hdr(void *obj);
/// @brief Get a Pixels view of the render target's color attachment (CPU readback).
void *rt_rendertarget3d_as_pixels(void *obj);
/// @brief Allocation-free readback into an existing same-size Pixels buffer.
void rt_rendertarget3d_copy_to(void *obj, void *pixels);

/// @brief Redirect subsequent draws to @p target instead of the swapchain.
void rt_canvas3d_set_render_target(void *canvas, void *target);
/// @brief Restore the swapchain as the active draw target.
void rt_canvas3d_reset_render_target(void *canvas);

//=========================================================================
// Mesh3D — 3D mesh with vertices and triangle indices
//=========================================================================

/// @brief Allocate an empty Mesh3D (no vertices, no triangles).
void *rt_mesh3d_new(void);
/// @brief Reset vertex and index counts to 0 without freeing the backing arrays.
void rt_mesh3d_clear(void *obj);
/// @brief Reserve backing storage for at least vertex_count vertices and triangle_count triangles.
void rt_mesh3d_reserve(void *obj, int64_t vertex_count, int64_t triangle_count);
/// @brief Build a unit-cube-style box mesh of size (sx, sy, sz) with normals and UVs.
void *rt_mesh3d_new_box(double sx, double sy, double sz);
/// @brief Build a UV-sphere mesh with the given radius and longitude segment count.
void *rt_mesh3d_new_sphere(double radius, int64_t segments);
/// @brief Build a flat XZ plane of size (sx, sz) facing +Y with a single quad.
void *rt_mesh3d_new_plane(double sx, double sz);
/// @brief Build a cylinder of radius @p r and height @p h with @p segments around the axis.
void *rt_mesh3d_new_cylinder(double r, double h, int64_t segments);
/// @brief Load a triangle mesh from a Wavefront .obj file (positions/normals/UVs).
void *rt_mesh3d_from_obj(rt_string path);
/// @brief Load a triangle mesh from a binary STL file (positions + per-face normals).
void *rt_mesh3d_from_stl(rt_string path);
/// @brief Number of unique vertices currently in the mesh.
int64_t rt_mesh3d_get_vertex_count(void *obj);
/// @brief Number of triangles currently in the mesh (== indices / 3).
int64_t rt_mesh3d_get_triangle_count(void *obj);
/// @brief Read one vertex's position/normal/uv (internal readback; 1 on success).
int8_t rt_mesh3d_get_vertex_raw(
    void *obj, int64_t index, double out_pos[3], double out_normal[3], double out_uv[2]);
/// @brief Read one triangle's vertex indices (internal readback; 1 on success).
int8_t rt_mesh3d_get_triangle_raw(void *obj, int64_t triangle, int64_t out_indices[3]);
/// @brief True when the mesh payload is resident and drawable.
int8_t rt_mesh3d_get_resident(void *obj);
/// @brief Mark the mesh payload resident/nonresident without releasing the Mesh3D handle.
void rt_mesh3d_set_resident(void *obj, int8_t resident);
/// @brief Estimated bytes for the currently resident vertex/index payload.
int64_t rt_mesh3d_get_resident_bytes(void *obj);
/// @brief Opt the mesh into the compact 48-byte GPU static-cache vertex encoding (R20).
void rt_mesh3d_set_compact_streams(void *obj, int8_t enabled);
/// @brief Whether the mesh opted into the compact GPU vertex-stream encoding.
int8_t rt_mesh3d_get_compact_streams(void *obj);
/// @brief Estimated retained CPU vertex/index bytes regardless of resident draw state.
int64_t rt_mesh3d_get_retained_bytes(void *obj);
/// @brief Free auxiliary CPU geometry (double positions, normal scratch) a
///   finished static mesh no longer needs; returns bytes released.
int64_t rt_mesh3d_release_cpu_scratch(void *obj);
/// @brief Append a vertex with position, normal, and UV.
void rt_mesh3d_add_vertex(
    void *obj, double x, double y, double z, double nx, double ny, double nz, double u, double v);
/// @brief Append a triangle by referencing three previously-added vertex indices (CCW = front).
void rt_mesh3d_add_triangle(void *obj, int64_t v0, int64_t v1, int64_t v2);

/* Non-trapping validity probe for AddTriangle: returns 1 when (v0,v1,v2) are distinct,
 * in-range indices whose positions form a non-degenerate face under the SAME
 * scale-relative area epsilon AddTriangle enforces. Asset loaders use this to skip
 * degenerate source triangles instead of trapping mid-import. */
int rt_mesh3d_triangle_indices_valid(void *obj, int64_t v0, int64_t v1, int64_t v2);
/// @brief Recompute smooth per-vertex normals from triangle face normals (overwrites existing).
void rt_mesh3d_recalc_normals(void *obj);
/// @brief Deep copy the mesh (independent storage; safe to mutate the clone).
void *rt_mesh3d_clone(void *obj);
/// @brief Transform every vertex position (and rotate normals) by the given Mat4.
void rt_mesh3d_transform(void *obj, void *mat4);
/// @brief Compute per-vertex tangent vectors from UVs (required for normal mapping).
void rt_mesh3d_calc_tangents(void *obj);

//=========================================================================
// Camera3D — perspective camera with view/projection matrices
//=========================================================================

/// @brief Create a perspective camera (FOV in degrees, aspect = width/height, near/far clip
/// planes).
void *rt_camera3d_new(double fov, double aspect, double near_val, double far_val);
/// @brief Create a perspective camera from a horizontal field of view in degrees.
/// @details Converts @p horizontal_fov to the vertical aperture used by the renderer's projection
///   matrix using @p aspect. This is useful for first-person/open-world cameras where designers
///   usually author FOV horizontally and a vertical interpretation would look too wide on
///   widescreen displays.
void *rt_camera3d_new_horizontal_fov(double horizontal_fov,
                                     double aspect,
                                     double near_val,
                                     double far_val);
/// @brief Create an orthographic camera (vertical world-units, aspect, near/far).
void *rt_camera3d_new_ortho(double size, double aspect, double near_val, double far_val);
/// @brief True if the camera was created via `_new_ortho` (no perspective foreshortening).
int8_t rt_camera3d_is_ortho(void *cam);
/// @brief Switch between perspective and orthographic projection, preserving both parameter sets.
void rt_camera3d_set_is_ortho(void *cam, int8_t is_ortho);
/// @brief Get the orthographic view-volume half-height retained by the camera.
double rt_camera3d_get_ortho_size(void *cam);
/// @brief Set the orthographic view-volume half-height retained by the camera.
void rt_camera3d_set_ortho_size(void *cam, double size);
/// @brief Aim the camera at a target point with an explicit up direction.
void rt_camera3d_look_at(void *obj, void *eye, void *target, void *up);
/// @brief Position the camera on a sphere around @p target at the given yaw/pitch in degrees.
void rt_camera3d_orbit(void *obj, void *target, double distance, double yaw, double pitch);
/// @brief Get the field of view in degrees (perspective cameras only).
double rt_camera3d_get_fov(void *obj);
/// @brief Set the field of view in degrees.
void rt_camera3d_set_fov(void *obj, double fov);
/// @brief Set the perspective camera's field of view using horizontal degrees for its aspect.
/// @details Orthographic cameras ignore the call. Perspective cameras convert the supplied
///   horizontal aperture through the camera's current aspect ratio and rebuild their projection
///   immediately.
void rt_camera3d_set_horizontal_fov(void *obj, double horizontal_fov);
/// @brief Get the near clip-plane distance.
double rt_camera3d_get_near_plane(void *obj);
/// @brief Get the sanitized near clip-plane distance used for projection/rendering.
/// @details This read-only value reflects Camera3D's precision guardrails, including the
///   enforced near/far ratio cap that prevents depth-buffer precision loss.
double rt_camera3d_get_effective_near_plane(void *obj);
/// @brief Set the near clip-plane distance.
void rt_camera3d_set_near_plane(void *obj, double near_plane);
/// @brief Get the far clip-plane distance.
double rt_camera3d_get_far_plane(void *obj);
/// @brief Get the sanitized far clip-plane distance used for projection/rendering.
/// @details The returned value is the clip plane after Camera3D validates finite values and
///   resolves degenerate near/far ranges.
double rt_camera3d_get_effective_far_plane(void *obj);
/// @brief Set the far clip-plane distance.
void rt_camera3d_set_far_plane(void *obj, double far_plane);
/// @brief Get the camera world-space position as a Vec3.
void *rt_camera3d_get_position(void *obj);
/// @brief Move the camera to the given world-space position (Vec3).
void rt_camera3d_set_position(void *obj, void *pos);
/// @brief Get the unit forward vector (the direction the camera is facing).
void *rt_camera3d_get_forward(void *obj);
/// @brief Get the unit right vector (perpendicular to forward and up).
void *rt_camera3d_get_right(void *obj);
/// @brief Return a normalized world-space picking direction for screen pixel (sx, sy).
/// Combine it with `ScreenToRayOrigin()` for perspective and orthographic picking.
/// Orthographic cameras return their forward direction (parallel rays).
/// @brief Project a world point to pixels; returns 1 when in front of the camera.
int8_t rt_camera3d_world_to_screen(void *obj,
                                   double x,
                                   double y,
                                   double z,
                                   int64_t sw,
                                   int64_t sh,
                                   double *out_sx,
                                   double *out_sy);
/// @brief VM-facing WorldToScreen: Vec3(pixelX, pixelY, visible ? 1 : 0).
void *rt_camera3d_world_to_screen_vec(void *obj, void *point, int64_t sw, int64_t sh);
void *rt_camera3d_screen_to_ray(void *obj, int64_t sx, int64_t sy, int64_t sw, int64_t sh);
/// @brief Return the world-space origin for a screen-space picking ray.
void *rt_camera3d_screen_to_ray_origin(void *obj, int64_t sx, int64_t sy, int64_t sw, int64_t sh);

//=========================================================================
// Material3D — surface appearance (color, texture, shininess)
//=========================================================================

#define RT_MATERIAL3D_WORKFLOW_LEGACY 0
#define RT_MATERIAL3D_WORKFLOW_PBR 1

#define RT_MATERIAL3D_ALPHA_MODE_OPAQUE 0
#define RT_MATERIAL3D_ALPHA_MODE_MASK 1
#define RT_MATERIAL3D_ALPHA_MODE_BLEND 2
#define RT_MATERIAL3D_SHADOW_MODE_AUTO 0
#define RT_MATERIAL3D_SHADOW_MODE_NONE 1
#define RT_MATERIAL3D_SHADOW_MODE_CAST 2

/// @brief Create a default white legacy-shaded material.
void *rt_material3d_new(void);
/// @brief Create a flat-color legacy material with the given diffuse color.
void *rt_material3d_new_color(double r, double g, double b);
/// @brief Create a textured legacy material from Pixels, TextureAsset3D, or RenderTarget3D.
void *rt_material3d_new_textured(void *pixels);
/// @brief Create a PBR-workflow material with the given base color (default metallic=0,
/// roughness=0.5).
void *rt_material3d_new_pbr(double r, double g, double b);
/// @brief Deep copy a material (independent storage and texture refs).
void *rt_material3d_clone(void *obj);
/// @brief Create a per-instance variant sharing the same shader but with mutable params.
void *rt_material3d_make_instance(void *obj);
/// @brief Set the diffuse / base color (legacy or PBR depending on workflow).
void rt_material3d_set_color(void *obj, double r, double g, double b);
/// @brief Read the diffuse / base color as a Vec3.
void *rt_material3d_get_color(void *obj);
/// @brief Borrow the current base-color/albedo map as decoded Pixels, or NULL.
void *rt_material3d_get_texture_pixels(void *obj);
/// @brief Borrow the current tangent-space normal map as decoded Pixels, or NULL.
void *rt_material3d_get_normal_map_pixels(void *obj);
/// @brief Borrow the current legacy specular map as decoded Pixels, or NULL.
void *rt_material3d_get_specular_map_pixels(void *obj);
/// @brief Borrow the current emissive map as decoded Pixels, or NULL.
void *rt_material3d_get_emissive_map_pixels(void *obj);
/// @brief Borrow the current packed metallic/roughness map as decoded Pixels, or NULL.
void *rt_material3d_get_metallic_roughness_map_pixels(void *obj);
/// @brief Borrow the current ambient-occlusion map as decoded Pixels, or NULL.
void *rt_material3d_get_ao_map_pixels(void *obj);
/// @brief Borrow the current baked lightmap as decoded Pixels, or NULL.
void *rt_material3d_get_lightmap_pixels(void *obj);
/// @brief Set the diffuse texture (legacy workflow); aliased to albedo for PBR.
void rt_material3d_set_texture(void *obj, void *pixels);
/// @brief Bind a RenderTarget3D's live contents as the albedo texture (auto-refreshing).
void rt_material3d_set_albedo_render_target(void *obj, void *target);
/// @brief Detach a render-target albedo binding.
void rt_material3d_clear_albedo_render_target(void *obj);
/// @brief Bind a RenderTarget3D's live contents as the emissive map.
void rt_material3d_set_emissive_render_target(void *obj, void *target);
/// @brief Set the PBR albedo (base color) texture.
void rt_material3d_set_albedo_map(void *obj, void *pixels);
/// @brief Set the legacy specular shininess exponent (higher = sharper highlights).
void rt_material3d_set_shininess(void *obj, double s);
/// @brief Mark the material as unlit (skip lighting calculations entirely).
void rt_material3d_set_unlit(void *obj, int8_t unlit);
void rt_material3d_set_ssr_enabled(void *obj, int8_t enabled);
int8_t rt_material3d_get_ssr_enabled(void *obj);
/// @brief True if unlit mode is enabled.
int8_t rt_material3d_get_unlit(void *obj);
/// @brief Switch shading model (0=Phong, 1=Toon, 2=PBR workflow, 3=Unlit, 4=Fresnel, 5=Emissive).
void rt_material3d_set_shading_model(void *obj, int64_t model);
/// @brief Read the current shading model.
int64_t rt_material3d_get_shading_model(void *obj);
/// @brief Write a value to a backend-specific custom shader parameter slot.
void rt_material3d_set_custom_param(void *obj, int64_t index, double value);
/// @brief Set the material alpha multiplier (1.0 = opaque, 0.0 = invisible).
void rt_material3d_set_alpha(void *obj, double alpha);
/// @brief Read the material alpha.
double rt_material3d_get_alpha(void *obj);
/// @brief Set the PBR metallic factor (0.0 = dielectric, 1.0 = pure metal).
void rt_material3d_set_metallic(void *obj, double value);
/// @brief Read the PBR metallic factor.
double rt_material3d_get_metallic(void *obj);
/// @brief Set the PBR roughness factor (0.0 = mirror, 1.0 = fully rough).
void rt_material3d_set_roughness(void *obj, double value);
/// @brief Read the PBR roughness factor.
double rt_material3d_get_roughness(void *obj);
/// @brief Set the ambient-occlusion factor (0.0 = full shadow, 1.0 = no occlusion).
void rt_material3d_set_ao(void *obj, double value);
/// @brief Read the AO factor.
double rt_material3d_get_ao(void *obj);
/// @brief Set the HDR emissive intensity multiplier (0 disables emission).
void rt_material3d_set_emissive_intensity(void *obj, double value);
/// @brief Read the emissive intensity multiplier.
double rt_material3d_get_emissive_intensity(void *obj);
/// @brief Bind a tangent-space normal map texture (requires `_calc_tangents` on the mesh).
void rt_material3d_set_normal_map(void *obj, void *pixels);
/// @brief True when a base-color/albedo texture is bound.
int8_t rt_material3d_get_has_texture(void *obj);
/// @brief True when a normal map texture is bound.
int8_t rt_material3d_get_has_normal_map(void *obj);
/// @brief Bind a packed metallic-roughness map (R = AO, G = roughness, B = metallic per glTF).
void rt_material3d_set_metallic_roughness_map(void *obj, void *pixels);
/// @brief True when a packed metallic-roughness texture is bound.
int8_t rt_material3d_get_has_metallic_roughness_map(void *obj);
/// @brief Bind a separate ambient-occlusion texture.
void rt_material3d_set_ao_map(void *obj, void *pixels);
/// @brief Assign a baked lightmap atlas (TEXCOORD_1); replaces flat ambient when set.
void rt_material3d_set_lightmap(void *obj, void *pixels);
/// @brief Return whether the baked lightmap slot is populated.
int8_t rt_material3d_get_has_lightmap(void *obj);
/// @brief True when a separate ambient-occlusion texture is bound.
int8_t rt_material3d_get_has_ao_map(void *obj);
/// @brief Bind a legacy specular highlight texture.
void rt_material3d_set_specular_map(void *obj, void *pixels);
/// @brief True when a specular map texture is bound.
int8_t rt_material3d_get_has_specular_map(void *obj);
/// @brief Bind an emissive texture (multiplied by emissive_intensity).
void rt_material3d_set_emissive_map(void *obj, void *pixels);
/// @brief True when an emissive map texture is bound.
int8_t rt_material3d_get_has_emissive_map(void *obj);
/// @brief True when an environment cubemap is bound.
int8_t rt_material3d_get_has_env_map(void *obj);
/// @brief Set the base emissive tint color.
void rt_material3d_set_emissive_color(void *obj, double r, double g, double b);
/// @brief Scale the normal-map effect (0 = flat, 1 = full strength, >1 = exaggerated).
void rt_material3d_set_normal_scale(void *obj, double value);
/// @brief Read the normal scale.
double rt_material3d_get_normal_scale(void *obj);
/// @brief Set texture anisotropy; 1 disables anisotropic filtering, values clamp to [1,16].
void rt_material3d_set_anisotropy(void *obj, int64_t anisotropy);
/// @brief Read texture anisotropy in the public [1,16] range.
int64_t rt_material3d_get_anisotropy(void *obj);
/// @brief Set alpha mode: 0=Opaque, 1=Mask (alpha test), 2=Blend (transparent).
void rt_material3d_set_alpha_mode(void *obj, int64_t mode);
/// @brief Read the alpha mode.
int64_t rt_material3d_get_alpha_mode(void *obj);
/// @brief Set shadow casting mode: 0=Auto, 1=None, 2=Cast even for alpha-blended materials.
void rt_material3d_set_shadow_mode(void *obj, int64_t mode);
/// @brief Read the shadow casting mode.
int64_t rt_material3d_get_shadow_mode(void *obj);
/// @brief Toggle two-sided rendering (disables backface culling for this material).
void rt_material3d_set_double_sided(void *obj, int8_t enabled);
/// @brief True if the material is configured for double-sided rendering.
int8_t rt_material3d_get_double_sided(void *obj);
/// @brief Set constant and slope-scaled depth bias for coplanar material draws.
/// @details The constant term shifts all fragments by the same depth amount; negative values pull
///   the material forward and positive values push it away. The slope-scaled term adds more offset
///   on steep screen-space triangles and is useful for decals, debug overlays, and other geometry
///   that intentionally sits on top of another surface.
void rt_material3d_set_depth_bias(void *obj, double constant_bias, double slope_scaled_bias);

//=========================================================================
// Light3D — directional, point, or ambient light source
//=========================================================================

/// @brief Create a directional light shining along @p direction with the given color (sun-like).
void *rt_light3d_new_directional(void *direction, double r, double g, double b);
/// @brief Create a point light at @p position with linear distance attenuation factor.
void *rt_light3d_new_point(void *position, double r, double g, double b, double attenuation);
/// @brief Create an ambient light contribution (illuminates all surfaces equally).
void *rt_light3d_new_ambient(double r, double g, double b);
/// @brief Create a spot light with inner/outer cone angles in degrees (smooth edge between).
void *rt_light3d_new_spot(void *position,
                          void *direction,
                          double r,
                          double g,
                          double b,
                          double attenuation,
                          double inner_angle,
                          double outer_angle);
/// @brief Create a one-sided oriented rectangle area light.
void *rt_light3d_new_area_rectangle(void *position,
                                    void *direction,
                                    double width,
                                    double height,
                                    double r,
                                    double g,
                                    double b,
                                    double attenuation,
                                    double range);
/// @brief Create an omnidirectional spherical area light.
void *rt_light3d_new_area_sphere(
    void *position, double radius, double r, double g, double b, double range);
/// @brief Create an isotropic volume light bounded by @p radius.
void *rt_light3d_new_volume(
    void *position, double radius, double r, double g, double b, double range);
/// @brief Multiply the light color by an intensity scalar (HDR-friendly).
void rt_light3d_set_intensity(void *obj, double intensity);
/// @brief Set the distance-falloff factor of a point/spot light (no-op for other types).
void rt_light3d_set_attenuation(void *obj, double attenuation);
/// @brief Get the light's distance-falloff factor (0 for directional/ambient).
double rt_light3d_get_attenuation(void *obj);
/// @brief Replace the light color (without altering intensity).
void rt_light3d_set_color(void *obj, double r, double g, double b);
/// @brief Get the light type (0=directional, 1=point, 2=ambient, 3=spot).
int64_t rt_light3d_get_type(void *obj);
/// @brief Get the light color as a Vec3.
void *rt_light3d_get_color(void *obj);
/// @brief Get the brightness multiplier.
double rt_light3d_get_intensity(void *obj);
/// @brief Enable or disable a light without removing it from its slot.
void rt_light3d_set_enabled(void *obj, int8_t enabled);
/// @brief True if the light contributes to backend light params.
int8_t rt_light3d_get_enabled(void *obj);
/// @brief Toggle whether this light is eligible for shadow-map selection.
void rt_light3d_set_casts_shadows(void *obj, int8_t enabled);
/// @brief True if this light may claim shadow-map slots.
int8_t rt_light3d_get_casts_shadows(void *obj);
/// @brief Get the normalized light direction as a Vec3.
void *rt_light3d_get_direction(void *obj);
/// @brief Get the light position as a Vec3.
void *rt_light3d_get_position(void *obj);
/// @brief Move the light to a new world position (Vec3).
void rt_light3d_set_position(void *obj, void *position);
/// @brief Re-aim the light; the direction (Vec3) is normalized.
void rt_light3d_set_direction(void *obj, void *direction);
/// @brief Get/set rectangle emitter width.
double rt_light3d_get_width(void *obj);
void rt_light3d_set_width(void *obj, double width);
/// @brief Get/set rectangle emitter height.
double rt_light3d_get_height(void *obj);
void rt_light3d_set_height(void *obj, double height);
/// @brief Get/set sphere/volume radius.
double rt_light3d_get_radius(void *obj);
void rt_light3d_set_radius(void *obj, double radius);
/// @brief Get/set FBX-compatible distance-decay mode (0 none, 1 linear, 2 quadratic, 3 cubic).
int64_t rt_light3d_get_decay_type(void *obj);
void rt_light3d_set_decay_type(void *obj, int64_t decay_type);
/// @brief Get/set finite local-light range.
double rt_light3d_get_range(void *obj);
void rt_light3d_set_range(void *obj, double range);
/// @brief Get the sanitized inner spot-cone angle in degrees (zero for non-spot lights).
double rt_light3d_get_inner_cone_degrees(void *obj);
/// @brief Get the sanitized outer spot-cone angle in degrees (zero for non-spot lights).
double rt_light3d_get_outer_cone_degrees(void *obj);
/// @brief Atomically set both spot-cone angles in degrees (no-op for non-spot lights).
void rt_light3d_set_spot_cone(void *obj, double inner_angle, double outer_angle);

/// @brief Register a temporary buffer to be freed at the end of the current frame.
/// @return 1 when ownership transfers to the canvas, 0 when the caller still owns `buffer`.
int rt_canvas3d_add_temp_buffer(void *canvas, void *buffer);
/// @brief Remove a previously-registered temporary buffer; caller owns/free()s it again.
int rt_canvas3d_remove_temp_buffer(void *canvas, void *buffer);

/* Screen-space HUD overlay */
/// @brief Draw a screen-space filled rectangle as a HUD element.
void rt_canvas3d_draw_rect2d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);
/// @brief Draw a screen-space filled rectangle blended with the given opacity (0..1).
void rt_canvas3d_draw_rect2d_alpha(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, double alpha);
/// @brief Draw a centered crosshair gizmo (4 lines around the canvas center).
void rt_canvas3d_draw_crosshair(void *canvas, int64_t color, int64_t size);
/// @brief Draw screen-space text on top of the rendered scene.
void rt_canvas3d_draw_text2d(void *canvas, int64_t x, int64_t y, rt_string text, int64_t color);
/// @brief Blit a Pixels image into the 2D overlay at (x,y) scaled to (w,h). NULL-safe.
void rt_canvas3d_draw_image2d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, void *pixels);
/// @brief Draw a screen-space line segment with thickness 1 and opacity (0..1).
void rt_canvas3d_draw_line2d(
    void *canvas, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t color, double alpha);
/// @brief Draw a screen-space 1px rectangle outline with opacity (0..1).
void rt_canvas3d_draw_frame2d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, double alpha);
/// @brief Draw a screen-space filled rounded rectangle with opacity (0..1).
void rt_canvas3d_draw_round_rect2d(void *canvas,
                                   int64_t x,
                                   int64_t y,
                                   int64_t w,
                                   int64_t h,
                                   int64_t radius,
                                   int64_t color,
                                   double alpha);
/// @brief Draw a screen-space rounded rectangle outline with opacity (0..1).
void rt_canvas3d_draw_round_frame2d(void *canvas,
                                    int64_t x,
                                    int64_t y,
                                    int64_t w,
                                    int64_t h,
                                    int64_t radius,
                                    int64_t color,
                                    double alpha);
/// @brief Draw screen-space text scaled by a size multiplier (1.0 = DrawText2D size).
void rt_canvas3d_draw_text2d_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t color, double scale);
/// @brief Blit a sub-region (sx,sy,sw,sh) of a Pixels image into the overlay rect (x,y,w,h).
void rt_canvas3d_draw_image2d_region(void *canvas,
                                     int64_t x,
                                     int64_t y,
                                     int64_t w,
                                     int64_t h,
                                     void *pixels,
                                     int64_t sx,
                                     int64_t sy,
                                     int64_t sw,
                                     int64_t sh);
/// @brief Restrict subsequent overlay 2D drawing to a screen rect (enqueue-time clipping).
void rt_canvas3d_set_clip_rect2d(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h);
/// @brief Remove the overlay 2D clip rect.
void rt_canvas3d_clear_clip_rect2d(void *canvas);
/// @brief Width in pixels of DrawText2DScaled output for @p text at @p scale.
int64_t rt_canvas3d_measure_text2d(void *canvas, rt_string text, double scale);
/// @brief Internal-facing: scaled variant backing DrawText2D/DrawText2DScaled.
void rt_canvas3d_draw_text_3d_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t color, double scale);

/* Debug gizmos */
/// @brief Draw an axis-aligned bounding box outline between min and max corners.
void rt_canvas3d_draw_aabb_wire(void *canvas, void *min_v, void *max_v, int64_t color);
/// @brief Raw-array form of rt_canvas3d_draw_aabb_wire: @p min_v and @p max_v are
///   double[3] corner coordinates instead of boxed Vec3 objects.
void rt_canvas3d_draw_aabb_wire_raw(void *canvas,
                                    const double *min_v,
                                    const double *max_v,
                                    int64_t color);
/// @brief Draw a wireframe sphere as great circles (3 orthogonal rings).
void rt_canvas3d_draw_sphere_wire(void *canvas, void *center, double radius, int64_t color);
/// @brief Draw a ray with optional arrowhead, capped at @p length world units.
void rt_canvas3d_draw_debug_ray(
    void *canvas, void *origin, void *dir, double length, int64_t color);
/// @brief Draw an XYZ axis gizmo at @p origin (X=red, Y=green, Z=blue).
void rt_canvas3d_draw_axis(void *canvas, void *origin, double scale);

/* Fog */
/// @brief Enable distance fog with linear falloff between @p near_dist and @p far_dist.
void rt_canvas3d_set_fog(
    void *canvas, double near_dist, double far_dist, double r, double g, double b);
/// @brief Disable fog.
void rt_canvas3d_clear_fog(void *canvas);

/* Shadows */
/// @brief Enable shadow-map rendering at the given square resolution (typical: 1024 or 2048).
void rt_canvas3d_enable_shadows(void *canvas, int64_t resolution);
/// @brief Disable shadow rendering.
void rt_canvas3d_disable_shadows(void *canvas);
/// @brief Set the shadow depth bias to combat shadow acne (typical: 0.001–0.005).
void rt_canvas3d_set_shadow_bias(void *canvas, double bias);
/// @brief Request cascaded shadow maps; counts > 1 require backend support.
void rt_canvas3d_set_shadow_cascades(void *canvas, int64_t count);
/// @brief Cap the camera distance covered by directional shadow maps (<= 0 restores auto).
void rt_canvas3d_set_shadow_distance(void *canvas, double distance);
/// @brief Configured directional shadow distance (0 = automatic min(camera far, 300)).
double rt_canvas3d_get_shadow_distance(void *canvas);

/* Coarse CPU visibility: frustum rejection plus optional low-resolution
 * screen-space occlusion. The two toggles are independent; frustum rejection defaults
 * on. Occlusion-enabled frames test opaque draws front-to-back so the coarse depth grid
 * receives near occluders first, then regroup survivors by backend state. */
/// @brief Enable/disable vsync presentation pacing (default on; see "vsync-control" cap).
void rt_canvas3d_set_vsync(void *canvas, int8_t enabled);
/// @brief Requested vsync state (defaults to on).
int8_t rt_canvas3d_get_vsync(void *canvas);
/**
 * @brief Try to render the window-backed 3D scene at a scale in `[0.25, 1]`.
 *
 * Reduced scales require the `"render-scale"` backend capability and are upscaled to
 * the logical output dimensions before overlays, readback, and presentation. Values
 * greater than or equal to one request native resolution and work on fixed-scale
 * backends. A capable backend can reject a transition during an active frame or when
 * target allocation fails; on rejection, the previous scale remains active.
 *
 * @param canvas Canvas3D receiver, or `NULL`.
 * @param scale Requested scene scale. Non-finite and values at least one request 1:1;
 * finite values below 0.25 are clamped to 0.25.
 * @return Non-zero if the requested scale is active, otherwise zero.
 */
int8_t rt_canvas3d_try_set_render_scale(void *canvas, double scale);
/**
 * @brief Return the currently active window-backed scene scale.
 * @param canvas Canvas3D receiver, or `NULL`.
 * @return A finite scale in `[0.25, 1]`, or `1` for a null/corrupt receiver.
 */
double rt_canvas3d_get_render_scale(void *canvas);
/// @brief Toggle coarse CPU frustum rejection (default on).
void rt_canvas3d_set_frustum_culling(void *canvas, int8_t enabled);
/// @brief Toggle conservative CPU occlusion skips (independent of frustum culling).
void rt_canvas3d_set_occlusion_culling(void *canvas, int8_t enabled);

/* Instanced rendering + Terrain */
/// @brief Submit a Mesh3DBatch for GPU-instanced rendering (one draw call per batch).
void rt_canvas3d_draw_instanced(void *canvas, void *batch);
/// @brief Submit a Terrain3D for chunked heightmap rendering.
void rt_canvas3d_draw_terrain(void *canvas, void *terrain);

/* Camera shake + smooth follow */
/// @brief Apply transient camera shake (decays over @p duration seconds with rate @p decay).
void rt_camera3d_shake(void *cam, double intensity, double duration, double decay);
/// @brief Smoothly orbit the camera at fixed distance/height behind @p target each frame.
void rt_camera3d_smooth_follow(
    void *cam, void *target, double distance, double height, double speed, double dt);
/// @brief Interpolate the camera's look direction toward @p target each frame.
void rt_camera3d_smooth_look_at(void *cam, void *target, double speed, double dt);

/* FPS camera */
/// @brief Initialize FPS-camera state (yaw=0, pitch=0) for first-person controls.
void rt_camera3d_fps_init(void *cam);
/// @brief Apply FPS controls: mouse delta rotates yaw/pitch, keys translate the camera.
void rt_camera3d_fps_update(void *cam,
                            double yaw_delta,
                            double pitch_delta,
                            double move_fwd,
                            double move_right,
                            double move_up,
                            double speed,
                            double dt);
/// @brief Read the current FPS-camera yaw in degrees.
double rt_camera3d_get_yaw(void *cam);
/// @brief Read the current FPS-camera pitch in degrees.
double rt_camera3d_get_pitch(void *cam);
/// @brief Set the FPS-camera yaw in degrees and rebuild the view immediately.
void rt_camera3d_set_yaw(void *cam, double yaw);
/// @brief Set the FPS-camera pitch in degrees (clamped internally to +/-89) and rebuild the view.
void rt_camera3d_set_pitch(void *cam, double pitch);

#ifdef __cplusplus
}
#endif
