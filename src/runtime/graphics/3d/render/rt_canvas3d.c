//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d.c
// Purpose: Zanna.Graphics3D.Canvas3D — 3D rendering surface that dispatches
//   through the vgfx3d_backend_t vtable. Backend selection is automatic
//   and platform-specific, with software fallback always available.
//
// Key invariants:
//   - Begin/End must bracket DrawMesh calls (no nesting)
//   - All rendering dispatches through backend->submit_draw
//   - Canvas3D owns its backend context and either an optional window or a
//     retained explicit RenderTarget3D
//
// Ownership/Lifetime:
//   - Canvas3D is GC-managed; its finalizer destroys the backend context and
//     any present window, then releases retained render resources
//
// Links: vgfx3d_backend.h, rt_canvas3d_internal.h,
//        docs/adr/0168-windowless-canvas3d-rendering.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D construction, frame lifecycle, input, presentation, and render state.
/// @details This translation unit owns the platform-window and backend-selection boundary for
/// `Zanna.Graphics3D.Canvas3D`. It also coordinates deterministic input and timing, frame-owned
/// resource snapshots, output resizing, lighting, fog, shadows, and presentation. Draw, post-FX,
/// shadow, occlusion, and streaming implementation fragments included below share the same
/// `rt_canvas3d` lifetime and deferred-submission invariants.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_action.h"
#include "rt_canvas3d_clusters.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_game3d_diagnostics.h"
#include "rt_graphics_internal.h"
#include "rt_heap.h"
#include "rt_input.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
#include "rt_time.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_utils.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Light flattening lives in rt_canvas3d_lighting.c; its prototype is declared here
// (not in rt_canvas3d_internal.h) because vgfx3d_light_params_t needs vgfx3d_backend.h,
// included above — keeping the type-less TUs that share the internal header clean.
int32_t build_light_params(rt_canvas3d *c, vgfx3d_light_params_t *out, int32_t max);

static int g_canvas3d_backend_fallback_notice_emitted = 0;

static const char CANVAS3D_FALLBACK_REASON_NONE[] = "";
static const char CANVAS3D_FALLBACK_REASON_UNAVAILABLE[] =
    "selected backend unavailable; using software";
static const char CANVAS3D_FALLBACK_REASON_INIT_FAILED[] =
    "selected backend failed to initialize; using software";

#define CANVAS3D_MAX_INSTANCES 1048576
#define CANVAS3D_FALLBACK_CHUNK_INSTANCES 65536
#define CANVAS3D_MAX_DIMENSION 16384
#define CANVAS3D_FPS_SAMPLE_COUNT 32
#define CANVAS3D_FLOAT_ABS_MAX 3.40282346638528859812e38
#define CANVAS3D_SYNTHETIC_KEY_QUEUE_MAX 64
#define CANVAS3D_SYNTHETIC_DT_DEFAULT_US 16667LL
#define CANVAS3D_SYNTHETIC_DT_MAX_US 10000000LL
#define CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX 1000000.0
#define CANVAS3D_FINAL_OVERLAY_RETAIN_CMD_CAP 4096
#define CANVAS3D_FINAL_OVERLAY_RETAIN_TEMP_BUF_CAP 4096
#define CANVAS3D_MATERIAL_UV_ABS_MAX 1000000.0
#define CANVAS3D_MATERIAL_CUSTOM_PARAM_ABS_MAX 1000000.0
#define CANVAS3D_MATERIAL_SHININESS_MAX 8192.0
#define CANVAS3D_MATERIAL_EMISSIVE_COLOR_MAX 1000000.0
#define CANVAS3D_MATERIAL_EMISSIVE_INTENSITY_MAX 1000000.0
#define CANVAS3D_MATERIAL_NORMAL_SCALE_MAX 1000.0
#define CANVAS3D_MATERIAL_DEPTH_BIAS_ABS_MAX 0.05
#define CANVAS3D_MATERIAL_SLOPE_DEPTH_BIAS_ABS_MAX 16.0
#define CANVAS3D_MAX_RAW_MORPH_SHAPES 32
#define CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP 24
#if defined(__clang__) || defined(__GNUC__)
#define CANVAS3D_UNUSED_PRIVATE __attribute__((unused))
#else
#define CANVAS3D_UNUSED_PRIVATE
#endif

static void *g_canvas3d_synthetic_owner = NULL;
static void canvas3d_release_synthetic_input(rt_canvas3d *c);
static void canvas3d_release_synthetic_state(rt_canvas3d *c);
int32_t canvas3d_active_light_limit(rt_canvas3d *c);
int32_t build_light_params(rt_canvas3d *c, vgfx3d_light_params_t *out, int32_t max);
uint32_t canvas3d_stamp_light_snapshot(rt_canvas3d *c,
                                       const vgfx3d_light_params_t *lights,
                                       int32_t light_count);

/// @brief Report whether the graphics-enabled Canvas3D implementation is compiled in.
/// @details This mirrors `Canvas.IsAvailable()` for 2D graphics. It is intentionally cheap and
///          state-free so applications can guard optional 3D paths before constructing a window.
/// @return 1 in this translation unit because it is compiled only when `ZANNA_ENABLE_GRAPHICS`
///         selects the real Graphics3D runtime.
int8_t rt_canvas3d_is_available(void) {
    return 1;
}

/// @brief Record one failed Canvas3D submission without disturbing already queued commands.
/// @details The failure status is sticky across successful draws and the diagnostic counter uses
///   saturating arithmetic. Keeping this helper non-static lets snapshot and transient-resource
///   translation units report failures through the same contract as deferred queue paths.
/// @param c Canvas that observed the failure; NULL is ignored.
/// @param status Non-zero `RT_CANVAS3D_SUBMISSION_*` failure class. Unknown values are normalized
///   to @ref RT_CANVAS3D_SUBMISSION_RESOURCE_FAILURE.
void canvas3d_record_submission_failure(rt_canvas3d *c, int32_t status) {
    if (!c)
        return;
    if (status < RT_CANVAS3D_SUBMISSION_QUEUE_FAILURE ||
        status > RT_CANVAS3D_SUBMISSION_INSTANCE_FAILURE)
        status = (int32_t)RT_CANVAS3D_SUBMISSION_RESOURCE_FAILURE;
    c->last_submission_status = status;
    if (c->submission_failure_count < INT64_MAX)
        c->submission_failure_count++;
}

/// @brief Arm the next main deferred-queue append to fail before command publication.
/// @details This implementation-only one-shot hook accepts the same checked runtime handles and
///   approved stack fixtures as production Canvas3D helpers. It allocates nothing and leaves all
///   commands currently in the queue byte-for-byte unchanged.
/// @param canvas Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_test_fail_next_queue_reserve(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    if (c)
        c->test_fail_next_queue_reserve = 1;
}

/// @brief Arm the next dynamic mesh snapshot to fail before allocating frame storage.
/// @details This implementation-only one-shot hook is consumed by the production snapshot path,
///   which records both legacy snapshot-drop telemetry and the additive submission diagnostic.
/// @param canvas Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_test_fail_next_mesh_snapshot(void *canvas) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    if (c)
        c->test_fail_next_mesh_snapshot = 1;
}

/// @brief Emit the process-wide backend fallback diagnostic at most once.
/// @details The atomic guard prevents duplicate messages when multiple canvases concurrently fail
/// to initialize their preferred backend. Empty or NULL names are replaced with descriptive
/// fallback labels, and the function does not retain either string.
/// @param requested Borrowed requested-backend name, or NULL when no name is available.
/// @param active Borrowed fallback-backend name, or NULL when no name is available.
static void canvas3d_emit_backend_fallback_notice_once(const char *requested, const char *active) {
    int expected = 0;
    if (!rt_atomic_compare_exchange_i32(&g_canvas3d_backend_fallback_notice_emitted,
                                        &expected,
                                        1,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
        return;
    fprintf(stderr,
            "Canvas3D.New: %s backend initialization failed; falling back to %s\n",
            (requested && *requested) ? requested : "selected",
            (active && *active) ? active : "software");
}

/// @brief Atomically swap the global synthetic-input owner to @p c, returning the previous owner.
/// @details Only one canvas may drive synthetic (test/replay) input at a time; this enforces it
/// with
///          an acquire-release exchange across MSVC and GCC/Clang atomics.
/// @param c New synthetic-input owner, or NULL to clear the global owner.
/// @return Previous borrowed owner pointer, which may be NULL. The caller is responsible for
/// releasing any self-reference held by that owner.
static rt_canvas3d *canvas3d_synthetic_owner_exchange(rt_canvas3d *c) {
    return (rt_canvas3d *)rt_atomic_exchange_ptr(&g_canvas3d_synthetic_owner, c, __ATOMIC_ACQ_REL);
}

/// @brief Atomically load the current global synthetic-input owner (acquire ordering).
/// @return Borrowed current owner pointer, or NULL when synthetic input has no owner.
static rt_canvas3d *canvas3d_synthetic_owner_load(void) {
    return (rt_canvas3d *)rt_atomic_load_ptr(&g_canvas3d_synthetic_owner, __ATOMIC_ACQUIRE);
}

/// @brief CAS the synthetic-input owner from @p expected_owner to @p desired_owner.
/// @param expected_owner Owner value that must still be installed for the exchange to succeed.
/// @param desired_owner Replacement owner, or NULL to relinquish ownership.
/// @return Non-zero if this caller won ownership (the owner equalled @p expected_owner).
static int canvas3d_synthetic_owner_compare_exchange(rt_canvas3d *expected_owner,
                                                     rt_canvas3d *desired_owner) {
    void *expected = expected_owner;
    return rt_atomic_compare_exchange_ptr(
        &g_canvas3d_synthetic_owner, &expected, desired_owner, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/// @brief Sanitize an input-source mode (0 live, 1 synthetic, 2 live+synthetic); out of
///   range falls back to 0 (live).
/// @param mode Runtime input-source selector.
/// @return Normalized mode in the inclusive range 0 through 2.
static int32_t canvas3d_input_source_from_mode(int64_t mode) {
    return (mode >= 0 && mode <= 2) ? (int32_t)mode : 0;
}

/// @brief Sanitize a clock-source mode: 1 = fixed synthetic delta, anything else = live.
/// @param mode Runtime clock-source selector.
/// @return 1 for synthetic time when @p mode is exactly 1; otherwise 0 for live time.
static int32_t canvas3d_clock_source_from_mode(int64_t mode) {
    return mode == 1 ? 1 : 0;
}

/// @brief Convert a synthetic frame delta (seconds) to microseconds, clamping negatives
///   to 0 and capping at CANVAS3D_SYNTHETIC_DT_MAX_US.
/// @param dt Requested frame duration in seconds.
/// @return Rounded, non-negative duration in microseconds, capped at the synthetic-time limit;
/// non-finite values return zero.
static int64_t canvas3d_synthetic_seconds_to_us(double dt) {
    if (!isfinite(dt) || dt < 0.0)
        return 0;
    if (dt > (double)CANVAS3D_SYNTHETIC_DT_MAX_US / 1000000.0)
        return CANVAS3D_SYNTHETIC_DT_MAX_US;
    return (int64_t)(dt * 1000000.0 + 0.5);
}

/// @brief Add a positive frame-time sample to the canvas's fixed-size rolling FPS window.
/// @details The sample ring is embedded in `rt_canvas3d` so normal frame timing never allocates.
///          Zero or negative deltas are ignored, preserving the first-frame `Fps == 0` behavior for
///          live windows and avoiding divide-by-zero if a deterministic test intentionally advances
///          with a zero synthetic timestep. The rolling window is maintained as both per-slot
///          values and a running total so `rt_canvas3d_get_fps` stays O(1).
/// @param c Canvas whose embedded FPS sample ring is updated; NULL is ignored.
/// @param delta_us Positive frame duration in microseconds; zero and negative samples are ignored.
static void canvas3d_record_frame_time_sample(rt_canvas3d *c, int64_t delta_us) {
    if (!c || delta_us <= 0)
        return;
    if (c->fps_sample_count < CANVAS3D_FPS_SAMPLE_COUNT) {
        int32_t idx = c->fps_sample_index++;
        c->fps_sample_us[idx] = delta_us;
        c->fps_sample_total_us += delta_us;
        c->fps_sample_count++;
        if (c->fps_sample_index >= CANVAS3D_FPS_SAMPLE_COUNT)
            c->fps_sample_index = 0;
        return;
    }
    if (c->fps_sample_index < 0 || c->fps_sample_index >= CANVAS3D_FPS_SAMPLE_COUNT)
        c->fps_sample_index = 0;
    c->fps_sample_total_us -= c->fps_sample_us[c->fps_sample_index];
    c->fps_sample_us[c->fps_sample_index] = delta_us;
    c->fps_sample_total_us += delta_us;
    c->fps_sample_index++;
    if (c->fps_sample_index >= CANVAS3D_FPS_SAMPLE_COUNT)
        c->fps_sample_index = 0;
    if (c->fps_sample_total_us < 0)
        c->fps_sample_total_us = 0;
}

/// @brief Round a synthetic mouse delta to an integer (half-away-from-zero), clamped to
///   ±CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX; non-finite → 0.
/// @param value Requested synthetic displacement in logical mouse units.
/// @return Rounded and clamped signed displacement, or zero for non-finite input.
static int64_t canvas3d_round_synthetic_mouse_delta(double value) {
    if (!isfinite(value))
        return 0;
    if (value > CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX)
        value = CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX;
    else if (value < -CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX)
        value = -CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX;
    return value >= 0.0 ? (int64_t)(value + 0.5) : (int64_t)(value - 0.5);
}

/// @brief Accumulate a queued synthetic mouse delta onto the running total, saturating
///   at ±CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX and treating non-finite inputs safely.
/// @param current Previously accumulated displacement; non-finite values are treated as zero.
/// @param delta Additional displacement; non-finite values leave @p current unchanged.
/// @return Finite saturated sum in logical mouse units.
static double canvas3d_accumulate_synthetic_mouse_delta(double current, double delta) {
    double next;
    if (!isfinite(delta))
        return current;
    if (!isfinite(current))
        current = 0.0;
    next = current + delta;
    if (!isfinite(next))
        return delta > 0.0 ? CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX : -CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX;
    if (next > CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX)
        return CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX;
    if (next < -CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX)
        return -CANVAS3D_SYNTHETIC_MOUSE_ABS_MAX;
    return next;
}

/// @brief Return the valid synthetic mouse-button bit mask without an undefined-width shift.
/// @details ZANNA_MOUSE_BUTTON_MAX is small today, but this guard keeps the mask calculation
///          defined if the input API grows. Values at or above 63 use every positive int64 bit.
/// @return Bit mask of supported synthetic mouse buttons.
static int64_t canvas3d_synthetic_mouse_button_mask(void) {
#if ZANNA_MOUSE_BUTTON_MAX >= 63
    return INT64_MAX;
#else
    return (int64_t)((UINT64_C(1) << ZANNA_MOUSE_BUTTON_MAX) - UINT64_C(1));
#endif
}

/// @brief Drive the canvas's delta-time fields from the latched synthetic timestep
///   (used when the clock source is synthetic for deterministic frames).
/// @param c Canvas timing state to update; NULL is ignored.
/// @param record_fps_sample Non-zero to append the synthetic delta to the rolling FPS window.
static void canvas3d_apply_synthetic_clock(rt_canvas3d *c, int record_fps_sample) {
    if (!c)
        return;
    if (c->synthetic_dt_us < 0)
        c->synthetic_dt_us = 0;
    c->delta_time_us = c->synthetic_dt_us;
    c->delta_time_ms = c->synthetic_dt_us > 0 ? c->synthetic_dt_us / 1000 : 0;
    if (record_fps_sample)
        canvas3d_record_frame_time_sample(c, c->delta_time_us);
    c->timing_serial++;
}

/// @brief Update the canvas delta-time from the real wall clock at each flip.
/// @details Measures microseconds since the previous flip (clamped non-negative) and records both
///          microsecond and millisecond deltas; the first flip reports zero.
/// @param c Canvas timing state to update; NULL is ignored.
static void canvas3d_update_live_clock(rt_canvas3d *c) {
    if (!c)
        return;
    int64_t now_us = rt_clock_ticks_us();
    if (c->last_flip_us > 0) {
        int64_t delta_us = now_us - c->last_flip_us;
        c->delta_time_us = delta_us > 0 ? delta_us : 0;
        c->delta_time_ms = delta_us > 0 ? delta_us / 1000 : 0;
        canvas3d_record_frame_time_sample(c, c->delta_time_us);
    } else {
        c->delta_time_us = 0;
        c->delta_time_ms = 0;
    }
    c->last_flip_us = now_us;
    c->timing_serial++;
}

/// @brief Release the self-reference a canvas held while it was the synthetic-input owner.
/// @param c Former owner whose retained GC reference is released; NULL and non-heap fixtures are
/// ignored.
static void canvas3d_release_global_owner_ref(rt_canvas3d *c) {
    if (c && rt_heap_is_payload(c) && rt_obj_release_check0(c))
        rt_obj_free(c);
}

/// @brief Make @p c the global synthetic-input owner, releasing any previous owner's reference.
/// @details Retains @p c for as long as it owns synthetic input so the handle can't be freed
///          out from under the input path.
/// @param c New owner, or NULL to clear ownership and release the previous owner's synthetic state.
static void canvas3d_set_synthetic_owner(rt_canvas3d *c) {
    rt_canvas3d *previous;
    if (c && rt_heap_is_payload(c))
        rt_obj_retain_maybe(c);
    previous = canvas3d_synthetic_owner_exchange(c);
    if (previous == c) {
        canvas3d_release_global_owner_ref(c);
        return;
    }
    if (previous) {
        canvas3d_release_synthetic_state(previous);
        canvas3d_release_global_owner_ref(previous);
    }
}

/// @brief Replay queued synthetic input into the live input runtime for one frame:
///   apply pending key transitions, the accumulated mouse delta, button state changes,
///   and wheel scroll, then clear the queues.
/// @param c Canvas containing the queued samples; NULL is ignored. The canvas becomes the global
/// synthetic-input owner before its samples are applied.
static void canvas3d_apply_synthetic_input(rt_canvas3d *c) {
    if (!c)
        return;
    canvas3d_set_synthetic_owner(c);

    for (int32_t i = 0; i < c->synthetic_key_count; i++) {
        int64_t key = c->synthetic_key_keys[i];
        if (key <= 0 || key >= ZANNA_KEY_MAX)
            continue;
        if (c->synthetic_key_downs[i]) {
            rt_keyboard_on_key_down(key);
            c->synthetic_key_state[key] = 1;
        } else {
            rt_keyboard_on_key_up(key);
            c->synthetic_key_state[key] = 0;
        }
    }
    c->synthetic_key_count = 0;

    int64_t dx = canvas3d_round_synthetic_mouse_delta(c->synthetic_mouse_dx);
    int64_t dy = canvas3d_round_synthetic_mouse_delta(c->synthetic_mouse_dy);
    if (dx != 0 || dy != 0)
        rt_mouse_force_delta(dx, dy);
    c->synthetic_mouse_dx = 0.0;
    c->synthetic_mouse_dy = 0.0;

    if (c->synthetic_mouse_has_buttons) {
        for (int i = 0; i < ZANNA_MOUSE_BUTTON_MAX; i++) {
            uint8_t down = (uint8_t)((c->synthetic_mouse_buttons >> i) & 1LL);
            if (down && !c->synthetic_mouse_button_state[i]) {
                rt_mouse_button_down(i);
                c->synthetic_mouse_button_state[i] = 1;
            } else if (!down && c->synthetic_mouse_button_state[i]) {
                rt_mouse_button_up(i);
                c->synthetic_mouse_button_state[i] = 0;
            }
        }
    }
    c->synthetic_mouse_has_buttons = 0;
    c->synthetic_mouse_buttons = 0;

    if (isfinite(c->synthetic_mouse_wheel_y) && c->synthetic_mouse_wheel_y != 0.0)
        rt_mouse_update_wheel(0.0, c->synthetic_mouse_wheel_y);
    c->synthetic_mouse_wheel_y = 0.0;
}

/// @brief Release every key/button the synthetic source is currently holding (called
///   when synthetic input is cleared/disabled) so no key stays stuck down.
/// @param c Canvas whose queued transitions and held-state arrays are cleared; NULL is ignored.
static void canvas3d_release_synthetic_state(rt_canvas3d *c) {
    if (!c)
        return;
    for (int i = 1; i < ZANNA_KEY_MAX; i++) {
        if (c->synthetic_key_state[i]) {
            rt_keyboard_on_key_up(i);
            c->synthetic_key_state[i] = 0;
        }
    }
    for (int i = 0; i < ZANNA_MOUSE_BUTTON_MAX; i++) {
        if (c->synthetic_mouse_button_state[i]) {
            rt_mouse_button_up(i);
            c->synthetic_mouse_button_state[i] = 0;
        }
    }
    c->synthetic_key_count = 0;
    c->synthetic_mouse_dx = 0.0;
    c->synthetic_mouse_dy = 0.0;
    c->synthetic_mouse_wheel_y = 0.0;
    c->synthetic_mouse_buttons = 0;
    c->synthetic_mouse_has_buttons = 0;
}

/// @brief Clear all queued synthetic keyboard/mouse input state on the canvas.
/// @details If @p c owns the process-wide synthetic source, ownership and the corresponding
/// self-reference are also released.
/// @param c Canvas to clear; NULL is ignored.
static void canvas3d_release_synthetic_input(rt_canvas3d *c) {
    if (!c)
        return;
    canvas3d_release_synthetic_state(c);
    if (canvas3d_synthetic_owner_compare_exchange(c, NULL))
        canvas3d_release_global_owner_ref(c);
}

/// @brief True when the active backend can apply GPU post-FX during present.
///
/// Requires the backend to expose `present_postfx` AND the canvas
/// not be in RTT mode (RTT outputs are read back directly without
/// post-processing).
/// @param c Canvas whose backend hooks and target mode are inspected; may be NULL.
/// @return Non-zero when post-FX can be applied by the GPU present hook.
static int canvas3d_backend_uses_gpu_postfx(const rt_canvas3d *c) {
    return c && c->backend && c->backend->present_postfx && c->render_target == NULL;
}

/// @brief True when GPU post-FX can be split from final presentation.
///
/// Split-capable backends composite the post-FX scene first, then Canvas3D
/// replays final-overlay commands over that composited target before calling
/// the normal present hook.
/// @param c Canvas whose backend hooks and target mode are inspected; may be NULL.
/// @return Non-zero when separate apply-post-FX and present hooks are available for a window.
static int canvas3d_backend_splits_gpu_postfx_present(const rt_canvas3d *c) {
    return c && c->backend && c->backend->apply_postfx && c->backend->present &&
           c->render_target == NULL;
}

/// @brief True when the canvas's RTT is owned by a hardware backend.
///
/// Software backends fall back to a CPU-side copy for RTT; hardware
/// backends (D3D11/OpenGL/Metal) keep the texture on the GPU and
/// only read it back when the user requests `Pixels()`.
/// @param c Canvas whose target and backend are inspected; may be NULL.
/// @return Non-zero when a render target is bound to a non-software backend.
static int canvas3d_backend_owns_gpu_rtt(const rt_canvas3d *c) {
    return c && c->render_target && c->backend && c->backend != &vgfx3d_software_backend;
}

/// @brief Clamp a double to `[0, 1]` and narrow to float; non-finite → 0.
/// @details Canvas-facing RGB/alpha inputs come in as `double` from the IL, but the
///   backends consume `float`. Sanitising and narrowing at this boundary keeps NaN out
///   of shader uniforms and caps values at the physical [0, 1] range.
/// @param value Runtime scalar to sanitize.
/// @return Finite single-precision value in the inclusive range 0 through 1.
float canvas3d_clamp01_f64(double value) {
    if (!isfinite(value))
        return 0.0f;
    if (value < 0.0)
        return 0.0f;
    if (value > 1.0)
        return 1.0f;
    return (float)value;
}

/// @brief Clamp a float to [0,1] (NaN/inf → 0) and convert to a 0-255 byte.
/// @param value Normalized channel value to sanitize and quantize.
/// @return Nearest unsigned 8-bit channel value after clamping.
uint8_t canvas3d_clamp01_to_u8(float value) {
    if (!isfinite(value))
        value = 0.0f;
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    return (uint8_t)(value * 255.0f + 0.5f);
}

/// @brief Check whether @p value is finite and within ±FLT_MAX.
/// @details Pre-flight test for safe `double → float` narrowing.
/// @param value Double-precision value to test.
/// @return Non-zero when conversion to float cannot overflow and the value is finite.
int canvas3d_double_fits_float(double value) {
    return isfinite(value) && value >= -CANVAS3D_FLOAT_ABS_MAX && value <= CANVAS3D_FLOAT_ABS_MAX;
}

/// @brief Narrow a 16-entry double Mat4 to float, returning 0 if any entry is non-finite.
/// @details Used by every matrix-keyed Canvas3D draw entry so a malformed Mat4
///          surfaces as a no-op instead of feeding garbage to the GPU backend.
/// @param src Borrowed array of 16 row-major double entries.
/// @param dst Writable array receiving 16 float entries only when validation succeeds.
/// @return Non-zero on complete conversion; zero for NULL arrays or any out-of-range entry.
static int canvas3d_mat4_d2f_checked(const double *src, float *dst) {
    if (!src || !dst)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!canvas3d_double_fits_float(src[i]))
            return 0;
        dst[i] = (float)src[i];
    }
    return 1;
}

/// @brief Whether the current frame uploads geometry relative to a camera origin (floating origin).
/// @details Active only for 3D frames with the mode enabled; 2D and out-of-frame paths upload
/// absolute.
/// @param c Canvas frame state to inspect; may be NULL.
/// @return Non-zero only during an active 3D frame with camera-relative upload enabled.
int canvas3d_uses_camera_relative_upload(const rt_canvas3d *c) {
    return c && c->camera_relative_upload && c->in_frame && !c->frame_is_2d;
}

/// @brief Reset the per-frame camera-relative origin to (0, 0, 0).
/// @param c Canvas whose three origin lanes are reset; NULL is ignored.
static void canvas3d_reset_camera_relative_origin(rt_canvas3d *c) {
    if (!c)
        return;
    c->camera_relative_origin[0] = 0.0;
    c->camera_relative_origin[1] = 0.0;
    c->camera_relative_origin[2] = 0.0;
}

/// @brief Read the active camera-relative frame origin into @p out_origin.
/// @param obj Canvas3D handle or approved stack fixture.
/// @param[out] out_origin Writable three-double array. It is zero-filled before validation.
/// @return 1 if camera-relative upload is active (origin written), 0 otherwise (origin zeroed).
int rt_canvas3d_get_camera_relative_origin(void *obj, double out_origin[3]) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (out_origin) {
        out_origin[0] = 0.0;
        out_origin[1] = 0.0;
        out_origin[2] = 0.0;
    }
    if (!c || !out_origin || !canvas3d_uses_camera_relative_upload(c))
        return 0;
    out_origin[0] = c->camera_relative_origin[0];
    out_origin[1] = c->camera_relative_origin[1];
    out_origin[2] = c->camera_relative_origin[2];
    return 1;
}

/// @brief Whether a row-major 4x4 matrix equals the identity (within 1e-12, all entries finite).
/// @param m Borrowed array of 16 row-major double entries.
/// @return Non-zero when every lane is finite and within the comparison tolerance of identity.
static int canvas3d_mat4_is_identity(const double *m) {
    if (!m)
        return 0;
    for (int i = 0; i < 16; i++) {
        double expected = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0 : 0.0;
        if (!isfinite(m[i]) || fabs(m[i] - expected) > 1e-12)
            return 0;
    }
    return 1;
}

/// @brief Return whether a double-position mesh needs generalized vertex rebasing.
/// @details AddVertex-built and procedural meshes preserve authoring coordinates in `positions64`,
///   but most of them are ordinary local meshes near the origin. Rewriting those vertices for every
///   camera-relative non-identity draw burns snapshot bandwidth without improving precision. This
///   helper opts in only when at least one authoritative local coordinate is outside the
///   float-friendly range or differs materially from the uploaded float copy, where rebasing can
///   prevent visible jitter/flicker after transforms.
/// @param mesh Mesh whose authoritative double positions and uploaded float copy are compared.
/// @return Non-zero when vertex rebasing is expected to preserve meaningful precision. The result
/// is cached against the mesh geometry revision.
static int canvas3d_mesh_positions64_needs_vertex_rebase(rt_mesh3d *mesh) {
    const double precision_risk_threshold = 65536.0;

    if (!mesh || !mesh->positions64 || mesh->vertex_count == 0)
        return 0;
    if (mesh->positions64_rebase_revision == mesh->geometry_revision)
        return mesh->positions64_rebase_needed ? 1 : 0;
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
        for (int axis = 0; axis < 3; axis++) {
            double authored = mesh->positions64[(size_t)i * 3u + (size_t)axis];
            double uploaded = (double)mesh->vertices[i].pos[axis];

            if (!isfinite(authored))
                continue;
            if (!canvas3d_double_fits_float(authored) ||
                fabs(authored) > precision_risk_threshold) {
                mesh->positions64_rebase_revision = mesh->geometry_revision;
                mesh->positions64_rebase_needed = 1;
                return 1;
            }
            if (fabs(authored - uploaded) > 1e-3) {
                mesh->positions64_rebase_revision = mesh->geometry_revision;
                mesh->positions64_rebase_needed = 1;
                return 1;
            }
        }
    }
    mesh->positions64_rebase_revision = mesh->geometry_revision;
    mesh->positions64_rebase_needed = 0;
    return 0;
}

