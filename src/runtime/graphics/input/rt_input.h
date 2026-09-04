//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/input/rt_input.h
// Purpose: Keyboard, mouse, and gamepad input handling for the Zanna.Input runtime namespace,
// providing per-frame key state, mouse position/button, and gamepad queries.
//
// Key invariants:
//   - Input state is frame-coherent; pressed/released edge lists reset each rt_input_poll call.
//   - Key code constants are GLFW-compatible integer values.
//   - Mouse coordinates are in logical pixels relative to the window top-left.
//   - Gamepad axes are in [-1.0, 1.0]; triggers are in [0.0, 1.0].
//
// Ownership/Lifetime:
//   - Canvas owns the input subsystem; input callbacks are non-owning function pointers.
//   - No heap allocation in state query functions.
//
// Links: src/runtime/graphics/input/rt_input.c (implementation),
//        src/runtime/core/rt_string.h,
//        docs/adr/0169-super-modifier-keys-and-studio-viewport-picking.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares keyboard, mouse, and gamepad runtime input surfaces.
 *
 * @details The API exposes stable GLFW-compatible key and button constants,
 *          frame-coherent level and edge queries, typed-text capture, logical
 *          mouse coordinates and cursor control, and normalized connected-pad
 *          state with runtime-friendly value accessors.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Key Code Constants (GLFW-compatible values)
//=========================================================================

// Unknown key
#define ZANNA_KEY_UNKNOWN 0

// Printable ASCII keys (letters and numbers match ASCII)
#define ZANNA_KEY_SPACE 32
#define ZANNA_KEY_QUOTE 39  // '
#define ZANNA_KEY_COMMA 44  // ,
#define ZANNA_KEY_MINUS 45  // -
#define ZANNA_KEY_PERIOD 46 // .
#define ZANNA_KEY_SLASH 47  // /

#define ZANNA_KEY_0 48
#define ZANNA_KEY_1 49
#define ZANNA_KEY_2 50
#define ZANNA_KEY_3 51
#define ZANNA_KEY_4 52
#define ZANNA_KEY_5 53
#define ZANNA_KEY_6 54
#define ZANNA_KEY_7 55
#define ZANNA_KEY_8 56
#define ZANNA_KEY_9 57

#define ZANNA_KEY_SEMICOLON 59 // ;
#define ZANNA_KEY_EQUALS 61    // =

#define ZANNA_KEY_A 65
#define ZANNA_KEY_B 66
#define ZANNA_KEY_C 67
#define ZANNA_KEY_D 68
#define ZANNA_KEY_E 69
#define ZANNA_KEY_F 70
#define ZANNA_KEY_G 71
#define ZANNA_KEY_H 72
#define ZANNA_KEY_I 73
#define ZANNA_KEY_J 74
#define ZANNA_KEY_K 75
#define ZANNA_KEY_L 76
#define ZANNA_KEY_M 77
#define ZANNA_KEY_N 78
#define ZANNA_KEY_O 79
#define ZANNA_KEY_P 80
#define ZANNA_KEY_Q 81
#define ZANNA_KEY_R 82
#define ZANNA_KEY_S 83
#define ZANNA_KEY_T 84
#define ZANNA_KEY_U 85
#define ZANNA_KEY_V 86
#define ZANNA_KEY_W 87
#define ZANNA_KEY_X 88
#define ZANNA_KEY_Y 89
#define ZANNA_KEY_Z 90

#define ZANNA_KEY_LBRACKET 91  // [
#define ZANNA_KEY_BACKSLASH 92 // backslash
#define ZANNA_KEY_RBRACKET 93  // ]
#define ZANNA_KEY_GRAVE 96     // `

// Special keys (GLFW-style values >= 256)
#define ZANNA_KEY_ESCAPE 256
#define ZANNA_KEY_ENTER 257
#define ZANNA_KEY_TAB 258
#define ZANNA_KEY_BACKSPACE 259
#define ZANNA_KEY_INSERT 260
#define ZANNA_KEY_DELETE 261
#define ZANNA_KEY_RIGHT 262
#define ZANNA_KEY_LEFT 263
#define ZANNA_KEY_DOWN 264
#define ZANNA_KEY_UP 265
#define ZANNA_KEY_PAGEUP 266
#define ZANNA_KEY_PAGEDOWN 267
#define ZANNA_KEY_HOME 268
#define ZANNA_KEY_END 269

// Function keys
#define ZANNA_KEY_F1 290
#define ZANNA_KEY_F2 291
#define ZANNA_KEY_F3 292
#define ZANNA_KEY_F4 293
#define ZANNA_KEY_F5 294
#define ZANNA_KEY_F6 295
#define ZANNA_KEY_F7 296
#define ZANNA_KEY_F8 297
#define ZANNA_KEY_F9 298
#define ZANNA_KEY_F10 299
#define ZANNA_KEY_F11 300
#define ZANNA_KEY_F12 301

// Numpad keys
#define ZANNA_KEY_NUM0 320
#define ZANNA_KEY_NUM1 321
#define ZANNA_KEY_NUM2 322
#define ZANNA_KEY_NUM3 323
#define ZANNA_KEY_NUM4 324
#define ZANNA_KEY_NUM5 325
#define ZANNA_KEY_NUM6 326
#define ZANNA_KEY_NUM7 327
#define ZANNA_KEY_NUM8 328
#define ZANNA_KEY_NUM9 329
#define ZANNA_KEY_NUMDOT 330
#define ZANNA_KEY_NUMDIV 331
#define ZANNA_KEY_NUMMUL 332
#define ZANNA_KEY_NUMSUB 333
#define ZANNA_KEY_NUMADD 334
#define ZANNA_KEY_NUMENTER 335

