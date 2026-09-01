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
/// @file
/// @brief Declares the C runtime bridge for Canvas3D and its core 3D resource types.
/// @details The API exposes opaque GC-managed Canvas3D, CubeMap3D, RenderTarget3D, Mesh3D,
/// Camera3D, Material3D, and Light3D handles. Unless a declaration says otherwise, object
/// parameters are borrowed, invalid handles are rejected without ownership transfer, constructors
/// return a new runtime reference, dimensions are pixels, angles are degrees, and world-space
/// distances use engine units.

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
/// @return 1 when the graphics implementation is compiled in; otherwise 0.
int8_t rt_canvas3d_is_available(void);
/// @brief Create a new 3D canvas window with the given title and pixel dimensions.
/// @param title Borrowed runtime string used as the platform-window title.
/// @param w Logical window width in pixels; valid values are 1 through 16384.
/// @param h Logical window height in pixels; valid values are 1 through 16384.
/// @return New GC-managed Canvas3D handle, or NULL after reporting construction failure.
void *rt_canvas3d_new(rt_string title, int64_t w, int64_t h);
/// @brief Create a fullscreen 3D canvas at desktop resolution (no windowed flash).
/// @param title Borrowed runtime string used as the platform-window title.
/// @return New GC-managed fullscreen Canvas3D handle, or NULL on construction failure.
void *rt_canvas3d_new_fullscreen(rt_string title);
/// @brief Create a 3D canvas that renders into an existing 2D canvas's window.
/// @details The window is borrowed, not owned: destroying this Canvas3D returns
/// presentation to the 2D canvas instead of closing the window. The caller must
/// keep the 2D canvas alive for this canvas's lifetime.
/// @param canvas2d Live Zanna.Graphics.Canvas handle whose window is adopted.
/// @return New GC-managed Canvas3D handle, or NULL after a validation trap.
void *rt_canvas3d_new_on_canvas(void *canvas2d);
/// @brief Create a windowless software Canvas3D bound to an explicit RenderTarget3D.
/// @param target Live RenderTarget3D retained as the initial output.
/// @return New deterministic software-backed Canvas3D handle, or NULL for invalid target or
/// construction failure.
void *rt_canvas3d_new_offscreen(void *target);
/// @brief Windowless constructor that requests the platform GPU backend with software
///        fallback (ADR 0191). Not byte-deterministic across backends; probes and bakes
///        must keep using rt_canvas3d_new_offscreen.
/// @param target Live RenderTarget3D retained as the initial output.
/// @return New offscreen Canvas3D using a headless platform backend or truthful software fallback,
/// or NULL on construction failure.
void *rt_canvas3d_new_offscreen_accelerated(void *target);
/// @brief Report whether the canvas was created without a platform window.
/// @param obj Borrowed Canvas3D handle.
/// @return 1 for a valid offscreen canvas; otherwise 0.
int8_t rt_canvas3d_get_is_offscreen(void *obj);
/// @brief Resize the canvas and active backend output targets.
/// @details Offscreen canvases cannot be resized directly because the bound render target owns
/// their dimensions. Invalid or out-of-range window dimensions are rejected.
/// @param obj Borrowed Canvas3D handle.
/// @param w Requested logical window width in pixels.
/// @param h Requested logical window height in pixels.
void rt_canvas3d_resize(void *obj, int64_t w, int64_t h);
/// @brief Clear the back buffer to the given RGB color (each channel 0.0–1.0).
/// @param obj Borrowed Canvas3D handle.
/// @param r Normalized red clear channel.
/// @param g Normalized green clear channel.
/// @param b Normalized blue clear channel.
void rt_canvas3d_clear(void *obj, double r, double g, double b);
/// @brief Begin a 3D draw pass with the given camera (must be paired with `_end`).
/// @param obj Borrowed Canvas3D handle that is not already inside a pass.
/// @param camera Borrowed live Camera3D defining the frame view and projection.
void rt_canvas3d_begin(void *obj, void *camera);
/// @brief Begin a view-model pass over the finished scene: keeps color, clears depth,
///        renders with an independent FOV (<= 0 keeps the camera FOV). Pair with `_end`.
/// @param obj Borrowed Canvas3D handle that is not already inside a pass.
/// @param camera Borrowed live Camera3D defining view orientation and clipping.
/// @param fov_y_degrees Vertical field of view in degrees; non-positive values retain the camera
/// aperture.
void rt_canvas3d_begin_view_model(void *obj, void *camera, double fov_y_degrees);
/// @brief Begin a 2D screen-space overlay pass (orthographic, no depth test).
/// @param obj Borrowed Canvas3D handle that is not already inside a pass.
void rt_canvas3d_begin_2d(void *obj);
/// @brief Draw a screen-space filled rectangle inside the current 2D pass.
/// @param canvas Borrowed Canvas3D handle in an active 2D pass.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_rect_3d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);
/// @brief Draw screen-space text inside the current 2D pass.
/// @param canvas Borrowed Canvas3D handle in an active 2D pass.
/// @param x Left baseline origin in logical pixels.
/// @param y Top baseline origin in logical pixels.
/// @param text Borrowed runtime string to render.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_text_3d(void *canvas, int64_t x, int64_t y, rt_string text, int64_t color);
/// @brief Submit a mesh+transform+material draw inside the active 3D pass.
/// @param obj Borrowed Canvas3D handle in an active 3D pass.
/// @param mesh Borrowed live Mesh3D; deferred submission retains or snapshots required payloads.
/// @param transform Borrowed Mat4 model transform.
/// @param material Borrowed Material3D, or NULL for the renderer's default material contract.
void rt_canvas3d_draw_mesh(void *obj, void *mesh, void *transform, void *material);
/// @brief Draw a mesh with a height-weighted per-vertex wind sway (foliage/canopies).
/// @param obj Borrowed Canvas3D handle in an active 3D pass.
/// @param mesh Borrowed live Mesh3D.
/// @param transform Borrowed Mat4 model transform.
/// @param material Borrowed Material3D.
/// @param dir_x World-space X component of the horizontal wind direction.
/// @param dir_z World-space Z component of the horizontal wind direction.
/// @param strength Maximum sway displacement in world units.
/// @param phase Animation phase supplied by the caller.
void rt_canvas3d_draw_mesh_wind(void *obj,
                                void *mesh,
                                void *transform,
                                void *material,
                                double dir_x,
                                double dir_z,
                                double strength,
                                double phase);
/// @brief End the current 3D or 2D pass and flush queued draws.
/// @param obj Borrowed Canvas3D handle with an active pass.
void rt_canvas3d_end(void *obj);
/// @brief Present the back buffer to the window (vsync-controlled by backend).
/// @details Finalizes post-FX and overlays exactly once, presents a window-backed canvas, and
/// updates frame timing. Offscreen canvases are not presented.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_flip(void *obj);
/// @brief Pump events and update per-frame input state.
/// @param obj Borrowed Canvas3D handle. Synthetic-only input permits an offscreen canvas.
/// @return 1 while the canvas remains open, 0 after a close request or invalid handle.
int64_t rt_canvas3d_poll(void *obj);
/// @brief Dequeue one event type captured by the most recent Poll call, or 0 if none.
/// @param obj Borrowed Canvas3D handle.
/// @return Oldest queued backend event type, or zero for an empty queue or invalid handle.
int64_t rt_canvas3d_poll_event(void *obj);
/// @brief True when the user has requested window close (clicked X or Alt-F4).
/// @param obj Borrowed Canvas3D handle.
/// @return Non-zero after a close request; zero for an open or invalid canvas.
int8_t rt_canvas3d_should_close(void *obj);
/// @brief Toggle wireframe rendering for subsequent mesh draws.
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero for edge-only rasterization; zero for filled rasterization.
void rt_canvas3d_set_wireframe(void *obj, int8_t enabled);
/// @brief Read whether wireframe rendering is enabled (ADR 0227).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Nonzero when wireframe rendering is active, or 0 for invalid handles.
int8_t rt_canvas3d_get_wireframe(void *obj);
/// @brief Toggle backface culling (CCW = front-facing).
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to cull clockwise back faces; zero to render both sides.
void rt_canvas3d_set_backface_cull(void *obj, int8_t enabled);
/// @brief Set a slope-scaled shadow bias used to reduce acne on glancing-angle casters.
/// @details The value is applied while rendering caster geometry into shadow maps. It complements
///   `rt_canvas3d_set_shadow_bias`, which biases the later shadow comparison. Higher values improve
///   stability for shallow slopes and coplanar caster triangles, while excessive values can visibly
///   detach shadows from geometry.
/// @param obj Borrowed Canvas3D handle.
/// @param bias Requested finite non-negative slope bias; unsupported values are sanitized.
void rt_canvas3d_set_shadow_slope_bias(void *obj, double bias);
/// @brief Set how dark fully-occluded texels get (0 = disabled, 1 = fully black; default 0.85).
/// @param obj Borrowed Canvas3D handle.
/// @param strength Occlusion darkness clamped to the inclusive normalized range.
void rt_canvas3d_set_shadow_strength(void *obj, double strength);
/// @brief Select the shadow PCF tier (0 = 4 taps, 1 = 8, 2 = 16 rotated-Poisson taps).
/// @param obj Borrowed Canvas3D handle.
/// @param quality Filtering tier clamped from zero through two.
void rt_canvas3d_set_shadow_quality(void *obj, int64_t quality);
/// @brief Get the canvas width in pixels.
/// @param obj Borrowed Canvas3D handle.
/// @return Bound render-target width when present, otherwise logical window width; zero if invalid.
int64_t rt_canvas3d_get_width(void *obj);
/// @brief Get the canvas height in pixels.
/// @param obj Borrowed Canvas3D handle.
/// @return Bound render-target height when present, otherwise logical window height; zero if
/// invalid.
int64_t rt_canvas3d_get_height(void *obj);
/// @brief Get the backing window width, ignoring any bound render target.
/// @param obj Borrowed Canvas3D handle.
/// @return Logical window width, or zero for invalid and offscreen canvases.
int64_t rt_canvas3d_get_window_width(void *obj);
/// @brief Get the backing window height, ignoring any bound render target.
/// @param obj Borrowed Canvas3D handle.
/// @return Logical window height, or zero for invalid and offscreen canvases.
int64_t rt_canvas3d_get_window_height(void *obj);
/// @brief Switch the canvas window between fullscreen and windowed mode (non-zero = fullscreen).
/// @param obj Borrowed window-backed Canvas3D handle.
/// @param enabled Non-zero to request fullscreen; zero to request windowed mode.
void rt_canvas3d_set_fullscreen(void *obj, int8_t enabled);
/// @brief Report whether the canvas window is currently fullscreen (1) or windowed (0).
/// @param obj Borrowed window-backed Canvas3D handle.
/// @return 1 in fullscreen mode; zero for windowed, offscreen, or invalid input.
int8_t rt_canvas3d_is_fullscreen(void *obj);
/// @brief Flip the canvas window between fullscreen and windowed mode.
/// @param obj Borrowed window-backed Canvas3D handle.
void rt_canvas3d_toggle_fullscreen(void *obj);
/// @brief Get the active output width (window, or current render target when bound).
/// @param obj Borrowed Canvas3D handle.
/// @return Active output width in pixels, or zero when unavailable.
int64_t rt_canvas3d_get_active_output_width(void *obj);
/// @brief Get the active output height (window, or current render target when bound).
/// @param obj Borrowed Canvas3D handle.
/// @return Active output height in pixels, or zero when unavailable.
int64_t rt_canvas3d_get_active_output_height(void *obj);
/// @brief Get the rolling-average FPS measured over recent Poll/Flip/synthetic timing samples.
/// @param obj Borrowed Canvas3D handle.
/// @return Rounded frames per second, or zero before any positive timing sample.
int64_t rt_canvas3d_get_fps(void *obj);
/// @brief Get milliseconds since the latest timing sample; Poll normally samples live clocks.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative elapsed milliseconds after the configured maximum-delta cap.
int64_t rt_canvas3d_get_delta_time(void *obj);
/// @brief Get seconds since the latest timing sample; Poll normally samples live clocks.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative elapsed seconds after the configured maximum-delta cap.
double rt_canvas3d_get_delta_time_sec(void *obj);
/// @brief Cap the per-frame delta time (smooths spikes after pause/breakpoint).
/// @param obj Borrowed Canvas3D handle.
/// @param max_ms Positive cap in milliseconds; non-positive values disable the cap.
void rt_canvas3d_set_dt_max(void *obj, int64_t max_ms);
/// @brief Apply a backend-safe quality profile (0 performance, 1 balanced, 2 cinematic).
/// @details The active backend may downgrade unsupported settings while retaining the requested
/// profile for diagnostics.
/// @param obj Borrowed Canvas3D handle.
/// @param quality Requested profile, normalized to performance, balanced, or cinematic.
void rt_canvas3d_set_quality(void *obj, int64_t quality);
/// @brief Last quality profile requested through SetQuality.
/// @param obj Borrowed Canvas3D handle.
/// @return Requested quality-profile identifier, or the runtime default for invalid input.
int64_t rt_canvas3d_get_quality_requested(void *obj);
/// @brief Quality profile actually active after backend fallback.
/// @param obj Borrowed Canvas3D handle.
/// @return Backend-safe active quality-profile identifier.
int64_t rt_canvas3d_get_quality_active(void *obj);
/// @brief True when the active quality profile was degraded for backend safety.
/// @param obj Borrowed Canvas3D handle.
/// @return 1 when requested and active profiles differ due to fallback; otherwise 0.
int8_t rt_canvas3d_get_quality_fallback(void *obj);
/// @brief Reason for the last quality fallback, or an empty string.
/// @param obj Borrowed Canvas3D handle.
/// @return Runtime string describing the downgrade, or an empty runtime string.
rt_string rt_canvas3d_get_quality_fallback_reason(void *obj);
/// @brief Select live, synthetic, or live+synthetic input for this canvas (0, 1, or 2).
/// @param obj Borrowed Canvas3D handle.
/// @param mode 0 for live, 1 for synthetic-only, or 2 for combined input; other values select live.
void rt_canvas3d_set_input_source(void *obj, int64_t mode);
/// @brief Queue a synthetic keyboard transition for the next synthetic input frame.
/// @param obj Borrowed Canvas3D handle.
/// @param key Valid runtime key code.
/// @param down Non-zero for key-down; zero for key-up.
void rt_canvas3d_push_synthetic_key(void *obj, int64_t key, int8_t down);
/// @brief Queue a synthetic mouse sample for the next synthetic input frame.
/// @param obj Borrowed Canvas3D handle.
/// @param dx Horizontal logical displacement accumulated for the frame.
/// @param dy Vertical logical displacement accumulated for the frame.
/// @param buttons Bitset describing the desired held-button state.
/// @param wheel Vertical wheel displacement accumulated for the frame.
void rt_canvas3d_push_synthetic_mouse(
    void *obj, double dx, double dy, int64_t buttons, double wheel);