/// @brief Compute the local-space vertex shift that rebases a model under camera-relative upload.
/// @details For world position `A*p + t`, the camera-relative target is `A*p + t - origin`.
///   Rewriting vertices as `p - s` and translation as `t + A*s - origin` preserves that value while
///   keeping both vertex positions and translation near the camera. Callers intentionally restrict
///   this to identity matrices or double-position meshes; ordinary single-precision authored meshes
///   are better served by translation-only rebasing because rewriting their vertices adds snapshot
///   cost without improving precision. Returns 0 for non-invertible or non-finite linear
///   transforms, in which case callers fall back to translation-only rebasing.
/// @param c Canvas providing the active camera-relative origin.
/// @param model_matrix Borrowed row-major 4-by-4 double model matrix.
/// @param[out] out_shift Receives the three-component local-space vertex shift on success.
/// @param[out] out_model_matrix Receives the adjusted row-major float matrix on success.
/// @return Non-zero when both outputs were computed; zero for invalid inputs, inactive rebasing,
/// unsupported projective matrices, singular transforms, or unsafe float narrowing.
static int canvas3d_compute_vertex_rebase_shift(const rt_canvas3d *c,
                                                const double *model_matrix,
                                                double out_shift[3],
                                                float out_model_matrix[16]) {
    double a00, a01, a02, a10, a11, a12, a20, a21, a22;
    double tx, ty, tz;
    double rx, ry, rz;
    double det;
    double inv00, inv01, inv02, inv10, inv11, inv12, inv20, inv21, inv22;
    double sx, sy, sz;
    double adjusted[16];

    if (!c || !model_matrix || !out_shift || !out_model_matrix ||
        !canvas3d_uses_camera_relative_upload(c))
        return 0;
    if (canvas3d_mat4_is_identity(model_matrix)) {
        for (int i = 0; i < 16; i++)
            out_model_matrix[i] = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0f : 0.0f;
        out_shift[0] = c->camera_relative_origin[0];
        out_shift[1] = c->camera_relative_origin[1];
        out_shift[2] = c->camera_relative_origin[2];
        return 1;
    }
    for (int i = 0; i < 16; i++) {
        if (!isfinite(model_matrix[i]))
            return 0;
        adjusted[i] = model_matrix[i];
    }
    if (fabs(model_matrix[12]) > 1e-12 || fabs(model_matrix[13]) > 1e-12 ||
        fabs(model_matrix[14]) > 1e-12 || fabs(model_matrix[15] - 1.0) > 1e-12)
        return 0;
    a00 = model_matrix[0];
    a01 = model_matrix[1];
    a02 = model_matrix[2];
    a10 = model_matrix[4];
    a11 = model_matrix[5];
    a12 = model_matrix[6];
    a20 = model_matrix[8];
    a21 = model_matrix[9];
    a22 = model_matrix[10];
    tx = model_matrix[3];
    ty = model_matrix[7];
    tz = model_matrix[11];
    rx = c->camera_relative_origin[0] - tx;
    ry = c->camera_relative_origin[1] - ty;
    rz = c->camera_relative_origin[2] - tz;
    det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) +
          a02 * (a10 * a21 - a11 * a20);
    if (!isfinite(det) || fabs(det) <= 1e-12)
        return 0;
    inv00 = (a11 * a22 - a12 * a21) / det;
    inv01 = (a02 * a21 - a01 * a22) / det;
    inv02 = (a01 * a12 - a02 * a11) / det;
    inv10 = (a12 * a20 - a10 * a22) / det;
    inv11 = (a00 * a22 - a02 * a20) / det;
    inv12 = (a02 * a10 - a00 * a12) / det;
    inv20 = (a10 * a21 - a11 * a20) / det;
    inv21 = (a01 * a20 - a00 * a21) / det;
    inv22 = (a00 * a11 - a01 * a10) / det;
    sx = inv00 * rx + inv01 * ry + inv02 * rz;
    sy = inv10 * rx + inv11 * ry + inv12 * rz;
    sz = inv20 * rx + inv21 * ry + inv22 * rz;
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(sz))
        return 0;
    adjusted[3] = tx + (a00 * sx + a01 * sy + a02 * sz) - c->camera_relative_origin[0];
    adjusted[7] = ty + (a10 * sx + a11 * sy + a12 * sz) - c->camera_relative_origin[1];
    adjusted[11] = tz + (a20 * sx + a21 * sy + a22 * sz) - c->camera_relative_origin[2];
    for (int i = 0; i < 16; i++) {
        if (!canvas3d_double_fits_float(adjusted[i]))
            return 0;
        out_model_matrix[i] = (float)adjusted[i];
    }
    out_shift[0] = sx;
    out_shift[1] = sy;
    out_shift[2] = sz;
    return 1;
}

/// @brief Narrow a model matrix after subtracting the active frame origin from translation.
/// @param c Canvas providing optional camera-relative state; may be NULL for absolute conversion.
/// @param src Borrowed array of 16 row-major double entries.
/// @param dst Writable array receiving 16 float entries on success.
/// @return Non-zero on complete finite conversion; zero for NULL arrays or unsafe narrowing.
static int canvas3d_model_mat4_d2f_checked(const rt_canvas3d *c, const double *src, float *dst) {
    if (!src || !dst)
        return 0;
    for (int i = 0; i < 16; i++) {
        double value = src[i];
        if (canvas3d_uses_camera_relative_upload(c)) {
            if (i == 3)
                value -= c->camera_relative_origin[0];
            else if (i == 7)
                value -= c->camera_relative_origin[1];
            else if (i == 11)
                value -= c->camera_relative_origin[2];
        }
        if (!canvas3d_double_fits_float(value))
            return 0;
        dst[i] = (float)value;
    }
    return 1;
}

/// @brief Copy a float 4x4 matrix, subtracting the camera-relative origin from its translation.
/// @details Under floating-origin upload the translation is rebased and then validated. Returning
///   failure is deliberately stricter than clamping: a failed instance is skipped/trapped by the
///   caller instead of being moved to the origin for a frame.
/// @param c Canvas providing optional camera-relative state; may be NULL for an absolute copy.
/// @param src Borrowed array of 16 row-major float entries.
/// @param dst Writable array receiving the validated frame-relative matrix.
/// @return Non-zero on success; zero for NULL arrays, non-finite input, or translation overflow.
static int canvas3d_copy_mat4_f32_for_frame(const rt_canvas3d *c, const float *src, float *dst) {
    if (!src || !dst)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(src[i]))
            return 0;
    }
    memcpy(dst, src, sizeof(float) * 16);
    if (canvas3d_uses_camera_relative_upload(c)) {
        double tx = (double)src[3] - c->camera_relative_origin[0];
        double ty = (double)src[7] - c->camera_relative_origin[1];
        double tz = (double)src[11] - c->camera_relative_origin[2];
        if (!canvas3d_double_fits_float(tx) || !canvas3d_double_fits_float(ty) ||
            !canvas3d_double_fits_float(tz))
            return 0;
        dst[3] = (float)tx;
        dst[7] = (float)ty;
        dst[11] = (float)tz;
    }
    return 1;
}

/// @brief Compute camera-relative vertex rebasing for a float instance matrix.
/// @details Instancing APIs pass float matrices, while the regular mesh path works from double
///   Mat4 values. This adapter keeps the same rebase math and validation contract for the
///   per-instance fallback used by large double-position meshes.
/// @param c Canvas providing the active camera-relative origin.
/// @param model_matrix Borrowed row-major 4-by-4 float model matrix.
/// @param[out] out_shift Receives the three-component local-space vertex shift on success.
/// @param[out] out_model_matrix Receives the adjusted row-major float matrix on success.
/// @return Non-zero when the float matrix was finite and generalized rebasing succeeded.
static int canvas3d_compute_vertex_rebase_shift_f32(const rt_canvas3d *c,
                                                    const float *model_matrix,
                                                    double out_shift[3],
                                                    float out_model_matrix[16]) {
    double m[16];
    if (!model_matrix)
        return 0;
    for (int i = 0; i < 16; ++i) {
        if (!isfinite(model_matrix[i]))
            return 0;
        m[i] = (double)model_matrix[i];
    }
    return canvas3d_compute_vertex_rebase_shift(c, m, out_shift, out_model_matrix);
}

/// @brief Verify every entry across an array of @p count Mat4s is finite.
/// @details Used to validate instanced draw matrix arrays before submission.
/// @param matrices Borrowed contiguous array containing @p count row-major 4-by-4 matrices.
/// @param count Number of matrices to inspect; must be positive.
/// @return Non-zero when all lanes are finite; zero for NULL, empty, or invalid input.
static int canvas3d_matrices_f32_are_finite(const float *matrices, int32_t count) {
    if (!matrices || count <= 0)
        return 0;
    for (int32_t i = 0; i < count; i++) {
        const float *m = &matrices[(size_t)i * 16u];
        for (int j = 0; j < 16; j++) {
            if (!isfinite(m[j]))
                return 0;
        }
    }
    return 1;
}

/// @brief Validate @p obj is a live `Zanna.Math.Mat4` heap object, NULL otherwise.
/// @param obj Candidate runtime object.
/// @return Borrowed Mat4 implementation pointer, or NULL for a wrong-class, dead, or NULL object.
static mat4_impl *canvas3d_mat4_checked(void *obj) {
    if (!obj || !rt_heap_is_payload(obj) || rt_obj_class_id(obj) != RT_MAT4_CLASS_ID)
        return NULL;
    return (mat4_impl *)obj;
}

/// @brief Narrow a non-negative double to float; NaN/inf → `fallback`, negatives → 0.
/// @details Used for scalar knobs without an upper bound (exposure, intensities, strengths)
///   where the canvas wants to preserve the author's value faithfully but still refuse
///   non-finite input.
/// @param value Runtime scalar to sanitize.
/// @param fallback Value returned for non-finite input.
/// @return Zero for negative input, `FLT_MAX` for overflow, @p fallback for non-finite input, or
/// the safely narrowed value.
float canvas3d_sanitize_nonnegative_f64(double value, float fallback) {
    if (!isfinite(value))
        return fallback;
    if (value < 0.0)
        return 0.0f;
    if (value > CANVAS3D_FLOAT_ABS_MAX)
        return FLT_MAX;
    return (float)value;
}

/// @brief Narrow a double to float, returning `fallback` when out of float range or non-finite.
/// @param value Runtime scalar to narrow.
/// @param fallback Value returned when @p value cannot be represented safely as float.
/// @return Safely narrowed value or @p fallback.
float canvas3d_sanitize_f64_to_float(double value, float fallback) {
    return canvas3d_double_fits_float(value) ? (float)value : fallback;
}