// Modifier keys
#define ZANNA_KEY_LSHIFT 340
#define ZANNA_KEY_LCTRL 341
#define ZANNA_KEY_LALT 342
#define ZANNA_KEY_LSUPER 343
#define ZANNA_KEY_RSHIFT 344
#define ZANNA_KEY_RCTRL 345
#define ZANNA_KEY_RALT 346
#define ZANNA_KEY_RSUPER 347

// Maximum key code we track
#define ZANNA_KEY_MAX 512

//=========================================================================
// Keyboard State Management
//=========================================================================

/// @brief Initialize the keyboard input system.
/// @details Called internally when Canvas is created.
void rt_keyboard_init(void);

/// @brief Reset keyboard state for new frame.
/// @details Called by Canvas.Poll() to clear pressed/released lists.
void rt_keyboard_begin_frame(void);

/// @brief Register a key press event.
/// @param key Public ZANNA_KEY_* key code.
void rt_keyboard_on_key_down(int64_t key);

/// @brief Register a key release event.
/// @param key Public ZANNA_KEY_* key code.
void rt_keyboard_on_key_up(int64_t key);

/// @brief Register a key press event from a vgfx window event.
/// @param key VGFX_KEY_* key code.
void rt_keyboard_on_vgfx_key_down(int64_t key);

/// @brief Register a key release event from a vgfx window event.
/// @param key VGFX_KEY_* key code.
void rt_keyboard_on_vgfx_key_up(int64_t key);

/// @brief Add text input character.
/// @param ch Unicode codepoint.
void rt_keyboard_text_input(int32_t ch);

/// @brief Set the active Canvas for keyboard input.
/// @param canvas Canvas handle (opaque pointer to vgfx window).
void rt_keyboard_set_canvas(void *canvas);

//=========================================================================
// Polling Methods (Current State)
//=========================================================================

/// @brief Check if a key is currently pressed.
/// @param key Key code to check.
/// @return 1 if pressed, 0 otherwise.
int8_t rt_keyboard_is_down(int64_t key);

/// @brief Check if a key is currently released.
/// @param key Key code to check.
/// @return 1 if released, 0 otherwise.
int8_t rt_keyboard_is_up(int64_t key);

/// @brief Check if any key is currently pressed.
/// @return 1 if any key is pressed, 0 otherwise.
int8_t rt_keyboard_any_down(void);

/// @brief Get the first pressed key code.
/// @return Key code of first pressed key, or 0 if none.
int64_t rt_keyboard_get_down(void);

//=========================================================================
// Event Methods (Since Last Poll)
//=========================================================================

/// @brief Check if a key was pressed this frame.
/// @param key Key code to check.
/// @return 1 if pressed this frame, 0 otherwise.
int8_t rt_keyboard_was_pressed(int64_t key);

/// @brief Check if a key was released this frame.
/// @param key Key code to check.
/// @return 1 if released this frame, 0 otherwise.
int8_t rt_keyboard_was_released(int64_t key);

/// @brief Get all keys pressed this frame.
/// @return Seq of key codes (integers).
void *rt_keyboard_get_pressed(void);

/// @brief Get all keys released this frame.
/// @return Seq of key codes (integers).
void *rt_keyboard_get_released(void);

//=========================================================================
// Text Input
//=========================================================================

/// @brief Get text typed since last poll.
/// @return String containing typed characters.
rt_string rt_keyboard_get_text(void);

/// @brief Enable text input mode.
/// @details Enables text input events on platforms that support IME.
void rt_keyboard_enable_text_input(void);

/// @brief Disable text input mode.
void rt_keyboard_disable_text_input(void);

//=========================================================================
// Modifier State
//=========================================================================

/// @brief Check if Shift key is held.
/// @return 1 if Shift is held, 0 otherwise.
int8_t rt_keyboard_shift(void);

/// @brief Check if Ctrl key is held.
/// @return 1 if Ctrl is held, 0 otherwise.
int8_t rt_keyboard_ctrl(void);

/// @brief Check if Alt key is held.
/// @return 1 if Alt is held, 0 otherwise.
int8_t rt_keyboard_alt(void);

/// @brief Check if Caps Lock is on.
/// @return 1 if Caps Lock is on, 0 otherwise.
int8_t rt_keyboard_caps_lock(void);

//=========================================================================
// Key Name Helper
//=========================================================================

/// @brief Get human-readable name for a key code.
/// @param key Key code.
/// @return String name (e.g., "A", "Enter", "F1").
rt_string rt_keyboard_key_name(int64_t key);

//=========================================================================
// Key Code Constants (Runtime getters)
//
/// @brief Runtime accessors returning the stable integer key code for each
///        named key (the function-call equivalents of the ZANNA_KEY_*
///        macros, for front-ends that bind functions rather than macros).
/// @details One getter per key; the function name is the key
///          (`rt_keyboard_key_a` → the 'A' key code, `rt_keyboard_key_f1` →
///          F1, `rt_keyboard_key_numdot` → numpad '.', etc.). The returned
///          code is what rt_keyboard_pressed()/rt_keyboard_down() expect.
///          `rt_keyboard_key_unknown()` is the sentinel for "no/invalid key".
//=========================================================================

