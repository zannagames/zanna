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

#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_decal3d.h"
#include "rt_g3d_commit_queue.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_gltf.h"
#include "rt_graphics3d_ids.h"
#include "rt_input.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_model3d.h"
#include "rt_navmesh3d.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_particles3d.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_textureasset3d.h"
#include "rt_threadpool.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return input ? game3d_nonnegative_clamped_or(input->look_sensitivity,
                                                 RT_GAME3D_DEFAULT_LOOK_SENSITIVITY,
                                                 RT_GAME3D_LOOK_SENSITIVITY_MAX)
                 : 0.0;
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
    input->mouse_dx = rt_mouse_delta_x();
    input->mouse_dy = rt_mouse_delta_y();
    input->mouse_fdx = rt_mouse_delta_xf();
    input->mouse_fdy = rt_mouse_delta_yf();
    input->wheel_y = game3d_finite_or(rt_mouse_wheel_yf(), 0.0);
    /* Snapshot bound-gamepad stick axes so Move/LookAxis observe a coherent
     * frame even if the pad is polled again mid-frame. */
    input->pad_connected = 0;
    input->pad_lx = 0.0;
    input->pad_ly = 0.0;
    input->pad_rx = 0.0;
    input->pad_ry = 0.0;
    if (input->bound_pad >= 0 && rt_pad_is_connected(input->bound_pad)) {
        input->pad_connected = 1;
        input->pad_lx = game3d_finite_or(rt_pad_left_x(input->bound_pad), 0.0);
        input->pad_ly = game3d_finite_or(rt_pad_left_y(input->bound_pad), 0.0);
        input->pad_rx = game3d_finite_or(rt_pad_right_x(input->bound_pad), 0.0);
        input->pad_ry = game3d_finite_or(rt_pad_right_y(input->bound_pad), 0.0);
    }
    input->has_snapshot = 1;
}

/// @brief True while `key` is held in the current snapshot or live keyboard state.
/// @param obj Input3D runtime handle.
/// @param key Runtime keyboard code.
/// @return Non-zero while held, otherwise zero.
int8_t rt_game3d_input_is_down(void *obj, int64_t key) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.isDown: invalid input");
    return game3d_input_key_down(input, key);
}

/// @brief True on the frame `key` transitions to down.
/// @param obj Input3D runtime handle.
/// @param key Runtime keyboard code.
/// @return Non-zero for a captured or live down edge, otherwise zero.
int8_t rt_game3d_input_pressed(void *obj, int64_t key) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.pressed: invalid input");
    return game3d_input_key_pressed(input, key);
}

/// @brief True on the frame `key` transitions to up.
/// @param obj Input3D runtime handle.
/// @param key Runtime keyboard code.
/// @return Non-zero for a captured or live up edge, otherwise zero.
int8_t rt_game3d_input_released(void *obj, int64_t key) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.released: invalid input");
    return game3d_input_key_released(input, key);
}

/// @brief Get this frame's raw mouse movement delta as a Vec2.
/// @details Sub-pixel precise while relative (raw) mouse mode is active.
/// @param obj Input3D runtime handle.
/// @return A newly allocated Vec2 containing snapshot-aware fractional X/Y deltas.
void *rt_game3d_input_mouse_delta(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.mouseDelta: invalid input");
    return rt_vec2_new(game3d_input_mouse_fdx(input), game3d_input_mouse_fdy(input));
}

/// @brief True while mouse `button` is held this frame.
/// @param obj Input3D runtime handle.
/// @param button Runtime mouse-button code.
/// @return Non-zero while held in the current snapshot or live state.
int8_t rt_game3d_input_mouse_button(void *obj, int64_t button) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.mouseButton: invalid input");
    return game3d_input_mouse_down(input, button);
}