/// @brief Clamp a finite double to [lo, hi] and narrow to float; non-finite -> fallback.
/// @param value Runtime scalar to sanitize.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @param fallback Value returned when @p value is non-finite or outside float range.
/// @return Safely narrowed value clamped to the supplied bounds, or @p fallback.
float canvas3d_clamp_f64_to_float(double value, double lo, double hi, float fallback) {
    if (!canvas3d_double_fits_float(value))
        return fallback;
    if (value < lo)
        value = lo;
    if (value > hi)
        value = hi;
    return (float)value;
}

/// @brief Floor a float to int32, clamped to [@p lo, @p hi]; non-finite input returns @p lo.
/// @param value Floating-point coordinate to floor.
/// @param lo Inclusive minimum result.
/// @param hi Inclusive maximum result.
/// @return Floored and clamped coordinate, or @p lo for non-finite input.
static int32_t canvas3d_floor_to_i32_clamped(float value, int32_t lo, int32_t hi) {
    if (!isfinite(value))
        return lo;
    if (value <= (float)lo)
        return lo;
    if (value >= (float)hi)
        return hi;
    return (int32_t)floorf(value);
}

/// @brief Ceil a float to int32, clamped to [@p lo, @p hi]; non-finite input returns @p lo.
/// @param value Floating-point coordinate to ceil.
/// @param lo Inclusive minimum result.
/// @param hi Inclusive maximum result.
/// @return Ceiled and clamped coordinate, or @p lo for non-finite input.
static int32_t canvas3d_ceil_to_i32_clamped(float value, int32_t lo, int32_t hi) {
    if (!isfinite(value))
        return lo;
    if (value <= (float)lo)
        return lo;
    if (value >= (float)hi)
        return hi;
    return (int32_t)ceilf(value);
}

/// @brief Clamp an integer cell coordinate into [@p lo, @p hi].
/// @param value Integer coordinate to clamp.
/// @param lo Inclusive minimum result.
/// @param hi Inclusive maximum result.
/// @return @p value constrained to the supplied bounds.
static int32_t canvas3d_clamp_i32(int32_t value, int32_t lo, int32_t hi) {
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/// @brief Validate an RGBA8 readback target: positive dimensions, a non-negative stride
///   at least 4·w bytes, and no 32-bit overflow in the row size.
/// @param w Target width in pixels.
/// @param h Target height in pixels.
/// @param stride Destination row stride in bytes.
/// @return Non-zero when the dimensions and stride describe a safe RGBA8 destination.
int canvas3d_rgba8_stride_valid(int32_t w, int32_t h, int32_t stride) {
    int64_t required;
    if (w <= 0 || h <= 0 || stride < 0)
        return 0;
    required = (int64_t)w * 4;
    if (required > INT32_MAX)
        return 0;
    return (int64_t)stride >= required;
}

/// @brief Clear the per-draw splat-map staging slot on the canvas.
/// @details Called on every early-return path of `rt_canvas3d_draw_mesh_matrix_keyed`
///          so a failed splat-configured draw cannot leak its splat-map and four
///          layer pointers into the next successful draw.
/// @param c Canvas whose pending splat state is zeroed; NULL is ignored.
static void canvas3d_clear_pending_splat(rt_canvas3d *c) {
    if (!c)
        return;
    c->pending_has_splat = 0;
    c->pending_splat_map = NULL;
    for (int i = 0; i < 4; i++) {
        c->pending_splat_layers[i] = NULL;
        c->pending_splat_layer_scales[i] = 0.0f;
    }
}

/// @brief True if every lane of a bone palette (bone_count × 16 floats, capped at
///   VGFX3D_MAX_BONES) is finite; guards skinning uploads against NaN-corrupted matrices.
/// @param palette Borrowed contiguous matrix palette.
/// @param bone_count Requested number of matrices; values above the backend limit are capped.
/// @return Non-zero when at least one matrix is requested and every inspected lane is finite.
static int canvas3d_palette_finite(const float *palette, int32_t bone_count) {
    size_t lane_count;
    if (!palette || bone_count <= 0)
        return 0;
    if (bone_count > VGFX3D_MAX_BONES)
        bone_count = VGFX3D_MAX_BONES;
    if ((size_t)bone_count > SIZE_MAX / (16u * sizeof(float)))
        return 0;
    lane_count = (size_t)bone_count * 16u;
    for (size_t i = 0; i < lane_count; i++) {
        if (!isfinite(palette[i]))
            return 0;
    }
    return 1;
}

static float *canvas3d_snapshot_float_payload(rt_canvas3d *c,
                                              const float *values,
                                              size_t count,
                                              const char *trap_message);

/// @brief Bind a mesh's bone palette (plus the previous-frame palette for motion blur)
///   onto a draw command, dropping all skinning data if any palette lane is non-finite.
/// @details Bone palettes are copied into Canvas3D frame-owned temp buffers so deferred GPU
///          uploads never borrow mutable animation-player memory. This prevents a later
///          animation tick, blend update, or skeleton reuse from changing queued draws and
///          producing one-frame skinning pops or motion-vector flicker. Shallow stack meshes used
///          by the explicit GPU-skinning fast path keep their source pointers: the owning animation
///          object is retained by that path and tests depend on this path remaining zero-copy.
/// @param c Canvas that owns any snapshots created for deferred submission.
/// @param cmd Draw command whose skinning fields are initialized or cleared.
/// @param mesh Borrowed source mesh containing palette and influence data.
/// @param prev_bone_palette Optional borrowed previous-frame palette overriding the mesh fallback.
static void canvas3d_bind_skinning_cmd(rt_canvas3d *c,
                                       vgfx3d_draw_cmd_t *cmd,
                                       const rt_mesh3d *mesh,
                                       const float *prev_bone_palette) {
    int32_t bone_count;
    const float *prev_source;
    size_t lane_count;
    float *palette_snapshot;
    if (!c || !cmd || !mesh)
        return;
    bone_count = mesh->bone_palette && mesh->bone_count > 0 ? mesh->bone_count : 0;
    if (bone_count > VGFX3D_MAX_BONES)
        bone_count = VGFX3D_MAX_BONES;
    if (!canvas3d_palette_finite(mesh->bone_palette, bone_count)) {
        cmd->bone_palette = NULL;
        cmd->prev_bone_palette = NULL;
        cmd->bone_count = 0;
        cmd->extra_influences = NULL;
        return;
    }
    lane_count = (size_t)bone_count * 16u;
    prev_source = prev_bone_palette ? prev_bone_palette : mesh->prev_bone_palette;
    /* Influences 5-8: import-time mesh data (never animation-mutated), so the
     * pointer is stable for the deferred draw — backends with
     * gpu_skinning_extras consume it, others leave it to CPU skinning. */
    cmd->extra_influences = bone_count > 0 ? mesh->extra_influences : NULL;
    if (!rt_heap_is_payload((void *)(uintptr_t)mesh)) {
        cmd->bone_palette = mesh->bone_palette;
        cmd->bone_count = bone_count;
        cmd->prev_bone_palette =
            canvas3d_palette_finite(prev_source, bone_count) ? prev_source : NULL;
        return;
    }
    palette_snapshot = canvas3d_snapshot_float_payload(
        c, mesh->bone_palette, lane_count, "Canvas3D.DrawMesh: bone palette snapshot failed");
    if (!palette_snapshot) {
        cmd->bone_palette = NULL;
        cmd->prev_bone_palette = NULL;
        cmd->bone_count = 0;
        cmd->extra_influences = NULL;
        return;
    }
    cmd->bone_palette = palette_snapshot;
    cmd->bone_count = bone_count;
    cmd->prev_bone_palette = canvas3d_palette_finite(prev_source, bone_count)
                                 ? canvas3d_snapshot_float_payload(
                                       c,
                                       prev_source,
                                       lane_count,
                                       "Canvas3D.DrawMesh: previous bone palette snapshot failed")
                                 : NULL;
}

/// @brief True if all `count` floats are finite (NULL/empty array returns false).
/// @param values Borrowed float array to validate.
/// @param count Number of lanes to inspect.
/// @return Non-zero when @p count is positive and every lane is finite.
static int canvas3d_float_array_finite(const float *values, size_t count) {
    if (!values || count == 0)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(values[i]))
            return 0;
    }
    return 1;
}

/// @brief Hash a finite float payload using its exact IEEE-754 bit pattern.
/// @details The hash is used only as a fast lookup key for same-frame snapshot reuse; callers still
/// verify matching cached entries with `memcmp`, so hash collisions cannot alias different
/// payloads. Non-finite lanes mark the payload invalid and stop hashing.
/// @param values Float payload to validate and hash.
/// @param count Number of float lanes in @p values.
/// @param out_finite Receives non-zero when every lane is finite.
/// @return FNV-1a hash of the finite payload bits, or a partial hash for invalid payloads.
static uint64_t canvas3d_hash_finite_float_payload(const float *values,
                                                   size_t count,
                                                   int *out_finite) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (out_finite)
        *out_finite = 0;
    if (!values || count == 0)
        return hash;
    for (size_t i = 0; i < count; ++i) {
        uint32_t bits = 0u;
        if (!isfinite(values[i]))
            return hash;
        memcpy(&bits, &values[i], sizeof(bits));
        hash ^= (uint64_t)bits;
        hash *= UINT64_C(1099511628211);
    }
    if (out_finite)
        *out_finite = 1;
    return hash;
}

/// @brief Find a same-frame float snapshot matching source pointer, size, hash, and contents.
/// @details Source pointer and content hash narrow the search, while the final byte comparison
/// keeps the cache correct if a mutable source array changes and later reuses the same memory
/// address.
/// @param c Canvas that owns the per-frame snapshot metadata.
/// @param values Current source payload.
/// @param count Float lane count.
/// @param content_hash Exact-bit payload hash computed from @p values.
/// @param byte_count Payload size in bytes.
/// @return Existing frame-owned snapshot, or NULL when no matching cache entry exists.
static float *canvas3d_find_float_snapshot(
    rt_canvas3d *c, const float *values, size_t count, uint64_t content_hash, size_t byte_count) {
    if (!c || !values || byte_count == 0u)
        return NULL;
    for (int32_t i = 0; i < c->float_snapshot_count; ++i) {
        rt_canvas3d_float_snapshot_entry *entry = &c->float_snapshots[i];
        if (entry->source == values && entry->count == count &&
            entry->content_hash == content_hash && entry->snapshot &&
            memcmp(entry->snapshot, values, byte_count) == 0)
            return entry->snapshot;
    }
    return NULL;
}

/// @brief Remember a newly copied float snapshot for reuse later in the same frame.
/// @details Failure to grow the metadata array is non-fatal because the snapshot itself is already
/// tracked by the normal temp-buffer list and will still be released at frame cleanup.
/// @param c Canvas that owns the per-frame snapshot metadata.
/// @param values Source payload pointer used as part of the reuse key.
/// @param count Float lane count.
/// @param content_hash Exact-bit payload hash.
/// @param snapshot Frame-owned copy to reuse.
static void canvas3d_record_float_snapshot(
    rt_canvas3d *c, const float *values, size_t count, uint64_t content_hash, float *snapshot) {
    rt_canvas3d_float_snapshot_entry *grown;
    int32_t new_cap;
    if (!c || !values || !snapshot)
        return;
    if (c->float_snapshot_count >= c->float_snapshot_capacity) {
        if (c->float_snapshot_capacity < 0 || c->float_snapshot_capacity > INT32_MAX / 2)
            return;
        new_cap = c->float_snapshot_capacity == 0 ? 16 : c->float_snapshot_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(*c->float_snapshots))
            return;
        grown = (rt_canvas3d_float_snapshot_entry *)realloc(c->float_snapshots,
                                                            (size_t)new_cap * sizeof(*grown));
        if (!grown)
            return;
        c->float_snapshots = grown;
        c->float_snapshot_capacity = new_cap;
    }
    c->float_snapshots[c->float_snapshot_count].source = values;
    c->float_snapshots[c->float_snapshot_count].count = count;
    c->float_snapshots[c->float_snapshot_count].content_hash = content_hash;
    c->float_snapshots[c->float_snapshot_count].snapshot = snapshot;
    c->float_snapshot_count++;
}

/// @brief Snapshot a finite float payload into a frame-owned temp buffer.
/// @details Deferred draws must not borrow mutable caller arrays for morph weights or raw morph
///   deltas. This helper copies the payload, registers it with the Canvas3D temp-buffer manager,
///   and returns the stable pointer used by the draw command. Repeated identical
///   source/count/content tuples in one frame reuse the first copy to avoid per-draw heap churn.
///   Allocation or tracking failure leaves the caller with NULL so it can disable the optional
///   feature safely.
/// @param c Canvas that owns the per-frame snapshot cache and temp-buffer list.
/// @param values Borrowed source array to validate and copy.
/// @param count Number of float lanes to snapshot.
/// @param trap_message Optional diagnostic used for allocation failures.
/// @return Frame-owned stable copy, a matching same-frame cached copy, or NULL for invalid input,
/// non-finite data, overflow, allocation failure, or tracking failure.
static float *canvas3d_snapshot_float_payload(rt_canvas3d *c,
                                              const float *values,
                                              size_t count,
                                              const char *trap_message) {
    float *snapshot;
    float *cached;
    size_t byte_count;
    uint64_t content_hash;
    int finite = 0;
    if (!c || !values || count == 0)
        return NULL;
    if (count > SIZE_MAX / sizeof(*snapshot)) {
        rt_trap(trap_message ? trap_message : "Canvas3D: float payload allocation overflow");
        return NULL;
    }
    byte_count = count * sizeof(*snapshot);
    content_hash = canvas3d_hash_finite_float_payload(values, count, &finite);
    if (!finite)
        return NULL;
    cached = canvas3d_find_float_snapshot(c, values, count, content_hash, byte_count);
    if (cached)
        return cached;
    snapshot = (float *)malloc(byte_count);
    if (!snapshot) {
        rt_trap(trap_message ? trap_message : "Canvas3D: float payload allocation failed");
        return NULL;
    }
    memcpy(snapshot, values, byte_count);
    if (!canvas3d_track_temp_buffer(c, snapshot)) {
        free(snapshot);
        return NULL;
    }
    canvas3d_record_float_snapshot(c, values, count, content_hash, snapshot);
    return snapshot;
}

/// @brief Bind a mesh's morph-target deltas/weights (plus previous weights) onto a draw
///   command.
/// @details Raw mesh-owned morph arrays are copied into frame-owned memory because callers may
///   mutate those buffers after the draw is queued. Retained `MorphTarget3D` payloads are forwarded
///   directly: the retained object supplies lifetime stability and its generation key guards
///   backend caches, preserving the zero-copy GPU morph path.
/// @param c Canvas that owns snapshots and retained morph-target objects for the frame.
/// @param cmd Draw command whose morph fields are initialized or cleared.
/// @param mesh Borrowed source mesh containing raw or retained morph payloads.
/// @param prev_morph_weights Optional borrowed previous-frame weights overriding the mesh fallback.
static void canvas3d_bind_morph_cmd(rt_canvas3d *c,
                                    vgfx3d_draw_cmd_t *cmd,
                                    const rt_mesh3d *mesh,
                                    const float *prev_morph_weights) {
    int32_t shape_count;
    uint32_t vertex_count;
    size_t delta_triplets;
    size_t delta_float_count;
    float *weights_snapshot = NULL;
    float *prev_weights_snapshot = NULL;
    const float *prev_source;
    if (!c || !cmd || !mesh)
        return;
    cmd->morph_deltas = NULL;
    cmd->morph_normal_deltas = NULL;
    cmd->morph_weights = NULL;
    cmd->prev_morph_weights = NULL;
    cmd->morph_shape_count = 0;
    cmd->morph_key = NULL;
    cmd->morph_revision = 0;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    shape_count = mesh->morph_shape_count;
    if (shape_count <= 0 || !mesh->morph_deltas || !mesh->morph_weights || vertex_count == 0)
        return;
    if (!mesh->morph_targets_ref && shape_count > CANVAS3D_MAX_RAW_MORPH_SHAPES)
        return;
    if ((size_t)shape_count > SIZE_MAX / (size_t)vertex_count)
        return;
    delta_triplets = (size_t)shape_count * (size_t)vertex_count;
    if (delta_triplets > SIZE_MAX / 3u)
        return;
    delta_float_count = delta_triplets * 3u;
    if (!canvas3d_float_array_finite(mesh->morph_weights, shape_count))
        return;
    if (!canvas3d_float_array_finite(mesh->morph_deltas, delta_float_count))
        return;
    prev_source = prev_morph_weights ? prev_morph_weights : mesh->prev_morph_weights;

    if (mesh->morph_targets_ref) {
        if (!canvas3d_track_temp_object(c, mesh->morph_targets_ref))
            return;
        cmd->morph_deltas = mesh->morph_deltas;
        cmd->morph_normal_deltas =
            canvas3d_float_array_finite(mesh->morph_normal_deltas, delta_float_count)
                ? mesh->morph_normal_deltas
                : NULL;
        cmd->morph_weights = mesh->morph_weights;
        cmd->prev_morph_weights =
            (prev_source && canvas3d_float_array_finite(prev_source, (size_t)shape_count))
                ? prev_source
                : NULL;
    } else {
        float *delta_snapshot =
            canvas3d_snapshot_float_payload(c,
                                            mesh->morph_deltas,
                                            delta_float_count,
                                            "Canvas3D.DrawMesh: raw morph delta snapshot failed");
        float *normal_snapshot = NULL;
        weights_snapshot =
            canvas3d_snapshot_float_payload(c,
                                            mesh->morph_weights,
                                            (size_t)shape_count,
                                            "Canvas3D.DrawMesh: morph weight snapshot failed");
        if (prev_source)
            prev_weights_snapshot = canvas3d_snapshot_float_payload(
                c,
                prev_source,
                (size_t)shape_count,
                "Canvas3D.DrawMesh: previous morph weight snapshot failed");
        if (!delta_snapshot) {
            canvas3d_release_tracked_temp_buffer(c, weights_snapshot);
            if (prev_weights_snapshot)
                canvas3d_release_tracked_temp_buffer(c, prev_weights_snapshot);
            return;
        }
        if (!weights_snapshot) {
            canvas3d_release_tracked_temp_buffer(c, delta_snapshot);
            if (prev_weights_snapshot)
                canvas3d_release_tracked_temp_buffer(c, prev_weights_snapshot);
            return;
        }
        if (mesh->morph_normal_deltas)
            normal_snapshot = canvas3d_snapshot_float_payload(
                c,
                mesh->morph_normal_deltas,
                delta_float_count,
                "Canvas3D.DrawMesh: raw morph normal snapshot failed");
        if (mesh->morph_normal_deltas && !normal_snapshot) {
            canvas3d_release_tracked_temp_buffer(c, delta_snapshot);
            canvas3d_release_tracked_temp_buffer(c, weights_snapshot);
            if (prev_weights_snapshot)
                canvas3d_release_tracked_temp_buffer(c, prev_weights_snapshot);
            return;
        }
        cmd->morph_deltas = delta_snapshot;
        cmd->morph_normal_deltas = normal_snapshot;
        cmd->morph_weights = weights_snapshot;
        cmd->prev_morph_weights = prev_weights_snapshot;
    }
    cmd->morph_shape_count = shape_count;
    cmd->morph_key = mesh->morph_targets_ref;
    cmd->morph_revision = mesh->morph_targets_ref
                              ? rt_morphtarget3d_get_payload_generation(mesh->morph_targets_ref)
                              : 0;
}

/// @brief True if both AABB corners are finite and min ≤ max on every axis.
/// @param minv Borrowed three-component minimum corner.
/// @param maxv Borrowed three-component maximum corner.
/// @return Non-zero when both arrays exist and describe an ordered finite box.
static int canvas3d_bounds_are_valid(const float minv[3], const float maxv[3]) {
    if (!minv || !maxv)
        return 0;
    for (int i = 0; i < 3; i++) {
        if (!isfinite(minv[i]) || !isfinite(maxv[i]) || minv[i] > maxv[i])
            return 0;
    }
    return 1;
}

