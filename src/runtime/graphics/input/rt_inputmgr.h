//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/input/rt_inputmgr.h
// Purpose: High-level input manager with debouncing and action mapping, providing edge detection,
// held-state queries, analog readings, and unified directional/confirm/cancel abstractions.
//
// Key invariants:
//   - Must be updated exactly once per frame with rt_inputmgr_update after polling events.
//   - Edge-detection results (pressed/released) are valid only for the frame they occur.
//   - Gamepad indices are in [0, 3]; passing -1 queries any connected gamepad.
//   - Debounce state is per-key and independent of analog input.
//
// Ownership/Lifetime:
//   - Input managers are GC-managed runtime objects; rt_inputmgr_destroy is a compatibility no-op.
//
// Links: src/runtime/graphics/input/rt_inputmgr.c (implementation), src/runtime/graphics/input/rt_input.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to an InputManager instance.
typedef struct rt_inputmgr_impl *rt_inputmgr;

/// @brief Allocates and initializes a new InputManager instance.
/// @return A new GC-managed InputManager handle, or NULL on allocation failure.
rt_inputmgr rt_inputmgr_new(void);

/// @brief Compatibility no-op for InputManager handles.
/// @details InputManager instances are GC-managed; this function exists for older callers that
///          paired construction with an explicit destroy call.
/// @param mgr The input manager to destroy. Passing NULL is a no-op.
void rt_inputmgr_destroy(rt_inputmgr mgr);

/// @brief Advance per-manager debounce timers by one frame.
/// @details Call once per frame after platform event polling. Keyboard, mouse, and gamepad edges
///          are owned by the lower input layer; this manager update only decrements positive
///          debounce timers. Skipping an update therefore delays debounce expiry.
/// @param mgr The input manager to update.
void rt_inputmgr_update(rt_inputmgr mgr);

//=============================================================================
// Keyboard - Just Pressed/Released (Edge Detection)
//=============================================================================

/// @brief Checks whether a keyboard key was first pressed on this frame.
/// @param mgr The input manager.
/// @param key Platform-specific key code identifying the key to query.
/// @return 1 if the key transitioned from released to pressed this frame,
///   0 otherwise.
int8_t rt_inputmgr_key_pressed(rt_inputmgr mgr, int64_t key);

/// @brief Checks whether a keyboard key was first released on this frame.
/// @param mgr The input manager.
/// @param key Platform-specific key code identifying the key to query.
/// @return 1 if the key transitioned from pressed to released this frame,
///   0 otherwise.
int8_t rt_inputmgr_key_released(rt_inputmgr mgr, int64_t key);

/// @brief Checks whether a keyboard key is currently held down.
/// @param mgr The input manager.
/// @param key Platform-specific key code identifying the key to query.
/// @return 1 if the key is down right now (regardless of when it was first
///   pressed), 0 otherwise.
int8_t rt_inputmgr_key_held(rt_inputmgr mgr, int64_t key);

//=============================================================================
// Keyboard - Debounced (for menus)
//=============================================================================

/// @brief Checks whether a key was pressed with debounce filtering applied.
///
/// Returns 1 at most once per key-down, then suppresses further positive
/// results until the key is released and the debounce delay has elapsed.
/// Ideal for menu navigation where holding a key should not produce rapid
/// repeated selections.
/// @param mgr The input manager.
/// @param key Platform-specific key code identifying the key to query.
/// @return 1 if the key press passed the debounce filter, 0 otherwise.
int8_t rt_inputmgr_key_pressed_debounced(rt_inputmgr mgr, int64_t key);

/// @brief Sets the debounce delay applied to debounced key queries.
/// @param mgr The input manager.
/// @param frames Number of frames that must elapse after a release before
///   the same key can register as pressed again. Must be >= 0.
void rt_inputmgr_set_debounce_delay(rt_inputmgr mgr, int64_t frames);

/// @brief Retrieves the current debounce delay setting.
/// @param mgr The input manager.
/// @return The debounce delay in frames.
int64_t rt_inputmgr_get_debounce_delay(rt_inputmgr mgr);

//=============================================================================
// Mouse - Just Pressed/Released (Edge Detection)
//=============================================================================

/// @brief Checks whether a mouse button was first pressed on this frame.
/// @param mgr The input manager.
/// @param button Mouse button index (0 = left, 1 = right, 2 = middle).
/// @return 1 if the button transitioned from released to pressed this frame,
///   0 otherwise.
int8_t rt_inputmgr_mouse_pressed(rt_inputmgr mgr, int64_t button);

