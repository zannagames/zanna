//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_input.c
// Purpose: Input3D for the Zanna.Game3D layer — per-frame keyboard/mouse query
//   with optional latched snapshot for deterministic replay. Split out of
//   rt_game3d.c; shares private types/helpers via rt_game3d_internal.h.
// Links: rt_game3d_internal.h, rt_input.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements coherent keyboard, mouse, and gamepad snapshots for Game3D.
/// @details Input3D can query shared device state before its first update, then
///          latches all key/button edges, pointer deltas, wheel motion, and
///          bound-pad stick axes for deterministic per-frame reuse. Higher-level
///          movement and look helpers apply radial deadzones, response curves,
///          normalization, and configurable sensitivity without mutating the
///          captured snapshot.

#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_input.h"
#include "rt_object.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

/// @brief Clear all cached gamepad state while preserving the selected sensitivity.
/// @param input Input3D payload to reset; NULL is ignored.
static void game3d_input_clear_pad_snapshot(rt_game3d_input *input) {
    if (!input)
        return;
    input->pad_connected = 0;
    input->pad_lx = 0.0;
    input->pad_ly = 0.0;
    input->pad_rx = 0.0;
    input->pad_ry = 0.0;
}

/// @brief Normalize one gamepad axis to the backend's documented range.
/// @param value Candidate axis value.
/// @return Finite value in `[-1, 1]`.
static double game3d_input_axis_value(double value) {
    return game3d_clamp(game3d_finite_or(value, 0.0), -1.0, 1.0);
}

/// @brief Repair scalar, flag, and gamepad snapshot invariants in an Input3D payload.
/// @param input Borrowed Input3D payload; NULL is ignored.
void game3d_input_repair_state(rt_game3d_input *input) {
    if (!input)
        return;
    input->look_sensitivity = game3d_nonnegative_clamped_or(input->look_sensitivity,
                                                            RT_GAME3D_DEFAULT_LOOK_SENSITIVITY,
                                                            RT_GAME3D_LOOK_SENSITIVITY_MAX);
    input->has_snapshot = input->has_snapshot ? 1 : 0;
    input->mouse_dx = game3d_clamp_mouse_delta_i64(input->mouse_dx);
    input->mouse_dy = game3d_clamp_mouse_delta_i64(input->mouse_dy);
    input->mouse_fdx = game3d_clamp_abs_or(input->mouse_fdx, 0.0, RT_GAME3D_COORD_ABS_MAX);
    input->mouse_fdy = game3d_clamp_abs_or(input->mouse_fdy, 0.0, RT_GAME3D_COORD_ABS_MAX);
    input->mouse_x = game3d_clamp_mouse_delta_i64(input->mouse_x);
    input->mouse_y = game3d_clamp_mouse_delta_i64(input->mouse_y);
    input->wheel_y = game3d_clamp_abs_or(input->wheel_y, 0.0, RT_GAME3D_COORD_ABS_MAX);
    input->pad_look_sensitivity =
        game3d_nonnegative_clamped_or(input->pad_look_sensitivity, 1.5, 20.0);
    if (input->bound_pad < 0 || input->bound_pad >= ZANNA_PAD_MAX) {
        input->bound_pad = -1;
        game3d_input_clear_pad_snapshot(input);
        return;
    }
    input->pad_connected = input->pad_connected ? 1 : 0;
    if (!input->pad_connected) {
        game3d_input_clear_pad_snapshot(input);
        return;
    }
    input->pad_lx = game3d_input_axis_value(input->pad_lx);
    input->pad_ly = game3d_input_axis_value(input->pad_ly);
    input->pad_rx = game3d_input_axis_value(input->pad_rx);
    input->pad_ry = game3d_input_axis_value(input->pad_ry);
}