/// @brief Fill out_min/out_max from the mesh's cached local AABB when it is valid,
///   otherwise compute the bounds directly from the vertex array.
/// @param mesh Borrowed mesh providing cached bounds and the safe vertex count.
/// @param vertices Borrowed vertex array used when cached bounds are unavailable.
/// @param[out] out_min Writable three-component minimum corner; NULL suppresses the operation.
/// @param[out] out_max Writable three-component maximum corner; NULL suppresses the operation.
static void canvas3d_copy_or_compute_local_bounds(const rt_mesh3d *mesh,
                                                  const vgfx3d_vertex_t *vertices,
                                                  float out_min[3],
                                                  float out_max[3]) {
    if (!out_min || !out_max)
        return;
    if (mesh && canvas3d_bounds_are_valid(mesh->aabb_min, mesh->aabb_max)) {
        memcpy(out_min, mesh->aabb_min, sizeof(float) * 3u);
        memcpy(out_max, mesh->aabb_max, sizeof(float) * 3u);
        return;
    }
    canvas3d_compute_vertices_aabb(vertices, rt_mesh3d_safe_vertex_count(mesh), out_min, out_max);
}

#include "rt_canvas3d_frame_postfx.inc"

/// @brief Estimate a physical backing size for a requested logical size.
/// @param c Canvas whose window scale is consulted; NULL implies scale 1.
/// @param logical Logical dimension in canvas units.
/// @return Rounded physical dimension. Non-positive or overflow-prone inputs are returned
/// unchanged.
static int32_t canvas3d_scale_logical_size(rt_canvas3d *c, int32_t logical) {
    float scale;

    if (!c || logical <= 0)
        return logical;
    scale = c->gfx_win ? vgfx_window_get_scale(c->gfx_win) : 1.0f;
    if (!isfinite(scale) || scale < 1.0f)
        scale = 1.0f;
    if ((double)logical > (double)INT32_MAX / (double)scale)
        return logical;
    return (int32_t)((double)logical * (double)scale + 0.5);
}

/// @brief Convert a physical framebuffer dimension back to the public logical space.
/// @param c Canvas whose window scale is consulted; NULL implies scale 1.
/// @param physical Physical framebuffer dimension in backing pixels.
/// @return Rounded logical dimension. Non-positive inputs are returned unchanged.
static int32_t canvas3d_unscale_physical_size(rt_canvas3d *c, int32_t physical) {
    float scale;

    if (physical <= 0)
        return physical;
    scale = (c && c->gfx_win) ? vgfx_window_get_scale(c->gfx_win) : 1.0f;
    if (!isfinite(scale) || scale < 1.0f)
        scale = 1.0f;
    return (int32_t)((double)physical / (double)scale + 0.5);
}

/// @brief Apply a window-size change to the canvas + active backend.
///
/// `logical_*` is the public coordinate/capture size, while `physical_*`
/// is the framebuffer/backing-pixel size used by native backends. Keeping
/// both prevents Retina/HiDPI resize events from leaking 2x dimensions into
/// Game3D and Canvas3D public APIs.
/// @param c Canvas and backend state to resize; NULL is ignored.
/// @param logical_w Positive public width in logical units.
/// @param logical_h Positive public height in logical units.
/// @param physical_w Backing width in pixels, or a non-positive value to derive it from scale.
/// @param physical_h Backing height in pixels, or a non-positive value to derive it from scale.
static void rt_canvas3d_apply_resize(
    rt_canvas3d *c, int32_t logical_w, int32_t logical_h, int32_t physical_w, int32_t physical_h) {
    int size_changed;
    int framebuffer_changed;

    if (!c || logical_w <= 0 || logical_h <= 0)
        return;
    if (physical_w <= 0)
        physical_w = canvas3d_scale_logical_size(c, logical_w);
    if (physical_h <= 0)
        physical_h = canvas3d_scale_logical_size(c, logical_h);
    if (physical_w <= 0)
        physical_w = logical_w;
    if (physical_h <= 0)
        physical_h = logical_h;

    size_changed = (c->width != logical_w || c->height != logical_h);
    framebuffer_changed =
        (c->framebuffer_width != physical_w || c->framebuffer_height != physical_h);
    if (!size_changed && !framebuffer_changed)
        return;
    c->width = logical_w;
    c->height = logical_h;
    c->framebuffer_width = physical_w;
    c->framebuffer_height = physical_h;
    if (framebuffer_changed && c->backend && c->backend->resize)
        c->backend->resize(c->backend_ctx, physical_w, physical_h);
}

/// @brief Record an event type code into the canvas's per-frame event tally (for
/// diagnostics/tests).
/// @details The fixed-capacity queue drops its oldest entry on overflow and increments a saturating
/// drop counter before appending the new type.
/// @param c Canvas event queue to update; NULL is ignored.
/// @param type Backend event code; `VGFX_EVENT_NONE` is ignored.
static void canvas3d_record_event_type(rt_canvas3d *c, int64_t type) {
    if (!c || type == VGFX_EVENT_NONE)
        return;
    c->last_event_type = type;
    if (c->event_type_count >= RT_CANVAS3D_EVENT_QUEUE_CAPACITY) {
        c->event_type_head = (c->event_type_head + 1) % RT_CANVAS3D_EVENT_QUEUE_CAPACITY;
        c->event_type_count--;
        if (c->event_type_dropped_count < INT64_MAX)
            c->event_type_dropped_count++;
    }
    int32_t tail = (c->event_type_head + c->event_type_count) % RT_CANVAS3D_EVENT_QUEUE_CAPACITY;
    c->event_type_queue[tail] = type;
    c->event_type_count++;
}

/// @brief Public resize entry point mirroring window resize events.
/// @details Offscreen canvases reject direct resizing because their dimensions belong to the bound
/// render target. Windowed canvases accept dimensions from 1 through
/// `CANVAS3D_MAX_DIMENSION`, resize the platform window, and immediately synchronize the backend.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param w Requested logical width in pixels.
/// @param h Requested logical height in pixels.
void rt_canvas3d_resize(void *obj, int64_t w, int64_t h) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (c->offscreen) {
        rt_trap("Canvas3D.Resize: an offscreen canvas must replace its render target");
        return;
    }
    if (w <= 0 || h <= 0 || w > CANVAS3D_MAX_DIMENSION || h > CANVAS3D_MAX_DIMENSION)
        return;
    if (c->gfx_win)
        vgfx_set_window_size(c->gfx_win, (int32_t)w, (int32_t)h);
    rt_canvas3d_apply_resize(c,
                             (int32_t)w,
                             (int32_t)h,
                             canvas3d_scale_logical_size(c, (int32_t)w),
                             canvas3d_scale_logical_size(c, (int32_t)h));
}

/// @brief Resolve the dimensions of the canvas's active render output.
///
/// When a render target is bound, 2D overlays and coordinate
/// conversions should operate in render-target pixel space rather than
/// the window's framebuffer size.
/// @param c Canvas whose bound target or logical window size is inspected.
/// @param[out] out_w Optional destination for the active width; initialized to zero.
/// @param[out] out_h Optional destination for the active height; initialized to zero.
static void canvas3d_active_output_size(const rt_canvas3d *c, int32_t *out_w, int32_t *out_h) {
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    if (!c)
        return;
    if (c->render_target) {
        if (out_w)
            *out_w = vgfx3d_rendertarget_valid_pixels(c->render_target, NULL)
                         ? c->render_target->width
                         : 0;
        if (out_h)
            *out_h = vgfx3d_rendertarget_valid_pixels(c->render_target, NULL)
                         ? c->render_target->height
                         : 0;
        return;
    }
    if (out_w)
        *out_w = c->width > 0 ? c->width : 0;
    if (out_h)
        *out_h = c->height > 0 ? c->height : 0;
}

/// @brief Window-system resize callback — `userdata` is the canvas pointer.
///
/// Hooked into the underlying `vgfx_window_t`'s resize event so the
/// canvas state stays in sync without requiring per-frame polling.
/// @param userdata Borrowed callback payload expected to point to the owning canvas.
/// @param w Physical framebuffer width reported by the platform.
/// @param h Physical framebuffer height reported by the platform.
static void rt_canvas3d_on_resize(void *userdata, int32_t w, int32_t h) {
    rt_canvas3d *c = (rt_canvas3d *)userdata;
    int32_t logical_w = 0;
    int32_t logical_h = 0;

    if (!c)
        return;
    if (!c->gfx_win || !vgfx_get_size(c->gfx_win, &logical_w, &logical_h))
        logical_w = canvas3d_unscale_physical_size(c, w);
    if (!c->gfx_win || logical_h <= 0)
        logical_h = canvas3d_unscale_physical_size(c, h);
    rt_canvas3d_apply_resize(c, logical_w, logical_h, w, h);
}

/// @brief Drop a GC-managed reference held in a `**slot` and zero the slot.
///
/// Idempotent — safe to call on already-NULL slots. Used in the
/// canvas finalizer to release every owned sub-object cleanly.
/// @param[in,out] slot Address of an owned GC-reference slot; NULL is ignored and a released slot
/// is set to NULL.
static void canvas3d_release_owned_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Replace the GC-managed reference in `*slot` with `value`, retain/release as needed.
/// @details Implements the canonical retain-then-release ownership swap so
///          the new value's refcount goes up *before* the old value's
///          goes down — important when `value == *slot`'s child or the
///          two objects share lifetime, since releasing first could free
///          the new value through a transitive reference. The early
///          return on `*slot == value` skips the round trip when the
///          assignment is a no-op.
/// @param[in,out] slot Address of the owned reference slot; NULL is ignored.
/// @param value New GC-managed value to retain, or NULL to clear the slot.
static void canvas3d_assign_owned_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    canvas3d_release_owned_ref(slot);
    *slot = value;
}

/// @brief Tell keyboard + mouse subsystems to forget this window.
///
/// Called when the canvas is destroyed. Without this, focus
/// queries would still return the dead window pointer until the
/// next focus event arrived.
/// @param gfx_win Window handle to detach from both input subsystems; NULL is ignored.
static void rt_canvas3d_detach_input(vgfx_window_t gfx_win) {
    if (!gfx_win)
        return;
    rt_keyboard_clear_canvas_if_matches(gfx_win);
    rt_mouse_clear_canvas_if_matches(gfx_win);
}

/// @brief Track a transient object only if this call inserted a new frame-retained reference.
/// @details The canvas transient-object list is deduplicated globally for the frame. Failure paths
///          must release only references they actually inserted; otherwise a later allocation
///          failure could untrack a mesh/material/texture already needed by an earlier queued draw.
/// @param c Canvas that owns the frame transient-object table.
/// @param obj GC-managed object to retain until frame cleanup.
/// @param inserted Optional out flag set to 1 only when a new retain was added.
/// @return Non-zero when @p obj is now tracked or was already tracked.
static int canvas3d_track_temp_object_new(rt_canvas3d *c, const void *obj, int *inserted) {
    void *tracked_obj = (void *)(uintptr_t)obj;
    if (inserted)
        *inserted = 0;
    if (!c || !obj)
        return 0;
    if (canvas3d_temp_object_set_contains(c, tracked_obj))
        return 1;
    if (!canvas3d_track_temp_object(c, tracked_obj))
        return 0;
    if (inserted)
        *inserted = 1;
    return 1;
}

/// @brief Track a draw resource and remember newly inserted refs for rollback.
/// @details Stores only resources newly retained by this draw attempt. If the draw later fails to
///          append to the queue, callers release the recorded refs without disturbing resources
///          that were already retained for previous queued draws.
/// @param c Canvas that owns the frame transient-object set.
/// @param obj Borrowed resource to retain, or NULL for an intentional no-op success.
/// @param[out] new_refs Optional caller-owned rollback array receiving newly retained objects.
/// @param[in,out] new_ref_count Optional count of populated rollback entries.
/// @param new_ref_capacity Capacity of @p new_refs; a newly inserted reference is rolled back when
/// the capacity is exhausted.
/// @return Non-zero when the resource is NULL, already tracked, or newly tracked and recorded.
static int canvas3d_track_draw_resource(rt_canvas3d *c,
                                        const void *obj,
                                        void **new_refs,
                                        int32_t *new_ref_count,
                                        int32_t new_ref_capacity) {
    int inserted = 0;
    void *tracked_obj = (void *)(uintptr_t)obj;

    if (!obj)
        return 1;
    if (!canvas3d_track_temp_object_new(c, obj, &inserted))
        return 0;
    if (inserted && new_refs && new_ref_count) {
        if (new_ref_capacity <= 0 || *new_ref_count < 0 || *new_ref_count >= new_ref_capacity) {
            canvas3d_release_tracked_temp_object(c, tracked_obj);
            return 0;
        }
        new_refs[(*new_ref_count)++] = tracked_obj;
    }
    return 1;
}

/// @brief Release every newly inserted transient object recorded for a failed draw append.
/// @param c Canvas that owns the tracked references; NULL is ignored.
/// @param new_refs Borrowed rollback array populated by `canvas3d_track_draw_resource`.
/// @param new_ref_count Number of entries to release in reverse insertion order.
static void canvas3d_release_new_draw_resources(rt_canvas3d *c,
                                                void *const *new_refs,
                                                int32_t new_ref_count) {
    if (!c || !new_refs || new_ref_count <= 0)
        return;
    for (int32_t i = new_ref_count - 1; i >= 0; --i)
        canvas3d_release_tracked_temp_object(c, new_refs[i]);
}

/// @brief Retain every GC-managed material resource whose resolved pointer is stored in @p cmd.
/// @details Draw commands borrow raw Pixels/TextureAsset/CubeMap pointers. Retaining the source
///          objects until frame end makes deferred submission stable even when the material is
///          edited or a texture slot is replaced before Canvas3D.End().
/// @param c Canvas that owns the frame transient-object set.
/// @param mat Borrowed material whose texture and environment slots are tracked.
/// @param[out] new_refs Optional rollback array receiving references inserted by this attempt.
/// @param[in,out] new_ref_count Optional rollback-array population count.
/// @param new_ref_capacity Maximum number of entries available in @p new_refs.
/// @return Non-zero when every applicable resource is retained or already tracked; zero leaves
/// rollback responsibility with the caller.
static int canvas3d_track_material_resources(rt_canvas3d *c,
                                             const rt_material3d *mat,
                                             void **new_refs,
                                             int32_t *new_ref_count,
                                             int32_t new_ref_capacity) {
    if (!c || !mat)
        return 0;
    if (!canvas3d_track_draw_resource(c, mat->texture, new_refs, new_ref_count, new_ref_capacity))
        return 0;
    if (!canvas3d_track_draw_resource(
            c, mat->normal_map, new_refs, new_ref_count, new_ref_capacity))
        return 0;
    if (!canvas3d_track_draw_resource(
            c, mat->specular_map, new_refs, new_ref_count, new_ref_capacity))
        return 0;
    if (!canvas3d_track_draw_resource(
            c, mat->emissive_map, new_refs, new_ref_count, new_ref_capacity))
        return 0;
    if (!canvas3d_track_draw_resource(
            c, mat->metallic_roughness_map, new_refs, new_ref_count, new_ref_capacity))
        return 0;
    if (!canvas3d_track_draw_resource(c, mat->ao_map, new_refs, new_ref_count, new_ref_capacity))
        return 0;
    if (rt_cubemap3d_is_complete(mat->env_map) &&
        !canvas3d_track_draw_resource(c, mat->env_map, new_refs, new_ref_count, new_ref_capacity))
        return 0;
    return 1;
}

#include "rt_canvas3d_deferred.inc"
#include "rt_canvas3d_occlusion.inc"
#include "rt_canvas3d_shadow.inc"
#include "rt_canvas3d_texture_stream.inc"

/// @brief Drop the cached CPU-rasterized skybox so the next frame re-renders it.
/// @details The CPU skybox is an expensive per-pixel raycast through the
///   cubemap; we cache the last result keyed on (resolution, camera VP,
///   cubemap generation). Call this when any of those inputs change in a way
///   `canvas3d_skybox_cache_matches` can't detect (e.g. destroying and
///   recreating the cubemap, or repointing the canvas at a different one).
// CPU skybox fallback (cubemap → pixel buffer, with a per-frame cache) lives in
// rt_canvas3d_skybox.c (shared via rt_canvas3d_internal.h).

/*==========================================================================
 * Canvas3D lifecycle
 *=========================================================================*/

/// @brief GC finalizer — release the backend context and every owned scratch buffer.
///
/// Walks the deferred-command, temp-buffer, temp-object, motion
/// history, and text-vertex arrays, freeing each. Backend contexts
/// (D3D11/OpenGL/Software) destroy themselves through their
/// virtual `destroy_ctx`. Idempotent: nulled pointers prevent
/// double-free if the GC sweeps the canvas twice during shutdown.
/// @param obj Canvas payload supplied by the runtime finalizer. It must point to an initialized
/// `rt_canvas3d`.
static void rt_canvas3d_finalize(void *obj) {
    rt_canvas3d *c = (rt_canvas3d *)obj;
    canvas3d_release_synthetic_input(c);
    if (c->backend && c->backend_ctx && c->render_target && c->backend->set_render_target)
        c->backend->set_render_target(c->backend_ctx, NULL);
    /* Destroy the backend context */
    if (c->backend && c->backend_ctx) {
        c->backend->destroy_ctx(c->backend_ctx);
        c->backend_ctx = NULL;
    }
    /* Free deferred draw command buffer */
    free(c->draw_cmds);
    c->draw_cmds = NULL;
    c->draw_count = c->draw_capacity = 0;
    free(c->trans_cmds);
    c->trans_cmds = NULL;
    c->trans_capacity = 0;
    free(c->auto_instance_matrices);
    c->auto_instance_matrices = NULL;
    c->auto_instance_matrix_capacity = 0;
    free(c->last_light_snapshot);
    c->last_light_snapshot = NULL;
    c->last_light_snapshot_valid = 0;
    free(c->frame_light_snapshots);
    c->frame_light_snapshots = NULL;
    c->frame_light_snapshot_count = 0;
    c->frame_light_snapshot_capacity = 0;
    free(c->cluster_tables);
    c->cluster_tables = NULL;
    c->cluster_table_count = 0;
    free(c->sort_cmds);
    c->sort_cmds = NULL;
    c->sort_capacity = 0;
    free(c->sort_keys);
    c->sort_keys = NULL;
    free(c->sort_keys_scratch);
    c->sort_keys_scratch = NULL;
    c->sort_key_capacity = 0;
    canvas3d_clear_final_overlay(c);
    free(c->final_overlay_cmds);
    c->final_overlay_cmds = NULL;
    c->final_overlay_capacity = 0;
    free(c->final_overlay_temp_buffers);
    c->final_overlay_temp_buffers = NULL;
    c->final_overlay_temp_buf_capacity = 0;
    free(c->final_overlay_arena);
    c->final_overlay_arena = NULL;
    c->final_overlay_arena_capacity = 0u;
    c->final_overlay_arena_used = 0u;
    c->final_overlay_arena_peak = 0u;
    canvas3d_frame_arena_free(c);
    free(c->final_overlay_temp_objects);
    c->final_overlay_temp_objects = NULL;
    c->final_overlay_temp_obj_capacity = 0;
    free(c->motion_history);
    c->motion_history = NULL;
    c->motion_history_count = c->motion_history_capacity = 0;
    free(c->motion_history_hash);
    c->motion_history_hash = NULL;
    c->motion_history_hash_capacity = 0;
    c->motion_history_hash_count = 0;
    free(c->occlusion_history);
    c->occlusion_history = NULL;
    c->occlusion_history_count = c->occlusion_history_capacity = 0;
    free(c->occlusion_history_hash);
    c->occlusion_history_hash = NULL;
    c->occlusion_history_hash_capacity = 0;
    free(c->occlusion_duplicate_counts);
    c->occlusion_duplicate_counts = NULL;
    c->occlusion_duplicate_count_capacity = 0;
    free(c->hiz_depth);
    c->hiz_depth = NULL;
    free(c->hiz_vertex_scratch);
    c->hiz_vertex_scratch = NULL;
    c->hiz_vertex_scratch_capacity = 0;
    free(c->shadow_draw_indices);
    c->shadow_draw_indices = NULL;
    c->shadow_draw_index_capacity = 0;
    free(c->texture_stream_entries);
    c->texture_stream_entries = NULL;
    c->texture_stream_capacity = 0;
    c->texture_stream_live = 0;
    /* Free any leftover temp buffers (e.g., from skinned draws) */
    canvas3d_clear_temp_buffers(c);
    free(c->temp_buffers);
    c->temp_buffers = NULL;
    c->temp_buf_count = c->temp_buf_capacity = 0;
    free(c->temp_buffer_set);
    c->temp_buffer_set = NULL;
    c->temp_buffer_set_capacity = 0;
    c->temp_buffer_set_count = 0;
    free(c->float_snapshots);
    c->float_snapshots = NULL;
    c->float_snapshot_count = c->float_snapshot_capacity = 0;
    free(c->mesh_snapshots);
    c->mesh_snapshots = NULL;
    c->mesh_snapshot_count = c->mesh_snapshot_capacity = 0;
    free(c->mesh_snapshot_hash);
    c->mesh_snapshot_hash = NULL;
    c->mesh_snapshot_hash_capacity = 0;
    vgfx3d_skinning_scratch_free(&c->skinning_scratch);
    canvas3d_clear_temp_objects(c);
    free(c->temp_objects);
    c->temp_objects = NULL;
    c->temp_obj_count = c->temp_obj_capacity = 0;
    free(c->temp_object_set);
    c->temp_object_set = NULL;
    c->temp_object_set_capacity = 0;
    c->temp_object_set_count = 0;
    free(c->text_vertices);
    c->text_vertices = NULL;
    c->text_vertex_capacity = 0;
    free(c->text_indices);
    c->text_indices = NULL;
    c->text_index_capacity = 0;
    canvas3d_clear_aa_text_cache(c);
    free(c->readback_rgba_scratch);
    c->readback_rgba_scratch = NULL;
    c->readback_rgba_scratch_capacity = 0u;

    /* Free shadow render targets if allocated */
    canvas3d_release_shadow_targets(c);

    if (c->skybox && rt_g3d_has_class(c->skybox, RT_G3D_CUBEMAP3D_CLASS_ID)) {
        if (rt_obj_release_check0(c->skybox))
            rt_obj_free(c->skybox);
    }
    c->skybox = NULL;
    rt_canvas3d_invalidate_skybox_cache(c);

    vgfx3d_postfx_chain_free(&c->frame_postfx_chain);
    canvas3d_release_owned_ref(&c->postfx);
    canvas3d_release_owned_ref((void **)&c->render_target_owner);
    c->render_target = NULL;
    for (int32_t i = 0; i < VGFX3D_MAX_LIGHTS; i++)
        canvas3d_release_owned_ref((void **)&c->lights[i]);

    if (c->gfx_win) {
        rt_canvas3d_detach_input(c->gfx_win);
        vgfx_destroy_window(c->gfx_win);
        c->gfx_win = NULL;
    }
}

