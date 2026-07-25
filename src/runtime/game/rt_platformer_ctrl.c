//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_platformer_ctrl.c
/// @file
/// @brief Implements a stateful platformer input-to-velocity controller.
//
// Purpose: Platformer movement controller implementing standard mechanics:
//   jump buffering (grace window before landing), coyote time (grace window
//   after leaving ledge), variable jump height (hold vs release), and separate
//   ground/air acceleration curves. All timing is ms-based for dt-independence.
//
// Key invariants:
//   - Velocities are in x100 (centipixel) units for sub-pixel precision.
//   - Jump buffer and coyote timers count DOWN in ms.
//   - ShouldJump is set when jump_buffer > 0 AND (on_ground OR coyote > 0).
//   - Gravity applies apex hang bonus when |vy| < threshold.
//
// Ownership/Lifetime:
//   - Runtime reference-counted object allocated by rt_obj_new_i64().
//
// Links: src/runtime/game/rt_platformer_ctrl.h
//
//===----------------------------------------------------------------------===//

#include "rt_platformer_ctrl.h"
#include "rt_object.h"
#include <stdlib.h>

/// @brief Private tuning, timer, input-history, and velocity state.
struct rt_platformer_ctrl_impl {
    // Output velocity (x100 centipixels)
    int64_t vx;
    int64_t vy;
    int64_t facing; // 1=right, -1=left

    // Jump buffer + coyote
    int64_t jump_buffer_ms;    // config: buffer window
    int64_t coyote_ms;         // config: coyote window
    int64_t jump_buffer_timer; // current buffer countdown
    int64_t coyote_timer;      // current coyote countdown
    int8_t should_jump;        // one-shot flag
    int8_t was_on_ground;      // previous frame ground state

    // Acceleration config (x100 units per 16ms frame)
    int64_t accel_ground;
    int64_t accel_air;
    int64_t decel;

    // Jump config (x100 units, negative = up)
    int64_t jump_force_full;
    int64_t jump_force_cut; // Force applied when jump released early

    // Speed caps (x100 units)
    int64_t max_speed;
    int64_t max_speed_sprint;

    // Gravity config
    int64_t gravity;
    int64_t max_fall;
    int64_t apex_threshold;   // |vy| below this gets reduced gravity
    int64_t apex_gravity_pct; // Gravity multiplier at apex (percentage, e.g. 60)

    // State
    int8_t jump_held_prev;
};

/// @brief Construct a platformer movement controller pre-tuned for snappy "Celeste-style" feel.
/// Defaults: 100ms jump buffer, 80ms coyote, ground accel 80, air accel 40, jump force -1500 full
/// / -600 cut, max speed 525 (788 sprint), gravity 78 with 60% apex bonus when |vy| < 200.
/// All velocities are in centipixels (×100) per 16ms baseline frame; multiply/divide by `dt/16` to
/// scale. Returns a GC-managed handle; NULL on allocation failure.
/// @return A new runtime-managed controller reference, or `NULL` on allocation
///         failure.
rt_platformer_ctrl rt_platformer_ctrl_new(void) {
    struct rt_platformer_ctrl_impl *ctrl = (struct rt_platformer_ctrl_impl *)rt_obj_new_i64(
        0, (int64_t)sizeof(struct rt_platformer_ctrl_impl));
    if (!ctrl)
        return NULL;

    ctrl->vx = 0;
    ctrl->vy = 0;
    ctrl->facing = 1;
    ctrl->jump_buffer_ms = 100;
    ctrl->coyote_ms = 80;
    ctrl->jump_buffer_timer = 0;
    ctrl->coyote_timer = 0;
    ctrl->should_jump = 0;
    ctrl->was_on_ground = 0;
    ctrl->accel_ground = 80;
    ctrl->accel_air = 40;
    ctrl->decel = 60;
    ctrl->jump_force_full = -1500;
    ctrl->jump_force_cut = -600;
    ctrl->max_speed = 525;
    ctrl->max_speed_sprint = 788;
    ctrl->gravity = 78;
    ctrl->max_fall = 1200;
    ctrl->apex_threshold = 200;
    ctrl->apex_gravity_pct = 60;
    ctrl->jump_held_prev = 0;

    return ctrl;
}