/// @brief Return the unknown-key sentinel.
/// @return `ZANNA_KEY_UNKNOWN`.
int64_t rt_keyboard_key_unknown(void);
/// @brief Return the A key code.
/// @return `ZANNA_KEY_A`.
int64_t rt_keyboard_key_a(void);
/// @brief Return the B key code.
/// @return `ZANNA_KEY_B`.
int64_t rt_keyboard_key_b(void);
/// @brief Return the C key code.
/// @return `ZANNA_KEY_C`.
int64_t rt_keyboard_key_c(void);
/// @brief Return the D key code.
/// @return `ZANNA_KEY_D`.
int64_t rt_keyboard_key_d(void);
/// @brief Return the E key code.
/// @return `ZANNA_KEY_E`.
int64_t rt_keyboard_key_e(void);
/// @brief Return the F key code.
/// @return `ZANNA_KEY_F`.
int64_t rt_keyboard_key_f(void);
/// @brief Return the G key code.
/// @return `ZANNA_KEY_G`.
int64_t rt_keyboard_key_g(void);
/// @brief Return the H key code.
/// @return `ZANNA_KEY_H`.
int64_t rt_keyboard_key_h(void);
/// @brief Return the I key code.
/// @return `ZANNA_KEY_I`.
int64_t rt_keyboard_key_i(void);
/// @brief Return the J key code.
/// @return `ZANNA_KEY_J`.
int64_t rt_keyboard_key_j(void);
/// @brief Return the K key code.
/// @return `ZANNA_KEY_K`.
int64_t rt_keyboard_key_k(void);
/// @brief Return the L key code.
/// @return `ZANNA_KEY_L`.
int64_t rt_keyboard_key_l(void);
/// @brief Return the M key code.
/// @return `ZANNA_KEY_M`.
int64_t rt_keyboard_key_m(void);
/// @brief Return the N key code.
/// @return `ZANNA_KEY_N`.
int64_t rt_keyboard_key_n(void);
/// @brief Return the O key code.
/// @return `ZANNA_KEY_O`.
int64_t rt_keyboard_key_o(void);
/// @brief Return the P key code.
/// @return `ZANNA_KEY_P`.
int64_t rt_keyboard_key_p(void);
/// @brief Return the Q key code.
/// @return `ZANNA_KEY_Q`.
int64_t rt_keyboard_key_q(void);
/// @brief Return the R key code.
/// @return `ZANNA_KEY_R`.
int64_t rt_keyboard_key_r(void);
/// @brief Return the S key code.
/// @return `ZANNA_KEY_S`.
int64_t rt_keyboard_key_s(void);
/// @brief Return the T key code.
/// @return `ZANNA_KEY_T`.
int64_t rt_keyboard_key_t(void);
/// @brief Return the U key code.
/// @return `ZANNA_KEY_U`.
int64_t rt_keyboard_key_u(void);
/// @brief Return the V key code.
/// @return `ZANNA_KEY_V`.
int64_t rt_keyboard_key_v(void);
/// @brief Return the W key code.
/// @return `ZANNA_KEY_W`.
int64_t rt_keyboard_key_w(void);
/// @brief Return the X key code.
/// @return `ZANNA_KEY_X`.
int64_t rt_keyboard_key_x(void);
/// @brief Return the Y key code.
/// @return `ZANNA_KEY_Y`.
int64_t rt_keyboard_key_y(void);
/// @brief Return the Z key code.
/// @return `ZANNA_KEY_Z`.
int64_t rt_keyboard_key_z(void);
/// @brief Return the row-digit 0 key code.
/// @return `ZANNA_KEY_0`.
int64_t rt_keyboard_key_0(void);
/// @brief Return the row-digit 1 key code.
/// @return `ZANNA_KEY_1`.
int64_t rt_keyboard_key_1(void);
/// @brief Return the row-digit 2 key code.
/// @return `ZANNA_KEY_2`.
int64_t rt_keyboard_key_2(void);
/// @brief Return the row-digit 3 key code.
/// @return `ZANNA_KEY_3`.
int64_t rt_keyboard_key_3(void);
/// @brief Return the row-digit 4 key code.
/// @return `ZANNA_KEY_4`.
int64_t rt_keyboard_key_4(void);
/// @brief Return the row-digit 5 key code.
/// @return `ZANNA_KEY_5`.
int64_t rt_keyboard_key_5(void);
/// @brief Return the row-digit 6 key code.
/// @return `ZANNA_KEY_6`.
int64_t rt_keyboard_key_6(void);
/// @brief Return the row-digit 7 key code.
/// @return `ZANNA_KEY_7`.
int64_t rt_keyboard_key_7(void);
/// @brief Return the row-digit 8 key code.
/// @return `ZANNA_KEY_8`.
int64_t rt_keyboard_key_8(void);
/// @brief Return the row-digit 9 key code.
/// @return `ZANNA_KEY_9`.
int64_t rt_keyboard_key_9(void);
/// @brief Return the F1 function-key code.
/// @return `ZANNA_KEY_F1`.
int64_t rt_keyboard_key_f1(void);
/// @brief Return the F2 function-key code.
/// @return `ZANNA_KEY_F2`.
int64_t rt_keyboard_key_f2(void);
/// @brief Return the F3 function-key code.
/// @return `ZANNA_KEY_F3`.
int64_t rt_keyboard_key_f3(void);
/// @brief Return the F4 function-key code.
/// @return `ZANNA_KEY_F4`.
int64_t rt_keyboard_key_f4(void);
/// @brief Return the F5 function-key code.
/// @return `ZANNA_KEY_F5`.
int64_t rt_keyboard_key_f5(void);
/// @brief Return the F6 function-key code.
/// @return `ZANNA_KEY_F6`.
int64_t rt_keyboard_key_f6(void);
/// @brief Return the F7 function-key code.
/// @return `ZANNA_KEY_F7`.
int64_t rt_keyboard_key_f7(void);
/// @brief Return the F8 function-key code.
/// @return `ZANNA_KEY_F8`.
int64_t rt_keyboard_key_f8(void);
/// @brief Return the F9 function-key code.
/// @return `ZANNA_KEY_F9`.
int64_t rt_keyboard_key_f9(void);
/// @brief Return the F10 function-key code.
/// @return `ZANNA_KEY_F10`.
int64_t rt_keyboard_key_f10(void);
/// @brief Return the F11 function-key code.
/// @return `ZANNA_KEY_F11`.
int64_t rt_keyboard_key_f11(void);
/// @brief Return the F12 function-key code.
/// @return `ZANNA_KEY_F12`.
int64_t rt_keyboard_key_f12(void);
/// @brief Return the Up-arrow key code.
/// @return `ZANNA_KEY_UP`.
int64_t rt_keyboard_key_up(void);
/// @brief Return the Down-arrow key code.
/// @return `ZANNA_KEY_DOWN`.
int64_t rt_keyboard_key_down(void);
/// @brief Return the Left-arrow key code.
/// @return `ZANNA_KEY_LEFT`.
int64_t rt_keyboard_key_left(void);
/// @brief Return the Right-arrow key code.
/// @return `ZANNA_KEY_RIGHT`.
int64_t rt_keyboard_key_right(void);
/// @brief Return the Home key code.
/// @return `ZANNA_KEY_HOME`.
int64_t rt_keyboard_key_home(void);
/// @brief Return the End key code.
/// @return `ZANNA_KEY_END`.
int64_t rt_keyboard_key_end(void);
/// @brief Return the Page Up key code.
/// @return `ZANNA_KEY_PAGEUP`.
int64_t rt_keyboard_key_pageup(void);
/// @brief Return the Page Down key code.
/// @return `ZANNA_KEY_PAGEDOWN`.
int64_t rt_keyboard_key_pagedown(void);
/// @brief Return the Insert key code.
/// @return `ZANNA_KEY_INSERT`.
int64_t rt_keyboard_key_insert(void);
/// @brief Return the Delete key code.
/// @return `ZANNA_KEY_DELETE`.
int64_t rt_keyboard_key_delete(void);
/// @brief Return the Backspace key code.
/// @return `ZANNA_KEY_BACKSPACE`.
int64_t rt_keyboard_key_backspace(void);
/// @brief Return the Tab key code.
/// @return `ZANNA_KEY_TAB`.
int64_t rt_keyboard_key_tab(void);
/// @brief Return the main Enter key code.
/// @return `ZANNA_KEY_ENTER`.
int64_t rt_keyboard_key_enter(void);
/// @brief Return the Space key code.
/// @return `ZANNA_KEY_SPACE`.
int64_t rt_keyboard_key_space(void);
/// @brief Return the Escape key code.
/// @return `ZANNA_KEY_ESCAPE`.
int64_t rt_keyboard_key_escape(void);
/// @brief Return the left Shift key code.
/// @return `ZANNA_KEY_LSHIFT`.
int64_t rt_keyboard_key_lshift(void);
/// @brief Return the right Shift key code.
/// @return `ZANNA_KEY_RSHIFT`.
int64_t rt_keyboard_key_rshift(void);
/// @brief Return the left Control key code.
/// @return `ZANNA_KEY_LCTRL`.
int64_t rt_keyboard_key_lctrl(void);
/// @brief Return the right Control key code.
/// @return `ZANNA_KEY_RCTRL`.
int64_t rt_keyboard_key_rctrl(void);
/// @brief Return the left Alt key code.
/// @return `ZANNA_KEY_LALT`.
int64_t rt_keyboard_key_lalt(void);
/// @brief Return the right Alt key code.
/// @return `ZANNA_KEY_RALT`.
int64_t rt_keyboard_key_ralt(void);
/// @brief Return the left Super key code.
/// @return `ZANNA_KEY_LSUPER`.
int64_t rt_keyboard_key_lsuper(void);
/// @brief Return the right Super key code.
/// @return `ZANNA_KEY_RSUPER`.
int64_t rt_keyboard_key_rsuper(void);
/// @brief Return the Minus key code.
/// @return `ZANNA_KEY_MINUS`.
int64_t rt_keyboard_key_minus(void);
/// @brief Return the Equals key code.
/// @return `ZANNA_KEY_EQUALS`.
int64_t rt_keyboard_key_equals(void);
/// @brief Return the left-bracket key code.
/// @return `ZANNA_KEY_LBRACKET`.
int64_t rt_keyboard_key_lbracket(void);
/// @brief Return the right-bracket key code.
/// @return `ZANNA_KEY_RBRACKET`.
int64_t rt_keyboard_key_rbracket(void);
/// @brief Return the Backslash key code.
/// @return `ZANNA_KEY_BACKSLASH`.
int64_t rt_keyboard_key_backslash(void);
/// @brief Return the Semicolon key code.
/// @return `ZANNA_KEY_SEMICOLON`.
int64_t rt_keyboard_key_semicolon(void);
/// @brief Return the Quote key code.
/// @return `ZANNA_KEY_QUOTE`.
int64_t rt_keyboard_key_quote(void);
/// @brief Return the Grave key code.
/// @return `ZANNA_KEY_GRAVE`.
int64_t rt_keyboard_key_grave(void);
/// @brief Return the Comma key code.
/// @return `ZANNA_KEY_COMMA`.
int64_t rt_keyboard_key_comma(void);
/// @brief Return the Period key code.
/// @return `ZANNA_KEY_PERIOD`.
int64_t rt_keyboard_key_period(void);
/// @brief Return the Slash key code.
/// @return `ZANNA_KEY_SLASH`.
int64_t rt_keyboard_key_slash(void);
/// @brief Return the numpad 0 key code.
/// @return `ZANNA_KEY_NUM0`.
int64_t rt_keyboard_key_num0(void);
/// @brief Return the numpad 1 key code.
/// @return `ZANNA_KEY_NUM1`.
int64_t rt_keyboard_key_num1(void);
/// @brief Return the numpad 2 key code.
/// @return `ZANNA_KEY_NUM2`.
int64_t rt_keyboard_key_num2(void);
/// @brief Return the numpad 3 key code.
/// @return `ZANNA_KEY_NUM3`.
int64_t rt_keyboard_key_num3(void);
/// @brief Return the numpad 4 key code.
/// @return `ZANNA_KEY_NUM4`.
int64_t rt_keyboard_key_num4(void);
/// @brief Return the numpad 5 key code.
/// @return `ZANNA_KEY_NUM5`.
int64_t rt_keyboard_key_num5(void);
/// @brief Return the numpad 6 key code.
/// @return `ZANNA_KEY_NUM6`.
int64_t rt_keyboard_key_num6(void);
/// @brief Return the numpad 7 key code.
/// @return `ZANNA_KEY_NUM7`.
int64_t rt_keyboard_key_num7(void);
/// @brief Return the numpad 8 key code.
/// @return `ZANNA_KEY_NUM8`.
int64_t rt_keyboard_key_num8(void);
/// @brief Return the numpad 9 key code.
/// @return `ZANNA_KEY_NUM9`.
int64_t rt_keyboard_key_num9(void);
/// @brief Return the numpad Add key code.
/// @return `ZANNA_KEY_NUMADD`.
int64_t rt_keyboard_key_numadd(void);
/// @brief Return the numpad Subtract key code.
/// @return `ZANNA_KEY_NUMSUB`.
int64_t rt_keyboard_key_numsub(void);
/// @brief Return the numpad Multiply key code.
/// @return `ZANNA_KEY_NUMMUL`.
int64_t rt_keyboard_key_nummul(void);
/// @brief Return the numpad Divide key code.
/// @return `ZANNA_KEY_NUMDIV`.
int64_t rt_keyboard_key_numdiv(void);
/// @brief Return the numpad Enter key code.
/// @return `ZANNA_KEY_NUMENTER`.
int64_t rt_keyboard_key_numenter(void);
/// @brief Return the numpad Decimal key code.
/// @return `ZANNA_KEY_NUMDOT`.
int64_t rt_keyboard_key_numdot(void);