/// @brief Tear down the canvas's platform window and flag it closed.
/// @details Detaches input, destroys the backing window, and sets `should_close`
///          so the next poll reports the canvas as closed. Safe on a NULL canvas.
/// @param c Canvas whose optional window is destroyed.
static void canvas3d_close_window(rt_canvas3d *c) {
    if (!c)
        return;
    if (c->gfx_win) {
        /* Restore normal cursor behavior before the window goes away —
         * relative mouse mode is a per-process setting on some platforms
         * (macOS cursor dissociation) and must not outlive the window. */
        if (c->relative_mouse_applied) {
            (void)vgfx_set_relative_mouse(c->gfx_win, 0);
            rt_mouse_set_relative_native(0);
            c->relative_mouse_applied = 0;
        }
        rt_canvas3d_detach_input(c->gfx_win);
        vgfx_destroy_window(c->gfx_win);
        c->gfx_win = NULL;
    }
    c->should_close = 1;
}

/// @brief Apply exactly one presentation clock to the Canvas3D window.
/// @details GPU backends use native display-sync/swap-interval control, so vgfx must not add its
///          independent CPU sleep. Software presentation has no native swap clock and keeps the
///          frame limit captured when the window was created while requested vsync is enabled.
/// @param c Canvas whose platform-window frame limiter is synchronized; NULL and offscreen
/// canvases are ignored.
static void canvas3d_apply_window_pacing(rt_canvas3d *c) {
    int software_backend;

    if (!c || !c->gfx_win)
        return;
    software_backend = c->backend == &vgfx3d_software_backend;
    vgfx_set_fps(
        c->gfx_win,
        canvas3d_window_pacing_fps(software_backend, c->vsync_enabled, c->software_frame_limit));
}

/// @brief Create a new 3D rendering canvas (window + backend context).
/// @details Opens a platform window, selects the platform-default rendering backend
///          with software fallback if initialization fails, and initializes the framebuffer,
///          depth buffer, deferred draw queue, and motion blur history. The canvas
///          is the main entry point for 3D rendering — call Begin/DrawMesh/End/Flip
///          each frame. GC finalizer destroys the backend context and window.
/// @param title Window title (runtime string).
/// @param w     Window width in pixels (1–CANVAS3D_MAX_DIMENSION).
/// @param h     Window height in pixels (1–CANVAS3D_MAX_DIMENSION).
/// @param fullscreen Non-zero to create a desktop-sized fullscreen window; ignored for offscreen
/// construction.
/// @param offscreen_target Optional validated RenderTarget3D whose dimensions and lifetime become
/// the canvas output contract. A non-NULL target suppresses window and live-input setup.
/// @param offscreen_prefer_gpu Non-zero to request a headless platform backend before software
/// fallback; meaningful only with @p offscreen_target.
/// @return Opaque canvas handle, or NULL on failure.
static void *canvas3d_new_impl(rt_string title,
                               int64_t w,
                               int64_t h,
                               int32_t fullscreen,
                               rt_rendertarget3d *offscreen_target,
                               int32_t offscreen_prefer_gpu) {
    vgfx_framebuffer_t fb;
    const int32_t offscreen = offscreen_target != NULL;

    if (fullscreen && !offscreen) {
        /* Fullscreen creation sizes the window to the desktop; the requested
         * dimensions are ignored (vgfx resolves the display size). */
        int32_t disp_w = 0;
        int32_t disp_h = 0;
        vgfx_get_display_size(&disp_w, &disp_h);
        w = disp_w > 0 ? disp_w : 1280;
        h = disp_h > 0 ? disp_h : 720;
    }
    if (w <= 0 || h <= 0 || w > CANVAS3D_MAX_DIMENSION || h > CANVAS3D_MAX_DIMENSION) {
        rt_trap(offscreen ? "Canvas3D.NewOffscreen: target dimensions must be 1-16384"
                          : "Canvas3D.New: dimensions must be 1-16384");
        return NULL;
    }
    /* ADR 0065: make the Game.UI widgets canvas-polymorphic by registering the
     * Canvas3D draw-ops binding (idempotent). */
    canvas3d_register_gameui_ops();
    int32_t initial_width = (int32_t)w;
    int32_t initial_height = (int32_t)h;
    int32_t initial_framebuffer_width = (int32_t)w;
    int32_t initial_framebuffer_height = (int32_t)h;

    rt_canvas3d *c =
        (rt_canvas3d *)rt_obj_new_i64(RT_G3D_CANVAS3D_CLASS_ID, (int64_t)sizeof(rt_canvas3d));
    if (!c) {
        rt_trap("Canvas3D.New: memory allocation failed");
        return NULL;
    }
    memset(c, 0, sizeof(rt_canvas3d));
    c->offscreen = offscreen ? 1 : 0;
    c->vsync_enabled = offscreen ? 0 : 1;
    rt_obj_set_finalizer(c, rt_canvas3d_finalize);

    if (!offscreen) {
        /* Create the platform window only for the established windowed/fullscreen constructors. */
        vgfx_window_params_t params = vgfx_window_params_default();
        params.width = (int32_t)w;
        params.height = (int32_t)h;
        params.fullscreen = fullscreen;
        if (title)
            params.title = rt_string_cstr(title);

        c->gfx_win = vgfx_create_window(&params);
        if (!c->gfx_win) {
            if (rt_obj_release_check0(c))
                rt_obj_free(c);
            rt_trap("Canvas3D.New: failed to create window (display server unavailable?)");
            return NULL;
        }
        c->software_frame_limit = vgfx_get_fps(c->gfx_win);

        vgfx_set_coord_scale(c->gfx_win, vgfx_window_get_scale(c->gfx_win));
        if (vgfx_get_framebuffer(c->gfx_win, &fb) && fb.width > 0 && fb.height > 0) {
            initial_framebuffer_width = fb.width;
            initial_framebuffer_height = fb.height;
        }
        if (fullscreen) {
            /* The window may have settled at a different logical size than the
             * display query suggested (menu bars, WM policy) — trust the window. */
            int32_t actual_w = 0;
            int32_t actual_h = 0;
            if (vgfx_get_size(c->gfx_win, &actual_w, &actual_h) && actual_w > 0 && actual_h > 0) {
                initial_width = actual_w;
                initial_height = actual_h;
            }
        }
    }

    c->width = offscreen ? 0 : initial_width;
    c->height = offscreen ? 0 : initial_height;
    c->framebuffer_width = offscreen ? 0 : initial_framebuffer_width;
    c->framebuffer_height = offscreen ? 0 : initial_framebuffer_height;

    /* Offscreen canvases default to the deterministic software backend so
     * probes, bakes, and goldens stay byte-reproducible. The accelerated
     * offscreen constructor (ADR 0191) opts into the platform backend with
     * the same software fallback windowed canvases use; backends that cannot
     * create a headless context fail create_ctx and fall back truthfully. */
    c->backend =
        (offscreen && !offscreen_prefer_gpu) ? &vgfx3d_software_backend : vgfx3d_select_backend();
    c->backend_requested_name = (c->backend && c->backend->name) ? c->backend->name : "unknown";
    c->backend_fallback = 0;
    c->backend_fallback_reason = CANVAS3D_FALLBACK_REASON_NONE;
    if (!c->backend || !c->backend->create_ctx) {
        const char *failed_backend_name = c->backend_requested_name;
        c->backend = &vgfx3d_software_backend;
        if (strcmp(failed_backend_name, "software") != 0) {
            c->backend_fallback = 1;
            c->backend_fallback_reason = CANVAS3D_FALLBACK_REASON_UNAVAILABLE;
            canvas3d_emit_backend_fallback_notice_once(failed_backend_name, c->backend->name);
        }
    }
    if (!c->backend || !c->backend->create_ctx) {
        if (rt_obj_release_check0(c))
            rt_obj_free(c);
        rt_trap(offscreen ? "Canvas3D.NewOffscreen: software backend is unavailable"
                          : "Canvas3D.New: no 3D backend is available");
        return NULL;
    }
    c->backend_ctx =
        c->backend->create_ctx(c->gfx_win, initial_framebuffer_width, initial_framebuffer_height);
    if (!c->backend_ctx) {
        const vgfx3d_backend_t *failed_backend = c->backend;
        const char *failed_backend_name =
            (failed_backend && failed_backend->name) ? failed_backend->name : "unknown";
        /* Selected backend failed — fall back to software. */
        c->backend = &vgfx3d_software_backend;
        c->backend_ctx = c->backend->create_ctx(
            c->gfx_win, initial_framebuffer_width, initial_framebuffer_height);
        if (!c->backend_ctx) {
            if (rt_obj_release_check0(c))
                rt_obj_free(c);
            rt_trap(offscreen ? "Canvas3D.NewOffscreen: software backend initialization failed"
                              : "Canvas3D.New: backend initialization failed");
            return NULL;
        }
        if (failed_backend != &vgfx3d_software_backend &&
            strcmp(failed_backend_name, "software") != 0) {
            c->backend_requested_name = failed_backend_name;
            c->backend_fallback = 1;
            c->backend_fallback_reason = CANVAS3D_FALLBACK_REASON_INIT_FAILED;
            canvas3d_emit_backend_fallback_notice_once(failed_backend_name, c->backend->name);
        }
    }
    if (c->gfx_win)
        vgfx_set_gpu_present(c->gfx_win, c->backend != &vgfx3d_software_backend);
    if (c->backend && c->backend->set_vsync && c->backend_ctx)
        c->backend->set_vsync(c->backend_ctx, c->vsync_enabled);
    if (c->backend && c->backend->set_capture_after_present && c->backend_ctx)
        c->backend->set_capture_after_present(c->backend_ctx, c->capture_after_present);
    canvas3d_apply_window_pacing(c);

    /* Plan 07: clustered forward+ defaults on for GPU backends (identified by the
     * present_postfx hook; software keeps the 16-light flat path and its perf
     * profile). ZANNA_3D_CLUSTERS=0 is the bisection escape hatch. */
    {
        const char *clusters_env = getenv("ZANNA_3D_CLUSTERS");
        if (!(clusters_env && clusters_env[0] == '0' && clusters_env[1] == '\0') &&
            c->backend->present_postfx)
            c->clustered_lighting = 1;
    }

    if (c->gfx_win)
        vgfx_set_resize_callback(c->gfx_win, rt_canvas3d_on_resize, c);

    c->ambient[0] = 0.1f;
    c->ambient[1] = 0.1f;
    c->ambient[2] = 0.1f;
    c->ibl_enabled = 0;
    c->ibl_intensity = 1.0f;
    c->backface_cull = 1;
    c->render_target = NULL;
    c->render_target_owner = NULL;
    c->postfx = NULL;
    c->temp_buffers = NULL;
    c->temp_buf_count = c->temp_buf_capacity = 0;
    c->temp_buffer_set = NULL;
    c->temp_buffer_set_capacity = 0;
    c->temp_buffer_set_count = 0;
    c->float_snapshots = NULL;
    c->float_snapshot_count = c->float_snapshot_capacity = 0;
    c->final_overlay_arena = NULL;
    c->final_overlay_arena_capacity = 0u;
    c->final_overlay_arena_used = 0u;
    c->final_overlay_arena_peak = 0u;
    c->mesh_snapshots = NULL;
    c->mesh_snapshot_count = c->mesh_snapshot_capacity = 0;
    c->mesh_snapshot_hash = NULL;
    c->mesh_snapshot_hash_capacity = 0;
    c->temp_objects = NULL;
    c->temp_obj_count = c->temp_obj_capacity = 0;
    c->temp_object_set = NULL;
    c->temp_object_set_capacity = 0;
    c->temp_object_set_count = 0;
    c->aa_text_cache = NULL;
    c->aa_text_cache_count = 0;
    c->aa_text_cache_bytes = 0;
    c->fog_enabled = 0;
    c->fog_near = 10.0f;
    c->fog_far = 50.0f;
    c->fog_color[0] = c->fog_color[1] = c->fog_color[2] = 0.5f;
    c->height_fog_enabled = 0;
    c->height_fog_base = 0.0f;
    c->height_fog_falloff = 0.1f;
    c->height_fog_density = 0.02f;
    c->height_fog_blend = 1.0f;
    c->height_fog_sun_color[0] = 1.0f;
    c->height_fog_sun_color[1] = 1.0f;
    c->height_fog_sun_color[2] = 1.0f;
    c->height_fog_sun_power = 8.0f;
    c->height_fog_sun_amount = 0.0f;
    c->cached_cam_near = 0.1f;
    c->cached_cam_far = 1000.0f;
    c->shadows_enabled = 0;
    c->shadow_resolution = 1024;
    c->shadow_bias = 0.005f;
    c->shadow_slope_bias = 1.0f;
    c->shadow_strength = 0.85f; /* reproduces the legacy mix(0.15, 1.0, vis) look */
    c->shadow_quality = 1;      /* 8-tap rotated-Poisson PCF */
    c->shadow_count = 0;
    c->shadow_distance = 0.0f; /* auto: min(camera far, 300) */
    /* Two cascades by default on CSM-capable backends: one map spanning the whole
     * shadow range wastes most of its texels far from the camera. */
    {
        rt_string capability = rt_const_cstr("shadow-csm");
        c->shadow_cascade_count = rt_canvas3d_backend_supports(c, capability) ? 2 : 1;
        rt_string_unref(capability);
    }
    c->cluster_light_budget = 64;
    c->last_dropped_light_count = 0;
    c->shadow_budget = VGFX3D_MAX_SHADOW_LIGHTS;
    c->last_shadow_slots_used = 0;
    c->last_shadow_requests_dropped = 0;
    memset(c->shadow_rts, 0, sizeof(c->shadow_rts));
    memset(c->shadow_light_vps, 0, sizeof(c->shadow_light_vps));
    c->frame_serial = 0;
    c->timing_serial = 0;
    c->opaque_depth_sorting = 1;
    /* Frustum culling defaults on: the deferred test is cheap (bounds are cached per
     * draw) and direct DrawMesh callers otherwise get zero submission culling. CPU
     * occlusion culling stays opt-in. */
    c->frustum_culling = 1;
    c->occlusion_culling = 0;
    c->render_scale = 1.0f;
    c->last_submission_status = (int32_t)RT_CANVAS3D_SUBMISSION_OK;
    c->submission_failure_count = 0;
    c->test_fail_next_queue_reserve = 0;
    c->test_fail_next_mesh_snapshot = 0;
    c->frame_draws_submitted = 0;
    c->frame_aabb_transforms = 0;
    c->frame_sort_passes = 0;
    c->frame_backend_state_changes = 0;
    c->frame_last_backend_state_key = 0;
    c->frame_has_backend_state_key = 0;
    c->occlusion_depth_margin = 0.02f;
    c->occlusion_rect_expand_cells = 1;
    c->motion_history = NULL;
    c->motion_history_count = 0;
    c->motion_history_capacity = 0;
    c->motion_history_retention_frames = 4;
    c->frame_is_2d = 0;
    c->has_last_scene_vp = 0;
    c->dt_max_ms = 100;
    c->quality_requested = RT_GRAPHICS3D_QUALITY_BALANCED;
    c->quality_active = RT_GRAPHICS3D_QUALITY_BALANCED;
    c->quality_fallback = 0;
    c->quality_fallback_reason = 0;
    c->input_source = 0;
    c->clock_source = 0;
    c->synthetic_dt_us = CANVAS3D_SYNTHETIC_DT_DEFAULT_US;

    if (offscreen) {
        rt_canvas3d_set_render_target(c, offscreen_target);
        if (c->render_target_owner != offscreen_target || !c->render_target) {
            if (rt_obj_release_check0(c))
                rt_obj_free(c);
            rt_trap("Canvas3D.NewOffscreen: failed to bind render target");
            return NULL;
        }
    } else {
        rt_keyboard_set_canvas(c->gfx_win);
        rt_mouse_set_canvas(c->gfx_win);
        rt_pad_init();
    }

    return c;
}

/// @brief Create a new 3D rendering canvas (window + backend context).
/// @details See canvas3d_new_impl — this is the windowed entry point.
/// @param title Borrowed runtime string used as the platform-window title.
/// @param w Logical window width in pixels, from 1 through `CANVAS3D_MAX_DIMENSION`.
/// @param h Logical window height in pixels, from 1 through `CANVAS3D_MAX_DIMENSION`.
/// @return New GC-managed Canvas3D handle, or NULL after reporting construction failure.
void *rt_canvas3d_new(rt_string title, int64_t w, int64_t h) {
    return canvas3d_new_impl(title, w, h, 0, NULL, 0);
}

/// @brief Create a fullscreen 3D canvas at desktop resolution.
/// @details The window is created directly in fullscreen (no windowed flash);
///          requested dimensions come from the primary display. Toggle back
///          to windowed at runtime via SetFullscreen/ToggleFullscreen.
/// @param title Borrowed runtime string used as the platform-window title.
/// @return New GC-managed fullscreen Canvas3D handle, or NULL after reporting construction failure.
void *rt_canvas3d_new_fullscreen(rt_string title) {
    return canvas3d_new_impl(title, 0, 0, 1, NULL, 0);
}

/// @brief Shared validation and construction for both windowless constructors.
/// @param target Candidate live RenderTarget3D object. The resulting canvas retains it on success.
/// @param prefer_gpu Non-zero to request a headless platform GPU backend before software fallback.
/// @return New GC-managed offscreen Canvas3D handle, or NULL after trapping invalid target,
/// allocation, or backend initialization failures.
static void *canvas3d_new_offscreen_impl(void *target, int32_t prefer_gpu) {
    rt_rendertarget3d *rtd =
        (rt_rendertarget3d *)rt_g3d_checked_or_null(target, RT_G3D_RENDERTARGET3D_CLASS_ID);
    if (!rtd || !rtd->target || !vgfx3d_rendertarget_valid_pixels(rtd->target, NULL)) {
        rt_trap("Canvas3D.NewOffscreen: target must be a live RenderTarget3D");
        return NULL;
    }
    if (!vgfx3d_rendertarget_ensure_color(rtd->target) ||
        !vgfx3d_rendertarget_ensure_depth(rtd->target)) {
        rt_trap("Canvas3D.NewOffscreen: render-target buffer allocation failed");
        return NULL;
    }
    return canvas3d_new_impl(
        NULL, (int64_t)rtd->target->width, (int64_t)rtd->target->height, 0, rtd, prefer_gpu);
}