/// @brief Allocate an Input3D handle with default look sensitivity; traps on OOM.
/// @return A newly allocated Input3D with no gamepad binding or snapshot, or
///         NULL after allocation failure.
void *rt_game3d_input_new(void) {
    rt_game3d_input *input =
        (rt_game3d_input *)rt_obj_new_i64(RT_G3D_GAME3D_INPUT_CLASS_ID, (int64_t)sizeof(*input));
    if (!input) {
        rt_trap("Game3D.Input3D.New: allocation failed");
        return NULL;
    }
    memset(input, 0, sizeof(*input));
    input->look_sensitivity = 0.01;
    input->bound_pad = -1;
    input->pad_look_sensitivity = 1.5;
    return input;
}

/// @brief Get the per-object look sensitivity (0 on invalid handle).
/// @param obj Input3D runtime handle.
/// @return The finite non-negative bounded mouse-look sensitivity, or zero when invalid.
double rt_game3d_input_get_look_sensitivity(void *obj) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.get_LookSensitivity: invalid input");
    game3d_input_repair_state(input);
    return input ? input->look_sensitivity : 0.0;
}

/// @brief Set the look sensitivity (negative/non-finite values reset to the default).
/// @param obj Input3D runtime handle.
/// @param sensitivity Requested mouse-delta scale; invalid/negative input selects
///                    0.01 and extreme values are capped.
void rt_game3d_input_set_look_sensitivity(void *obj, double sensitivity) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.set_LookSensitivity: invalid input");
    if (!input)
        return;
    input->look_sensitivity =
        game3d_nonnegative_clamped_or(sensitivity, 0.01, RT_GAME3D_LOOK_SENSITIVITY_MAX);
}

/// @brief Roll input edge state forward one frame; the shared device state is polled
///   by the canvas, then copied here so each Input3D object observes a coherent
///   per-frame snapshot even if later polling mutates the process-wide state.
/// @param obj Input3D runtime handle.
/// @post Snapshot-aware queries use the copied state until the next update.
void rt_game3d_input_update(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.update: invalid input");
    if (!input)
        return;
    game3d_input_repair_state(input);
    for (int64_t key = 0; key < ZANNA_KEY_MAX; key++) {
        input->key_down[key] = rt_keyboard_is_down(key) ? 1 : 0;
        input->key_pressed[key] = rt_keyboard_was_pressed(key) ? 1 : 0;
        input->key_released[key] = rt_keyboard_was_released(key) ? 1 : 0;
    }
    for (int64_t button = 0; button < ZANNA_MOUSE_BUTTON_MAX; button++) {
        input->mouse_down[button] = rt_mouse_is_down(button) ? 1 : 0;
        input->mouse_pressed[button] = rt_mouse_was_pressed(button) ? 1 : 0;
        input->mouse_released[button] = rt_mouse_was_released(button) ? 1 : 0;
    }
    input->mouse_dx = game3d_clamp_mouse_delta_i64(rt_mouse_delta_x());
    input->mouse_dy = game3d_clamp_mouse_delta_i64(rt_mouse_delta_y());
    input->mouse_fdx = game3d_clamp_abs_or(rt_mouse_delta_xf(), 0.0, RT_GAME3D_COORD_ABS_MAX);
    input->mouse_fdy = game3d_clamp_abs_or(rt_mouse_delta_yf(), 0.0, RT_GAME3D_COORD_ABS_MAX);
    input->mouse_x = game3d_clamp_mouse_delta_i64(rt_mouse_x());
    input->mouse_y = game3d_clamp_mouse_delta_i64(rt_mouse_y());
    input->wheel_y = game3d_clamp_abs_or(rt_mouse_wheel_yf(), 0.0, RT_GAME3D_COORD_ABS_MAX);
    /* Snapshot bound-gamepad stick axes so Move/LookAxis observe a coherent
     * frame even if the pad is polled again mid-frame. */
    game3d_input_clear_pad_snapshot(input);
    if (input->bound_pad >= 0 && input->bound_pad < ZANNA_PAD_MAX &&
        rt_pad_is_connected(input->bound_pad)) {
        input->pad_connected = 1;
        input->pad_lx = game3d_input_axis_value(rt_pad_left_x(input->bound_pad));
        input->pad_ly = game3d_input_axis_value(rt_pad_left_y(input->bound_pad));
        input->pad_rx = game3d_input_axis_value(rt_pad_right_x(input->bound_pad));
        input->pad_ry = game3d_input_axis_value(rt_pad_right_y(input->bound_pad));
    }
    input->has_snapshot = 1;
}