//=========================================================================
// Mouse Input
//=========================================================================

//=========================================================================
// Mouse Button Constants
//=========================================================================

#define ZANNA_MOUSE_BUTTON_LEFT 0
#define ZANNA_MOUSE_BUTTON_RIGHT 1
#define ZANNA_MOUSE_BUTTON_MIDDLE 2
#define ZANNA_MOUSE_BUTTON_X1 3
#define ZANNA_MOUSE_BUTTON_X2 4
#define ZANNA_MOUSE_BUTTON_MAX 5

//=========================================================================
// Mouse State Management
//=========================================================================

/// @brief Initialize the mouse input system.
/// @details Called internally when Canvas is created.
void rt_mouse_init(void);

/// @brief Reset mouse state for new frame.
/// @details Called by Canvas.Poll() to clear deltas and event lists.
void rt_mouse_begin_frame(void);

/// @brief Recompute the absolute mouse delta after this frame's events are pumped.
/// @details Called by the poll paths after event processing so Mouse.DeltaX/Y
///          describe the motion observed during this poll rather than lagging one
///          frame behind Mouse.X/Y. Relative-mode overrides (rt_mouse_force_delta*)
///          are applied afterwards by the Canvas3D poll and take precedence.
void rt_mouse_finalize_frame(void);