/// @brief Create a deterministic windowless renderer bound to an explicit RenderTarget3D.
/// @details The software backend accepts a NULL platform window and writes directly into the
///          retained target. No global input canvas, resize callback, or presentation state is
///          installed. The caller can replace the target later with SetRenderTarget.
/// @param target Live RenderTarget3D object with valid color and depth storage.
/// @return New GC-managed software-backed offscreen canvas, or NULL on validation or construction
/// failure. The canvas retains the target.
void *rt_canvas3d_new_offscreen(void *target) {
    return canvas3d_new_offscreen_impl(target, 0);
}

/// @brief Create a windowless renderer that requests the platform GPU backend (ADR 0191).
/// @details Selection and fallback mirror windowed canvases: when the platform backend cannot
///          create a headless context, construction falls back to the deterministic software
///          backend and reports it through IsBackendFallback/BackendName. Output is NOT
///          promised to be byte-identical to the software constructor; determinism-sensitive
///          callers (probes, bakes, goldens) must keep using NewOffscreen.
/// @param target Live RenderTarget3D object with valid color and depth storage.
/// @return New GC-managed offscreen canvas using the platform backend or truthful software
/// fallback, or NULL on validation or construction failure.
void *rt_canvas3d_new_offscreen_accelerated(void *target) {
    return canvas3d_new_offscreen_impl(target, 1);
}

/// @brief Report whether a Canvas3D was created without a platform window.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return 1 for a valid offscreen canvas; otherwise 0.
int8_t rt_canvas3d_get_is_offscreen(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->offscreen ? 1 : 0;
}

#include "rt_canvas3d_draw.inc"
#include "rt_canvas3d_instanced.inc"
#include "rt_canvas3d_render_pass.inc"

/*==========================================================================
 * Finalization and window lifecycle
 *=========================================================================*/

/// @brief Replay recorded final-overlay commands after post-FX.
/// @details Starts a dedicated overlay frame, submits each retained command in order, ends the
/// backend frame, and restores the canvas to an out-of-frame state. Missing backend state or an
/// empty overlay is a no-op.
/// @param c Canvas owning the final-overlay command list and backend context.
static void canvas3d_replay_final_overlay(rt_canvas3d *c) {
    deferred_draw_t *cmds;

    if (!c || c->final_overlay_count <= 0 || !c->backend)
        return;
    if (!canvas3d_begin_overlay_frame(c, 1))
        return;
    cmds = (deferred_draw_t *)c->final_overlay_cmds;
    for (int32_t i = 0; i < c->final_overlay_count; i++)
        canvas3d_submit_screen_overlay_deferred(c, &cmds[i]);
    c->backend->end_frame(c->backend_ctx);
    c->in_frame = 0;
    c->frame_is_2d = 0;
}

/// @brief Apply post-FX and final overlay exactly once, optionally presenting to the window.
/// @details `present_to_window` is false for screenshot capture so finalization can composite the
///   final frame without swapping/presenting a drawable as a side effect.
/// @param c Canvas whose current frame is finalized; NULL and canvases without an output are
/// ignored.
/// @param present_to_window Non-zero to permit the backend presentation path; zero composites for
/// readback only.
static void canvas3d_finalize_frame_impl(rt_canvas3d *c, int present_to_window) {
    if (!c)
        return;
    if (!c->gfx_win && !c->render_target)
        return;
    if (c->frame_finalized)
        return;

    if (c->final_overlay_recording)
        rt_canvas3d_end_overlay(c);
    if (c->in_frame)
        rt_canvas3d_end(c);

    if (present_to_window && canvas3d_backend_uses_gpu_postfx(c) &&
        (c->final_overlay_count == 0 || canvas3d_backend_splits_gpu_postfx_present(c))) {
        if (c->frame_gpu_postfx_enabled) {
            if (canvas3d_backend_splits_gpu_postfx_present(c)) {
                // Replay the final overlay (HUD) into the backend's overlay
                // target BEFORE applying post-FX. Each split-capable backend
                // composites the overlay on top of the post-processed scene only
                // when it was already drawn this frame (the overlay-composite
                // pass in the backend's post-FX encoder is gated on that flag).
                // Applying post-FX first left the just-drawn overlay in a
                // separate target that present never composited, so the HUD
                // flickered against the 3D scene depending on buffer aliasing.
                canvas3d_replay_final_overlay(c);
                c->backend->apply_postfx(c->backend_ctx, &c->frame_postfx_chain);
                c->backend->present(c->backend_ctx);
            } else {
                c->backend->present_postfx(c->backend_ctx, &c->frame_postfx_chain);
            }
            c->frame_presented_by_finalize = 1;
            c->frame_finalized = 1;
            return;
        }
    }
    if (!present_to_window && canvas3d_backend_splits_gpu_postfx_present(c) &&
        c->frame_gpu_postfx_enabled) {
        // Same overlay-before-post-FX ordering as the on-screen path so a
        // captured frame includes the HUD composited over the post-processed
        // scene rather than dropping it.
        canvas3d_replay_final_overlay(c);
        c->backend->apply_postfx(c->backend_ctx, &c->frame_postfx_chain);
        c->frame_finalized = 1;
        return;
    }

    /* The CPU chain mutates the output buffer in place (post-FX gamma-out,
     * overlay composite). Present clears frame_finalized for the next render,
     * so a capture that follows a presented frame would otherwise rerun the
     * chain on already-finalized pixels — double-applying gamma washes the
     * frame gray. Run it at most once per rendered frame; Clear/Begin mark
     * the next render and re-arm it. */
    if (!c->cpu_finalized_this_render) {
        c->cpu_finalized_this_render = 1;
        rt_postfx3d_apply_to_canvas(c);
        canvas3d_replay_final_overlay(c);
    }
    c->frame_finalized = 1;
}

/// @brief Apply post-FX and final overlay exactly once.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_finalize_frame(void *obj) {
    canvas3d_finalize_frame_impl(rt_canvas3d_checked_or_stack(obj), 1);
}

/// @brief Return whether the current frame has already been finalized.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return 1 when the valid canvas has completed final compositing; otherwise 0.
int8_t rt_canvas3d_get_frame_finalized(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->frame_finalized ? 1 : 0;
}

/// @brief Capture finalized frame pixels, finalizing first if needed.
/// @details Finalization is performed without presenting a window drawable, after which the normal
/// screenshot path returns the active output. Ownership matches `rt_canvas3d_screenshot`.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Screenshot Pixels object from the active output, or NULL on invalid input or readback
/// failure.
void *rt_canvas3d_screenshot_final(void *obj) {
    canvas3d_finalize_frame_impl(rt_canvas3d_checked_or_stack(obj), 0);
    return rt_canvas3d_screenshot(obj);
}

/// @brief Finalize the current frame if needed, then copy it into an existing Pixels object.
/// @param obj Canvas3D handle or approved stack fixture.
/// @param pixels Caller-owned destination Pixels object accepted by the normal screenshot-copy API.
/// @return 1 on successful finalization/readback, or 0 for invalid input/readback failure.
int8_t rt_canvas3d_try_copy_screenshot_final_to(void *obj, void *pixels) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    canvas3d_finalize_frame_impl(c, 0);
    return rt_canvas3d_try_copy_screenshot_to(obj, pixels);
}

/// @brief Present the rendered frame to the window (swaps buffers).
/// @details Finalizes the frame if needed, then presents finalized pixels.
///          Updates the FPS counter and delta-time calculation for the next
///          frame.
/// @param obj Windowed Canvas3D handle or approved stack fixture. Invalid and offscreen canvases
/// are ignored.
void rt_canvas3d_flip(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!c->gfx_win)
        return;

    rt_canvas3d_finalize_frame(obj);

    /* Present the GPU drawable / swap the back buffer after all queued passes
     * for the frame have rendered into the backend's scene targets. */
    if (!c->frame_presented_by_finalize && c->backend && c->backend->present)
        c->backend->present(c->backend_ctx);

    /* Always call vgfx_update to keep the window alive and process display
     * refresh. GPU backends own the final on-screen present path. */
    vgfx_update(c->gfx_win);
    canvas3d_reset_finalized_frame_state(c);

    if (c->frame_timing_updated_by_poll) {
        c->frame_timing_updated_by_poll = 0;
    } else if (c->clock_source == 1) {
        canvas3d_apply_synthetic_clock(c, 1);
    } else {
        canvas3d_update_live_clock(c);
    }

    if (vgfx_close_requested(c->gfx_win))
        canvas3d_close_window(c);
}

/// @brief Notify the mouse subsystem from logical Canvas3D coordinates.
/// @details `vgfx_mouse_pos()` already returns logical coordinates once
///          Canvas3D enables the window coordinate scale, so live polling must
///          pass those values through without applying the scale a second time.
/// @param x Logical horizontal cursor coordinate.
/// @param y Logical vertical cursor coordinate.
static void rt_canvas3d_update_mouse_from_logical(int32_t x, int32_t y) {
    rt_mouse_update_pos((int64_t)x, (int64_t)y);
}

/// @brief Translate physical pixel event coordinates to logical coordinates.
/// @details Platform mouse events carry physical backing-pixel coordinates.
///          Dividing by the per-window scale keeps event positions aligned
///          with Canvas3D's public logical coordinate space.
/// @param gfx_win Window whose HiDPI scale converts backing pixels to logical units.
/// @param x Physical horizontal event coordinate in backing pixels.
/// @param y Physical vertical event coordinate in backing pixels.
static void rt_canvas3d_update_mouse_from_physical(vgfx_window_t gfx_win, int32_t x, int32_t y) {
    float scale = vgfx_window_get_scale(gfx_win);
    if (!isfinite(scale) || scale < 0.001f)
        scale = 1.0f;
    rt_canvas3d_update_mouse_from_logical((int32_t)((double)x / (double)scale),
                                          (int32_t)((double)y / (double)scale));
}

/// @brief Pump platform events, advance per-frame input state, and return whether the window is
/// still open.
///
/// Called once per game-loop iteration. Drives keyboard/mouse/
/// gamepad/action input subsystems, updates the wall-clock dt
/// (capped at `dt_max` to prevent huge jumps after pauses), and
/// dispatches resize / focus / close events.
/// @param obj Canvas3D handle or approved stack fixture. A window is required unless the input
/// source is synthetic-only.
/// @return 1 if the window remains open, 0 if the user requested close.
int64_t rt_canvas3d_poll(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    if (!c->gfx_win && c->input_source != 1)
        return 0;

    int8_t use_live = c->input_source != 1;
    int8_t use_synthetic = c->input_source != 0;
    int8_t captured = use_live ? rt_mouse_is_captured() : 0;
    int8_t relative_native = 0;
    c->last_event_type = VGFX_EVENT_NONE;

    /* Begin frame (resets per-frame state for keyboard/mouse/pad) */
    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();
    rt_pad_begin_frame();

    if (c->clock_source == 0) {
        canvas3d_update_live_clock(c);
        c->frame_timing_updated_by_poll = 1;
    }

    if (use_live) {
        rt_pad_poll();

        if (!vgfx_pump_events(c->gfx_win)) {
            canvas3d_close_window(c);
            rt_action_update();
            return 0;
        }

        /* Reconcile relative (raw) mouse mode with the platform window. The
         * runtime input layer records the request; this poll owns the window
         * handle, so it applies the change and reports back whether native
         * raw deltas are available (else the warp-to-center path serves). */
        int8_t relative_requested = (int8_t)(captured && rt_mouse_get_relative_mode());
        if (relative_requested != c->relative_mouse_applied) {
            int32_t native = vgfx_set_relative_mouse(c->gfx_win, relative_requested);
            rt_mouse_set_relative_native(relative_requested ? (int8_t)native : 0);
            c->relative_mouse_applied = relative_requested;
        }
        relative_native = (int8_t)(captured && rt_mouse_get_relative_native());

        if (captured && relative_native) {
            /* Native raw deltas: unbounded, unaccelerated (platform-
             * dependent), sub-pixel. No warping needed. */
            double rdx = 0.0;
            double rdy = 0.0;
            vgfx_get_relative_deltas(c->gfx_win, &rdx, &rdy);
            rt_mouse_force_delta_f(rdx, rdy);
        }

        /* Process events before sampled fallback deltas so queued warp/move events are drained
         * before the center-offset path reads the live cursor position. */
        vgfx_event_t evt;
        while (vgfx_poll_event(c->gfx_win, &evt)) {
            canvas3d_record_event_type(c, (int64_t)evt.type);
            if (evt.type == VGFX_EVENT_KEY_DOWN)
                rt_keyboard_on_vgfx_key_down((int64_t)evt.data.key.key);
            else if (evt.type == VGFX_EVENT_KEY_UP)
                rt_keyboard_on_vgfx_key_up((int64_t)evt.data.key.key);
            else if (evt.type == VGFX_EVENT_TEXT_INPUT)
                rt_keyboard_text_input((int32_t)evt.data.text.codepoint);
            else if (evt.type == VGFX_EVENT_CLOSE) {
                canvas3d_close_window(c);
                break;
            } else if (!captured && evt.type == VGFX_EVENT_MOUSE_MOVE) {
                rt_canvas3d_update_mouse_from_physical(
                    c->gfx_win, evt.data.mouse_move.x, evt.data.mouse_move.y);
            } else if (evt.type == VGFX_EVENT_MOUSE_DOWN) {
                rt_canvas3d_update_mouse_from_physical(
                    c->gfx_win, evt.data.mouse_button.x, evt.data.mouse_button.y);
                rt_mouse_button_down((int64_t)evt.data.mouse_button.button);
            } else if (evt.type == VGFX_EVENT_MOUSE_UP) {
                rt_canvas3d_update_mouse_from_physical(
                    c->gfx_win, evt.data.mouse_button.x, evt.data.mouse_button.y);
                rt_mouse_button_up((int64_t)evt.data.mouse_button.button);
            } else if (evt.type == VGFX_EVENT_RESIZE) {
                rt_canvas3d_apply_resize(c,
                                         evt.data.resize.logical_width,
                                         evt.data.resize.logical_height,
                                         evt.data.resize.width,
                                         evt.data.resize.height);
            } else if (evt.type == VGFX_EVENT_SCROLL) {
                rt_canvas3d_update_mouse_from_physical(
                    c->gfx_win, evt.data.scroll.x, evt.data.scroll.y);
                rt_mouse_update_wheel((double)evt.data.scroll.delta_x,
                                      (double)evt.data.scroll.delta_y);
            }
        }

        if (!c->gfx_win) {
            rt_action_update();
            return 0;
        }

        /* Read current platform mouse position. vgfx_mouse_pos() returns logical coordinates
         * because Canvas3D enables coord_scale at window creation. */
        int32_t mx, my;
        vgfx_mouse_pos(c->gfx_win, &mx, &my);
        if (captured && !relative_native) {
            int32_t cw, ch;
            vgfx_get_size(c->gfx_win, &cw, &ch);
            int32_t cx = cw / 2, cy = ch / 2;
            int64_t dx = (int64_t)((double)mx - (double)cx);
            int64_t dy = (int64_t)((double)my - (double)cy);
            rt_mouse_force_delta(dx, dy);
        } else if (!captured) {
            rt_canvas3d_update_mouse_from_logical(mx, my);
            /* Recompute the absolute delta after this frame's motion so
             * Mouse.DeltaX/Y describe the same frame as Mouse.X/Y. Captured
             * paths above force their own (relative) deltas instead. */
            rt_mouse_finalize_frame();
        }
    }

    if (use_synthetic)
        canvas3d_apply_synthetic_input(c);

    /* Update action mapping state after input devices and event queues are
     * finalized so action queries observe this frame's input. */
    rt_action_update();

    if (c->clock_source == 1) {
        canvas3d_apply_synthetic_clock(c, 1);
        c->frame_timing_updated_by_poll = 1;
    }

    /* Warp cursor to center for next frame (only for the captured fallback
     * path — native relative mode never needs warping). */
    if (use_live && captured && !relative_native) {
        int32_t cw, ch;
        vgfx_get_size(c->gfx_win, &cw, &ch);
        vgfx_warp_cursor(c->gfx_win, cw / 2, ch / 2);
    }

    return c->should_close ? 0 : 1;
}

/// @brief Pop the next queued window/input event for the canvas, encoded as an int64 code.
/// @details Returns one event per call (0 when the queue is empty), advancing the per-frame event
///          tally; pair with the window pump that fills the queue.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Oldest queued `VGFX_EVENT_*` type, or `VGFX_EVENT_NONE` for invalid or empty input.
int64_t rt_canvas3d_poll_event(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || c->event_type_count <= 0)
        return VGFX_EVENT_NONE;
    int64_t type = c->event_type_queue[c->event_type_head];
    c->event_type_head = (c->event_type_head + 1) % RT_CANVAS3D_EVENT_QUEUE_CAPACITY;
    c->event_type_count--;
    return type;
}

/// @brief Check if the canvas window received a close request.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Non-zero when a valid canvas has been marked for closure; zero for invalid input.
int8_t rt_canvas3d_should_close(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->should_close : 0;
}

/// @brief Enable or disable wireframe rendering mode.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to render polygon edges; zero for filled rasterization.
void rt_canvas3d_set_wireframe(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (c)
        c->wireframe = enabled ? 1 : 0;
}

/// @brief Read whether wireframe rendering is enabled (ADR 0227).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Nonzero when wireframe rendering is active, or 0 for invalid handles.
int8_t rt_canvas3d_get_wireframe(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->wireframe ? 1 : 0;
}

/// @brief Enable or disable backface culling (CCW winding = front face).
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to cull clockwise back faces; zero to submit both sides.
void rt_canvas3d_set_backface_cull(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (c)
        c->backface_cull = enabled ? 1 : 0;
}

/// @brief `Canvas3D.BackfaceCull` — read the retained culling flag (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Nonzero while backface culling is enabled.
int8_t rt_canvas3d_get_backface_cull(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->backface_cull ? 1 : 0;
}

/// @brief Park a `malloc`'d buffer for end-of-frame disposal.
///
/// Used by skinning / morph-target paths that allocate
/// per-draw vertex transforms — the canvas owns the lifetime
/// until after the GPU has consumed the data on `end()`.
/// @param obj Canvas3D handle or approved stack fixture.
/// @param buffer Non-NULL malloc-compatible allocation whose ownership transfers on success.
/// @return Non-zero when the canvas accepted ownership; zero leaves ownership with the caller.
int rt_canvas3d_add_temp_buffer(void *obj, void *buffer) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !buffer)
        return 0;
    return canvas3d_track_temp_buffer(c, buffer);
}

/// @brief Undo temp-buffer ownership transfer before frame cleanup.
/// @param obj Canvas3D handle or approved stack fixture.
/// @param buffer Previously tracked allocation to remove without freeing.
/// @return Non-zero when the allocation was found and untracked; zero otherwise.
int rt_canvas3d_remove_temp_buffer(void *obj, void *buffer) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !buffer)
        return 0;
    return canvas3d_untrack_temp_buffer(c, buffer);
}

/// @brief Park a GC-managed object reference for end-of-frame release.
///
/// Lets a draw call reference an object (mesh, material, pixels)
/// that might otherwise be collected before the deferred queue
/// flushes. The canvas drops the reference in `rt_canvas3d_end`.
/// @param obj Canvas3D handle or approved stack fixture.
/// @param value Non-NULL GC-managed object to retain through frame cleanup.
/// @return Non-zero when the object is retained or already tracked; zero on invalid input or
/// allocation failure.
int rt_canvas3d_add_temp_object(void *obj, void *value) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !value)
        return 0;
    return canvas3d_track_temp_object(c, value);
}

/// @brief Get the current canvas width in pixels (updates on window resize).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Active render-target width when bound, otherwise logical window width; zero when
/// invalid.
int64_t rt_canvas3d_get_width(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    int32_t out_w = 0;
    canvas3d_active_output_size(c, &out_w, NULL);
    return out_w;
}