/// @brief True while `key` is held in the current snapshot or live keyboard state.
/// @param obj Input3D runtime handle.
/// @param key Runtime keyboard code.
/// @return Non-zero while held, otherwise zero.
int8_t rt_game3d_input_is_down(void *obj, int64_t key) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.isDown: invalid input");
    return input ? game3d_input_key_down(input, key) : 0;
}

/// @brief True on the frame `key` transitions to down.
/// @param obj Input3D runtime handle.
/// @param key Runtime keyboard code.
/// @return Non-zero for a captured or live down edge, otherwise zero.
int8_t rt_game3d_input_pressed(void *obj, int64_t key) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.pressed: invalid input");
    return input ? game3d_input_key_pressed(input, key) : 0;
}

/// @brief True on the frame `key` transitions to up.
/// @param obj Input3D runtime handle.
/// @param key Runtime keyboard code.
/// @return Non-zero for a captured or live up edge, otherwise zero.
int8_t rt_game3d_input_released(void *obj, int64_t key) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.released: invalid input");
    return input ? game3d_input_key_released(input, key) : 0;
}

/// @brief Get this frame's raw mouse movement delta as a Vec2.
/// @details Sub-pixel precise while relative (raw) mouse mode is active.
/// @param obj Input3D runtime handle.
/// @return A newly allocated Vec2 containing snapshot-aware fractional X/Y deltas.
void *rt_game3d_input_mouse_delta(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.mouseDelta: invalid input");
    return rt_vec2_new(input ? game3d_input_mouse_fdx(input) : 0.0,
                       input ? game3d_input_mouse_fdy(input) : 0.0);
}

/// @brief Absolute window-local cursor X for this frame's snapshot (ADR 0233).
/// @param obj Input3D runtime handle.
/// @return Cursor X in pixels, or zero when invalid.
int64_t rt_game3d_input_get_mouse_x(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.get_MouseX: invalid input");
    return input ? game3d_input_mouse_x(input) : 0;
}

/// @brief Absolute window-local cursor Y for this frame's snapshot (ADR 0233).
/// @param obj Input3D runtime handle.
/// @return Cursor Y in pixels, or zero when invalid.
int64_t rt_game3d_input_get_mouse_y(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.get_MouseY: invalid input");
    return input ? game3d_input_mouse_y(input) : 0;
}

/// @brief Absolute window-local cursor position as a fresh Vec2 (ADR 0233).
/// @details Pairs with `Camera3D.ScreenToRay`/`ScreenToRayOrigin` for picking.
/// @param obj Input3D runtime handle.
/// @return A newly allocated Vec2 with the snapshot-aware cursor position.
void *rt_game3d_input_mouse_position(void *obj) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.mousePosition: invalid input");
    return rt_vec2_new(input ? (double)game3d_input_mouse_x(input) : 0.0,
                       input ? (double)game3d_input_mouse_y(input) : 0.0);
}

/// @brief True while mouse `button` is held this frame.
/// @param obj Input3D runtime handle.
/// @param button Runtime mouse-button code.
/// @return Non-zero while held in the current snapshot or live state.
int8_t rt_game3d_input_mouse_button(void *obj, int64_t button) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.mouseButton: invalid input");
    return input ? game3d_input_mouse_down(input, button) : 0;
}

/// @brief True on the frame mouse `button` transitions to down.
/// @param obj Input3D runtime handle.
/// @param button Runtime mouse-button code.
/// @return Non-zero for a captured or live down edge, otherwise zero.
int8_t rt_game3d_input_mouse_pressed(void *obj, int64_t button) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.mousePressed: invalid input");
    return input ? game3d_input_mouse_pressed_snapshot(input, button) : 0;
}