/// @brief Update mouse position.
/// @param x Current X position.
/// @param y Current Y position.
void rt_mouse_update_pos(int64_t x, int64_t y);

/// @brief Override mouse delta for the current frame without moving the cursor.
/// @param dx Forced horizontal delta in logical canvas pixels.
/// @param dy Forced vertical delta in logical canvas pixels.
void rt_mouse_force_delta(int64_t dx, int64_t dy);

/// @brief Override mouse delta with sub-pixel precision (relative mouse mode).
/// @details Sets both the f64 deltas returned by rt_mouse_delta_xf/yf and the
///          rounded i64 deltas returned by rt_mouse_delta_x/y. Used by the
///          Canvas3D poll while native raw mouse deltas are active.
/// @param dx Exact horizontal delta in logical canvas pixels.
/// @param dy Exact vertical delta in logical canvas pixels.
void rt_mouse_force_delta_f(double dx, double dy);

//=========================================================================
// Relative (raw) mouse mode — FPS mouse-look
//=========================================================================

/// @brief Request or release relative (raw) mouse mode.
/// @details Enabling implies mouse capture (cursor hidden, absolute position
///          frozen); disabling releases capture. The platform window applies
///          the actual raw-input mode on its next poll and reports back via
///          rt_mouse_set_relative_native().
/// @param enabled Non-zero to request relative mode, zero to release it.
void rt_mouse_set_relative_mode(int8_t enabled);