/// @brief Get the current canvas height in pixels (updates on window resize).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Active render-target height when bound, otherwise logical window height; zero when
/// invalid.
int64_t rt_canvas3d_get_height(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    int32_t out_h = 0;
    canvas3d_active_output_size(c, NULL, &out_h);
    return out_h;
}

/// @brief Get the backing window's logical width, ignoring any bound render target.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Positive logical window width, or zero for invalid, offscreen, or unavailable state.
int64_t rt_canvas3d_get_window_width(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return (c && c->width > 0) ? c->width : 0;
}

/// @brief Get the backing window's logical height, ignoring any bound render target.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Positive logical window height, or zero for invalid, offscreen, or unavailable state.
int64_t rt_canvas3d_get_window_height(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return (c && c->height > 0) ? c->height : 0;
}

/// @brief Re-query the live window size after a mode change and propagate it into
///        the canvas/backend so the next frame uses the new dimensions.
/// @details Mirrors the 2D canvas's resync-after-fullscreen path. The window's own
///          resize event is also hooked (`rt_canvas3d_on_resize`), but a mode toggle
///          should take effect immediately rather than waiting for the next polled
///          event. The per-frame projection derives aspect from the active output
///          size (see `canvas3d_active_output_size`), so updating width/height here
///          is sufficient to keep the view un-stretched across the toggle.
/// @param c Windowed canvas whose live logical size is queried; NULL and offscreen canvases are
/// ignored.
static void rt_canvas3d_resync_window_size(rt_canvas3d *c) {
    int32_t logical_w = 0;
    int32_t logical_h = 0;
    if (!c || !c->gfx_win)
        return;
    if (vgfx_get_size(c->gfx_win, &logical_w, &logical_h) && logical_w > 0 && logical_h > 0)
        rt_canvas3d_apply_resize(c, logical_w, logical_h, 0, 0);
}

/// @brief Switch the canvas window between fullscreen and windowed mode.
/// @param obj Canvas3D handle. NULL-safe.
/// @param enabled Non-zero requests fullscreen; zero requests windowed.
void rt_canvas3d_set_fullscreen(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !c->gfx_win)
        return;
    vgfx_set_fullscreen(c->gfx_win, enabled ? 1 : 0);
    rt_canvas3d_resync_window_size(c);
}

/// @brief Report whether the canvas window is currently fullscreen.
/// @param obj Canvas3D handle. NULL-safe.
/// @return 1 when fullscreen, 0 when windowed or unavailable.
int8_t rt_canvas3d_is_fullscreen(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !c->gfx_win)
        return 0;
    return vgfx_is_fullscreen(c->gfx_win) > 0 ? 1 : 0;
}

/// @brief Flip the canvas window between fullscreen and windowed mode.
/// @param obj Canvas3D handle. NULL-safe.
void rt_canvas3d_toggle_fullscreen(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !c->gfx_win)
        return;
    vgfx_set_fullscreen(c->gfx_win, vgfx_is_fullscreen(c->gfx_win) > 0 ? 0 : 1);
    rt_canvas3d_resync_window_size(c);
}

/// @brief Explicit alias for Width: the active output width, including render targets.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Same value and invalid-input behavior as `rt_canvas3d_get_width`.
int64_t rt_canvas3d_get_active_output_width(void *obj) {
    return rt_canvas3d_get_width(obj);
}

/// @brief Explicit alias for Height: the active output height, including render targets.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Same value and invalid-input behavior as `rt_canvas3d_get_height`.
int64_t rt_canvas3d_get_active_output_height(void *obj) {
    return rt_canvas3d_get_height(obj);
}

/// @brief Get the rolling-average frames-per-second from recent timing samples.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Nearest integer FPS from the rolling microsecond window, with legacy delta fallbacks;
/// zero when no positive timing sample is available or the handle is invalid.
int64_t rt_canvas3d_get_fps(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    if (c->fps_sample_count > 0 && c->fps_sample_total_us > 0) {
        int64_t numerator = (int64_t)c->fps_sample_count * INT64_C(1000000);
        return (numerator + c->fps_sample_total_us / 2) / c->fps_sample_total_us;
    }
    if (c->delta_time_us > 0)
        return (1000000 + c->delta_time_us / 2) / c->delta_time_us;
    return c->delta_time_ms > 0 ? 1000 / c->delta_time_ms : 0;
}

/// @brief Get the time elapsed since the last frame in milliseconds.
/// @details Clamped to dt_max (default 100ms) to prevent physics explosions
///          after long pauses (e.g., window drag, breakpoint, alt-tab).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Non-negative elapsed milliseconds, capped by `dt_max_ms` when enabled; zero for invalid
/// input or a non-positive sample.
int64_t rt_canvas3d_get_delta_time(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0;
    int64_t dt = c->delta_time_ms;
    if (dt <= 0)
        return 0;
    if (c->dt_max_ms > 0) {
        if (dt > c->dt_max_ms)
            dt = c->dt_max_ms;
    }
    return dt;
}

/// @brief Get the time elapsed since the last frame in seconds.
/// @details Uses microsecond timing when available, applies the same optional `dt_max_ms` cap as
/// the millisecond getter, and falls back to that getter for legacy timing state.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Non-negative elapsed seconds, or zero for invalid input or no positive sample.
double rt_canvas3d_get_delta_time_sec(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return 0.0;
    if (c->delta_time_us > 0) {
        int64_t dt_us = c->delta_time_us;
        if (c->dt_max_ms > 0) {
            int64_t max_us = c->dt_max_ms <= INT64_MAX / 1000 ? c->dt_max_ms * 1000 : INT64_MAX;
            if (dt_us > max_us)
                dt_us = max_us;
        }
        return (double)dt_us / 1000000.0;
    }
    return (double)rt_canvas3d_get_delta_time(obj) / 1000.0;
}

/// @brief Cap the per-frame delta-time at `max_ms` milliseconds (prevents huge jumps after pauses).
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param max_ms Positive cap in milliseconds. Non-positive values disable clamping, and very large
/// values are limited so conversion to microseconds cannot overflow.
void rt_canvas3d_set_dt_max(void *obj, int64_t max_ms) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (c)
        c->dt_max_ms = max_ms > 0 ? (max_ms > INT64_MAX / 1000 ? INT64_MAX / 1000 : max_ms) : 0;
}

/// @brief `Canvas3D.MaxDeltaTime` — read the retained delta-time cap (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained cap in milliseconds (zero = uncapped), or zero when invalid.
int64_t rt_canvas3d_get_dt_max(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->dt_max_ms : 0;
}

/// @brief Select live, synthetic, or live+synthetic input.
/// @details Modes outside the supported range select live input. Returning to live-only releases
/// held synthetic keys/buttons and ownership; selecting a synthetic mode transfers the single
/// process-wide synthetic owner when necessary.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param mode 0 for live input, 1 for synthetic-only, or 2 for combined live and synthetic input.
void rt_canvas3d_set_input_source(void *obj, int64_t mode) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    int32_t next = canvas3d_input_source_from_mode(mode);
    if (next == 0 && c->input_source != 0)
        canvas3d_release_synthetic_input(c);
    else if (next != 0) {
        rt_canvas3d *owner = canvas3d_synthetic_owner_load();
        if (owner && owner != c)
            canvas3d_set_synthetic_owner(c);
    }
    c->input_source = next;
}

/// @brief Queue a synthetic keyboard transition for the next synthetic input frame.
/// @details Invalid key codes and samples beyond the fixed queue capacity are ignored without
/// displacing already queued transitions.
/// @param obj Canvas3D handle or approved stack fixture.
/// @param key Runtime key code strictly between zero and `ZANNA_KEY_MAX`.
/// @param down Non-zero for key-down; zero for key-up.
void rt_canvas3d_push_synthetic_key(void *obj, int64_t key, int8_t down) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || key <= 0 || key >= ZANNA_KEY_MAX)
        return;
    if (c->synthetic_key_count >= CANVAS3D_SYNTHETIC_KEY_QUEUE_MAX)
        return;
    int32_t idx = c->synthetic_key_count++;
    c->synthetic_key_keys[idx] = key;
    c->synthetic_key_downs[idx] = down ? 1 : 0;
}

/// @brief Queue a synthetic mouse movement/button/wheel sample.
/// @details Movement and wheel deltas accumulate with finite saturation. The button bitset replaces
/// the previous pending button sample after masking unsupported bits.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param dx Horizontal logical displacement to accumulate.
/// @param dy Vertical logical displacement to accumulate.
/// @param buttons Bitset of buttons that should be held after the sample; non-positive values clear
/// all supported buttons.
/// @param wheel Vertical wheel displacement to accumulate.
void rt_canvas3d_push_synthetic_mouse(
    void *obj, double dx, double dy, int64_t buttons, double wheel) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->synthetic_mouse_dx = canvas3d_accumulate_synthetic_mouse_delta(c->synthetic_mouse_dx, dx);
    c->synthetic_mouse_dy = canvas3d_accumulate_synthetic_mouse_delta(c->synthetic_mouse_dy, dy);
    c->synthetic_mouse_wheel_y =
        canvas3d_accumulate_synthetic_mouse_delta(c->synthetic_mouse_wheel_y, wheel);
    c->synthetic_mouse_buttons = buttons > 0 ? buttons & canvas3d_synthetic_mouse_button_mask() : 0;
    c->synthetic_mouse_has_buttons = 1;
}

/// @brief Clear queued synthetic input and release keys/buttons held by the synthetic source.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_clear_synthetic_input(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    canvas3d_release_synthetic_input(c);
}

/// @brief Select live wall-clock or fixed synthetic delta-time source.
/// @details Selecting synthetic time immediately publishes the stored fixed delta without recording
/// an FPS sample. Modes other than 1 select the live clock.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param mode 1 for fixed synthetic time; any other value for live wall-clock time.
void rt_canvas3d_set_clock_source(void *obj, int64_t mode) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->clock_source = canvas3d_clock_source_from_mode(mode);
    c->frame_timing_updated_by_poll = 0;
    if (c->clock_source == 1)
        canvas3d_apply_synthetic_clock(c, 0);
}

/// @brief Set the fixed synthetic delta time in seconds.
/// @details The value is rounded to microseconds, clamps negative and non-finite input to zero, and
/// caps excessively large durations. An active synthetic clock is updated immediately.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param dt Requested deterministic frame duration in seconds.
void rt_canvas3d_set_synthetic_delta_time_sec(void *obj, double dt) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->synthetic_dt_us = canvas3d_synthetic_seconds_to_us(dt);
    if (c->clock_source == 1)
        canvas3d_apply_synthetic_clock(c, 0);
}

/// @brief Advance one deterministic synthetic input/timing frame.
/// @details Resets per-frame device state, applies queued synthetic input, refreshes actions, and,
/// when selected, publishes and samples the fixed synthetic delta. Rendering is not advanced.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_advance_synthetic_frame(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();
    rt_pad_begin_frame();
    canvas3d_apply_synthetic_input(c);
    rt_action_update();
    if (c->clock_source == 1) {
        canvas3d_apply_synthetic_clock(c, 1);
        c->frame_timing_updated_by_poll = 1;
    }
}

/// @brief Assign a light to one of the per-canvas light slots.
/// @details Slot index must be in [0, VGFX3D_MAX_LIGHTS). Pass NULL to clear a slot.
/// The canvas retains a valid replacement and releases the previous slot value. Type and range
/// violations trap without modifying the slot.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param index Zero-based fixed light-slot index.
/// @param light Live Light3D object to retain, or NULL to clear the slot.
void rt_canvas3d_set_light(void *obj, int64_t index, void *light) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (index < 0 || index >= VGFX3D_MAX_LIGHTS) {
        rt_trap("Canvas3D.SetLight: index out of range");
        return;
    }
    if (light && !rt_obj_is_instance(light, RT_G3D_LIGHT3D_CLASS_ID, sizeof(rt_light3d))) {
        rt_trap("Canvas3D.SetLight: light must be Light3D");
        return;
    }
    canvas3d_assign_owned_ref((void **)&c->lights[index], light);
    canvas3d_invalidate_light_flatten_cache(c);
}

/// @brief Clear every retained per-canvas light slot.
/// @details Releases all light references and invalidates the flattened light-parameter cache.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_clear_lights(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    for (int32_t i = 0; i < VGFX3D_MAX_LIGHTS; i++)
        canvas3d_release_owned_ref((void **)&c->lights[i]);
    canvas3d_invalidate_light_flatten_cache(c);
}

/// @brief Count active per-canvas light slots.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Number of slots containing a live, enabled Light3D; zero for invalid input.
int64_t rt_canvas3d_get_light_count(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    int64_t count = 0;
    if (!c)
        return 0;
    for (int32_t i = 0; i < VGFX3D_MAX_LIGHTS; i++) {
        rt_light3d *light =
            rt_obj_is_instance(c->lights[i], RT_G3D_LIGHT3D_CLASS_ID, sizeof(rt_light3d))
                ? (rt_light3d *)c->lights[i]
                : NULL;
        if (light && light->enabled)
            count++;
    }
    return count;
}

/// @brief Attempt to enable or disable clustered lighting without trapping.
///
/// @param obj Canvas3D instance.
/// @param enabled Non-zero to request clustered/forward+ lighting; zero to force the
///        classic forward-light path.
/// @return 1 when the requested state was applied, or 0 when @p obj is invalid or
///         clustered lighting was requested but is disabled by the environment or
///         unsupported by the active backend.
int8_t rt_canvas3d_try_set_clustered_lighting(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    const char *clusters_env;
    if (!c)
        return 0;
    if (!enabled) {
        c->clustered_lighting = 0;
        return 1;
    }
    clusters_env = getenv("ZANNA_3D_CLUSTERS");
    if (clusters_env && clusters_env[0] == '0' && clusters_env[1] == '\0') {
        c->clustered_lighting = 0; /* env kill switch wins over explicit enables */
        return 0;
    }
    rt_string capability = rt_const_cstr("clustered-lighting");
    int8_t supported = rt_canvas3d_backend_supports(c, capability);
    rt_string_unref(capability);
    if (!supported) {
        c->clustered_lighting = 0;
        return 0;
    }
    c->clustered_lighting = 1;
    return 1;
}

/// @brief Enable the clustered-lighting path only when advertised by the backend.
/// @details Disabling always succeeds for a valid canvas. An unsupported explicit enable reports a
/// runtime trap after preserving the classic forward-light path.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to request clustered forward-plus lighting; zero for classic lighting.
void rt_canvas3d_set_clustered_lighting(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (rt_canvas3d_try_set_clustered_lighting(c, enabled))
        return;
    if (enabled) {
        rt_trap("Canvas3D.ClusteredLighting: clustered lighting is not supported by this "
                "backend");
        return;
    }
}

/// @brief Report whether the clustered-lighting path is currently enabled.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return 1 when clustered lighting is active; otherwise 0.
int8_t rt_canvas3d_get_clustered_lighting(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return (c && c->clustered_lighting) ? 1 : 0;
}

/// @brief Report the active lighting budget: 64 when clustered lighting is on, else 16.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Backend-appropriate maximum number of flattened active lights.
int64_t rt_canvas3d_get_max_active_lights(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return canvas3d_active_light_limit(c);
}

/// @brief Install a readable key/fill/ambient setup without enabling implicit fallback lighting.
/// @details Replaces all retained lights, sets a neutral ambient color, creates two directional
/// lights, and transfers their references into slots zero and one. Allocation failures may leave
/// either slot empty but do not leak partially created runtime objects.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_set_default_lighting(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    void *key_dir;
    void *fill_dir;
    void *key;
    void *fill;
    if (!c)
        return;

    rt_canvas3d_clear_lights(c);
    c->ambient[0] = 0.18f;
    c->ambient[1] = 0.18f;
    c->ambient[2] = 0.20f;
    if (c->shadows_enabled && (!isfinite(c->shadow_bias) || c->shadow_bias <= 0.0f))
        c->shadow_bias = 0.005f;

    key_dir = rt_vec3_new(-0.45, -0.75, -0.48);
    fill_dir = rt_vec3_new(0.65, -0.35, 0.55);
    key = key_dir ? rt_light3d_new_directional(key_dir, 1.0, 0.96, 0.88) : NULL;
    fill = fill_dir ? rt_light3d_new_directional(fill_dir, 0.55, 0.65, 1.0) : NULL;
    if (key)
        rt_light3d_set_intensity(key, 1.35);
    if (fill)
        rt_light3d_set_intensity(fill, 0.35);
    canvas3d_assign_owned_ref((void **)&c->lights[0], key);
    canvas3d_assign_owned_ref((void **)&c->lights[1], fill);

    if (key && rt_obj_release_check0(key))
        rt_obj_free(key);
    if (fill && rt_obj_release_check0(fill))
        rt_obj_free(fill);
    if (key_dir && rt_obj_release_check0(key_dir))
        rt_obj_free(key_dir);
    if (fill_dir && rt_obj_release_check0(fill_dir))
        rt_obj_free(fill_dir);
}

/// @brief Set the global ambient light color for the canvas (applied to all surfaces).
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param r Red channel; clamped to the inclusive normalized range, with non-finite input becoming
/// zero.
/// @param g Green channel with the same sanitization as @p r.
/// @param b Blue channel with the same sanitization as @p r.
void rt_canvas3d_set_ambient(void *obj, double r, double g, double b) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->ambient[0] = canvas3d_clamp01_f64(r);
    c->ambient[1] = canvas3d_clamp01_f64(g);
    c->ambient[2] = canvas3d_clamp01_f64(b);
    /* Ambient participates in the light revision stamp; drop the flatten cache so the
     * next queued draw re-stamps instead of reusing a stale shared revision. */
    canvas3d_invalidate_light_flatten_cache(c);
}

/// @brief `Canvas3D.AmbientColor` — fresh `Vec3` of the retained ambient color (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Newly allocated color snapshot; origin for an invalid handle.
void *rt_canvas3d_get_ambient_color(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new((double)c->ambient[0], (double)c->ambient[1], (double)c->ambient[2]);
}

/// @brief Enable or disable image-based lighting from the canvas skybox.
/// @details This setter only toggles state. The skybox's SH-9 irradiance and
///   GGX-prefiltered specular mip chain are prepared lazily by the first eligible
///   PBR draw so UI/control paths do not hitch when changing IBL state.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to allow skybox-based diffuse and specular environment lighting.
void rt_canvas3d_set_ibl_enabled(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->ibl_enabled = enabled ? 1 : 0;
}

/// @brief True when image-based lighting is enabled for this canvas.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return 1 when IBL is enabled on a valid canvas; otherwise 0.
int8_t rt_canvas3d_get_ibl_enabled(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return (c && c->ibl_enabled) ? 1 : 0;
}

/// @brief Scale the environment lighting contribution (default 1.0, clamped to [0, 8]).
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param intensity Requested non-negative multiplier. Non-finite and negative values become zero;
/// values above eight are capped.
void rt_canvas3d_set_ibl_intensity(void *obj, double intensity) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!isfinite(intensity) || intensity < 0.0)
        intensity = 0.0;
    if (intensity > 8.0)
        intensity = 8.0;
    c->ibl_intensity = (float)intensity;
}

/// @brief Current environment lighting intensity scale.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Stored IBL multiplier, or zero for invalid input.
double rt_canvas3d_get_ibl_intensity(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (double)c->ibl_intensity : 0.0;
}

/*==========================================================================
 * Debug drawing — transform 3D points to screen via backend VP
 *=========================================================================*/

/*==========================================================================
 * Fog — linear distance fog
 *=========================================================================*/

/// @brief Configure the global fog parameters (color, near/far, density).
///
/// Applied by the postFX stage as a depth-keyed blend toward
/// `fog_color`. Setting `fog_density` to 0 disables fog without
/// changing the queued draws.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param near_dist Non-negative distance at which linear fog begins.
/// @param far_dist Distance at which fog reaches full strength; sanitized to remain greater than
/// the near distance.
/// @param r Normalized fog red channel.
/// @param g Normalized fog green channel.
/// @param b Normalized fog blue channel.
void rt_canvas3d_set_fog(
    void *obj, double near_dist, double far_dist, double r, double g, double b) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    float sanitized_near = canvas3d_sanitize_nonnegative_f64(near_dist, 0.0f);
    float sanitized_far = canvas3d_sanitize_nonnegative_f64(far_dist, sanitized_near + 1.0f);
    if (sanitized_far <= sanitized_near + 1e-4f)
        sanitized_far = sanitized_near + 1.0f;
    c->fog_enabled = 1;
    c->fog_near = sanitized_near;
    c->fog_far = sanitized_far;
    c->fog_color[0] = canvas3d_clamp01_f64(r);
    c->fog_color[1] = canvas3d_clamp01_f64(g);
    c->fog_color[2] = canvas3d_clamp01_f64(b);
}