/// @brief Drop queued synthetic input and release keys/buttons held by the synthetic source.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_clear_synthetic_input(void *obj);
/// @brief Select live wall-clock or fixed synthetic delta-time source (0 or 1).
/// @param obj Borrowed Canvas3D handle.
/// @param mode 1 for fixed synthetic time; any other value selects the live wall clock.
void rt_canvas3d_set_clock_source(void *obj, int64_t mode);
/// @brief Set the fixed synthetic delta time in seconds.
/// @param obj Borrowed Canvas3D handle.
/// @param dt Requested deterministic frame duration in seconds; invalid values become zero.
void rt_canvas3d_set_synthetic_delta_time_sec(void *obj, double dt);
/// @brief Advance one deterministic input/timing frame without pumping platform events.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_advance_synthetic_frame(void *obj);
/// @brief Bind or clear a Light3D slot; the canvas retains the assigned light until replaced.
/// @param obj Borrowed Canvas3D handle.
/// @param index Zero-based slot in the fixed Canvas3D light array.
/// @param light Borrowed live Light3D to retain, or NULL to clear the slot.
void rt_canvas3d_set_light(void *obj, int64_t index, void *light);
/// @brief Clear all retained canvas light slots.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_clear_lights(void *obj);
/// @brief Install a conservative key/fill/ambient setup for readable default scenes.
/// @param obj Borrowed Canvas3D handle whose existing light slots are replaced.
void rt_canvas3d_set_default_lighting(void *obj);
/// @brief Count currently assigned canvas light slots.
/// @param obj Borrowed Canvas3D handle.
/// @return Number of live, enabled lights in retained canvas slots.
int64_t rt_canvas3d_get_light_count(void *obj);
/// @brief Try to set clustered/forward+ lighting without trapping.
///
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to request clustered lighting; zero for classic forward lighting.
/// @return 1 when the requested state is applied, or 0 when enabling is blocked
///         by the active backend, an environment kill switch, or an invalid canvas.
int8_t rt_canvas3d_try_set_clustered_lighting(void *obj, int8_t enabled);
/// @brief Enable clustered/forward+ lighting when the backend advertises support.
///
/// Unlike rt_canvas3d_try_set_clustered_lighting(), this strict property setter
/// traps when enabling is requested on an unsupported backend.
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to request clustered lighting; zero for classic forward lighting.
void rt_canvas3d_set_clustered_lighting(void *obj, int8_t enabled);
/// @brief Report whether clustered/forward+ lighting is currently enabled.
/// @param obj Borrowed Canvas3D handle.
/// @return 1 when clustered lighting is active; otherwise 0.
int8_t rt_canvas3d_get_clustered_lighting(void *obj);
/// @brief Current maximum active light count for the selected lighting path.
/// @param obj Borrowed Canvas3D handle.
/// @return Active flattened-light budget for the current backend and lighting path.
int64_t rt_canvas3d_get_max_active_lights(void *obj);
/// @brief Set the ambient light color applied to all lit materials.
/// @param obj Borrowed Canvas3D handle.
/// @param r Normalized red ambient channel.
/// @param g Normalized green ambient channel.
/// @param b Normalized blue ambient channel.
void rt_canvas3d_set_ambient(void *obj, double r, double g, double b);
/// @brief Enable/disable image-based lighting from the canvas skybox (PBR draws
///   without an explicit material env map use SH irradiance + prefiltered specular).
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to enable skybox-based diffuse and specular environment lighting.
void rt_canvas3d_set_ibl_enabled(void *obj, int8_t enabled);
/// @brief True when image-based lighting is enabled for this canvas.
/// @param obj Borrowed Canvas3D handle.
/// @return 1 when IBL is enabled; otherwise 0.
int8_t rt_canvas3d_get_ibl_enabled(void *obj);
/// @brief Scale the environment lighting contribution (default 1.0, clamped to [0, 8]).
/// @param obj Borrowed Canvas3D handle.
/// @param intensity Requested non-negative environment multiplier.
void rt_canvas3d_set_ibl_intensity(void *obj, double intensity);
/// @brief Current environment lighting intensity scale.
/// @param obj Borrowed Canvas3D handle.
/// @return Stored environment multiplier, or zero for invalid input.
double rt_canvas3d_get_ibl_intensity(void *obj);
/// @brief Draw a debug 3D line between two Vec3 points.
/// @param obj Borrowed Canvas3D handle with suitable drawing state.
/// @param from Borrowed Vec3 start point in world space.
/// @param to Borrowed Vec3 end point in world space.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_line3d(void *obj, void *from, void *to, int64_t color);
/// @brief Raw-array form of rt_canvas3d_draw_line3d: @p from and @p to are
///   double[3] world-space coordinates instead of boxed Vec3 objects.
/// @param obj Borrowed Canvas3D handle.
/// @param from Borrowed three-double start point.
/// @param to Borrowed three-double end point.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_line3d_raw(void *obj, const double *from, const double *to, int64_t color);
/// @brief Draw a debug 3D point (square) at the given position with pixel size.
/// @param obj Borrowed Canvas3D handle.
/// @param pos Borrowed Vec3 world-space point.
/// @param color Packed runtime color value.
/// @param size Square marker extent in screen pixels.
void rt_canvas3d_draw_point3d(void *obj, void *pos, int64_t color, int64_t size);
/// @brief Get the active backend name ("d3d11", "metal", "opengl", or "software").
/// @param obj Borrowed Canvas3D handle.
/// @return Runtime string naming the active backend, or the implementation's invalid-handle value.
rt_string rt_canvas3d_get_backend(void *obj);
/// @brief Return true when Canvas3D fell back from the selected GPU backend to software.
/// @param obj Borrowed Canvas3D handle.
/// @return 1 when construction selected a fallback backend; otherwise 0.
int8_t rt_canvas3d_get_backend_fallback(void *obj);
/// @brief Human-readable reason for BackendFallback, or an empty string when no fallback happened.
/// @param obj Borrowed Canvas3D handle.
/// @return Runtime string describing fallback, or an empty runtime string.
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
/* Materials bound to a RenderTarget3D sample the backend's retained native
 * colour texture directly (ADR 0299): no CPU mirror readback, no GPU stall,
 * no re-upload. Backends without this bit keep the Pixels-mirror path. */
#define RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET_SAMPLING 0x2000000000LL

/// @brief Return a 64-bit RT_CANVAS3D_BACKEND_CAP_* bitmask for the active backend.
/// @param obj Borrowed Canvas3D handle.
/// @return Capability bitmask, or zero for invalid input.
int64_t rt_canvas3d_get_backend_capabilities(void *obj);
/// @brief Return whether the active backend supports a named capability.
/// @param obj Borrowed Canvas3D handle.
/// @param capability Borrowed runtime string containing a registered capability name.
/// @return 1 when supported; otherwise 0.
int8_t rt_canvas3d_backend_supports(void *obj, rt_string capability);
/// @brief Force all skinned draws through the CPU skinning path (debug/bisection).
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to bypass GPU skinning; zero to restore capability-based selection.
void rt_canvas3d_set_force_cpu_skinning(void *obj, int8_t enabled);
/// @brief Lifetime count of skinned draws routed to GPU vertex-shader skinning.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime GPU-skinned draw count, or zero for invalid input.
int64_t rt_canvas3d_get_gpu_skinned_draw_count(void *obj);
/// @brief Lifetime bone-palette bytes handed to the backend for GPU skinning.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime palette-upload byte count, or zero for invalid input.
int64_t rt_canvas3d_get_skinning_upload_bytes(void *obj);
/// @brief Number of main 3D draw submissions queued by the latest ended frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest main-pass draw count, or zero for invalid input.
int64_t rt_canvas3d_get_draw_count(void *obj);
/// @brief Per-pass draw submissions for the latest frame (Game3D.RenderPass ids).
/// @param obj Borrowed Canvas3D handle.
/// @param pass Runtime render-pass identifier.
/// @return Latest draw count for @p pass, or zero for invalid input or pass.
int64_t rt_canvas3d_pass_draw_count(void *obj, int64_t pass);
/// @brief Per-pass instance tally for the latest frame (instanced draws expanded).
/// @param obj Borrowed Canvas3D handle.
/// @param pass Runtime render-pass identifier.
/// @return Latest expanded instance count for @p pass, or zero for invalid input or pass.
int64_t rt_canvas3d_pass_instance_count(void *obj, int64_t pass);
/// @brief Number of draw submissions rejected by visibility culling in the latest scene draw.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest aggregate visibility-rejection count, or zero for invalid input.
int64_t rt_canvas3d_get_occluded_draw_count(void *obj);
/// @brief Configure height-fog sun inscattering (amount 0 disables; default off).
/// @param obj Borrowed Canvas3D handle.
/// @param r Non-negative red channel of the scattering tint.
/// @param g Non-negative green channel of the scattering tint.
/// @param b Non-negative blue channel of the scattering tint.
/// @param power Non-negative angular-lobe exponent.
/// @param amount Normalized scattering contribution; zero disables the term.
void rt_canvas3d_set_height_fog_sun(
    void *obj, double r, double g, double b, double power, double amount);
/// @brief Disable height fog only (distance fog remains).
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_clear_height_fog(void *obj);
/// @brief True while exponential height fog is enabled.
/// @param obj Borrowed Canvas3D handle.
/// @return 1 when height fog is enabled; otherwise 0.
int8_t rt_canvas3d_get_height_fog_enabled(void *obj);
/// @brief Enable exponential height fog that pools below a configurable world-space height.
/// @details Height fog combines with distance fog through transmittance and reuses the current fog
/// color.
/// @param obj Borrowed Canvas3D handle.
/// @param base_height World-space reference height.
/// @param falloff Non-negative exponential thinning rate.
/// @param density Non-negative density at the reference height.
/// @param blend Normalized contribution weight.
void rt_canvas3d_set_height_fog(
    void *obj, double base_height, double falloff, double density, double blend);
/// @brief Draw anti-aliased screen-space text at an arbitrary scale.
/// @param obj Borrowed Canvas3D handle.
/// @param x Left origin in logical pixels.
/// @param y Top origin in logical pixels.
/// @param text Borrowed runtime string; at most 512 bytes are rendered.
/// @param color Packed runtime color value.
/// @param scale Positive size multiplier.
void rt_canvas3d_draw_text2d_aa(
    void *obj, int64_t x, int64_t y, rt_string text, int64_t color, double scale);
/// @brief Width in pixels of DrawText2DAA output for @p text at @p scale.
/// @param obj Borrowed Canvas3D handle.
/// @param text Borrowed runtime string; at most 512 bytes are measured.
/// @param scale Positive size multiplier.
/// @return Rounded rendered width in pixels, or zero for invalid input.
int64_t rt_canvas3d_measure_text2d_aa(void *obj, rt_string text, double scale);
/// @brief Draw kerned TrueType text with a loaded TtfFont (E1 font bridge).
/// @param obj Borrowed Canvas3D handle.
/// @param font Borrowed live TtfFont handle.
/// @param x Left origin in logical pixels.
/// @param y Top origin in logical pixels (top of the text box, not baseline).
/// @param text Borrowed runtime string; at most 512 bytes ending on a complete UTF-8 codepoint are
///             rendered.
/// @param size_px Font pixel size (clamped to the TtfFont range).
/// @param color Packed 0xRRGGBB color; alpha comes from glyph coverage.
void rt_canvas3d_draw_text2d_ttf(
    void *obj, void *font, int64_t x, int64_t y, rt_string text, double size_px, int64_t color);