/// @brief Whether relative mouse mode has been requested.
/// @return One when requested, otherwise zero.
int8_t rt_mouse_get_relative_mode(void);

/// @brief Record whether the platform delivers native raw deltas (poll-side).
/// @param native Non-zero when the active backend supplies native raw motion.
void rt_mouse_set_relative_native(int8_t native);

/// @brief Whether native raw deltas are active (vs warp-to-center fallback).
/// @return One only when relative mode is requested and native raw input is active.
int8_t rt_mouse_get_relative_native(void);

/// @brief Sub-pixel horizontal mouse delta for the current frame.
/// @return Horizontal delta in logical canvas pixels.
double rt_mouse_delta_xf(void);

/// @brief Sub-pixel vertical mouse delta for the current frame.
/// @return Vertical delta in logical canvas pixels.
double rt_mouse_delta_yf(void);

/// @brief Register a mouse button press event.
/// @param button Button index.
void rt_mouse_button_down(int64_t button);

/// @brief Register a mouse button release event.
/// @param button Button index.
void rt_mouse_button_up(int64_t button);

/// @brief Update scroll wheel deltas.
/// @param dx Horizontal scroll delta.
/// @param dy Vertical scroll delta.
void rt_mouse_update_wheel(double dx, double dy);

/// @brief Set the active Canvas for mouse input.
/// @param canvas Canvas handle (opaque pointer to vgfx window).
void rt_mouse_set_canvas(void *canvas);

/// @brief Clear the keyboard binding if it points at the given canvas.
/// @param canvas Canvas being destroyed; NULL is ignored.
void rt_keyboard_clear_canvas_if_matches(void *canvas);

/// @brief Clear the mouse binding if it points at the given canvas.
/// @param canvas Canvas being destroyed; NULL is ignored.
void rt_mouse_clear_canvas_if_matches(void *canvas);

/// @brief Cancel all held keyboard and mouse input after the active window loses focus.
/// @details Clears level and per-frame edge state without synthesizing activation-capable
/// release edges. Callers can treat the resulting up level as gesture cancellation.
void rt_input_focus_lost(void);

//=========================================================================
// Position Methods
//=========================================================================

/// @brief Get current mouse X position.
/// @return X coordinate in canvas pixels.
int64_t rt_mouse_x(void);

/// @brief Get current mouse Y position.
/// @return Y coordinate in canvas pixels.
int64_t rt_mouse_y(void);

/// @brief Get X movement since last poll.
/// @return Delta X in pixels.
int64_t rt_mouse_delta_x(void);

/// @brief Get Y movement since last poll.
/// @return Delta Y in pixels.
int64_t rt_mouse_delta_y(void);

//=========================================================================
// Button State (Polling)
//=========================================================================

/// @brief Check if a mouse button is currently pressed.
/// @param button Button index to check.
/// @return 1 if pressed, 0 otherwise.
int8_t rt_mouse_is_down(int64_t button);

/// @brief Check if a mouse button is currently released.
/// @param button Button index to check.
/// @return 1 if released, 0 otherwise.
int8_t rt_mouse_is_up(int64_t button);

/// @brief Check if left mouse button is pressed.
/// @return 1 if pressed, 0 otherwise.
int8_t rt_mouse_left(void);

/// @brief Check if right mouse button is pressed.
/// @return 1 if pressed, 0 otherwise.
int8_t rt_mouse_right(void);

/// @brief Check if middle mouse button is pressed.
/// @return 1 if pressed, 0 otherwise.
int8_t rt_mouse_middle(void);

//=========================================================================
// Button Events (Since Last Poll)
//=========================================================================

/// @brief Check if a button was pressed this frame.
/// @param button Button index to check.
/// @return 1 if pressed this frame, 0 otherwise.
int8_t rt_mouse_was_pressed(int64_t button);

/// @brief Check if a button was released this frame.
/// @param button Button index to check.
/// @return 1 if released this frame, 0 otherwise.
int8_t rt_mouse_was_released(int64_t button);

/// @brief Check if a button was clicked this frame.
/// @param button Button index to check.
/// @return 1 if clicked (pressed and released quickly), 0 otherwise.
int8_t rt_mouse_was_clicked(int64_t button);

/// @brief Check if a button was double-clicked this frame.
/// @param button Button index to check.
/// @return 1 if double-clicked, 0 otherwise.
int8_t rt_mouse_was_double_clicked(int64_t button);

//=========================================================================
// Scroll Wheel
//=========================================================================

/// @brief Get horizontal scroll delta.
/// @return Whole-step horizontal scroll amount since last poll.
/// @details Legacy integer accessor. Fractional wheel/trackpad motion is
///          available via rt_mouse_wheel_xf().
int64_t rt_mouse_wheel_x(void);