/// @brief `Canvas3D.FogEnabled` — read whether distance fog is active (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Nonzero while distance fog is enabled.
int8_t rt_canvas3d_get_fog_enabled(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->fog_enabled ? 1 : 0;
}

/// @brief `Canvas3D.FogNear` — read the retained fog start distance (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained near distance, or zero for an invalid handle.
double rt_canvas3d_get_fog_near(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (double)c->fog_near : 0.0;
}

/// @brief `Canvas3D.FogFar` — read the retained full-strength fog distance (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained far distance, or zero for an invalid handle.
double rt_canvas3d_get_fog_far(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (double)c->fog_far : 0.0;
}

/// @brief `Canvas3D.FogColor` — fresh `Vec3` of the retained fog color (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Newly allocated color snapshot; origin for an invalid handle.
void *rt_canvas3d_get_fog_color(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new((double)c->fog_color[0], (double)c->fog_color[1], (double)c->fog_color[2]);
}

/// @brief Enable exponential height fog on top of (or independent of) distance fog.
/// @details Fog density scales by exp(-(worldY - baseHeight) * falloff), so fog pools
///   below @p base_height and thins above it. It combines with distance fog via
///   combined transmittance (1 - (1-df)(1-hf)), weighted by @p blend, and shares the
///   distance fog's color (call SetFog first to pick the color; height fog alone
///   defaults to the current fog color).
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param base_height World-space height at which the exponential density reference is evaluated.
/// @param falloff Non-negative exponential thinning rate per world-space height unit.
/// @param density Non-negative base density.
/// @param blend Contribution weight clamped to the inclusive normalized range.
void rt_canvas3d_set_height_fog(
    void *obj, double base_height, double falloff, double density, double blend) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->height_fog_enabled = 1;
    c->height_fog_base = canvas3d_sanitize_f64_to_float(base_height, 0.0f);
    c->height_fog_falloff = canvas3d_sanitize_nonnegative_f64(falloff, 0.1f);
    c->height_fog_density = canvas3d_sanitize_nonnegative_f64(density, 0.02f);
    float b = (float)(isfinite(blend) ? blend : 1.0);
    if (b < 0.0f)
        b = 0.0f;
    if (b > 1.0f)
        b = 1.0f;
    c->height_fog_blend = b;
}

/// @brief Configure height-fog sun inscattering: fog tints toward @p r,@p g,@p b by
///   @p amount * pow(max(dot(viewDir, sunDir), 0), @p power). The sun direction is
///   resolved per frame from the first enabled directional light; amount 0 (the
///   default) disables the term so legacy height fog renders unchanged.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param r Non-negative red channel for the sun-scattering tint.
/// @param g Non-negative green channel for the sun-scattering tint.
/// @param b Non-negative blue channel for the sun-scattering tint.
/// @param power Non-negative angular exponent controlling the forward-scattering lobe.
/// @param amount Tint contribution clamped to the inclusive normalized range.
void rt_canvas3d_set_height_fog_sun(
    void *obj, double r, double g, double b, double power, double amount) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->height_fog_sun_color[0] = canvas3d_sanitize_nonnegative_f64(r, 1.0f);
    c->height_fog_sun_color[1] = canvas3d_sanitize_nonnegative_f64(g, 1.0f);
    c->height_fog_sun_color[2] = canvas3d_sanitize_nonnegative_f64(b, 1.0f);
    c->height_fog_sun_power = canvas3d_sanitize_nonnegative_f64(power, 8.0f);
    float a = (float)(isfinite(amount) ? amount : 0.0);
    if (a < 0.0f)
        a = 0.0f;
    if (a > 1.0f)
        a = 1.0f;
    c->height_fog_sun_amount = a;
}

/// @brief Disable height fog only (distance fog, if set, remains active).
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_clear_height_fog(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->height_fog_enabled = 0;
}

/// @brief True while exponential height fog is enabled on this canvas.
/// @param obj Canvas3D handle or approved stack fixture.
/// @return 1 when height fog is enabled on a valid canvas; otherwise 0.
int8_t rt_canvas3d_get_height_fog_enabled(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->height_fog_enabled ? 1 : 0;
}

/// @brief Disable distance AND height fog on the canvas.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_clear_fog(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->fog_enabled = 0;
    c->height_fog_enabled = 0;
}

/*==========================================================================
 * Shadow Mapping
 *=========================================================================*/

/// @brief Enable shadow mapping with the given shadow map resolution.
/// @details Creates a shadow depth buffer and configures directional and spot
///          light shadow casting. The shadow map is rendered from the light's
///          perspective and sampled during the main render pass.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param resolution Requested square-map dimension, clamped from 64 through 4096 pixels.
/// Allocation failure leaves shadows disabled.
void rt_canvas3d_enable_shadows(void *obj, int64_t resolution) {
    int ok;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (resolution < 64)
        resolution = 64;
    if (resolution > 4096)
        resolution = 4096;
    int32_t res = (int32_t)resolution;
    c->shadows_enabled = 1;
    c->shadow_resolution = res;
    ok = canvas3d_ensure_shadow_targets(c, res);
    if (!ok)
        c->shadows_enabled = 0;
}

/// @brief Disable shadow mapping and free the shadow depth buffer.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
void rt_canvas3d_disable_shadows(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->shadows_enabled = 0;
    canvas3d_release_shadow_targets(c);
}

/// @brief Set the shadow map depth bias to reduce shadow acne artifacts.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param bias Sampling-comparison bias clamped to the supported finite non-negative range;
/// non-finite input restores the default.
void rt_canvas3d_set_shadow_bias(void *obj, double bias) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->shadow_bias =
        canvas3d_clamp_f64_to_float(bias, 0.0, CANVAS3D_MATERIAL_DEPTH_BIAS_ABS_MAX, 0.005f);
}

/// @brief `Canvas3D.ShadowBias` — read the retained shadow sampling bias (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained bias, or zero for an invalid handle.
double rt_canvas3d_get_shadow_bias(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (double)c->shadow_bias : 0.0;
}

/// @brief Set the slope-scaled shadow-map rasterization bias for all casters.
/// @details This complements `SetShadowBias`, which offsets the sampling comparison, by asking GPU
///   backends to bias depth during the shadow-map draw itself. Raising it reduces acne on grazing
///   angles and helps stabilize flickering coplanar caster triangles; values that are too high can
///   detach shadows from their casters. The value is clamped to a finite non-negative range.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param bias Requested slope-scaled rasterization bias; non-finite input restores the default.
void rt_canvas3d_set_shadow_slope_bias(void *obj, double bias) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->shadow_slope_bias =
        canvas3d_clamp_f64_to_float(bias, 0.0, CANVAS3D_MATERIAL_SLOPE_DEPTH_BIAS_ABS_MAX, 1.0f);
}

/// @brief `Canvas3D.ShadowSlopeBias` — read the retained slope-scaled bias (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained slope bias, or zero for an invalid handle.
double rt_canvas3d_get_shadow_slope_bias(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (double)c->shadow_slope_bias : 0.0;
}

/// @brief Set how dark fully-occluded texels get (0 = shadows disabled, 1 = fully black).
/// @details Replaces the previously hard-coded 0.15 lit floor; the default 0.85
///   reproduces the legacy look. Values are clamped to [0, 1].
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param strength Requested occlusion darkness; non-finite input restores 0.85.
void rt_canvas3d_set_shadow_strength(void *obj, double strength) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!isfinite(strength))
        strength = 0.85;
    if (strength < 0.0)
        strength = 0.0;
    if (strength > 1.0)
        strength = 1.0;
    c->shadow_strength = (float)strength;
}

/// @brief `Canvas3D.ShadowStrength` — read the retained occlusion darkness (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained strength in [0, 1], or zero for an invalid handle.
double rt_canvas3d_get_shadow_strength(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (double)c->shadow_strength : 0.0;
}

/// @brief Select the shadow PCF filtering tier (0 = 4 taps, 1 = 8, 2 = 16 Poisson taps).
/// @details Higher tiers produce smoother penumbrae at higher per-pixel cost. Values
///   outside [0, 2] clamp. `Canvas3D.SetQuality` also drives this (PERFORMANCE=0,
///   BALANCED=1, CINEMATIC=2); an explicit call overrides the profile until the next
///   SetQuality.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param quality Requested filtering tier, clamped from zero through two.
void rt_canvas3d_set_shadow_quality(void *obj, int64_t quality) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (quality < 0)
        quality = 0;
    if (quality > 2)
        quality = 2;
    c->shadow_quality = (int32_t)quality;
}

/// @brief `Canvas3D.ShadowQuality` — read the retained PCF filtering tier (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained tier 0-2, or zero for an invalid handle.
int64_t rt_canvas3d_get_shadow_quality(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (int64_t)c->shadow_quality : 0;
}

/// @brief Configure cascaded shadow-map count, preserving current single-map fallback.
/// @details Counts above one require the active backend's `shadow-csm` capability. An unsupported
/// request traps without changing the existing cascade count.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param count Requested cascade count, clamped from one through `VGFX3D_CSM_SLOTS`.
void rt_canvas3d_set_shadow_cascades(void *obj, int64_t count) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (count < 1)
        count = 1;
    if (count > VGFX3D_CSM_SLOTS)
        count = VGFX3D_CSM_SLOTS;
    if (count > 1) {
        rt_string capability = rt_const_cstr("shadow-csm");
        int8_t supported = rt_canvas3d_backend_supports(c, capability);
        rt_string_unref(capability);
        if (!supported) {
            rt_trap(
                "Canvas3D.SetShadowCascades: cascaded shadows are not supported by this backend");
            return;
        }
    }
    c->shadow_cascade_count = (int32_t)count;
}

/// @brief `Canvas3D.ShadowCascades` — read the retained cascade count (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Retained cascade count (>= 1), or zero for an invalid handle.
int64_t rt_canvas3d_get_shadow_cascades(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? (int64_t)c->shadow_cascade_count : 0;
}

/// @brief Default auto shadow-coverage distance in world units (see shadow_distance).
#define CANVAS3D_SHADOW_DISTANCE_AUTO_MAX 300.0f

/// @brief Set the maximum camera distance covered by directional shadows.
/// @details Values <= 0 (and non-finite values) restore the automatic default of
///   min(camera far, 300). Explicit values are clamped to the camera far plane at use
///   time, so a distance larger than the clip range never degrades cascade fitting.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param distance Requested coverage in world units. Non-positive and non-finite values select
/// automatic coverage; very large values are capped before storage.
void rt_canvas3d_set_shadow_distance(void *obj, double distance) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!isfinite(distance) || distance <= 0.0) {
        c->shadow_distance = 0.0f;
        return;
    }
    if (distance > 1.0e9)
        distance = 1.0e9;
    c->shadow_distance = (float)distance;
}

/// @brief Read back the configured shadow distance (0 = automatic).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Stored positive distance in world units, or zero for automatic, invalid, or corrupt
/// state.
double rt_canvas3d_get_shadow_distance(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !isfinite(c->shadow_distance) || c->shadow_distance < 0.0f)
        return 0.0;
    return (double)c->shadow_distance;
}

/// @brief Enable or disable vertical-sync presentation pacing.
/// @details Every backend defaults to vsync on. GPU paths delegate pacing exclusively to the
///   backend's native display-sync mechanism; software presentation uses the vgfx window cap.
///   Disabling removes both forms of pacing for lowest latency (with possible tearing). The
///   requested state is retained even when a backend has no native control so the getter reflects
///   intent and the software limiter still follows it.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to request synchronized presentation; zero to disable presentation
/// pacing.
void rt_canvas3d_set_vsync(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->vsync_enabled = enabled ? 1 : 0;
    if (c->backend && c->backend->set_vsync && c->backend_ctx)
        c->backend->set_vsync(c->backend_ctx, c->vsync_enabled);
    canvas3d_apply_window_pacing(c);
}

/// @brief Requested vsync state (defaults to on).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Stored requested state for a valid canvas; 1 for invalid input to preserve the default.
int8_t rt_canvas3d_get_vsync(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->vsync_enabled : 1;
}

/// @brief Request capture of every presented frame for post-present readback.
/// @details On GPU backends whose on-screen present path hands the swapchain image to the
///   display (contents undefined afterwards — the Metal direct-present route), enabling this
///   blits the frame into a readable texture BEFORE presentation, so CaptureFinalFrame /
///   screenshot readback taken after Present() returns the shown image at the cost of one
///   full-screen copy per frame. Backends whose readback is already present-safe (software,
///   offscreen/composited routes) retain the request but need no copy. Default off.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to capture every presented frame.
void rt_canvas3d_set_capture_after_present(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->capture_after_present = enabled ? 1 : 0;
    if (c->backend && c->backend->set_capture_after_present && c->backend_ctx)
        c->backend->set_capture_after_present(c->backend_ctx, c->capture_after_present);
}

/// @brief Requested capture-after-present state (defaults to off).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Stored requested state, or 0 for invalid input.
int8_t rt_canvas3d_get_capture_after_present(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->capture_after_present : 0;
}

/// @brief Attempt to set the scene render scale without trapping.
/// @details Scale is clamped to [0.25, 1]. Values greater than or equal to one
///   request native-resolution rendering, including on fixed-scale backends. Reduced
///   scales require backend support (`BackendSupports("render-scale")`). A capable
///   backend may reject any scale transition while a frame or presentation transaction
///   is active; in that case both the Canvas3D property and backend resources retain
///   their previous scale. Successful reduced-scale rendering happens at scale times
///   the logical output dimensions and is upscaled for presentation.
/// @param obj Canvas3D receiver, or `NULL`.
/// @param scale Requested scene scale. Non-finite and values at least one request 1:1;
///   finite values below 0.25 are clamped to 0.25.
/// @return 1 when the requested scale is active; 0 for a null receiver, an unsupported
///   reduced scale, or a backend transaction/allocation failure.
int8_t rt_canvas3d_try_set_render_scale(void *obj, double scale) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    float clamped;
    if (!c)
        return 0;
    if (!isfinite(scale) || scale >= 1.0)
        clamped = 1.0f;
    else if (scale < 0.25)
        clamped = 0.25f;
    else
        clamped = (float)scale;
    if (clamped >= 0.999f) {
        if (c->backend && c->backend->set_render_scale && c->backend_ctx &&
            !c->backend->set_render_scale(c->backend_ctx, 1.0f))
            return 0;
        c->render_scale = 1.0f;
        return 1;
    }
    if (!c->backend || !c->backend->set_render_scale || !c->backend_ctx)
        return 0;
    if (!c->backend->set_render_scale(c->backend_ctx, clamped))
        return 0;
    c->render_scale = clamped;
    return 1;
}

/// @brief Currently requested render scale (1 = native resolution).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Stored finite scale in the inclusive supported range; 1.0 for invalid or corrupt state.
double rt_canvas3d_get_render_scale(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !isfinite(c->render_scale) || c->render_scale <= 0.0f || c->render_scale > 1.0f)
        return 1.0;
    return (double)c->render_scale;
}

/// @brief Refresh the shadow-caster sweep vector from the canvas + scene light slots.
/// @details Finds the first enabled shadow-casting directional light (mirroring the
///   shadow pass's primary-light preference) and stores its normalized travel direction
///   scaled by the effective shadow distance. Scene culling extends node bounds by this
///   vector so off-screen casters keep contributing shadows instead of popping at the
///   frustum edge.
/// @param c Canvas whose retained and scene light lists are searched and whose sweep state is
/// replaced. NULL is ignored.
void canvas3d_update_shadow_caster_sweep(rt_canvas3d *c) {
    const rt_light3d *dir_light = NULL;
    double dx;
    double dy;
    double dz;
    double len;
    float reach;

    if (!c)
        return;
    c->shadow_caster_sweep_active = 0;
    c->shadow_caster_sweep[0] = 0.0f;
    c->shadow_caster_sweep[1] = 0.0f;
    c->shadow_caster_sweep[2] = 0.0f;
    if (!c->shadows_enabled)
        return;
    for (int i = 0; i < VGFX3D_MAX_LIGHTS && !dir_light; i++) {
        const rt_light3d *l = c->lights[i];
        if (l && l->enabled && l->casts_shadows && l->type == 0)
            dir_light = l;
    }
    for (int i = 0; i < c->scene_light_count && i < VGFX3D_MAX_LIGHTS && !dir_light; i++) {
        const rt_light3d *l = c->scene_lights[i];
        if (l && l->enabled && l->casts_shadows && l->type == 0)
            dir_light = l;
    }
    if (!dir_light)
        return;
    dx = dir_light->direction[0];
    dy = dir_light->direction[1];
    dz = dir_light->direction[2];
    len = sqrt(dx * dx + dy * dy + dz * dz);
    if (!isfinite(len) || len < 1e-7) {
        dx = 0.0;
        dy = -1.0;
        dz = 0.0;
        len = 1.0;
    }
    reach = canvas3d_effective_shadow_distance(c);
    c->shadow_caster_sweep[0] = (float)(dx / len) * reach;
    c->shadow_caster_sweep[1] = (float)(dy / len) * reach;
    c->shadow_caster_sweep[2] = (float)(dz / len) * reach;
    c->shadow_caster_sweep_active = 1;
}

/// @brief Effective directional-shadow coverage distance for this canvas.
/// @details Uses the explicit positive setting when available, otherwise the automatic maximum,
/// then clamps coverage to the cached camera far plane and a one-world-unit minimum.
/// @param c Canvas providing cached camera and shadow settings; NULL uses defaults.
/// @return Finite coverage distance in world units.
float canvas3d_effective_shadow_distance(const rt_canvas3d *c) {
    float far_plane = 1000.0f;
    float distance;

    if (c && isfinite(c->cached_cam_far) && c->cached_cam_far > 0.0f)
        far_plane = c->cached_cam_far;
    if (c && isfinite(c->shadow_distance) && c->shadow_distance > 0.0f)
        distance = c->shadow_distance;
    else
        distance = CANVAS3D_SHADOW_DISTANCE_AUTO_MAX;
    if (distance > far_plane)
        distance = far_plane;
    if (distance < 1.0f)
        distance = 1.0f;
    return distance;
}

/// @brief Enable or disable coarse CPU frustum culling for draw submission.
/// @details Independent of occlusion culling: the CPU occlusion grid derives its own
///   projected coverage from the cached view-projection, so disabling frustum culling
///   no longer force-disables occlusion culling.
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to reject draws outside the cached view frustum.
void rt_canvas3d_set_frustum_culling(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    enabled = enabled ? 1 : 0;
    if (c->frustum_culling != enabled)
        canvas3d_clear_occlusion_history(c);
    c->frustum_culling = enabled;
}

/// @brief `Canvas3D.FrustumCulling` — read the retained frustum-cull flag (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Nonzero while frustum culling is enabled.
int8_t rt_canvas3d_get_frustum_culling(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->frustum_culling ? 1 : 0;
}

/// @brief Enable or disable coarse CPU occlusion culling for draw submission.
/// @details Independent of frustum culling: toggling occlusion no longer overwrites the
///   frustum-culling flag (frustum culling defaults on; games may configure each
///   independently).
/// @param obj Canvas3D handle or approved stack fixture; invalid handles are ignored.
/// @param enabled Non-zero to enable the coarse CPU occlusion-history path.
void rt_canvas3d_set_occlusion_culling(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    enabled = enabled ? 1 : 0;
    if (c->occlusion_culling != enabled)
        canvas3d_clear_occlusion_history(c);
    c->occlusion_culling = enabled;
}

/// @brief `Canvas3D.OcclusionCulling` — read the retained occlusion-cull flag (ADR 0233).
/// @param obj Canvas3D handle or approved stack fixture.
/// @return Nonzero while CPU occlusion culling is enabled.
int8_t rt_canvas3d_get_occlusion_culling(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->occlusion_culling ? 1 : 0;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