/// @brief Width in pixels of DrawText2DTtf output for @p text at @p size_px.
/// @param obj Borrowed Canvas3D handle.
/// @param font Borrowed live TtfFont handle.
/// @param text Borrowed runtime string; at most 512 bytes ending on a complete UTF-8 codepoint are
///             measured.
/// @param size_px Font pixel size (clamped to the TtfFont range).
/// @return Rounded rendered width in pixels, or zero for invalid input.
int64_t rt_canvas3d_measure_text2d_ttf(void *obj, void *font, rt_string text, double size_px);
/// @brief Draw a 9-slice image: corners unscaled, edges axis-stretched, center stretched.
/// @param obj Borrowed Canvas3D handle.
/// @param x Destination left edge in logical pixels.
/// @param y Destination top edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param pixels Borrowed source Pixels object.
/// @param inset_l Left source inset in pixels.
/// @param inset_t Top source inset in pixels.
/// @param inset_r Right source inset in pixels.
/// @param inset_b Bottom source inset in pixels.
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
/// @param obj Borrowed Canvas3D handle.
/// @return Latest fallback-expanded instance count, or zero for invalid input.
int64_t rt_canvas3d_get_instanced_fallback_count(void *obj);
/// @brief Instances skipped because a chunked fallback queue reservation actually failed.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest dropped fallback-instance count, or zero for invalid input.
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
/// @param obj Borrowed Canvas3D handle.
/// @return Saturating lifetime event-drop count, or zero for invalid input.
int64_t rt_canvas3d_get_event_drop_count(void *obj);
/// @brief Mesh snapshot bytes copied by the current/latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative current/latest frame byte count, or zero for invalid input.
int64_t rt_canvas3d_get_mesh_snapshot_bytes(void *obj);
/// @brief Mesh snapshot allocation or budget denials in the current/latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative denial count, or zero for invalid input.
int64_t rt_canvas3d_get_mesh_snapshot_drop_count(void *obj);
/// @brief Requested mesh snapshot bytes denied in the current/latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative denied byte count, or zero for invalid input.
int64_t rt_canvas3d_get_mesh_snapshot_dropped_bytes(void *obj);
/// @brief Per-frame mesh snapshot byte budget used by deferred geometry copies.
/// @param obj Borrowed Canvas3D handle.
/// @return Configured per-frame snapshot budget in bytes, or zero for invalid input.
int64_t rt_canvas3d_get_mesh_snapshot_budget_bytes(void *obj);
/// @brief Set the shadow-light slot budget (1..max slots).
/// @param obj Borrowed Canvas3D handle.
/// @param budget Requested slot count, clamped to the implementation range.
void rt_canvas3d_set_shadow_budget(void *obj, int64_t budget);
/// @brief Shadow slots rendered in the latest frame (cascades included).
/// @param obj Borrowed Canvas3D handle.
/// @return Latest rendered slot count, or zero for invalid input.
int64_t rt_canvas3d_get_shadow_slots_used(void *obj);
/// @brief Shadow-requesting lights denied a slot in the latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest denied-request count, or zero for invalid input.
int64_t rt_canvas3d_get_shadow_requests_dropped(void *obj);
/// @brief Shadow slots satisfied without re-rendering in the latest frame
///   (signature reuse + ADR 0309 render-target inheritance).
/// @param obj Borrowed Canvas3D handle.
/// @return Latest cached/inherited slot count, or zero for invalid input.
int64_t rt_canvas3d_get_shadow_slots_cached(void *obj);
/// @brief ADR 0309: opt render-target frames into shadow inheritance.
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Nonzero to enable, zero to disable (the default).
void rt_canvas3d_set_render_target_shadow_inherit(void *obj, int64_t enabled);
/// @brief Set the per-cluster light-index capacity (8..64, default 64).
/// @param obj Borrowed Canvas3D handle.
/// @param budget Requested per-cluster capacity, clamped from 8 through 64.
void rt_canvas3d_set_cluster_light_budget(void *obj, int64_t budget);
/// @brief Lifetime count of cluster light-index entries truncated by capacity.
/// @param obj Borrowed Canvas3D handle.
/// @return Saturating lifetime truncation count, or zero for invalid input.
int64_t rt_canvas3d_get_cluster_overflow_count(void *obj);
/// @brief Enabled lights truncated by the forward-path light limit this frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest dropped-light count, or zero for invalid input.
int64_t rt_canvas3d_get_dropped_light_count(void *obj);
/// @brief Number of draw submissions rejected by CPU frustum culling in the latest ended frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest frustum-rejection count, or zero for invalid input.
int64_t rt_canvas3d_get_frustum_culled_draw_count(void *obj);
/// @brief Number of opaque draw submissions rejected by the CPU occlusion grid in the latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest CPU-occlusion rejection count, or zero for invalid input.
int64_t rt_canvas3d_get_cpu_occluded_draw_count(void *obj);
/// @brief Number of opaque draw candidates tested by the CPU occlusion grid in the latest frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest candidate count, or zero for invalid input.
int64_t rt_canvas3d_get_occlusion_candidate_count(void *obj);
/// @brief Texture payload bytes uploaded to backend storage in the latest ended frame.
/// @param obj Borrowed Canvas3D handle.
/// @return Latest texture-upload byte count, or zero for invalid input.
int64_t rt_canvas3d_get_texture_upload_bytes(void *obj);
/// @brief Latest completed backend GPU frame time in microseconds, or 0 when unsupported.
/// @param obj Borrowed Canvas3D handle.
/// @return Backend-reported microseconds, or zero when unavailable.
int64_t rt_canvas3d_get_frame_gpu_time_us(void *obj);
/// @brief CPU milliseconds one render stage took last frame (0=shadow, 1=main,
///   2=overlay, 3=backend end-of-frame).
/// @param obj Borrowed Canvas3D handle.
/// @param pass Zero-based timing-stage identifier.
/// @return Latest CPU duration in milliseconds, or zero for invalid or unavailable input.
double rt_canvas3d_get_pass_cpu_ms(void *obj, int64_t pass);
/// @brief Number of PassCpuMs stages.
/// @param obj Borrowed Canvas3D handle.
/// @return Number of addressable CPU timing stages, or zero for invalid input.
int64_t rt_canvas3d_get_pass_count(void *obj);
/// @brief Backend draw submissions issued since the latest public Begin/Begin2D.
/// @param obj Borrowed Canvas3D handle.
/// @return Current pass submission count, or zero for invalid input.
int64_t rt_canvas3d_get_draws_submitted(void *obj);
/// @brief World-AABB transform computations performed since the latest public Begin/Begin2D.
/// @param obj Borrowed Canvas3D handle.
/// @return Current pass AABB-transform count, or zero for invalid input.
int64_t rt_canvas3d_get_aabb_transforms(void *obj);
/// @brief Stable deferred sort passes run since the latest public Begin/Begin2D.
/// @param obj Borrowed Canvas3D handle.
/// @return Current pass sort count, or zero for invalid input.
int64_t rt_canvas3d_get_sort_passes(void *obj);
/// @brief Material/backend state-group transitions observed during backend submission.
/// @param obj Borrowed Canvas3D handle.
/// @return Current/latest frame state-transition count, or zero for invalid input.
int64_t rt_canvas3d_get_backend_state_changes(void *obj);
/// @brief Set the backend texture upload byte budget for each frame; negative disables the budget.
/// @param obj Borrowed Canvas3D handle.
/// @param bytes Non-negative per-frame budget in bytes, or a negative value for unlimited uploads.
void rt_canvas3d_set_texture_upload_budget(void *obj, int64_t bytes);
/// @brief Texture payload bytes still waiting for backend texture upload budget.
/// @param obj Borrowed Canvas3D handle.
/// @return Pending payload bytes, or zero for invalid input.
int64_t rt_canvas3d_get_texture_upload_pending_bytes(void *obj);
/// @brief Enable or disable automatic TextureAsset3D mip-residency streaming (default off).
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to permit automatic mip-window changes.
void rt_canvas3d_set_texture_streaming(void *obj, int8_t enabled);
/// @brief Bias streaming's desired mip level; positive drops more detail. Clamped to [-16, 16].
/// @param obj Borrowed Canvas3D handle.
/// @param bias Requested signed mip-level bias.
void rt_canvas3d_set_texture_streaming_bias(void *obj, double bias);
/// @brief Lifetime count of resident-window demotions applied by texture streaming.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime demotion count, or zero for invalid input.
int64_t rt_canvas3d_get_texture_streaming_demotions(void *obj);
/// @brief Successful draw calls emitted by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime backend draw-call count, or zero for invalid input.
int64_t rt_canvas3d_get_backend_draw_calls(void *obj);
/// @brief Draw commands rejected inside the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime backend rejection count, or zero for invalid input.
int64_t rt_canvas3d_get_backend_dropped_draws(void *obj);
/// @brief Static mesh cache hits observed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime cache-hit count, or zero for invalid input.
int64_t rt_canvas3d_get_backend_mesh_cache_hits(void *obj);
/// @brief Static mesh cache misses observed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime cache-miss count, or zero for invalid input.
int64_t rt_canvas3d_get_backend_mesh_cache_misses(void *obj);
/// @brief Transient mesh uploads performed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime stream-upload count, or zero for invalid input.
int64_t rt_canvas3d_get_backend_mesh_stream_uploads(void *obj);
/// @brief Fallback texture binds observed by the active backend since canvas creation.
/// @param obj Borrowed Canvas3D handle.
/// @return Non-negative lifetime fallback-bind count, or zero for invalid input.
int64_t rt_canvas3d_get_backend_texture_fallback_binds(void *obj);
/// @brief Active backend present path: 0 unknown, 1 direct GPU drawable, 2 offscreen resolve.
/// @param obj Borrowed Canvas3D handle.
/// @return Present-path identifier, or zero when unknown or invalid.
int64_t rt_canvas3d_get_backend_present_path(void *obj);
/// @brief Capture the current back-buffer contents into a fresh Pixels object.
/// @param obj Borrowed Canvas3D handle.
/// @return New GC-managed Pixels snapshot, or NULL on invalid input or readback failure.
void *rt_canvas3d_screenshot(void *obj);
/// @brief Try to copy the current back buffer into an existing same-size Pixels object.
/// @details Reuses canvas-owned GPU staging storage after warm-up and bumps Pixels generation.
/// @param obj Borrowed Canvas3D handle.
/// @param pixels Borrowed writable Pixels destination matching the active output size.
/// @return 1 on successful readback and copy; otherwise 0.
int8_t rt_canvas3d_try_copy_screenshot_to(void *obj, void *pixels);
/// @brief Begin recording a final overlay pass composited after post-FX during finalization.
/// @param obj Borrowed Canvas3D handle not already recording a final overlay.
void rt_canvas3d_begin_overlay(void *obj);
/// @brief Finish recording the final overlay pass started by BeginOverlay.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_end_overlay(void *obj);
/// @brief Discard any recorded final overlay commands for the current frame.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_clear_overlay(void *obj);
/// @brief Apply post-FX and final overlay exactly once, without presenting.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_finalize_frame(void *obj);
/// @brief Capture finalized pixels, finalizing first if needed.
/// @param obj Borrowed Canvas3D handle.
/// @return New GC-managed Pixels snapshot, or NULL on invalid input or readback failure.
void *rt_canvas3d_screenshot_final(void *obj);
/// @brief Finalize if needed, then try to copy into an existing same-size Pixels object.
/// @param obj Borrowed Canvas3D handle.
/// @param pixels Borrowed writable Pixels destination matching the active output size.
/// @return 1 on successful finalization and readback; otherwise 0.
int8_t rt_canvas3d_try_copy_screenshot_final_to(void *obj, void *pixels);
/// @brief Return whether the current frame has already been finalized.
/// @param obj Borrowed Canvas3D handle.
/// @return 1 when final compositing is complete for the frame; otherwise 0.
int8_t rt_canvas3d_get_frame_finalized(void *obj);

//=========================================================================
// CubeMap3D — 6-face cube map for skybox + reflections
//=========================================================================

/// @brief Build a cubemap from six Pixels faces (px = +X, nx = -X, etc.).
/// @details All faces must be live compatible square Pixels objects. The returned cubemap retains
/// the face resources required by its implementation.
/// @param px Positive-X face.
/// @param nx Negative-X face.
/// @param py Positive-Y face.
/// @param ny Negative-Y face.
/// @param pz Positive-Z face.
/// @param nz Negative-Z face.
/// @return New GC-managed CubeMap3D handle, or NULL on validation or allocation failure.
void *rt_cubemap3d_new(void *px, void *nx, void *py, void *ny, void *pz, void *nz);
/// @brief Load a Radiance .hdr equirectangular panorama as a CubeMap3D
///   (linear decode, engine face projection, Reinhard range compression at
///   @p exposure; exposure <= 0 defaults to 1).
/// @param path Borrowed runtime string containing the panorama path.
/// @param exposure Positive decode multiplier; non-positive values use 1.
/// @return New GC-managed CubeMap3D, or NULL when loading, decoding, or allocation fails.
void *rt_cubemap3d_load_hdr_panorama(rt_string path, double exposure);
/// @brief Bind a cubemap as the scene skybox (rendered behind opaque geometry).
/// @param canvas Borrowed Canvas3D handle.
/// @param cubemap Borrowed live CubeMap3D retained until replaced or cleared.
void rt_canvas3d_set_skybox(void *canvas, void *cubemap);
/// @brief Remove the skybox; subsequent clears use the regular clear color.
/// @param canvas Borrowed Canvas3D handle.
void rt_canvas3d_clear_skybox(void *canvas);
/// @brief Bind a cubemap as a material's environment reflection map.
/// @param obj Borrowed Material3D handle.
/// @param cubemap Borrowed CubeMap3D retained until replaced, or NULL to clear.
void rt_material3d_set_env_map(void *obj, void *cubemap);
/// @brief Set how strongly the material reflects its env map (0.0–1.0).
/// @param obj Borrowed Material3D handle.
/// @param r Reflectivity clamped to the inclusive normalized range.
void rt_material3d_set_reflectivity(void *obj, double r);
/// @brief Read the current reflectivity scalar.
/// @param obj Borrowed Material3D handle.
/// @return Stored normalized reflectivity, or zero for invalid input.
double rt_material3d_get_reflectivity(void *obj);

//=========================================================================
// RenderTarget3D — offscreen rendering target
//=========================================================================

/// @brief Allocate an offscreen render target of the given pixel dimensions.
/// @param width Positive color/depth attachment width in pixels.
/// @param height Positive color/depth attachment height in pixels.
/// @return New GC-managed LDR RenderTarget3D, or NULL on validation or allocation failure.
void *rt_rendertarget3d_new(int64_t width, int64_t height);
/// @brief Allocate an HDR offscreen render target with RGBA16F color storage.
/// @param width Positive color/depth attachment width in pixels.
/// @param height Positive color/depth attachment height in pixels.
/// @return New GC-managed HDR RenderTarget3D, or NULL on validation or allocation failure.
void *rt_rendertarget3d_new_hdr(int64_t width, int64_t height);
/// @brief Width of the render target in pixels.
/// @param obj Borrowed RenderTarget3D handle.
/// @return Positive target width, or zero for invalid input.
int64_t rt_rendertarget3d_get_width(void *obj);
/// @brief Height of the render target in pixels.
/// @param obj Borrowed RenderTarget3D handle.
/// @return Positive target height, or zero for invalid input.
int64_t rt_rendertarget3d_get_height(void *obj);
/// @brief Return non-zero when the render target stores HDR color internally.
/// @param obj Borrowed RenderTarget3D handle.
/// @return Non-zero for RGBA16F storage; zero for LDR or invalid input.
int32_t rt_rendertarget3d_get_is_hdr(void *obj);
/// @brief Get a Pixels view of the render target's color attachment (CPU readback).
/// @param obj Borrowed RenderTarget3D handle.
/// @return GC-managed Pixels representation of the current color attachment, or NULL on failure.
void *rt_rendertarget3d_as_pixels(void *obj);
/// @brief Allocation-free readback into an existing same-size Pixels buffer.
/// @param obj Borrowed RenderTarget3D handle.
/// @param pixels Borrowed writable same-size Pixels destination.
void rt_rendertarget3d_copy_to(void *obj, void *pixels);
int rt_rendertarget3d_try_read_rgba(void *obj, uint8_t *dst, int64_t width, int64_t height);

/// @brief Redirect subsequent draws to @p target instead of the swapchain.
/// @details The canvas retains a validated target and releases any previous target binding.
/// @param canvas Borrowed Canvas3D handle outside an incompatible backend transaction.
/// @param target Borrowed live RenderTarget3D to retain.
void rt_canvas3d_set_render_target(void *canvas, void *target);
/// @brief Restore the swapchain as the active draw target.
/// @param canvas Borrowed Canvas3D handle. Offscreen canvases may require another explicit target.
void rt_canvas3d_reset_render_target(void *canvas);

//=========================================================================
// Mesh3D — 3D mesh with vertices and triangle indices
//=========================================================================