/// @brief Get this frame's mouse wheel scroll delta along Y.
/// @param obj Input3D runtime handle.
/// @return The snapshot-aware fractional vertical wheel displacement.
double rt_game3d_input_wheel_y(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.wheelY: invalid input");
    return input ? game3d_input_wheel_y_snapshot(input) : 0.0;
}

/// @brief Compute a normalized WASD/arrow move axis from the input state into x/z components.
/// @details Combines the held direction keys into a unit-ish 2D vector for character/camera
/// movement.
/// @param input Input3D snapshot; NULL falls back to live keyboard state and no pad.
/// @param[out] out_x Optional destination for strafe input.
/// @param[out] out_y Optional destination for vertical Space/modifier input.
/// @param[out] out_z Optional destination for forward/back input.
void game3d_input_move_axis_components(rt_game3d_input *input,
                                       double *out_x,
                                       double *out_y,
                                       double *out_z) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    game3d_input_repair_state(input);
    if (game3d_input_key_down(input, rt_keyboard_key_d()) ||
        game3d_input_key_down(input, rt_keyboard_key_right()))
        x += 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_a()) ||
        game3d_input_key_down(input, rt_keyboard_key_left()))
        x -= 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_w()) ||
        game3d_input_key_down(input, rt_keyboard_key_up()))
        z += 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_s()) ||
        game3d_input_key_down(input, rt_keyboard_key_down()))
        z -= 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_space()))
        y += 1.0;
    if (game3d_input_key_down(input, rt_keyboard_key_lshift()) ||
        game3d_input_key_down(input, rt_keyboard_key_rshift()) ||
        game3d_input_key_down(input, rt_keyboard_key_lctrl()) ||
        game3d_input_key_down(input, rt_keyboard_key_rctrl()))
        y -= 1.0;
    /* Merge the bound gamepad's left stick (radial deadzone, magnitude
     * preserving) — keyboard and stick sum, then normalize below. Stick +Y
     * is down on every pad backend, so it maps to backward (-z). */
    if (input && input->pad_connected) {
        double lx = input->pad_lx;
        double ly = input->pad_ly;
        double mag = hypot(lx, ly);
        const double deadzone = 0.18;
        if (mag > deadzone) {
            double scale = (mag - deadzone) / (1.0 - deadzone);
            if (scale > 1.0)
                scale = 1.0;
            scale /= mag;
            x += lx * scale;
            z -= ly * scale;
        }
    }
    game3d_normalize_axis3(&x, &y, &z);
    if (out_x)
        *out_x = x;
    if (out_y)
        *out_y = y;
    if (out_z)
        *out_z = z;
}

/// @brief Build the WASD/arrow/space/shift movement axis as a Vec3
///   (x = strafe, y = up/down, z = forward/back); see header.
/// @param obj Input3D runtime handle.
/// @return A newly allocated normalized Vec3 combining keyboard and bound left-stick input.
void *rt_game3d_input_move_axis(void *obj) {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.moveAxis: invalid input");
    if (input)
        game3d_input_move_axis_components(input, &x, &y, &z);
    return rt_vec3_new(x, y, z);
}