/// @brief Checks whether a mouse button was first released on this frame.
/// @param mgr The input manager.
/// @param button Mouse button index (0 = left, 1 = right, 2 = middle).
/// @return 1 if the button transitioned from pressed to released this frame,
///   0 otherwise.
int8_t rt_inputmgr_mouse_released(rt_inputmgr mgr, int64_t button);

/// @brief Checks whether a mouse button is currently held down.
/// @param mgr The input manager.
/// @param button Mouse button index (0 = left, 1 = right, 2 = middle).
/// @return 1 if the button is down right now, 0 otherwise.
int8_t rt_inputmgr_mouse_held(rt_inputmgr mgr, int64_t button);

//=============================================================================
// Mouse - Position and Movement
//=============================================================================

/// @brief Retrieves the current mouse cursor X position in screen pixels.
/// @param mgr The input manager.
/// @return The X coordinate of the cursor, relative to the window origin.
int64_t rt_inputmgr_mouse_x(rt_inputmgr mgr);

/// @brief Retrieves the current mouse cursor Y position in screen pixels.
/// @param mgr The input manager.
/// @return The Y coordinate of the cursor, relative to the window origin.
int64_t rt_inputmgr_mouse_y(rt_inputmgr mgr);

/// @brief Retrieves the mouse cursor X movement since the previous frame.
/// @param mgr The input manager.
/// @return Horizontal pixel displacement (positive = rightward).
int64_t rt_inputmgr_mouse_delta_x(rt_inputmgr mgr);

/// @brief Retrieves the mouse cursor Y movement since the previous frame.
/// @param mgr The input manager.
/// @return Vertical pixel displacement (positive = downward).
int64_t rt_inputmgr_mouse_delta_y(rt_inputmgr mgr);

/// @brief Retrieves the vertical scroll wheel delta for this frame.
/// @param mgr The input manager.
/// @return Scroll amount (positive = scroll up, negative = scroll down).
int64_t rt_inputmgr_scroll_y(rt_inputmgr mgr);

/// @brief Retrieves the horizontal scroll wheel delta for this frame.
/// @param mgr The input manager.
/// @return Scroll amount (positive = scroll right, negative = scroll left).
int64_t rt_inputmgr_scroll_x(rt_inputmgr mgr);

/// @brief Retrieves the vertical scroll wheel delta for this frame with full precision.
/// @param mgr The input manager; global mouse state is queried directly.
/// @return Fractional vertical scroll amount (positive = up).
double rt_inputmgr_scroll_yf(rt_inputmgr mgr);

/// @brief Retrieves the horizontal scroll wheel delta for this frame with full precision.
/// @param mgr The input manager; global mouse state is queried directly.
/// @return Fractional horizontal scroll amount (positive = right).
double rt_inputmgr_scroll_xf(rt_inputmgr mgr);

//=============================================================================
// Gamepad - Just Pressed/Released (Edge Detection)
//=============================================================================

/// @brief Checks whether a gamepad button was first pressed on this frame.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3], or -1 to query any connected gamepad.
/// @param button Platform-specific gamepad button constant.
/// @return 1 if the button transitioned from released to pressed this frame,
///   0 otherwise.
int8_t rt_inputmgr_pad_pressed(rt_inputmgr mgr, int64_t pad, int64_t button);

/// @brief Checks whether a gamepad button was first released on this frame.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3], or -1 to query any connected gamepad.
/// @param button Platform-specific gamepad button constant.
/// @return 1 if the button transitioned from pressed to released this frame,
///   0 otherwise.
int8_t rt_inputmgr_pad_released(rt_inputmgr mgr, int64_t pad, int64_t button);

/// @brief Checks whether a gamepad button is currently held down.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3], or -1 to query any connected gamepad.
/// @param button Platform-specific gamepad button constant.
/// @return 1 if the button is down right now, 0 otherwise.
int8_t rt_inputmgr_pad_held(rt_inputmgr mgr, int64_t pad, int64_t button);

//=============================================================================
// Gamepad - Analog Inputs
//=============================================================================

/// @brief Reads the left analog stick horizontal axis for a gamepad.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3].
/// @return Axis deflection from -1.0 (full left) to 1.0 (full right),
///   with 0.0 at center/rest.
double rt_inputmgr_pad_left_x(rt_inputmgr mgr, int64_t pad);