/// @brief Allocate an empty Mesh3D (no vertices, no triangles).
/// @return New GC-managed empty Mesh3D, or NULL on allocation failure.
void *rt_mesh3d_new(void);
/// @brief Reset vertex and index counts to 0 without freeing the backing arrays.
/// @param obj Borrowed Mesh3D handle whose storage capacity is preserved.
void rt_mesh3d_clear(void *obj);
/// @brief Reserve backing storage for at least vertex_count vertices and triangle_count triangles.
/// @param obj Borrowed Mesh3D handle.
/// @param vertex_count Requested non-negative vertex capacity.
/// @param triangle_count Requested non-negative triangle capacity; index storage reserves three
/// entries per triangle.
void rt_mesh3d_reserve(void *obj, int64_t vertex_count, int64_t triangle_count);
/// @brief Build a unit-cube-style box mesh of size (sx, sy, sz) with normals and UVs.
/// @param sx Box extent along X in world units.
/// @param sy Box extent along Y in world units.
/// @param sz Box extent along Z in world units.
/// @return New GC-managed box mesh, or NULL on invalid dimensions or allocation failure.
void *rt_mesh3d_new_box(double sx, double sy, double sz);
/// @brief Build a UV-sphere mesh with the given radius and longitude segment count.
/// @param radius Sphere radius in world units.
/// @param segments Longitudinal tessellation count; the implementation enforces safe limits.
/// @return New GC-managed sphere mesh, or NULL on invalid input or allocation failure.
void *rt_mesh3d_new_sphere(double radius, int64_t segments);
/// @brief Build a flat XZ plane of size (sx, sz) facing +Y with a single quad.
/// @param sx Plane extent along X in world units.
/// @param sz Plane extent along Z in world units.
/// @return New GC-managed plane mesh, or NULL on invalid input or allocation failure.
void *rt_mesh3d_new_plane(double sx, double sz);
/// @brief Build a cylinder of radius @p r and height @p h with @p segments around the axis.
/// @param r Cylinder radius in world units.
/// @param h Cylinder height in world units.
/// @param segments Circumferential tessellation count.
/// @return New GC-managed cylinder mesh, or NULL on invalid input or allocation failure.
void *rt_mesh3d_new_cylinder(double r, double h, int64_t segments);
/// @brief Load a triangle mesh from a Wavefront .obj file (positions/normals/UVs).
/// @param path Borrowed runtime string containing the OBJ path.
/// @return New GC-managed mesh, or NULL after a file, parse, validation, or allocation failure.
void *rt_mesh3d_from_obj(rt_string path);
/// @brief Load a triangle mesh from a binary STL file (positions + per-face normals).
/// @param path Borrowed runtime string containing the STL path.
/// @return New GC-managed mesh, or NULL after a file, parse, validation, or allocation failure.
void *rt_mesh3d_from_stl(rt_string path);
/// @brief Number of unique vertices currently in the mesh.
/// @param obj Borrowed Mesh3D handle.
/// @return Non-negative vertex count, or zero for invalid input.
int64_t rt_mesh3d_get_vertex_count(void *obj);
/// @brief Read one mesh-local vertex position without exposing mutable geometry storage.
/// @details The returned vector preserves an authoritative double-precision position sidecar
/// when one exists and is independent of later mesh mutation.
/// @param obj Borrowed Mesh3D handle.
/// @param index Zero-based vertex index.
/// @return Fresh GC-managed Vec3 handle, or NULL for an invalid mesh or out-of-range index.
void *rt_mesh3d_get_vertex_position(void *obj, int64_t index);
/// @brief Minimum corner of the mesh-local axis-aligned bounding box.
/// @param obj Mesh3D receiver.
/// @return Fresh GC-managed Vec3, or NULL for an invalid or empty mesh.
/// @brief Append @p src_obj's geometry into @p obj.
/// @details Merges triangles so many parts can render as one draw call. Takes
///          no transform: meshes are GC-managed with no destroy entry point, so
///          the caller clones and transforms first
///          (`part = src.Clone(); part.Transform(m); dst.Append(part);`),
///          keeping that allocation explicit. Skinned and morph-target meshes
///          are rejected rather than silently corrupted.
/// @param obj Destination mesh, mutated in place.
/// @param src_obj Source mesh; unchanged.
void rt_mesh3d_append(void *obj, void *src_obj);
void *rt_mesh3d_get_bounds_min(void *obj);
/// @brief Maximum corner of the mesh-local axis-aligned bounding box.
/// @param obj Mesh3D receiver.
/// @return Fresh GC-managed Vec3, or NULL for an invalid or empty mesh.
void *rt_mesh3d_get_bounds_max(void *obj);
/// @brief Centre of the mesh-local bounding box.
/// @param obj Mesh3D receiver.
/// @return Fresh GC-managed Vec3, or NULL for an invalid or empty mesh.
void *rt_mesh3d_get_bounds_center(void *obj);
/// @brief Extent of the mesh-local bounding box along each axis.
/// @param obj Mesh3D receiver.
/// @return Fresh GC-managed Vec3, or NULL for an invalid or empty mesh.
void *rt_mesh3d_get_bounds_size(void *obj);
/// @brief Radius of the mesh-local bounding sphere.
/// @param obj Mesh3D receiver.
/// @return Bounding-sphere radius, or zero for an invalid or empty mesh.
double rt_mesh3d_get_bounds_radius(void *obj);
/// @brief Number of triangles currently in the mesh (== indices / 3).
/// @param obj Borrowed Mesh3D handle.
/// @return Non-negative triangle count, or zero for invalid input.
int64_t rt_mesh3d_get_triangle_count(void *obj);
/// @brief Read one vertex's position/normal/uv (internal readback; 1 on success).
/// @param obj Borrowed Mesh3D handle.
/// @param index Zero-based vertex index.
/// @param[out] out_pos Writable three-double position array.
/// @param[out] out_normal Writable three-double normal array.
/// @param[out] out_uv Writable two-double texture-coordinate array.
/// @return 1 when every output was written; otherwise 0.
int8_t rt_mesh3d_get_vertex_raw(
    void *obj, int64_t index, double out_pos[3], double out_normal[3], double out_uv[2]);
/// @brief Read one triangle's vertex indices (internal readback; 1 on success).
/// @param obj Borrowed Mesh3D handle.
/// @param triangle Zero-based triangle index.
/// @param[out] out_indices Writable three-element index array.
/// @return 1 when the triangle exists and the output was written; otherwise 0.
int8_t rt_mesh3d_get_triangle_raw(void *obj, int64_t triangle, int64_t out_indices[3]);
/// @brief True when the mesh payload is resident and drawable.
/// @param obj Borrowed Mesh3D handle.
/// @return 1 when resident; otherwise 0.
int8_t rt_mesh3d_get_resident(void *obj);
/// @brief Mark the mesh payload resident/nonresident without releasing the Mesh3D handle.
/// @param obj Borrowed Mesh3D handle.
/// @param resident Non-zero to make retained payload drawable; zero to mark it unavailable.
void rt_mesh3d_set_resident(void *obj, int8_t resident);
/// @brief Estimated bytes for the currently resident vertex/index payload.
/// @param obj Borrowed Mesh3D handle.
/// @return Non-negative resident byte estimate, or zero for invalid/nonresident input.
int64_t rt_mesh3d_get_resident_bytes(void *obj);
/// @brief Opt the mesh into the compact 48-byte GPU static-cache vertex encoding (R20).
/// @param obj Borrowed Mesh3D handle.
/// @param enabled Non-zero to prefer compact static streams; zero for the full encoding.
void rt_mesh3d_set_compact_streams(void *obj, int8_t enabled);
/// @brief Whether the mesh opted into the compact GPU vertex-stream encoding.
/// @param obj Borrowed Mesh3D handle.
/// @return 1 when compact streams are requested; otherwise 0.
int8_t rt_mesh3d_get_compact_streams(void *obj);
/// @brief Estimated retained CPU vertex/index bytes regardless of resident draw state.
/// @param obj Borrowed Mesh3D handle.
/// @return Non-negative retained byte estimate, or zero for invalid input.
int64_t rt_mesh3d_get_retained_bytes(void *obj);
/// @brief Free auxiliary CPU geometry (double positions, normal scratch) a
///   finished static mesh no longer needs; returns bytes released.
/// @param obj Borrowed Mesh3D handle whose core drawable streams are preserved.
/// @return Number of auxiliary bytes released, or zero when none or invalid.
int64_t rt_mesh3d_release_cpu_scratch(void *obj);
/// @brief Append a vertex with position, normal, and UV.
/// @param obj Borrowed mutable Mesh3D handle.
/// @param x Position X in mesh-local units.
/// @param y Position Y in mesh-local units.
/// @param z Position Z in mesh-local units.
/// @param nx Normal X component.
/// @param ny Normal Y component.
/// @param nz Normal Z component.
/// @param u Texture U coordinate.
/// @param v Texture V coordinate.
void rt_mesh3d_add_vertex(
    void *obj, double x, double y, double z, double nx, double ny, double nz, double u, double v);
/// @brief Append a triangle by referencing three previously-added vertex indices (CCW = front).
/// @param obj Borrowed mutable Mesh3D handle.
/// @param v0 First zero-based vertex index.
/// @param v1 Second zero-based vertex index.
/// @param v2 Third zero-based vertex index.
void rt_mesh3d_add_triangle(void *obj, int64_t v0, int64_t v1, int64_t v2);

/// @brief Test whether three indices satisfy AddTriangle without trapping.
/// @details The indices must be distinct, in range, and form a face above the same scale-relative
/// area epsilon enforced by `rt_mesh3d_add_triangle`. Loaders can use this probe to skip malformed
/// source faces before mutation.
/// @param obj Borrowed Mesh3D handle.
/// @param v0 Candidate first vertex index.
/// @param v1 Candidate second vertex index.
/// @param v2 Candidate third vertex index.
/// @return 1 when AddTriangle would accept the face; otherwise 0.
int rt_mesh3d_triangle_indices_valid(void *obj, int64_t v0, int64_t v1, int64_t v2);
/// @brief Recompute smooth per-vertex normals from triangle face normals (overwrites existing).
/// @param obj Borrowed mutable Mesh3D handle.
void rt_mesh3d_recalc_normals(void *obj);
/// @brief Rasterize the UV footprint of triangles inside an object-space Y band into a
///   Pixels mask (opaque white; untouched elsewhere) — body-zone texture masking.
/// @param obj Borrowed Mesh3D handle.
/// @param mask_pixels Borrowed mutable Pixels handle receiving the coverage.
/// @param y_min Inclusive lower object-space Y bound.
/// @param y_max Inclusive upper object-space Y bound.
void rt_mesh3d_rasterize_uv_mask_y(void *obj, void *mask_pixels, double y_min, double y_max);

/// @brief Rasterize the mesh's triangles into UV space writing the
///        BARYCENTRIC-INTERPOLATED object-space Y per texel (see
///        rt_mesh3d.c). [y_min, y_max] maps to luminance 1..255
///        (0 = uncovered); same conservative half-texel coverage as
///        rt_mesh3d_rasterize_uv_mask_y; overlapping triangles last-win.
///        Gives callers texel-exact height classification where the band
///        op could only include whole straddling triangles.
/// @param obj Mesh3D receiver; invalid handles are ignored.
/// @param height_pixels Pixels handle receiving the height map (any size).
/// @param y_min Object-space Y mapping to luminance 1.
/// @param y_max Object-space Y mapping to luminance 255 (> y_min).
void rt_mesh3d_rasterize_uv_height(void *obj, void *height_pixels, double y_min, double y_max);
/// @brief Deep copy the mesh (independent storage; safe to mutate the clone).
/// @param obj Borrowed source Mesh3D handle.
/// @return New independent GC-managed Mesh3D, or NULL on invalid input or allocation failure.
void *rt_mesh3d_clone(void *obj);
/// @brief Transform every vertex position (and rotate normals) by the given Mat4.
/// @param obj Borrowed mutable Mesh3D handle.
/// @param mat4 Borrowed Mat4 applied to positions and normal directions.
void rt_mesh3d_transform(void *obj, void *mat4);
/// @brief Produce the left/right-mirrored copy of a mesh (`Mesh3D.Mirror`, ADR 0306).
/// @details Positions, normals and tangents are reflected across model X = 0 with the
///          `Mesh3D.Transform` math (winding reversed, tangent handedness negated), morph
///          deltas are reflected, and every bone influence is remapped to its sagittal
///          partner (`rt_skeleton3d_mirror_bone`: exact side-token swap, humanoid-role
///          flip, else self) so a skinned copy reads as the opposite-handed character on
///          the SAME skeleton. Meshes without bone weights mirror as plain geometry.
/// @param obj Borrowed source Mesh3D handle.
/// @param skeleton Borrowed Skeleton3D used for the partner resolution; NULL uses the
///        mesh's attached skeleton. A non-skeleton handle yields NULL.
/// @return New independent GC-managed Mesh3D, or NULL on invalid input / allocation failure.
void *rt_mesh3d_mirror(void *obj, void *skeleton);
/// @brief Wrap the mesh's X extent around a vertical circular arc (`Mesh3D.BendArc`, ADR 0294).
/// @details The arc centre lies on local +Z, `radius` from the chord midpoint; vertices at depth z
///          land on radius `radius - z`. Normals/tangents rotate with the local frame; winding is
///          preserved. Traps (without mutating) on invalid arguments, skinned/morph meshes, a
///          zero X extent, or any vertex whose depth reaches the radius.
/// @param obj Borrowed mutable Mesh3D handle.
/// @param radius Bend radius (finite, > 0).
/// @param arc_degrees Angle the X extent spans after bending, in (0, 360].
void rt_mesh3d_bend_arc(void *obj, double radius, double arc_degrees);
/// @brief Compute per-vertex tangent vectors from UVs (required for normal mapping).
/// @param obj Borrowed mutable Mesh3D handle.
void rt_mesh3d_calc_tangents(void *obj);

//=========================================================================
// Camera3D — perspective camera with view/projection matrices
//=========================================================================

/// @brief Create a perspective camera (FOV in degrees, aspect = width/height, near/far clip
/// planes).
/// @param fov Vertical field of view in degrees.
/// @param aspect Viewport width divided by height.
/// @param near_val Requested near clipping distance in world units.
/// @param far_val Requested far clipping distance in world units.
/// @return New GC-managed Camera3D with sanitized projection parameters, or NULL on allocation
/// failure.
void *rt_camera3d_new(double fov, double aspect, double near_val, double far_val);
/// @brief Create a perspective camera from a horizontal field of view in degrees.
/// @details Converts @p horizontal_fov to the vertical aperture used by the renderer's projection
///   matrix using @p aspect. This is useful for first-person/open-world cameras where designers
///   usually author FOV horizontally and a vertical interpretation would look too wide on
///   widescreen displays.
/// @param horizontal_fov Horizontal aperture in degrees.
/// @param aspect Viewport width divided by height.
/// @param near_val Requested near clipping distance in world units.
/// @param far_val Requested far clipping distance in world units.
/// @return New GC-managed perspective Camera3D, or NULL on allocation failure.
void *rt_camera3d_new_horizontal_fov(double horizontal_fov,
                                     double aspect,
                                     double near_val,
                                     double far_val);