/// @brief Build the mouse-look axis as a Vec2 (mouse delta scaled by sensitivity).
/// @details Sub-pixel precise in relative mouse mode; merges the bound
///          gamepad's right stick (response curve x^1.8, per-frame contribution
///          scaled by the pad look sensitivity).
/// @param obj Input3D runtime handle.
/// @return A newly allocated bounded Vec2 combining scaled mouse and right-stick deltas.
void *rt_game3d_input_look_axis(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.lookAxis: invalid input");
    if (!input)
        return rt_vec2_new(0.0, 0.0);
    game3d_input_repair_state(input);
    double s = input->look_sensitivity;
    double dx = game3d_input_mouse_fdx(input) * s;
    double dy = game3d_input_mouse_fdy(input) * s;
    if (input && input->pad_connected) {
        double rx = input->pad_rx;
        double ry = input->pad_ry;
        double mag = hypot(rx, ry);
        const double deadzone = 0.18;
        if (mag > deadzone) {
            double scale = (mag - deadzone) / (1.0 - deadzone);
            if (scale > 1.0)
                scale = 1.0;
            /* Response curve: fine aim near center, fast sweep at the rim. */
            scale = pow(scale, 1.8) / mag;
            double ps = input->pad_look_sensitivity;
            dx += rx * scale * ps;
            dy += ry * scale * ps;
        }
    }
    double x = game3d_clamp_abs_or(dx, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    double y = game3d_clamp_abs_or(dy, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
    return rt_vec2_new(x, y);
}

/// @brief Capture and hide the OS cursor for relative mouse-look.
/// @param obj Input3D runtime handle, validated before changing global mouse state.
void rt_game3d_input_capture_mouse(void *obj) {
    if (!game3d_input_checked(obj, "Game3D.Input3D.captureMouse: invalid input"))
        return;
    rt_mouse_capture();
}

/// @brief Release the captured cursor back to the OS.
/// @param obj Input3D runtime handle, validated before changing global mouse state.
void rt_game3d_input_release_mouse(void *obj) {
    if (!game3d_input_checked(obj, "Game3D.Input3D.releaseMouse: invalid input"))
        return;
    rt_mouse_release();
}

/// @brief Enable/disable raw relative mouse-look (capture + OS raw deltas).
/// @details Convenience over Mouse.SetRelativeMode: enabling captures the
///          cursor and requests native raw motion; LookAxis/MouseDelta become
///          sub-pixel precise while retaining the runtime's finite coordinate
///          bound. Disabling releases the capture.
/// @param obj Input3D runtime handle, validated before changing global mouse state.
/// @param enabled Non-zero to enable relative mode, zero to disable it.
void rt_game3d_input_set_relative_look(void *obj, int8_t enabled) {
    if (!game3d_input_checked(obj, "Game3D.Input3D.setRelativeLook: invalid input"))
        return;
    rt_mouse_set_relative_mode(enabled ? 1 : 0);
}

/// @brief Bind a gamepad index into MoveAxis/LookAxis (-1 unbinds).
/// @param obj Input3D runtime handle.
/// @param pad Supported non-negative gamepad index, or exactly -1 to unbind and
///            clear the captured pad state immediately.
void rt_game3d_input_bind_pad(void *obj, int64_t pad) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.bindPad: invalid input");
    if (!input)
        return;
    if (pad < -1 || pad >= ZANNA_PAD_MAX) {
        rt_trap("Game3D.Input3D.bindPad: pad index must be -1 or a supported controller slot");
        return;
    }
    input->bound_pad = pad;
    game3d_input_clear_pad_snapshot(input);
}

/// @brief Currently bound gamepad index (-1 when unbound).
/// @param obj Input3D runtime handle.
/// @return The configured gamepad index, or -1 when unbound or invalid.
int64_t rt_game3d_input_get_pad_bound(void *obj) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.get_PadBound: invalid input");
    game3d_input_repair_state(input);
    return input ? input->bound_pad : -1;
}

/// @brief Set the right-stick look sensitivity (degrees per frame at full tilt).
/// @param obj Input3D runtime handle.
/// @param sensitivity Requested finite non-negative scale, capped at 20;
///                    invalid values select zero.
void rt_game3d_input_set_pad_look_sensitivity(void *obj, double sensitivity) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.setPadLookSensitivity: invalid input");
    if (!input)
        return;
    input->pad_look_sensitivity = game3d_nonnegative_clamped_or(sensitivity, 0.0, 20.0);
}

/// @brief Get the right-stick look sensitivity.
/// @param obj Input3D runtime handle.
/// @return The configured per-frame scale, or zero when invalid.
double rt_game3d_input_get_pad_look_sensitivity(void *obj) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.get_PadLookSensitivity: invalid input");
    game3d_input_repair_state(input);
    return input ? input->pad_look_sensitivity : 0.0;
}