/// @brief Release one runtime reference to the controller.
/// @param ctrl Controller to release; `NULL` is a no-op.
/// @details The structure is freed when the reference count reaches zero.
void rt_platformer_ctrl_destroy(rt_platformer_ctrl ctrl) {
    if (ctrl && rt_obj_release_check0(ctrl))
        rt_obj_free(ctrl);
}

// =============================================================================
// Configuration setters
// Each tuning knob is a direct write — no validation. Negative or zero values
// disable the corresponding behavior (e.g., jump_buffer_ms=0 → no buffering).
// =============================================================================

/// @brief Set the jump-buffer window: pressing jump up to this many ms BEFORE landing still
/// triggers a jump on touchdown. Default 100ms; set 0 to disable buffering.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param ms New unvalidated timer duration in milliseconds.
void rt_platformer_ctrl_set_jump_buffer(rt_platformer_ctrl ctrl, int64_t ms) {
    if (ctrl)
        ctrl->jump_buffer_ms = ms;
}

/// @brief Set the coyote-time window: jumps remain valid up to this many ms AFTER walking off a
/// ledge. Default 80ms; set 0 to require strictly grounded jumps.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param ms New unvalidated timer duration in milliseconds.
void rt_platformer_ctrl_set_coyote_time(rt_platformer_ctrl ctrl, int64_t ms) {
    if (ctrl)
        ctrl->coyote_ms = ms;
}

/// @brief Configure horizontal acceleration: separate values for grounded vs airborne, plus
/// deceleration applied when no input is held. All in centipixels per 16ms baseline frame.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param ground Grounded acceleration.
/// @param air Airborne acceleration.
/// @param decel Neutral-input deceleration.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_acceleration(rt_platformer_ctrl ctrl,
                                         int64_t ground,
                                         int64_t air,
                                         int64_t decel) {
    if (!ctrl)
        return;
    ctrl->accel_ground = ground;
    ctrl->accel_air = air;
    ctrl->decel = decel;
}

/// @brief Configure vertical jump impulse (negative = upward). `full_force` is applied on takeoff;
/// `cut_force` is the velocity cap applied when the jump button is released early (variable jump
/// height). Releasing while `vy < cut_force` snaps `vy` up to `cut_force` for a shorter hop.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param full_force Full jump impulse returned by get_jump_force().
/// @param cut_force Upward-velocity cutoff applied on jump-release edges.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_jump_force(rt_platformer_ctrl ctrl,
                                       int64_t full_force,
                                       int64_t cut_force) {
    if (!ctrl)
        return;
    ctrl->jump_force_full = full_force;
    ctrl->jump_force_cut = cut_force;
}

/// @brief Set the horizontal speed caps for normal walking vs sprinting (centipixels per 16ms).
/// `update()` chooses between them via the `sprint` input flag each frame.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param normal Nonsprinting cap.
/// @param sprint Sprinting cap.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_max_speed(rt_platformer_ctrl ctrl, int64_t normal, int64_t sprint) {
    if (!ctrl)
        return;
    ctrl->max_speed = normal;
    ctrl->max_speed_sprint = sprint;
}

/// @brief Configure gravity acceleration and terminal downward velocity.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param gravity Velocity delta per 16ms baseline frame.
/// @param max_fall Maximum downward velocity.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_gravity(rt_platformer_ctrl ctrl, int64_t gravity, int64_t max_fall) {
    if (!ctrl)
        return;
    ctrl->gravity = gravity;
    ctrl->max_fall = max_fall;
}

/// @brief Tune the apex-hang bonus: when |vy| < `threshold`, gravity is scaled by
/// `gravity_mult_pct` percent (e.g. 60 = 60%). Lets the player float briefly at jump apex,
/// extending air-time near the top of the arc — a hallmark of forgiving platformer feel.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param threshold Absolute vertical-velocity threshold.
/// @param gravity_mult_pct Percentage applied to gravity inside the threshold.
/// @details Values are stored without validation.
void rt_platformer_ctrl_set_apex_bonus(rt_platformer_ctrl ctrl,
                                       int64_t threshold,
                                       int64_t gravity_mult_pct) {
    if (!ctrl)
        return;
    ctrl->apex_threshold = threshold;
    ctrl->apex_gravity_pct = gravity_mult_pct;
}