/// @brief Create an orthographic camera (vertical world-units, aspect, near/far).
/// @param size Orthographic view-volume half-height in world units.
/// @param aspect Viewport width divided by height.
/// @param near_val Requested near clipping distance in world units.
/// @param far_val Requested far clipping distance in world units.
/// @return New GC-managed orthographic Camera3D, or NULL on allocation failure.
void *rt_camera3d_new_ortho(double size, double aspect, double near_val, double far_val);
/// @brief True if the camera was created via `_new_ortho` (no perspective foreshortening).
/// @param cam Borrowed Camera3D handle.
/// @return 1 when orthographic projection is active; otherwise 0.
int8_t rt_camera3d_is_ortho(void *cam);
/// @brief Switch between perspective and orthographic projection, preserving both parameter sets.
/// @param cam Borrowed Camera3D handle.
/// @param is_ortho Non-zero for orthographic projection; zero for perspective.
void rt_camera3d_set_is_ortho(void *cam, int8_t is_ortho);
/// @brief Get the orthographic view-volume half-height retained by the camera.
/// @param cam Borrowed Camera3D handle.
/// @return Positive retained half-height, or zero for invalid input.
double rt_camera3d_get_ortho_size(void *cam);
/// @brief Set the orthographic view-volume half-height retained by the camera.
/// @param cam Borrowed Camera3D handle.
/// @param size Requested positive half-height in world units.
void rt_camera3d_set_ortho_size(void *cam, double size);
/// @brief Aim the camera at a target point with an explicit up direction.
/// @param obj Borrowed Camera3D handle.
/// @param eye Borrowed Vec3 world-space camera position.
/// @param target Borrowed Vec3 world-space aim point.
/// @param up Borrowed Vec3 preferred up direction.
void rt_camera3d_look_at(void *obj, void *eye, void *target, void *up);
/// @brief Position the camera on a sphere around @p target at the given yaw/pitch in degrees.
/// @param obj Borrowed Camera3D handle.
/// @param target Borrowed Vec3 orbit center.
/// @param distance Camera distance from the center in world units.
/// @param yaw Horizontal orbit angle in degrees.
/// @param pitch Vertical orbit angle in degrees.
void rt_camera3d_orbit(void *obj, void *target, double distance, double yaw, double pitch);
/// @brief Get the field of view in degrees (perspective cameras only).
/// @param obj Borrowed Camera3D handle.
/// @return Stored vertical field of view in degrees, or zero for invalid input.
double rt_camera3d_get_fov(void *obj);
/// @brief Set the field of view in degrees.
/// @param obj Borrowed perspective Camera3D handle.
/// @param fov Requested vertical aperture in degrees; sanitized by camera guardrails.
void rt_camera3d_set_fov(void *obj, double fov);
/// @brief Set the perspective camera's field of view using horizontal degrees for its aspect.
/// @details Orthographic cameras ignore the call. Perspective cameras convert the supplied
///   horizontal aperture through the camera's current aspect ratio and rebuild their projection
///   immediately.
/// @param obj Borrowed perspective Camera3D handle.
/// @param horizontal_fov Requested horizontal aperture in degrees.
void rt_camera3d_set_horizontal_fov(void *obj, double horizontal_fov);
/// @brief Get the near clip-plane distance.
/// @param obj Borrowed Camera3D handle.
/// @return Authored near-plane distance, or zero for invalid input.
double rt_camera3d_get_near_plane(void *obj);
/// @brief Get the sanitized near clip-plane distance used for projection/rendering.
/// @details This read-only value reflects Camera3D's precision guardrails, including the
///   enforced near/far ratio cap that prevents depth-buffer precision loss.
/// @param obj Borrowed Camera3D handle.
/// @return Sanitized positive near-plane distance used by projection, or zero for invalid input.
double rt_camera3d_get_effective_near_plane(void *obj);
/// @brief Set the near clip-plane distance.
/// @param obj Borrowed Camera3D handle.
/// @param near_plane Requested distance in world units; projection guardrails sanitize it.
void rt_camera3d_set_near_plane(void *obj, double near_plane);
/// @brief Get the far clip-plane distance.
/// @param obj Borrowed Camera3D handle.
/// @return Authored far-plane distance, or zero for invalid input.
double rt_camera3d_get_far_plane(void *obj);
/// @brief Get the sanitized far clip-plane distance used for projection/rendering.
/// @details The returned value is the clip plane after Camera3D validates finite values and
///   resolves degenerate near/far ranges.
/// @param obj Borrowed Camera3D handle.
/// @return Sanitized far-plane distance used by projection, or zero for invalid input.
double rt_camera3d_get_effective_far_plane(void *obj);
/// @brief Set the far clip-plane distance.
/// @param obj Borrowed Camera3D handle.
/// @param far_plane Requested distance in world units; projection guardrails sanitize it.
void rt_camera3d_set_far_plane(void *obj, double far_plane);
/// @brief Get the camera world-space position as a Vec3.
/// @param obj Borrowed Camera3D handle.
/// @return New GC-managed Vec3 containing the position, or NULL for invalid input/allocation
/// failure.
void *rt_camera3d_get_position(void *obj);
/// @brief Get the camera world-space position without allocating a Vec3 wrapper.
/// @param obj Borrowed Camera3D handle.
/// @param x Required output for the sanitized world X coordinate.
/// @param y Required output for the sanitized world Y coordinate.
/// @param z Required output for the sanitized world Z coordinate.
/// @return Nonzero when all outputs were written from a valid camera; otherwise zero.
int8_t rt_camera3d_get_position_components(void *obj, double *x, double *y, double *z);
/// @brief Move the camera to the given world-space position (Vec3).
/// @param obj Borrowed Camera3D handle.
/// @param pos Borrowed Vec3 world-space position.
void rt_camera3d_set_position(void *obj, void *pos);
/// @brief Get the unit forward vector (the direction the camera is facing).
/// @param obj Borrowed Camera3D handle.
/// @return New GC-managed normalized Vec3, or NULL for invalid input/allocation failure.
void *rt_camera3d_get_forward(void *obj);
/// @brief Get the unit right vector (perpendicular to forward and up).
/// @param obj Borrowed Camera3D handle.
/// @return New GC-managed normalized Vec3, or NULL for invalid input/allocation failure.
void *rt_camera3d_get_right(void *obj);
/// @brief Get the unit up vector (the camera's screen-up axis, ADR 0227).
/// @param obj Borrowed Camera3D handle.
/// @return New GC-managed normalized Vec3, or NULL for invalid input/allocation failure.
void *rt_camera3d_get_up(void *obj);
/// @brief Get the camera's retained row-major view matrix (ADR 0227).
/// @param obj Borrowed Camera3D handle.
/// @return New GC-managed Mat4 copy, or NULL for invalid input/allocation failure.
void *rt_camera3d_get_view_matrix(void *obj);
/// @brief Get the camera's retained row-major projection matrix (ADR 0227).
/// @details Exactly the projection the renderer last synced for this camera —
///          including reversed-Z on backends that use it — so Zia-side
///          projection math can never drift from the rendered result.
/// @param obj Borrowed Camera3D handle.
/// @return New GC-managed Mat4 copy, or NULL for invalid input/allocation failure.
void *rt_camera3d_get_projection_matrix(void *obj);
/// @brief Get the width-to-height aspect ratio the projection was built with (ADR 0227).
/// @param obj Borrowed Camera3D handle.
/// @return Retained positive aspect ratio, or 0.0 for an invalid camera.
double rt_camera3d_get_aspect(void *obj);
/// @brief Project a world point to pixels; returns 1 when in front of the camera.
/// @param obj Borrowed Camera3D handle.
/// @param x World-space X coordinate.
/// @param y World-space Y coordinate.
/// @param z World-space Z coordinate.
/// @param sw Positive viewport width in pixels.
/// @param sh Positive viewport height in pixels.
/// @param[out] out_sx Optional destination for horizontal screen pixels.
/// @param[out] out_sy Optional destination for vertical screen pixels.
/// @return 1 when the point projects in front of the camera; otherwise 0.
int8_t rt_camera3d_world_to_screen(void *obj,
                                   double x,
                                   double y,
                                   double z,
                                   int64_t sw,
                                   int64_t sh,
                                   double *out_sx,
                                   double *out_sy);