/// @brief Reads the left analog stick vertical axis for a gamepad.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3].
/// @return Axis deflection from -1.0 (full up) to 1.0 (full down),
///   with 0.0 at center/rest.
double rt_inputmgr_pad_left_y(rt_inputmgr mgr, int64_t pad);

/// @brief Reads the right analog stick horizontal axis for a gamepad.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3].
/// @return Axis deflection from -1.0 (full left) to 1.0 (full right),
///   with 0.0 at center/rest.
double rt_inputmgr_pad_right_x(rt_inputmgr mgr, int64_t pad);

/// @brief Reads the right analog stick vertical axis for a gamepad.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3].
/// @return Axis deflection from -1.0 (full up) to 1.0 (full down),
///   with 0.0 at center/rest.
double rt_inputmgr_pad_right_y(rt_inputmgr mgr, int64_t pad);

/// @brief Reads the left trigger analog value for a gamepad.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3].
/// @return Trigger pressure from 0.0 (released) to 1.0 (fully depressed).
double rt_inputmgr_pad_left_trigger(rt_inputmgr mgr, int64_t pad);

/// @brief Reads the right trigger analog value for a gamepad.
/// @param mgr The input manager.
/// @param pad Gamepad index in [0, 3].
/// @return Trigger pressure from 0.0 (released) to 1.0 (fully depressed).
double rt_inputmgr_pad_right_trigger(rt_inputmgr mgr, int64_t pad);

//=============================================================================
// Unified Direction Input (combines keyboard, D-pad, and sticks)
//=============================================================================

/// @brief Checks whether any "up" input is active across all input devices.
///
/// Combines the Up arrow key, W key, D-pad up, and left stick deflection
/// into a single boolean query.
/// @param mgr The input manager.
/// @return 1 if any upward input is detected, 0 otherwise.
int8_t rt_inputmgr_up(rt_inputmgr mgr);

/// @brief Checks whether any "down" input is active across all input devices.
///
/// Combines the Down arrow key, S key, D-pad down, and left stick
/// deflection into a single boolean query.
/// @param mgr The input manager.
/// @return 1 if any downward input is detected, 0 otherwise.
int8_t rt_inputmgr_down(rt_inputmgr mgr);

/// @brief Checks whether any "left" input is active across all input devices.
///
/// Combines the Left arrow key, A key, D-pad left, and left stick
/// deflection into a single boolean query.
/// @param mgr The input manager.
/// @return 1 if any leftward input is detected, 0 otherwise.
int8_t rt_inputmgr_left(rt_inputmgr mgr);

/// @brief Checks whether any "right" input is active across all input devices.
///
/// Combines the Right arrow key, D key, D-pad right, and left stick
/// deflection into a single boolean query.
/// @param mgr The input manager.
/// @return 1 if any rightward input is detected, 0 otherwise.
int8_t rt_inputmgr_right(rt_inputmgr mgr);

/// @brief Checks whether any "confirm" input is active (Enter, Space, or
///   gamepad A button).
/// @param mgr The input manager.
/// @return 1 if a confirm action is detected, 0 otherwise.
int8_t rt_inputmgr_confirm(rt_inputmgr mgr);

/// @brief Checks whether any "cancel" input is active (Escape or gamepad B
///   button).
/// @param mgr The input manager.
/// @return 1 if a cancel action is detected, 0 otherwise.
int8_t rt_inputmgr_cancel(rt_inputmgr mgr);

/// @brief Reads the unified horizontal axis from all input sources.
///
/// Merges keyboard arrow/WASD keys, D-pad, and left analog stick into a
/// single floating-point axis.
/// @param mgr The input manager.
/// @return A value from -1.0 (full left) to 1.0 (full right). Digital
///   inputs produce -1.0, 0.0, or 1.0; analog inputs produce continuous
///   values.
double rt_inputmgr_axis_x(rt_inputmgr mgr);

/// @brief Reads the unified vertical axis from all input sources.
///
/// Merges keyboard arrow/WASD keys, D-pad, and left analog stick into a
/// single floating-point axis.
/// @param mgr The input manager.
/// @return A value from -1.0 (full up) to 1.0 (full down). Digital inputs
///   produce -1.0, 0.0, or 1.0; analog inputs produce continuous values.
double rt_inputmgr_axis_y(rt_inputmgr mgr);

#ifdef __cplusplus
}
#endif