/// @brief Get vertical scroll delta.
/// @return Whole-step vertical scroll amount (positive = up) since last poll.
/// @details Legacy integer accessor. Fractional wheel/trackpad motion is
///          available via rt_mouse_wheel_yf().
int64_t rt_mouse_wheel_y(void);

/// @brief Get horizontal scroll delta with full precision.
/// @return Horizontal scroll amount since last poll.
double rt_mouse_wheel_xf(void);

/// @brief Get vertical scroll delta with full precision.
/// @return Vertical scroll amount (positive = up) since last poll.
double rt_mouse_wheel_yf(void);

//=========================================================================
// Cursor Control
//=========================================================================

/// @brief Show the system cursor.
void rt_mouse_show(void);

/// @brief Hide the system cursor.
void rt_mouse_hide(void);

/// @brief Check if cursor is hidden.
/// @return 1 if hidden, 0 otherwise.
int8_t rt_mouse_is_hidden(void);

/// @brief Capture the mouse to the window.
/// @details For FPS-style games, confines cursor to window.
void rt_mouse_capture(void);

/// @brief Release mouse capture.
void rt_mouse_release(void);

/// @brief Check if mouse is captured.
/// @return 1 if captured, 0 otherwise.
int8_t rt_mouse_is_captured(void);

/// @brief Warp cursor to a specific position.
/// @param x Target X coordinate.
/// @param y Target Y coordinate.
void rt_mouse_set_pos(int64_t x, int64_t y);

//=========================================================================
// Button Constant Getters
//
/// @brief Runtime accessors returning the integer code for each mouse button
///        (function-call equivalents of the ZANNA_MOUSE_BUTTON_* macros). The
///        name is the button (`rt_mouse_button_left` → left button, `x1`/`x2`
///        → the side buttons); the code is what rt_mouse_* queries expect.
//=========================================================================

/// @brief Return the left mouse-button code.
/// @return `ZANNA_MOUSE_BUTTON_LEFT`.
int64_t rt_mouse_button_left(void);
/// @brief Return the right mouse-button code.
/// @return `ZANNA_MOUSE_BUTTON_RIGHT`.
int64_t rt_mouse_button_right(void);
/// @brief Return the middle mouse-button code.
/// @return `ZANNA_MOUSE_BUTTON_MIDDLE`.
int64_t rt_mouse_button_middle(void);
/// @brief Return the X1/back mouse-button code.
/// @return `ZANNA_MOUSE_BUTTON_X1`.
int64_t rt_mouse_button_x1(void);
/// @brief Return the X2/forward mouse-button code.
/// @return `ZANNA_MOUSE_BUTTON_X2`.
int64_t rt_mouse_button_x2(void);

//=========================================================================
// Gamepad/Controller Input
//=========================================================================

//=========================================================================
// Gamepad Button Constants (Standard Gamepad Layout)
//=========================================================================

#define ZANNA_PAD_A 0      // Xbox A / PlayStation Cross
#define ZANNA_PAD_B 1      // Xbox B / PlayStation Circle
#define ZANNA_PAD_X 2      // Xbox X / PlayStation Square
#define ZANNA_PAD_Y 3      // Xbox Y / PlayStation Triangle
#define ZANNA_PAD_LB 4     // Left bumper/shoulder
#define ZANNA_PAD_RB 5     // Right bumper/shoulder
#define ZANNA_PAD_BACK 6   // Back/Select/Share
#define ZANNA_PAD_START 7  // Start/Options
#define ZANNA_PAD_LSTICK 8 // Left stick click
#define ZANNA_PAD_RSTICK 9 // Right stick click
#define ZANNA_PAD_UP 10    // D-pad up
#define ZANNA_PAD_DOWN 11  // D-pad down
#define ZANNA_PAD_LEFT 12  // D-pad left
#define ZANNA_PAD_RIGHT 13 // D-pad right
#define ZANNA_PAD_GUIDE 14 // Xbox button / PlayStation button
#define ZANNA_PAD_BUTTON_MAX 15

// Maximum number of supported controllers
#define ZANNA_PAD_MAX 4

//=========================================================================
// Gamepad State Management
//=========================================================================

/// @brief Initialize the gamepad input system.
/// @details Called internally when Canvas is created.
void rt_pad_init(void);

/// @brief Reset gamepad state for new frame.
/// @details Called by Canvas.Poll() to clear pressed/released lists.
void rt_pad_begin_frame(void);

/// @brief Poll connected gamepads and update state.
/// @details Should be called each frame to detect hot-plug events.
void rt_pad_poll(void);

//=========================================================================
// Controller Enumeration
//=========================================================================

/// @brief Get number of connected controllers.
/// @return Number of connected controllers (0-4).
int64_t rt_pad_count(void);

/// @brief Check if a controller is connected.
/// @param index Controller index (0-3).
/// @return 1 if connected, 0 otherwise.
int8_t rt_pad_is_connected(int64_t index);

/// @brief Get controller name/description.
/// @param index Controller index (0-3).
/// @return Controller name string, or empty string if not connected.
rt_string rt_pad_name(int64_t index);

//=========================================================================
// Button State (Polling)
//=========================================================================

/// @brief Check if a button is currently pressed.
/// @param index Controller index (0-3).
/// @param button Button constant (ZANNA_PAD_*).
/// @return 1 if pressed, 0 otherwise.
int8_t rt_pad_is_down(int64_t index, int64_t button);