/// @brief VM-facing WorldToScreen: Vec3(pixelX, pixelY, visible ? 1 : 0).
/// @param obj Borrowed Camera3D handle.
/// @param point Borrowed Vec3 world-space point.
/// @param sw Positive viewport width in pixels.
/// @param sh Positive viewport height in pixels.
/// @return New GC-managed Vec3 carrying screen X, screen Y, and visibility, or NULL on invalid
/// input/allocation failure.
void *rt_camera3d_world_to_screen_vec(void *obj, void *point, int64_t sw, int64_t sh);
/// @brief Return a normalized world-space picking direction for a screen pixel.
/// @details Combine the result with `rt_camera3d_screen_to_ray_origin`. Orthographic cameras return
/// parallel forward directions, while perspective cameras produce eye-originating rays.
/// @param obj Borrowed Camera3D handle.
/// @param sx Horizontal screen coordinate in pixels.
/// @param sy Vertical screen coordinate in pixels.
/// @param sw Positive viewport width in pixels.
/// @param sh Positive viewport height in pixels.
/// @return New GC-managed normalized direction Vec3, or NULL on invalid input/allocation failure.
void *rt_camera3d_screen_to_ray(void *obj, int64_t sx, int64_t sy, int64_t sw, int64_t sh);
/// @brief Return the world-space origin for a screen-space picking ray.
/// @param obj Borrowed Camera3D handle.
/// @param sx Horizontal screen coordinate in pixels.
/// @param sy Vertical screen coordinate in pixels.
/// @param sw Positive viewport width in pixels.
/// @param sh Positive viewport height in pixels.
/// @return New GC-managed ray-origin Vec3, or NULL on invalid input/allocation failure.
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
/// @return New GC-managed Material3D with default legacy parameters, or NULL on allocation failure.
void *rt_material3d_new(void);
/// @brief Create a flat-color legacy material with the given diffuse color.
/// @param r Normalized red diffuse channel.
/// @param g Normalized green diffuse channel.
/// @param b Normalized blue diffuse channel.
/// @return New GC-managed legacy Material3D, or NULL on allocation failure.
void *rt_material3d_new_color(double r, double g, double b);
/// @brief Create a textured legacy material from Pixels, TextureAsset3D, or RenderTarget3D.
/// @param pixels Borrowed supported texture source retained by the material on success.
/// @return New GC-managed textured Material3D, or NULL on validation/allocation failure.
void *rt_material3d_new_textured(void *pixels);
/// @brief Create a PBR-workflow material with the given base color (default metallic=0,
/// roughness=0.5).
/// @param r Normalized red base-color channel.
/// @param g Normalized green base-color channel.
/// @param b Normalized blue base-color channel.
/// @return New GC-managed PBR Material3D, or NULL on allocation failure.
void *rt_material3d_new_pbr(double r, double g, double b);
/// @brief Deep copy a material (independent storage and texture refs).
/// @param obj Borrowed source Material3D.
/// @return New GC-managed material with retained copies of resource references, or NULL on failure.
void *rt_material3d_clone(void *obj);
/// @brief Create a per-instance variant sharing the same shader but with mutable params.
/// @param obj Borrowed source Material3D.
/// @return New GC-managed material instance, or NULL on invalid input/allocation failure.
void *rt_material3d_make_instance(void *obj);
/// @brief Set the diffuse / base color (legacy or PBR depending on workflow).
/// @param obj Borrowed Material3D handle.
/// @param r Normalized red channel.
/// @param g Normalized green channel.
/// @param b Normalized blue channel.
void rt_material3d_set_color(void *obj, double r, double g, double b);
/// @brief Read the diffuse / base color as a Vec3.
/// @param obj Borrowed Material3D handle.
/// @return New GC-managed Vec3 containing the color, or NULL on invalid input/allocation failure.
void *rt_material3d_get_color(void *obj);
/// @brief Borrow the current base-color/albedo map as decoded Pixels, or NULL.
/// @param obj Borrowed Material3D handle.
/// @return Borrowed decoded Pixels view, or NULL when absent, compressed-only, or invalid.
void *rt_material3d_get_texture_pixels(void *obj);
/// @brief Borrow the current tangent-space normal map as decoded Pixels, or NULL.
/// @param obj Borrowed Material3D handle.
/// @return Borrowed decoded Pixels view, or NULL when absent, compressed-only, or invalid.
void *rt_material3d_get_normal_map_pixels(void *obj);
/// @brief Borrow the current legacy specular map as decoded Pixels, or NULL.
/// @param obj Borrowed Material3D handle.
/// @return Borrowed decoded Pixels view, or NULL when absent, compressed-only, or invalid.
void *rt_material3d_get_specular_map_pixels(void *obj);
/// @brief Borrow the current emissive map as decoded Pixels, or NULL.
/// @param obj Borrowed Material3D handle.
/// @return Borrowed decoded Pixels view, or NULL when absent, compressed-only, or invalid.
void *rt_material3d_get_emissive_map_pixels(void *obj);
/// @brief Borrow the current packed metallic/roughness map as decoded Pixels, or NULL.
/// @param obj Borrowed Material3D handle.
/// @return Borrowed decoded Pixels view, or NULL when absent, compressed-only, or invalid.
void *rt_material3d_get_metallic_roughness_map_pixels(void *obj);
/// @brief Borrow the current ambient-occlusion map as decoded Pixels, or NULL.
/// @param obj Borrowed Material3D handle.
/// @return Borrowed decoded Pixels view, or NULL when absent, compressed-only, or invalid.
void *rt_material3d_get_ao_map_pixels(void *obj);
/// @brief Borrow the current baked lightmap as decoded Pixels, or NULL.
/// @param obj Borrowed Material3D handle.
/// @return Borrowed decoded Pixels view, or NULL when absent or invalid.
void *rt_material3d_get_lightmap_pixels(void *obj);
/// @brief Set the diffuse texture (legacy workflow); aliased to albedo for PBR.
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed supported texture source retained until replaced, or NULL to clear.
void rt_material3d_set_texture(void *obj, void *pixels);
/// @brief Eagerly return an ADOPTED window to its lending 2D canvas (ADR
///        0242 single-window handoff): input detached, presentation handed
///        back, canvas flagged closed. Idempotent; no-op for owned windows.
///        Called by World3D destruction so a shell can re-adopt the window
///        immediately regardless of lingering script references to the old
///        Canvas3D (whose finalizer previously owned the loan return).
/// @param canvas3d Borrowed Canvas3D handle (invalid handles are ignored).
void rt_canvas3d_release_adopted_window(void *canvas3d);
/// @brief Bind a RenderTarget3D's live contents as the albedo texture (auto-refreshing).
/// @param obj Borrowed Material3D handle.
/// @param target Borrowed live RenderTarget3D retained until replaced or detached.
void rt_material3d_set_albedo_render_target(void *obj, void *target);
/// @brief Detach a render-target albedo binding.
/// @param obj Borrowed Material3D handle.
void rt_material3d_clear_albedo_render_target(void *obj);
/// @brief Bind a RenderTarget3D's live contents as the emissive map.
/// @param obj Borrowed Material3D handle.
/// @param target Borrowed live RenderTarget3D retained until replaced or cleared.
void rt_material3d_set_emissive_render_target(void *obj, void *target);
/// @brief Set the PBR albedo (base color) texture.
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed supported texture source retained until replaced, or NULL to clear.
void rt_material3d_set_albedo_map(void *obj, void *pixels);
/// @brief Set the legacy specular shininess exponent (higher = sharper highlights).
/// @param obj Borrowed Material3D handle.
/// @param s Requested finite non-negative exponent, sanitized to the supported range.
void rt_material3d_set_shininess(void *obj, double s);
/// @brief Mark the material as unlit (skip lighting calculations entirely).
/// @param obj Borrowed Material3D handle.
/// @param unlit Non-zero to bypass lighting; zero to use the configured shading workflow.
void rt_material3d_set_unlit(void *obj, int8_t unlit);
/// @brief Enable or disable screen-space reflections for this material.
/// @param obj Borrowed Material3D handle.
/// @param enabled Non-zero to make eligible fragments participate in SSR.
void rt_material3d_set_ssr_enabled(void *obj, int8_t enabled);
/// @brief Report whether screen-space reflections are enabled for this material.
/// @param obj Borrowed Material3D handle.
/// @return 1 when SSR is enabled; otherwise 0.
int8_t rt_material3d_get_ssr_enabled(void *obj);
/// @brief True if unlit mode is enabled.
/// @param obj Borrowed Material3D handle.
/// @return 1 when lighting is bypassed; otherwise 0.
int8_t rt_material3d_get_unlit(void *obj);
/// @brief Switch shading model (0=Phong, 1=Toon, 2=PBR workflow, 3=Unlit, 4=Fresnel, 5=Emissive).
/// @param obj Borrowed Material3D handle.
/// @param model Registered shading-model identifier; invalid values are normalized or rejected.
void rt_material3d_set_shading_model(void *obj, int64_t model);
/// @brief Read the current shading model.
/// @param obj Borrowed Material3D handle.
/// @return Current shading-model identifier, or the default identifier for invalid input.
int64_t rt_material3d_get_shading_model(void *obj);
/// @brief Write a value to a backend-specific custom shader parameter slot.
/// @param obj Borrowed Material3D handle.
/// @param index Zero-based custom-parameter slot.
/// @param value Finite scalar value; unsupported indices or values are rejected.
void rt_material3d_set_custom_param(void *obj, int64_t index, double value);
/// @brief Set the material alpha multiplier (1.0 = opaque, 0.0 = invisible).
/// @param obj Borrowed Material3D handle.
/// @param alpha Opacity clamped to the inclusive normalized range.
void rt_material3d_set_alpha(void *obj, double alpha);
/// @brief Read the material alpha.
/// @param obj Borrowed Material3D handle.
/// @return Stored normalized opacity, or the implementation default for invalid input.
double rt_material3d_get_alpha(void *obj);
/// @brief Set the PBR metallic factor (0.0 = dielectric, 1.0 = pure metal).
/// @param obj Borrowed Material3D handle.
/// @param value Metallic factor clamped to the inclusive normalized range.
void rt_material3d_set_metallic(void *obj, double value);
/// @brief Read the PBR metallic factor.
/// @param obj Borrowed Material3D handle.
/// @return Stored normalized metallic factor, or zero for invalid input.
double rt_material3d_get_metallic(void *obj);
/// @brief Set the PBR roughness factor (0.0 = mirror, 1.0 = fully rough).
/// @param obj Borrowed Material3D handle.
/// @param value Roughness clamped to the inclusive normalized range.
void rt_material3d_set_roughness(void *obj, double value);
/// @brief Read the PBR roughness factor.
/// @param obj Borrowed Material3D handle.
/// @return Stored normalized roughness, or the implementation default for invalid input.
double rt_material3d_get_roughness(void *obj);
/// @brief Set the ambient-occlusion factor (0.0 = full shadow, 1.0 = no occlusion).
/// @param obj Borrowed Material3D handle.
/// @param value Occlusion factor clamped to the inclusive normalized range.
void rt_material3d_set_ao(void *obj, double value);
/// @brief Read the AO factor.
/// @param obj Borrowed Material3D handle.
/// @return Stored normalized occlusion factor, or the implementation default for invalid input.
double rt_material3d_get_ao(void *obj);
/// @brief Set the HDR emissive intensity multiplier (0 disables emission).
/// @param obj Borrowed Material3D handle.
/// @param value Requested finite non-negative emission multiplier.
void rt_material3d_set_emissive_intensity(void *obj, double value);
/// @brief Read the emissive intensity multiplier.
/// @param obj Borrowed Material3D handle.
/// @return Stored non-negative multiplier, or zero for invalid input.
double rt_material3d_get_emissive_intensity(void *obj);
/// @brief Bind a tangent-space normal map texture (requires `_calc_tangents` on the mesh).
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed supported texture source retained until replaced, or NULL to clear.
void rt_material3d_set_normal_map(void *obj, void *pixels);
/// @brief True when a base-color/albedo texture is bound.
/// @param obj Borrowed Material3D handle.
/// @return 1 when the slot contains a valid supported texture source; otherwise 0.
int8_t rt_material3d_get_has_texture(void *obj);
/// @brief True when a normal map texture is bound.
/// @param obj Borrowed Material3D handle.
/// @return 1 when the normal-map slot is populated; otherwise 0.
int8_t rt_material3d_get_has_normal_map(void *obj);
/// @brief Bind a packed metallic-roughness map (R = AO, G = roughness, B = metallic per glTF).
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed supported texture source retained until replaced, or NULL to clear.
void rt_material3d_set_metallic_roughness_map(void *obj, void *pixels);
/// @brief True when a packed metallic-roughness texture is bound.
/// @param obj Borrowed Material3D handle.
/// @return 1 when the packed map slot is populated; otherwise 0.
int8_t rt_material3d_get_has_metallic_roughness_map(void *obj);
/// @brief Bind a separate ambient-occlusion texture.
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed supported texture source retained until replaced, or NULL to clear.
void rt_material3d_set_ao_map(void *obj, void *pixels);
/// @brief Assign a baked lightmap atlas (TEXCOORD_1); replaces flat ambient when set.
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed Pixels lightmap retained until replaced, or NULL to clear.
void rt_material3d_set_lightmap(void *obj, void *pixels);
/// @brief Return whether the baked lightmap slot is populated.
/// @param obj Borrowed Material3D handle.
/// @return 1 when a lightmap is bound; otherwise 0.
int8_t rt_material3d_get_has_lightmap(void *obj);
/// @brief True when a separate ambient-occlusion texture is bound.
/// @param obj Borrowed Material3D handle.
/// @return 1 when the AO map slot is populated; otherwise 0.
int8_t rt_material3d_get_has_ao_map(void *obj);
/// @brief Bind a legacy specular highlight texture.
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed supported texture source retained until replaced, or NULL to clear.
void rt_material3d_set_specular_map(void *obj, void *pixels);
/// @brief True when a specular map texture is bound.
/// @param obj Borrowed Material3D handle.
/// @return 1 when the specular-map slot is populated; otherwise 0.
int8_t rt_material3d_get_has_specular_map(void *obj);
/// @brief Bind an emissive texture (multiplied by emissive_intensity).
/// @param obj Borrowed Material3D handle.
/// @param pixels Borrowed supported texture source retained until replaced, or NULL to clear.
void rt_material3d_set_emissive_map(void *obj, void *pixels);
/// @brief True when an emissive map texture is bound.
/// @param obj Borrowed Material3D handle.
/// @return 1 when the emissive-map slot is populated; otherwise 0.
int8_t rt_material3d_get_has_emissive_map(void *obj);
/// @brief True when an environment cubemap is bound.
/// @param obj Borrowed Material3D handle.
/// @return 1 when a complete environment cubemap is retained; otherwise 0.
int8_t rt_material3d_get_has_env_map(void *obj);
/// @brief Set the base emissive tint color.
/// @param obj Borrowed Material3D handle.
/// @param r Non-negative red emission channel.
/// @param g Non-negative green emission channel.
/// @param b Non-negative blue emission channel.
void rt_material3d_set_emissive_color(void *obj, double r, double g, double b);
/// @brief Scale the normal-map effect (0 = flat, 1 = full strength, >1 = exaggerated).
/// @param obj Borrowed Material3D handle.
/// @param value Requested finite non-negative normal perturbation scale.
void rt_material3d_set_normal_scale(void *obj, double value);
/// @brief Read the normal scale.
/// @param obj Borrowed Material3D handle.
/// @return Stored non-negative normal scale, or the implementation default for invalid input.
double rt_material3d_get_normal_scale(void *obj);
/// @brief Set texture anisotropy; 1 disables anisotropic filtering, values clamp to [1,16].
/// @param obj Borrowed Material3D handle.
/// @param anisotropy Requested sample ratio clamped from 1 through 16.
void rt_material3d_set_anisotropy(void *obj, int64_t anisotropy);
/// @brief Read texture anisotropy in the public [1,16] range.
/// @param obj Borrowed Material3D handle.
/// @return Stored anisotropy ratio, or 1 for invalid input.
int64_t rt_material3d_get_anisotropy(void *obj);
/// @brief Set the sampler filters for every texture slot (Plan 61 / ADR 0272).
/// @param obj Borrowed Material3D handle.
/// @param min_filter Minification: 0=Linear, 1=Nearest (out of range = Linear).
/// @param mag_filter Magnification: 0=Linear, 1=Nearest (out of range = Linear).
/// @param mip_filter Mip selection: 0=None, 1=Nearest, 2=Linear (trilinear);
///        out of range = None. Mip modes engage only when the bound texture
///        carries a mip chain (the backends already generate one).
void rt_material3d_set_texture_filters(void *obj,
                                       int64_t min_filter,
                                       int64_t mag_filter,
                                       int64_t mip_filter);
/// @brief Set alpha mode: 0=Opaque, 1=Mask (alpha test), 2=Blend (transparent).
/// @param obj Borrowed Material3D handle.
/// @param mode One of the `RT_MATERIAL3D_ALPHA_MODE_*` constants.
void rt_material3d_set_alpha_mode(void *obj, int64_t mode);
/// @brief Read the alpha mode.
/// @param obj Borrowed Material3D handle.
/// @return Current `RT_MATERIAL3D_ALPHA_MODE_*` value.
int64_t rt_material3d_get_alpha_mode(void *obj);
/// @brief Set shadow casting mode: 0=Auto, 1=None, 2=Cast even for alpha-blended materials.
/// @param obj Borrowed Material3D handle.
/// @param mode One of the `RT_MATERIAL3D_SHADOW_MODE_*` constants.
void rt_material3d_set_shadow_mode(void *obj, int64_t mode);
/// @brief Read the shadow casting mode.
/// @param obj Borrowed Material3D handle.
/// @return Current `RT_MATERIAL3D_SHADOW_MODE_*` value.
int64_t rt_material3d_get_shadow_mode(void *obj);
/// @brief Toggle two-sided rendering (disables backface culling for this material).
/// @param obj Borrowed Material3D handle.
/// @param enabled Non-zero for two-sided rasterization; zero for normal culling policy.
void rt_material3d_set_double_sided(void *obj, int8_t enabled);
/// @brief True if the material is configured for double-sided rendering.
/// @param obj Borrowed Material3D handle.
/// @return 1 when two-sided rendering is requested; otherwise 0.
int8_t rt_material3d_get_double_sided(void *obj);
/// @brief Set constant and slope-scaled depth bias for coplanar material draws.
/// @details The constant term shifts all fragments by the same depth amount; negative values pull
///   the material forward and positive values push it away. The slope-scaled term adds more offset
///   on steep screen-space triangles and is useful for decals, debug overlays, and other geometry
///   that intentionally sits on top of another surface.
/// @param obj Borrowed Material3D handle.
/// @param constant_bias Signed constant depth offset, sanitized to the supported finite range.
/// @param slope_scaled_bias Signed slope-dependent offset, sanitized to the supported finite range.
void rt_material3d_set_depth_bias(void *obj, double constant_bias, double slope_scaled_bias);

/* Material readback (ADR 0233). Map-slot getters return the borrowed retained
 * source handle or NULL; SetAlbedoMap/SetAlbedoRenderTarget alias the diffuse
 * slot and SetEmissiveRenderTarget aliases the emissive slot. */
/// @brief Borrowed diffuse/albedo source or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_texture(void *obj);
/// @brief Borrowed normal-map source or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_normal_map(void *obj);
/// @brief Borrowed specular-map source or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_specular_map(void *obj);
/// @brief Borrowed emissive-map source or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_emissive_map(void *obj);
/// @brief Borrowed metallic-roughness source or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_metallic_roughness_map(void *obj);
/// @brief Borrowed ambient-occlusion source or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_ao_map(void *obj);
/// @brief Borrowed baked-GI lightmap source or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_lightmap(void *obj);
/// @brief Borrowed environment cubemap or NULL. @param obj Borrowed Material3D handle.
void *rt_material3d_get_env_map(void *obj);
/// @brief Fresh Vec3 of the emissive multiplier. @param obj Borrowed Material3D handle.
void *rt_material3d_get_emissive_color(void *obj);
/// @brief Retained specular exponent. @param obj Borrowed Material3D handle.
double rt_material3d_get_shininess(void *obj);
/// @brief Retained constant depth bias. @param obj Borrowed Material3D handle.
double rt_material3d_get_depth_bias(void *obj);
/// @brief Retained slope-scaled depth bias. @param obj Borrowed Material3D handle.
double rt_material3d_get_depth_slope_bias(void *obj);
/// @brief Retained custom parameter (0 out of range). @param obj Borrowed Material3D handle.
/// @param index Zero-based parameter index.
double rt_material3d_get_custom_param(void *obj, int64_t index);

//=========================================================================
// Light3D — directional, point, or ambient light source
//=========================================================================

/// @brief Create a directional light shining along @p direction with the given color (sun-like).
/// @param direction Borrowed Vec3 direction, normalized by the constructor.
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
/// @return New GC-managed directional Light3D, or NULL on invalid input/allocation failure.
void *rt_light3d_new_directional(void *direction, double r, double g, double b);
/// @brief Create a point light at @p position with linear distance attenuation factor.
/// @param position Borrowed Vec3 world-space position.
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
/// @param attenuation Non-negative distance-falloff factor.
/// @return New GC-managed point Light3D, or NULL on invalid input/allocation failure.
void *rt_light3d_new_point(void *position, double r, double g, double b, double attenuation);
/// @brief Create an ambient light contribution (illuminates all surfaces equally).
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
/// @return New GC-managed ambient Light3D, or NULL on allocation failure.
void *rt_light3d_new_ambient(double r, double g, double b);
/// @brief Create a spot light with inner/outer cone angles in degrees (smooth edge between).
/// @param position Borrowed Vec3 world-space position.
/// @param direction Borrowed Vec3 aim direction, normalized by the constructor.
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
/// @param attenuation Non-negative distance-falloff factor.
/// @param inner_angle Fully illuminated inner half-angle in degrees.
/// @param outer_angle Outer half-angle in degrees; sanitized to remain outside the inner cone.
/// @return New GC-managed spot Light3D, or NULL on invalid input/allocation failure.
void *rt_light3d_new_spot(void *position,
                          void *direction,
                          double r,
                          double g,
                          double b,
                          double attenuation,
                          double inner_angle,
                          double outer_angle);