/// @brief Per-frame controller tick. Consumes input flags + ground state, updates internal
/// velocity. Stages, in order:
///   1. **Coyote refresh:** while on_ground, hold timer at full; on the falling-from-ledge
///   transition
///      (was_on_ground && vy>=0 && !on_ground) start the countdown.
///   2. **Jump-buffer arming:** any `jump_pressed` edge resets the buffer; both timers tick down by
///   `dt`.
///   3. **Jump resolution:** if the buffer is active AND (grounded OR coyote active), set
///      `should_jump=1` and consume both timers (caller reads via `should_jump()`).
///   4. **Variable jump cut:** on the falling edge of `jump_held`, if `vy < jump_force_cut`, snap
///      `vy` up to `jump_force_cut` for a shorter hop.
///   5. **Horizontal:** apply ground vs air acceleration in input direction (clamped to walk/sprint
///      cap); decelerate when neutral.
///   6. **Gravity:** apply per-frame gravity scaled by `dt/16`, with apex-hang reduction when
///      |vy| < apex_threshold; cap at `max_fall`.
/// All scaling uses integer math: `value * dt / 16`. `dt <= 0` early-outs.
/// @param ctrl Controller to update; `NULL` is a no-op.
/// @param dt Elapsed milliseconds; nonpositive values do nothing.
/// @param input_left Nonzero when left input is held.
/// @param input_right Nonzero when right input is held; wins if both directions
///        are nonzero.
/// @param jump_pressed Nonzero on a jump-press edge.
/// @param jump_held Nonzero while jump remains held.
/// @param on_ground Nonzero while grounded.
/// @param sprint Nonzero to select the sprint speed cap.
/// @details The controller updates velocity only; the caller integrates
///          position and applies the returned full jump impulse after consuming
///          should_jump().
void rt_platformer_ctrl_update(rt_platformer_ctrl ctrl,
                               int64_t dt,
                               int8_t input_left,
                               int8_t input_right,
                               int8_t jump_pressed,
                               int8_t jump_held,
                               int8_t on_ground,
                               int8_t sprint) {
    if (!ctrl || dt <= 0)
        return;

    int64_t dt_scale = dt; // ms since last frame
    int64_t dt_base = 16;  // baseline 60fps frame

    ctrl->should_jump = 0;

    // --- Coyote time ---
    if (on_ground) {
        ctrl->coyote_timer = ctrl->coyote_ms;
    } else {
        if (ctrl->was_on_ground && ctrl->vy >= 0) {
            // Just left ground (didn't jump) — start coyote window
            ctrl->coyote_timer = ctrl->coyote_ms;
        }
        if (ctrl->coyote_timer > 0)
            ctrl->coyote_timer -= dt_scale;
    }

    // --- Jump buffer ---
    if (jump_pressed)
        ctrl->jump_buffer_timer = ctrl->jump_buffer_ms;
    if (ctrl->jump_buffer_timer > 0)
        ctrl->jump_buffer_timer -= dt_scale;

    // --- Resolve jump ---
    if (ctrl->jump_buffer_timer > 0 && (on_ground || ctrl->coyote_timer > 0)) {
        ctrl->should_jump = 1;
        ctrl->jump_buffer_timer = 0;
        ctrl->coyote_timer = 0;
    }

    // --- Variable jump height (cut jump on release) ---
    if (!jump_held && ctrl->jump_held_prev && ctrl->vy < ctrl->jump_force_cut) {
        ctrl->vy = ctrl->jump_force_cut;
    }
    ctrl->jump_held_prev = jump_held;

    // --- Horizontal acceleration ---
    int64_t move_dir = 0;
    if (input_left)
        move_dir = -1;
    if (input_right)
        move_dir = 1;

    if (move_dir != 0) {
        ctrl->facing = move_dir;
        int64_t accel = on_ground ? ctrl->accel_ground : ctrl->accel_air;
        ctrl->vx += move_dir * accel * dt_scale / dt_base;

        int64_t cap = sprint ? ctrl->max_speed_sprint : ctrl->max_speed;
        if (ctrl->vx > cap)
            ctrl->vx = cap;
        if (ctrl->vx < -cap)
            ctrl->vx = -cap;
    } else {
        // Decelerate
        int64_t dec = ctrl->decel * dt_scale / dt_base;
        if (ctrl->vx > 0) {
            ctrl->vx -= dec;
            if (ctrl->vx < 0)
                ctrl->vx = 0;
        } else if (ctrl->vx < 0) {
            ctrl->vx += dec;
            if (ctrl->vx > 0)
                ctrl->vx = 0;
        }
    }

    // --- Gravity with apex hang ---
    int64_t g = ctrl->gravity * dt_scale / dt_base;
    int64_t abs_vy = ctrl->vy;
    if (abs_vy < 0)
        abs_vy = -abs_vy;
    if (abs_vy < ctrl->apex_threshold) {
        g = g * ctrl->apex_gravity_pct / 100;
    }
    ctrl->vy += g;
    if (ctrl->vy > ctrl->max_fall)
        ctrl->vy = ctrl->max_fall;

    ctrl->was_on_ground = on_ground;
}