/// @brief True on the frame mouse `button` transitions to down.
/// @param obj Input3D runtime handle.
/// @param button Runtime mouse-button code.
/// @return Non-zero for a captured or live down edge, otherwise zero.
int8_t rt_game3d_input_mouse_pressed(void *obj, int64_t button) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.mousePressed: invalid input");
    return game3d_input_mouse_pressed_snapshot(input, button);
}

/// @brief Get this frame's mouse wheel scroll delta along Y.
/// @param obj Input3D runtime handle.
/// @return The snapshot-aware fractional vertical wheel displacement.
double rt_game3d_input_wheel_y(void *obj) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.wheelY: invalid input");
    return game3d_input_wheel_y_snapshot(input);
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
        double mag = sqrt(lx * lx + ly * ly);
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
    double s =
        input ? game3d_nonnegative_clamped_or(input->look_sensitivity,
                                              0.01,
                                              RT_GAME3D_LOOK_SENSITIVITY_MAX)
              : 0.01;
    double dx = game3d_input_mouse_fdx(input) * s;
    double dy = game3d_input_mouse_fdy(input) * s;
    if (input && input->pad_connected) {
        double rx = input->pad_rx;
        double ry = input->pad_ry;
        double mag = sqrt(rx * rx + ry * ry);
        const double deadzone = 0.18;
        if (mag > deadzone) {
            double scale = (mag - deadzone) / (1.0 - deadzone);
            if (scale > 1.0)
                scale = 1.0;
            /* Response curve: fine aim near center, fast sweep at the rim. */
            scale = pow(scale, 1.8) / mag;
            double ps = game3d_nonnegative_clamped_or(input->pad_look_sensitivity, 0.0, 20.0);
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
    (void)game3d_input_checked(obj, "Game3D.Input3D.captureMouse: invalid input");
    rt_mouse_capture();
}

/// @brief Release the captured cursor back to the OS.
/// @param obj Input3D runtime handle, validated before changing global mouse state.
void rt_game3d_input_release_mouse(void *obj) {
    (void)game3d_input_checked(obj, "Game3D.Input3D.releaseMouse: invalid input");
    rt_mouse_release();
}

/// @brief Enable/disable raw relative mouse-look (capture + OS raw deltas).
/// @details Convenience over Mouse.SetRelativeMode: enabling captures the
///          cursor and requests native raw motion; LookAxis/MouseDelta become
///          unbounded and sub-pixel. Disabling releases the capture.
/// @param obj Input3D runtime handle, validated before changing global mouse state.
/// @param enabled Non-zero to enable relative mode, zero to disable it.
void rt_game3d_input_set_relative_look(void *obj, int8_t enabled) {
    (void)game3d_input_checked(obj, "Game3D.Input3D.setRelativeLook: invalid input");
    rt_mouse_set_relative_mode(enabled);
}

/// @brief Bind a gamepad index into MoveAxis/LookAxis (-1 unbinds).
/// @param obj Input3D runtime handle.
/// @param pad Non-negative gamepad index, or any negative value to unbind and
///            clear the captured pad state immediately.
void rt_game3d_input_bind_pad(void *obj, int64_t pad) {
    rt_game3d_input *input = game3d_input_checked(obj, "Game3D.Input3D.bindPad: invalid input");
    if (!input)
        return;
    input->bound_pad = pad < 0 ? -1 : pad;
    if (input->bound_pad < 0) {
        input->pad_connected = 0;
        input->pad_lx = 0.0;
        input->pad_ly = 0.0;
        input->pad_rx = 0.0;
        input->pad_ry = 0.0;
    }
}

/// @brief Currently bound gamepad index (-1 when unbound).
/// @param obj Input3D runtime handle.
/// @return The configured gamepad index, or -1 when unbound or invalid.
int64_t rt_game3d_input_get_pad_bound(void *obj) {
    rt_game3d_input *input =
        game3d_input_checked(obj, "Game3D.Input3D.get_PadBound: invalid input");
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
    return input ? input->pad_look_sensitivity : 0.0;
}