/// @brief Create a one-sided oriented rectangle area light.
/// @param position Borrowed Vec3 world-space emitter center.
/// @param direction Borrowed Vec3 emission normal, normalized by the constructor.
/// @param width Positive emitter width in world units.
/// @param height Positive emitter height in world units.
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
/// @param attenuation Non-negative distance-falloff factor.
/// @param range Positive finite influence range in world units.
/// @return New GC-managed rectangle Light3D, or NULL on invalid input/allocation failure.
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
/// @param position Borrowed Vec3 world-space emitter center.
/// @param radius Positive emitter radius in world units.
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
/// @param range Positive finite influence range in world units.
/// @return New GC-managed sphere-area Light3D, or NULL on invalid input/allocation failure.
void *rt_light3d_new_area_sphere(
    void *position, double radius, double r, double g, double b, double range);
/// @brief Create an isotropic volume light bounded by @p radius.
/// @param position Borrowed Vec3 world-space volume center.
/// @param radius Positive volume radius in world units.
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
/// @param range Positive finite influence range in world units.
/// @return New GC-managed volume Light3D, or NULL on invalid input/allocation failure.
void *rt_light3d_new_volume(
    void *position, double radius, double r, double g, double b, double range);
/// @brief Multiply the light color by an intensity scalar (HDR-friendly).
/// @param obj Borrowed Light3D handle.
/// @param intensity Requested finite non-negative brightness multiplier.
void rt_light3d_set_intensity(void *obj, double intensity);
/// @brief Set the distance-falloff factor of a point, spot, rectangle, or sphere light.
/// @param obj Borrowed Light3D handle.
/// @param attenuation Requested finite non-negative falloff factor.
void rt_light3d_set_attenuation(void *obj, double attenuation);
/// @brief Get the light's distance-falloff factor (0 for directional/ambient).
/// @param obj Borrowed Light3D handle.
/// @return Stored non-negative factor, or zero when inapplicable or invalid.
double rt_light3d_get_attenuation(void *obj);
/// @brief Replace the light color (without altering intensity).
/// @param obj Borrowed Light3D handle.
/// @param r Non-negative red radiance channel.
/// @param g Non-negative green radiance channel.
/// @param b Non-negative blue radiance channel.
void rt_light3d_set_color(void *obj, double r, double g, double b);
/// @brief Get the light type (0=directional, 1=point, 2=ambient, 3=spot,
///   4=rectangle, 5=sphere, 6=volume).
/// @param obj Borrowed Light3D handle.
/// @return Registered light-type identifier, or the implementation default for invalid input.
int64_t rt_light3d_get_type(void *obj);
/// @brief Get the light color as a Vec3.
/// @param obj Borrowed Light3D handle.
/// @return New GC-managed color Vec3, or NULL on invalid input/allocation failure.
void *rt_light3d_get_color(void *obj);
/// @brief Get the brightness multiplier.
/// @param obj Borrowed Light3D handle.
/// @return Stored non-negative intensity, or zero for invalid input.
double rt_light3d_get_intensity(void *obj);
/// @brief Enable or disable a light without removing it from its slot.
/// @param obj Borrowed Light3D handle.
/// @param enabled Non-zero to contribute to lighting; zero to suppress the light.
void rt_light3d_set_enabled(void *obj, int8_t enabled);
/// @brief True if the light contributes to backend light params.
/// @param obj Borrowed Light3D handle.
/// @return 1 when enabled; otherwise 0.
int8_t rt_light3d_get_enabled(void *obj);
/// @brief Toggle whether this light is eligible for shadow-map selection.
/// @param obj Borrowed Light3D handle.
/// @param enabled Non-zero to request shadow slots when supported.
void rt_light3d_set_casts_shadows(void *obj, int8_t enabled);
/// @brief True if this light may claim shadow-map slots.
/// @param obj Borrowed Light3D handle.
/// @return 1 when shadow casting is requested; otherwise 0.
int8_t rt_light3d_get_casts_shadows(void *obj);
/// @brief Get the normalized light direction as a Vec3.
/// @param obj Borrowed Light3D handle.
/// @return New GC-managed direction Vec3, or NULL on invalid input/allocation failure.
void *rt_light3d_get_direction(void *obj);
/// @brief Get the light position as a Vec3.
/// @param obj Borrowed Light3D handle.
/// @return New GC-managed position Vec3, or NULL on invalid input/allocation failure.
void *rt_light3d_get_position(void *obj);
/// @brief Move the light to a new world position (Vec3).
/// @param obj Borrowed Light3D handle.
/// @param position Borrowed Vec3 world-space position.
void rt_light3d_set_position(void *obj, void *position);
/// @brief Re-aim the light; the direction (Vec3) is normalized.
/// @param obj Borrowed Light3D handle.
/// @param direction Borrowed non-zero Vec3 direction.
void rt_light3d_set_direction(void *obj, void *direction);
/// @brief Get the rectangle emitter width.
/// @param obj Borrowed Light3D handle.
/// @return Positive width in world units, or zero when inapplicable/invalid.
double rt_light3d_get_width(void *obj);
/// @brief Set the rectangle emitter width.
/// @param obj Borrowed rectangle Light3D handle.
/// @param width Requested positive width in world units.
void rt_light3d_set_width(void *obj, double width);
/// @brief Get the rectangle emitter height.
/// @param obj Borrowed Light3D handle.
/// @return Positive height in world units, or zero when inapplicable/invalid.
double rt_light3d_get_height(void *obj);
/// @brief Set the rectangle emitter height.
/// @param obj Borrowed rectangle Light3D handle.
/// @param height Requested positive height in world units.
void rt_light3d_set_height(void *obj, double height);
/// @brief Get the sphere or volume emitter radius.
/// @param obj Borrowed Light3D handle.
/// @return Positive radius in world units, or zero when inapplicable/invalid.
double rt_light3d_get_radius(void *obj);
/// @brief Set the sphere or volume emitter radius.
/// @param obj Borrowed sphere-area or volume Light3D handle.
/// @param radius Requested positive radius in world units.
void rt_light3d_set_radius(void *obj, double radius);
/// @brief Get the FBX-compatible distance-decay mode.
/// @param obj Borrowed Light3D handle.
/// @return 0 for none, 1 for linear, 2 for quadratic, or 3 for cubic decay.
int64_t rt_light3d_get_decay_type(void *obj);
/// @brief Set the FBX-compatible distance-decay mode.
/// @param obj Borrowed local Light3D handle.
/// @param decay_type Decay identifier from zero through three.
void rt_light3d_set_decay_type(void *obj, int64_t decay_type);
/// @brief Get the finite local-light influence range.
/// @param obj Borrowed Light3D handle.
/// @return Non-negative range in world units, or zero when invalid.
double rt_light3d_get_range(void *obj);
/// @brief Set the finite local-light influence range.
/// @param obj Borrowed local Light3D handle.
/// @param range Requested finite non-negative range in world units.
void rt_light3d_set_range(void *obj, double range);
/// @brief Get the sanitized inner spot-cone angle in degrees (zero for non-spot lights).
/// @param obj Borrowed Light3D handle.
/// @return Inner cone half-angle in degrees, or zero when inapplicable/invalid.
double rt_light3d_get_inner_cone_degrees(void *obj);
/// @brief Get the sanitized outer spot-cone angle in degrees (zero for non-spot lights).
/// @param obj Borrowed Light3D handle.
/// @return Outer cone half-angle in degrees, or zero when inapplicable/invalid.
double rt_light3d_get_outer_cone_degrees(void *obj);
/// @brief Atomically set both spot-cone angles in degrees (no-op for non-spot lights).
/// @param obj Borrowed spot Light3D handle.
/// @param inner_angle Requested inner half-angle in degrees.
/// @param outer_angle Requested outer half-angle in degrees; sanitized relative to the inner angle.
void rt_light3d_set_spot_cone(void *obj, double inner_angle, double outer_angle);

/// @brief Register a temporary buffer to be freed at the end of the current frame.
/// @param canvas Borrowed Canvas3D handle that assumes ownership on success.
/// @param buffer Non-NULL malloc-compatible allocation.
/// @return 1 when ownership transfers to the canvas, 0 when the caller still owns `buffer`.
int rt_canvas3d_add_temp_buffer(void *canvas, void *buffer);
/// @brief Remove a previously-registered temporary buffer; caller owns/free()s it again.
/// @param canvas Borrowed Canvas3D handle currently tracking @p buffer.
/// @param buffer Allocation to untrack without freeing.
/// @return 1 when ownership returned to the caller; otherwise 0.
int rt_canvas3d_remove_temp_buffer(void *canvas, void *buffer);

/* Screen-space HUD overlay */
/// @brief Draw a screen-space filled rectangle as a HUD element.
/// @param canvas Borrowed Canvas3D handle with active compatible overlay state.
/// @param x Destination left edge in logical pixels.
/// @param y Destination top edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_rect2d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);
/// @brief Draw a screen-space filled rectangle blended with the given opacity (0..1).
/// @param canvas Borrowed Canvas3D handle.
/// @param x Destination left edge in logical pixels.
/// @param y Destination top edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param color Packed runtime color value.
/// @param alpha Opacity clamped to the inclusive normalized range.
void rt_canvas3d_draw_rect2d_alpha(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, double alpha);
/// @brief Draw a centered crosshair gizmo (4 lines around the canvas center).
/// @param canvas Borrowed Canvas3D handle.
/// @param color Packed runtime color value.
/// @param size Arm length in logical pixels.
void rt_canvas3d_draw_crosshair(void *canvas, int64_t color, int64_t size);
/// @brief Draw screen-space text on top of the rendered scene.
/// @param canvas Borrowed Canvas3D handle.
/// @param x Left origin in logical pixels.
/// @param y Top origin in logical pixels.
/// @param text Borrowed runtime string; at most 512 bytes are rendered.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_text2d(void *canvas, int64_t x, int64_t y, rt_string text, int64_t color);
/// @brief Blit a Pixels image into the 2D overlay at (x,y) scaled to (w,h). NULL-safe.
/// @param canvas Borrowed Canvas3D handle.
/// @param x Destination left edge in logical pixels.
/// @param y Destination top edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param pixels Borrowed source Pixels object.
void rt_canvas3d_draw_image2d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, void *pixels);
/// @brief Draw a screen-space line segment with thickness 1 and opacity (0..1).
/// @param canvas Borrowed Canvas3D handle.
/// @param x0 Start X in logical pixels.
/// @param y0 Start Y in logical pixels.
/// @param x1 End X in logical pixels.
/// @param y1 End Y in logical pixels.
/// @param color Packed runtime color value.
/// @param alpha Opacity clamped to the inclusive normalized range.
void rt_canvas3d_draw_line2d(
    void *canvas, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t color, double alpha);
/// @brief Draw a screen-space 1px rectangle outline with opacity (0..1).
/// @param canvas Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param color Packed runtime color value.
/// @param alpha Opacity clamped to the inclusive normalized range.
void rt_canvas3d_draw_frame2d(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, double alpha);
/// @brief Draw a screen-space filled rounded rectangle with opacity (0..1).
/// @param canvas Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param radius Corner radius in logical pixels.
/// @param color Packed runtime color value.
/// @param alpha Opacity clamped to the inclusive normalized range.
void rt_canvas3d_draw_round_rect2d(void *canvas,
                                   int64_t x,
                                   int64_t y,
                                   int64_t w,
                                   int64_t h,
                                   int64_t radius,
                                   int64_t color,
                                   double alpha);
/// @brief Draw a screen-space rounded rectangle outline with opacity (0..1).
/// @param canvas Borrowed Canvas3D handle.
/// @param x Left edge in logical pixels.
/// @param y Top edge in logical pixels.
/// @param w Width in logical pixels.
/// @param h Height in logical pixels.
/// @param radius Corner radius in logical pixels.
/// @param color Packed runtime color value.
/// @param alpha Opacity clamped to the inclusive normalized range.
void rt_canvas3d_draw_round_frame2d(void *canvas,
                                    int64_t x,
                                    int64_t y,
                                    int64_t w,
                                    int64_t h,
                                    int64_t radius,
                                    int64_t color,
                                    double alpha);
/// @brief Draw screen-space text scaled by a size multiplier (1.0 = DrawText2D size).
/// @param canvas Borrowed Canvas3D handle.
/// @param x Left origin in logical pixels.
/// @param y Top origin in logical pixels.
/// @param text Borrowed runtime string; at most 512 bytes are rendered.
/// @param color Packed runtime color value.
/// @param scale Positive font-size multiplier.
void rt_canvas3d_draw_text2d_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t color, double scale);
/// @brief Blit a sub-region (sx,sy,sw,sh) of a Pixels image into the overlay rect (x,y,w,h).
/// @param canvas Borrowed Canvas3D handle.
/// @param x Destination left edge in logical pixels.
/// @param y Destination top edge in logical pixels.
/// @param w Destination width in logical pixels.
/// @param h Destination height in logical pixels.
/// @param pixels Borrowed source Pixels object.
/// @param sx Source left edge in pixels.
/// @param sy Source top edge in pixels.
/// @param sw Source width in pixels.
/// @param sh Source height in pixels.
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
/// @param canvas Borrowed Canvas3D handle.
/// @param x Clip left edge in logical pixels.
/// @param y Clip top edge in logical pixels.
/// @param w Clip width in logical pixels.
/// @param h Clip height in logical pixels.
void rt_canvas3d_set_clip_rect2d(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h);
/// @brief Remove the overlay 2D clip rect.
/// @param canvas Borrowed Canvas3D handle.
void rt_canvas3d_clear_clip_rect2d(void *canvas);
/// @brief Width in pixels of DrawText2DScaled output for @p text at @p scale.
/// @param canvas Borrowed Canvas3D handle.
/// @param text Borrowed runtime string; at most 512 bytes are measured.
/// @param scale Positive font-size multiplier.
/// @return Rounded rendered width in pixels, or zero for invalid input.
int64_t rt_canvas3d_measure_text2d(void *canvas, rt_string text, double scale);
/// @brief Internal-facing: scaled variant backing DrawText2D/DrawText2DScaled.
/// @param canvas Borrowed Canvas3D handle.
/// @param x Left origin in logical pixels.
/// @param y Top origin in logical pixels.
/// @param text Borrowed runtime string; at most 512 bytes are rendered.
/// @param color Packed runtime color value.
/// @param scale Positive font-size multiplier.
void rt_canvas3d_draw_text_3d_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t color, double scale);