// =============================================================================
// Output queries
// Read-only accessors plus a one-shot consume for `should_jump`. The expected
// caller pattern each frame: update() → read vx/vy → check should_jump() → apply
// jump_force if needed → integrate position externally.
// =============================================================================

/// @brief Read horizontal velocity (centipixels per 16ms baseline frame). Divide by 100 for
/// px/frame.
/// @param ctrl Controller to query.
/// @return Current horizontal velocity, or zero for `NULL`.
int64_t rt_platformer_ctrl_get_vx(rt_platformer_ctrl ctrl) {
    return ctrl ? ctrl->vx : 0;
}

/// @brief Read vertical velocity (centipixels per 16ms baseline frame). Negative = upward.
/// @param ctrl Controller to query.
/// @return Current vertical velocity, or zero for `NULL`.
int64_t rt_platformer_ctrl_get_vy(rt_platformer_ctrl ctrl) {
    return ctrl ? ctrl->vy : 0;
}

/// @brief One-shot read of the "jump now" signal. Returns 1 once after a successful jump-resolve in
/// `update()` (buffer + (ground|coyote)), and **clears the flag on read** so the caller doesn't
/// double-apply jump force on subsequent queries within the same frame.
/// @param ctrl Controller whose signal is consumed.
/// @return Pending jump signal, or zero for `NULL`.
int8_t rt_platformer_ctrl_should_jump(rt_platformer_ctrl ctrl) {
    if (!ctrl)
        return 0;
    int8_t result = ctrl->should_jump;
    ctrl->should_jump = 0; // One-shot: consumed on read
    return result;
}

/// @brief Read the configured full-jump impulse (negative = upward). Caller assigns this to `vy`
/// when `should_jump()` is true.
/// @param ctrl Controller to query.
/// @return Configured full jump force, or zero for `NULL`.
int64_t rt_platformer_ctrl_get_jump_force(rt_platformer_ctrl ctrl) {
    return ctrl ? ctrl->jump_force_full : 0;
}

/// @brief Read the player's facing direction (+1 = right, -1 = left). Updated only when the player
/// actively inputs movement; remains sticky during slides/decel.
/// @param ctrl Controller to query.
/// @return Sticky facing direction, or `1` for `NULL`.
int64_t rt_platformer_ctrl_get_facing(rt_platformer_ctrl ctrl) {
    return ctrl ? ctrl->facing : 1;
}

/// @brief Test whether horizontal velocity is nonzero.
/// @param ctrl Controller to query.
/// @return `1` when `vx != 0`, otherwise `0`; null returns zero.
int8_t rt_platformer_ctrl_is_moving(rt_platformer_ctrl ctrl) {
    return ctrl ? (ctrl->vx != 0 ? 1 : 0) : 0;
}

/// @brief Force-set horizontal velocity (e.g. to apply an external knockback or wall-bounce).
/// Subsequent `update()` calls will continue from this value (caps still apply).
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param vx New unvalidated horizontal velocity.
void rt_platformer_ctrl_set_vx(rt_platformer_ctrl ctrl, int64_t vx) {
    if (ctrl)
        ctrl->vx = vx;
}

/// @brief Force-set vertical velocity (typical use: caller writes `jump_force` here when
/// `should_jump()` returned 1). Negative = upward.
/// @param ctrl Controller to modify; `NULL` is a no-op.
/// @param vy New unvalidated vertical velocity.
void rt_platformer_ctrl_set_vy(rt_platformer_ctrl ctrl, int64_t vy) {
    if (ctrl)
        ctrl->vy = vy;
}