/// @brief Check if a button is currently released.
/// @param index Controller index (0-3).
/// @param button Button constant (ZANNA_PAD_*).
/// @return 1 if released, 0 otherwise.
int8_t rt_pad_is_up(int64_t index, int64_t button);

//=========================================================================
// Button Events (Since Last Poll)
//=========================================================================

/// @brief Check if a button was pressed this frame.
/// @param index Controller index (0-3).
/// @param button Button constant.
/// @return 1 if pressed this frame, 0 otherwise.
int8_t rt_pad_was_pressed(int64_t index, int64_t button);

/// @brief Check if a button was released this frame.
/// @param index Controller index (0-3).
/// @param button Button constant.
/// @return 1 if released this frame, 0 otherwise.
int8_t rt_pad_was_released(int64_t index, int64_t button);

//=========================================================================
// Analog Inputs
//=========================================================================

/// @brief Get left stick X axis value.
/// @param index Controller index (0-3).
/// @return Value from -1.0 to 1.0 (left to right).
double rt_pad_left_x(int64_t index);

/// @brief Get left stick Y axis value.
/// @param index Controller index (0-3).
/// @return Value from -1.0 to 1.0 (up to down).
double rt_pad_left_y(int64_t index);

/// @brief Get right stick X axis value.
/// @param index Controller index (0-3).
/// @return Value from -1.0 to 1.0 (left to right).
double rt_pad_right_x(int64_t index);

/// @brief Get right stick Y axis value.
/// @param index Controller index (0-3).
/// @return Value from -1.0 to 1.0 (up to down).
double rt_pad_right_y(int64_t index);

/// @brief Get left trigger value.
/// @param index Controller index (0-3).
/// @return Value from 0.0 to 1.0 (released to fully pressed).
double rt_pad_left_trigger(int64_t index);

/// @brief Get right trigger value.
/// @param index Controller index (0-3).
/// @return Value from 0.0 to 1.0 (released to fully pressed).
double rt_pad_right_trigger(int64_t index);

//=========================================================================
// Deadzone Handling
//=========================================================================

/// @brief Set stick deadzone radius.
/// @param radius Deadzone radius (0.0 to 1.0, default 0.1).
void rt_pad_set_deadzone(double radius);

/// @brief Get current deadzone radius.
/// @return Current deadzone radius.
double rt_pad_get_deadzone(void);

//=========================================================================
// Vibration/Rumble
//=========================================================================

/// @brief Set controller vibration.
/// @param index Controller index (0-3).
/// @param left_motor Left motor intensity (0.0 to 1.0).
/// @param right_motor Right motor intensity (0.0 to 1.0).
void rt_pad_vibrate(int64_t index, double left_motor, double right_motor);

/// @brief Stop controller vibration.
/// @param index Controller index (0-3).
void rt_pad_stop_vibration(int64_t index);

//=========================================================================
// Button Constant Getters (for runtime.def)
//
/// @brief Runtime accessors returning the integer code for each gamepad
///        button. The name is the button (`rt_pad_button_a`/`b`/`x`/`y` face
///        buttons, `lb`/`rb` shoulders, `lstick`/`rstick` stick clicks,
///        `up`/`down`/`left`/`right` d-pad, `back`/`start`/`guide`); the code
///        is what rt_pad_* queries expect.
//=========================================================================

/// @brief Return the south/A face-button code.
/// @return `ZANNA_PAD_A`.
int64_t rt_pad_button_a(void);
/// @brief Return the east/B face-button code.
/// @return `ZANNA_PAD_B`.
int64_t rt_pad_button_b(void);
/// @brief Return the west/X face-button code.
/// @return `ZANNA_PAD_X`.
int64_t rt_pad_button_x(void);
/// @brief Return the north/Y face-button code.
/// @return `ZANNA_PAD_Y`.
int64_t rt_pad_button_y(void);
/// @brief Return the left-bumper code.
/// @return `ZANNA_PAD_LB`.
int64_t rt_pad_button_lb(void);
/// @brief Return the right-bumper code.
/// @return `ZANNA_PAD_RB`.
int64_t rt_pad_button_rb(void);
/// @brief Return the Back/Select button code.
/// @return `ZANNA_PAD_BACK`.
int64_t rt_pad_button_back(void);
/// @brief Return the Start/Options button code.
/// @return `ZANNA_PAD_START`.
int64_t rt_pad_button_start(void);
/// @brief Return the left-stick-click code.
/// @return `ZANNA_PAD_LSTICK`.
int64_t rt_pad_button_lstick(void);
/// @brief Return the right-stick-click code.
/// @return `ZANNA_PAD_RSTICK`.
int64_t rt_pad_button_rstick(void);
/// @brief Return the D-pad Up code.
/// @return `ZANNA_PAD_UP`.
int64_t rt_pad_button_up(void);
/// @brief Return the D-pad Down code.
/// @return `ZANNA_PAD_DOWN`.
int64_t rt_pad_button_down(void);
/// @brief Return the D-pad Left code.
/// @return `ZANNA_PAD_LEFT`.
int64_t rt_pad_button_left(void);
/// @brief Return the D-pad Right code.
/// @return `ZANNA_PAD_RIGHT`.
int64_t rt_pad_button_right(void);
/// @brief Return the platform Guide button code.
/// @return `ZANNA_PAD_GUIDE`.
int64_t rt_pad_button_guide(void);

#ifdef __cplusplus
}
#endif