/* Debug gizmos */
/// @brief Draw an axis-aligned bounding box outline between min and max corners.
/// @param canvas Borrowed Canvas3D handle.
/// @param min_v Borrowed Vec3 minimum corner in world space.
/// @param max_v Borrowed Vec3 maximum corner in world space.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_aabb_wire(void *canvas, void *min_v, void *max_v, int64_t color);
/// @brief Raw-array form of rt_canvas3d_draw_aabb_wire: @p min_v and @p max_v are
///   double[3] corner coordinates instead of boxed Vec3 objects.
/// @param canvas Borrowed Canvas3D handle.
/// @param min_v Borrowed three-double minimum corner.
/// @param max_v Borrowed three-double maximum corner.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_aabb_wire_raw(void *canvas,
                                    const double *min_v,
                                    const double *max_v,
                                    int64_t color);
/// @brief Draw a wireframe sphere as great circles (3 orthogonal rings).
/// @param canvas Borrowed Canvas3D handle.
/// @param center Borrowed Vec3 world-space center.
/// @param radius Positive radius in world units.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_sphere_wire(void *canvas, void *center, double radius, int64_t color);
/// @brief Draw a ray with optional arrowhead, capped at @p length world units.
/// @param canvas Borrowed Canvas3D handle.
/// @param origin Borrowed Vec3 world-space ray origin.
/// @param dir Borrowed Vec3 direction.
/// @param length Non-negative displayed length in world units.
/// @param color Packed runtime color value.
void rt_canvas3d_draw_debug_ray(
    void *canvas, void *origin, void *dir, double length, int64_t color);
/// @brief Draw an XYZ axis gizmo at @p origin (X=red, Y=green, Z=blue).
/// @param canvas Borrowed Canvas3D handle.
/// @param origin Borrowed Vec3 world-space origin.
/// @param scale Axis length multiplier in world units.
void rt_canvas3d_draw_axis(void *canvas, void *origin, double scale);

/* Fog */
/// @brief Enable distance fog with linear falloff between @p near_dist and @p far_dist.
/// @param canvas Borrowed Canvas3D handle.
/// @param near_dist Non-negative distance where fog begins.
/// @param far_dist Distance where fog reaches full strength; sanitized above @p near_dist.
/// @param r Normalized fog red channel.
/// @param g Normalized fog green channel.
/// @param b Normalized fog blue channel.
void rt_canvas3d_set_fog(
    void *canvas, double near_dist, double far_dist, double r, double g, double b);
/// @brief Disable fog.
/// @param canvas Borrowed Canvas3D handle; both distance and height fog are disabled.
void rt_canvas3d_clear_fog(void *canvas);

/* Shadows */
/// @brief Enable shadow-map rendering at the given square resolution (typical: 1024 or 2048).
/// @param canvas Borrowed Canvas3D handle.
/// @param resolution Requested map dimension, clamped from 64 through 4096 pixels.
void rt_canvas3d_enable_shadows(void *canvas, int64_t resolution);
/// @brief Disable shadow rendering.
/// @param canvas Borrowed Canvas3D handle; allocated shadow targets are released.
void rt_canvas3d_disable_shadows(void *canvas);
/// @brief Set the shadow depth bias to combat shadow acne (typical: 0.001–0.005).
/// @param canvas Borrowed Canvas3D handle.
/// @param bias Requested finite non-negative comparison bias.
void rt_canvas3d_set_shadow_bias(void *canvas, double bias);
/// @brief Request cascaded shadow maps; counts > 1 require backend support.
/// @param canvas Borrowed Canvas3D handle.
/// @param count Requested cascade count, clamped to the backend slot limit.
void rt_canvas3d_set_shadow_cascades(void *canvas, int64_t count);
/// @brief Cap the camera distance covered by directional shadow maps (<= 0 restores auto).
/// @param canvas Borrowed Canvas3D handle.
/// @param distance Coverage in world units; non-positive/non-finite values select automatic range.
void rt_canvas3d_set_shadow_distance(void *canvas, double distance);
/// @brief Configured directional shadow distance (0 = automatic min(camera far, 300)).
/// @param canvas Borrowed Canvas3D handle.
/// @return Positive configured distance in world units, or zero for automatic/invalid state.
double rt_canvas3d_get_shadow_distance(void *canvas);

/* Coarse CPU visibility: frustum rejection plus optional low-resolution
 * screen-space occlusion. The two toggles are independent; frustum rejection defaults
 * on. Occlusion-enabled frames test opaque draws front-to-back so the coarse depth grid
 * receives near occluders first, then regroup survivors by backend state. */
/// @brief Enable/disable vsync presentation pacing (default on; see "vsync-control" cap).
/// @param canvas Borrowed Canvas3D handle.
/// @param enabled Non-zero to request synchronized presentation; zero for unpaced presentation.
void rt_canvas3d_set_vsync(void *canvas, int8_t enabled);

/// @brief Request pre-present capture so readback after Present() sees the shown frame.
/// @param canvas Canvas3D handle.
/// @param enabled Non-zero to capture every presented frame (one blit/frame on GPU present paths).
void rt_canvas3d_set_capture_after_present(void *canvas, int8_t enabled);

/// @brief Requested capture-after-present state (defaults to off).
/// @param canvas Canvas3D handle.
/// @return Non-zero when pre-present capture is requested.
int8_t rt_canvas3d_get_capture_after_present(void *canvas);
/// @brief Requested vsync state (defaults to on).
/// @param canvas Borrowed Canvas3D handle.
/// @return Requested state, or 1 for invalid input to preserve the default contract.
int8_t rt_canvas3d_get_vsync(void *canvas);
/// @brief Try to render the window-backed 3D scene at a scale in `[0.25, 1]`.
/// @details Reduced scales require the `"render-scale"` backend capability and are upscaled to
/// the logical output dimensions before overlays, readback, and presentation. Values greater
/// than or equal to one request native resolution and work on fixed-scale backends. A capable
/// backend can reject a transition during an active frame or when target allocation fails; on
/// rejection, the previous scale remains active.
/// @param canvas Canvas3D receiver, or `NULL`.
/// @param scale Requested scene scale. Non-finite and values at least one request 1:1; finite
/// values below 0.25 are clamped to 0.25.
/// @return Non-zero if the requested scale is active, otherwise zero.
int8_t rt_canvas3d_try_set_render_scale(void *canvas, double scale);
/**
 * @brief Return the currently active window-backed scene scale.
 * @param canvas Canvas3D receiver, or `NULL`.
 * @return A finite scale in `[0.25, 1]`, or `1` for a null/corrupt receiver.
 */
double rt_canvas3d_get_render_scale(void *canvas);
/// @brief Toggle coarse CPU frustum rejection (default on).
/// @param canvas Borrowed Canvas3D handle.
/// @param enabled Non-zero to reject draws outside the current camera frustum.
void rt_canvas3d_set_frustum_culling(void *canvas, int8_t enabled);
/// @brief Toggle conservative CPU occlusion skips (independent of frustum culling).
/// @param canvas Borrowed Canvas3D handle.
/// @param enabled Non-zero to enable history-backed coarse occlusion testing.
void rt_canvas3d_set_occlusion_culling(void *canvas, int8_t enabled);

/* Render-settings readback (ADR 0233) — each write-only setter's read peer over
 * retained state. Handle-returning getters are borrowed (NULL when unbound);
 * Vec3-returning getters allocate fresh snapshots. */
/// @brief Retained shadow sampling bias. @param canvas Borrowed Canvas3D handle.
double rt_canvas3d_get_shadow_bias(void *canvas);
/// @brief Retained slope-scaled shadow bias. @param canvas Borrowed Canvas3D handle.
double rt_canvas3d_get_shadow_slope_bias(void *canvas);
/// @brief Retained shadow occlusion darkness. @param canvas Borrowed Canvas3D handle.
double rt_canvas3d_get_shadow_strength(void *canvas);
/// @brief Retained PCF filtering tier (0-2). @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_shadow_quality(void *canvas);
/// @brief Retained cascade count (>= 1). @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_shadow_cascades(void *canvas);
/// @brief Retained shadow-light slot budget. @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_shadow_budget(void *canvas);
/// @brief Retained per-cluster light-index capacity. @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_cluster_light_budget(void *canvas);
/// @brief Retained backface-cull flag. @param canvas Borrowed Canvas3D handle.
int8_t rt_canvas3d_get_backface_cull(void *canvas);
/// @brief Whether distance fog is active. @param canvas Borrowed Canvas3D handle.
int8_t rt_canvas3d_get_fog_enabled(void *canvas);
/// @brief Retained fog start distance. @param canvas Borrowed Canvas3D handle.
double rt_canvas3d_get_fog_near(void *canvas);
/// @brief Retained full-strength fog distance. @param canvas Borrowed Canvas3D handle.
double rt_canvas3d_get_fog_far(void *canvas);
/// @brief Fresh Vec3 of the retained fog color. @param canvas Borrowed Canvas3D handle.
void *rt_canvas3d_get_fog_color(void *canvas);
/// @brief Fresh Vec3 of the retained ambient color. @param canvas Borrowed Canvas3D handle.
void *rt_canvas3d_get_ambient_color(void *canvas);
/// @brief Borrowed retained skybox cubemap or NULL. @param canvas Borrowed Canvas3D handle.
void *rt_canvas3d_get_skybox(void *canvas);
/// @brief Borrowed retained render target or NULL. @param canvas Borrowed Canvas3D handle.
void *rt_canvas3d_get_render_target(void *canvas);
/// @brief Borrowed retained post-effect chain or NULL. @param canvas Borrowed Canvas3D handle.
void *rt_canvas3d_get_post_fx(void *canvas);
/// @brief Retained frustum-cull flag. @param canvas Borrowed Canvas3D handle.
int8_t rt_canvas3d_get_frustum_culling(void *canvas);
/// @brief Retained occlusion-cull flag. @param canvas Borrowed Canvas3D handle.
int8_t rt_canvas3d_get_occlusion_culling(void *canvas);
/// @brief Retained texture-streaming flag. @param canvas Borrowed Canvas3D handle.
int8_t rt_canvas3d_get_texture_streaming(void *canvas);
/// @brief Retained streaming mip bias. @param canvas Borrowed Canvas3D handle.
double rt_canvas3d_get_texture_streaming_bias(void *canvas);
/// @brief Retained delta-time cap in ms (0 = uncapped). @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_dt_max(void *canvas);
/// @brief Retained CPU-skinning override flag. @param canvas Borrowed Canvas3D handle.
int8_t rt_canvas3d_get_force_cpu_skinning(void *canvas);
/// @brief Whether an overlay clip rect is active. @param canvas Borrowed Canvas3D handle.
int8_t rt_canvas3d_get_clip_rect_active(void *canvas);
/// @brief Retained clip-rect left edge (0 when inactive). @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_clip_rect_x(void *canvas);
/// @brief Retained clip-rect top edge (0 when inactive). @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_clip_rect_y(void *canvas);
/// @brief Retained clip-rect width (0 when inactive). @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_clip_rect_width(void *canvas);
/// @brief Retained clip-rect height (0 when inactive). @param canvas Borrowed Canvas3D handle.
int64_t rt_canvas3d_get_clip_rect_height(void *canvas);

/* Instanced rendering + Terrain */
/// @brief Submit a Mesh3DBatch for GPU-instanced rendering (one draw call per batch).
/// @param canvas Borrowed Canvas3D handle in an active 3D pass.
/// @param batch Borrowed live Mesh3DBatch; required frame resources are retained or snapshotted.
void rt_canvas3d_draw_instanced(void *canvas, void *batch);
/// @brief Submit a Terrain3D for chunked heightmap rendering.
/// @param canvas Borrowed Canvas3D handle in an active 3D pass.
/// @param terrain Borrowed live Terrain3D.
void rt_canvas3d_draw_terrain(void *canvas, void *terrain);

/* Camera shake + smooth follow */
/// @brief Apply transient camera shake (decays over @p duration seconds with rate @p decay).
/// @param cam Borrowed Camera3D handle.
/// @param intensity Initial shake displacement magnitude.
/// @param duration Non-negative lifetime in seconds.
/// @param decay Non-negative damping rate.
void rt_camera3d_shake(void *cam, double intensity, double duration, double decay);
/// @brief Smoothly orbit the camera at fixed distance/height behind @p target each frame.
/// @param cam Borrowed Camera3D handle.
/// @param target Borrowed Vec3 world-space follow point.
/// @param distance Desired trailing distance in world units.
/// @param height Desired vertical offset in world units.
/// @param speed Non-negative interpolation speed.
/// @param dt Non-negative frame duration in seconds.
void rt_camera3d_smooth_follow(
    void *cam, void *target, double distance, double height, double speed, double dt);
/// @brief Interpolate the camera's look direction toward @p target each frame.
/// @param cam Borrowed Camera3D handle.
/// @param target Borrowed Vec3 world-space aim point.
/// @param speed Non-negative interpolation speed.
/// @param dt Non-negative frame duration in seconds.
void rt_camera3d_smooth_look_at(void *cam, void *target, double speed, double dt);

/* FPS camera */
/// @brief Initialize FPS-camera state (yaw=0, pitch=0) for first-person controls.
/// @param cam Borrowed Camera3D handle.
void rt_camera3d_fps_init(void *cam);
/// @brief Apply FPS controls: mouse delta rotates yaw/pitch, keys translate the camera.
/// @param cam Borrowed Camera3D handle initialized for FPS control.
/// @param yaw_delta Horizontal rotation delta in degrees.
/// @param pitch_delta Vertical rotation delta in degrees.
/// @param move_fwd Signed forward-axis input.
/// @param move_right Signed right-axis input.
/// @param move_up Signed up-axis input.
/// @param speed Translation speed in world units per second.
/// @param dt Non-negative frame duration in seconds.
void rt_camera3d_fps_update(void *cam,
                            double yaw_delta,
                            double pitch_delta,
                            double move_fwd,
                            double move_right,
                            double move_up,
                            double speed,
                            double dt);
/// @brief Read the current FPS-camera yaw in degrees.
/// @param cam Borrowed Camera3D handle.
/// @return Current yaw in degrees, or zero for invalid input.
double rt_camera3d_get_yaw(void *cam);
/// @brief Read the current FPS-camera pitch in degrees.
/// @param cam Borrowed Camera3D handle.
/// @return Current pitch in degrees, or zero for invalid input.
double rt_camera3d_get_pitch(void *cam);
/// @brief Set the FPS-camera yaw in degrees and rebuild the view immediately.
/// @param cam Borrowed Camera3D handle.
/// @param yaw Requested yaw in degrees.
void rt_camera3d_set_yaw(void *cam, double yaw);
/// @brief Set the FPS-camera pitch in degrees (clamped internally to +/-89) and rebuild the view.
/// @param cam Borrowed Camera3D handle.
/// @param pitch Requested pitch in degrees.
void rt_camera3d_set_pitch(void *cam, double pitch);

#ifdef __cplusplus
}
#endif
