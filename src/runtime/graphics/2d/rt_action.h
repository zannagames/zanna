//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_action.h
/// @file
/// @brief Declares the main-thread named input-action mapping API.
// Purpose: Action mapping system abstracting raw input (keyboard, mouse, gamepad) into named
// actions with button and axis types, providing per-frame state queries.
//
// Key invariants:
//   - Action names are unique across all registered actions.
//   - Button and axis actions are disjoint; a name cannot be both.
//   - Axis() clamps accumulated values to [-1.0, 1.0]; AxisRaw() does not.
//   - All state queries reflect the current frame after rt_action_update.
//   - Actions and bindings enumerate newest-first because both registries use
//     head insertion.
//
// Ownership/Lifetime:
//   - The action system is globally initialized with rt_action_init and shutdown with
//   rt_action_shutdown.
//   - Definition APIs borrow action-name strings and copy their bytes into
//     private malloc-owned storage; they do not retain the input handles.
//   - Returned sequences and nonempty runtime strings are owned unless a
//     function documents the immortal empty-string fallback.
//
// Links: src/runtime/graphics/2d/rt_action.c (core),
//        src/runtime/graphics/2d/rt_action_io.c (persistence),
//        src/runtime/graphics/2d/rt_action_presets.c (presets)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Axis Constants (for gamepad analog bindings)
//=========================================================================

/// @brief Physical gamepad-axis identifiers accepted by pad-axis bindings.
typedef enum {
    /// Left stick horizontal axis.
    ZANNA_AXIS_LEFT_X = 0,
    /// Left stick vertical axis.
    ZANNA_AXIS_LEFT_Y = 1,
    /// Right stick horizontal axis.
    ZANNA_AXIS_RIGHT_X = 2,
    /// Right stick vertical axis.
    ZANNA_AXIS_RIGHT_Y = 3,
    /// Left analog trigger.
    ZANNA_AXIS_LEFT_TRIGGER = 4,
    /// Right analog trigger.
    ZANNA_AXIS_RIGHT_TRIGGER = 5,
    /// Exclusive upper bound; not a bindable axis.
    ZANNA_AXIS_MAX = 6,
} zanna_axis_t;

//=========================================================================
// Action System Lifecycle
//=========================================================================

/// @brief Initialize the action mapping system.
/// @details The first call creates an empty registry. Repeated calls are
///          idempotent and preserve existing definitions. Called automatically
///          when Canvas is created and lazily by definition/preset/load APIs.
void rt_action_init(void);

/// @brief Shutdown the action mapping system and free all resources.
/// @details Clears every action and binding, then marks the registry
///          uninitialized. Safe to call repeatedly.
void rt_action_shutdown(void);

/// @brief Update action states for new frame.
/// @details Called by Canvas.Poll() after input devices are updated. Rebuilds
///          all pressed, released, held, and axis caches from the current
///          device snapshot. An uninitialized registry is unchanged.
void rt_action_update(void);

/// @brief Clear all defined actions and bindings.
/// @details Leaves the registry initialized so new actions can be defined
///          immediately.
void rt_action_clear(void);

//=========================================================================
// Action Definition
//=========================================================================

/// @brief Define a new button action.
/// @details Copies the nonempty name and initializes all cached state to zero.
/// @param name Borrowed action name (for example, `"jump"` or `"pause"`).
/// @return `1` on success; `0` if the name is invalid/duplicate or allocation
///         fails.
int8_t rt_action_define(rt_string name);

/// @brief Define a new axis action.
/// @details Copies the nonempty name and initializes the accumulated value to
///          zero.
/// @param name Borrowed action name (for example, `"move_x"` or `"look_y"`).
/// @return `1` on success; `0` if the name is invalid/duplicate or allocation
///         fails.
int8_t rt_action_define_axis(rt_string name);

/// @brief Check if an action is defined.
/// @param name Borrowed action name to search for.
/// @return `1` for an exact match; otherwise `0`.
int8_t rt_action_exists(rt_string name);

/// @brief Check if an action is an axis action.
/// @param name Borrowed action name to query.
/// @return `1` if the action is axis-style; `0` for button or missing actions.
int8_t rt_action_is_axis(rt_string name);

/// @brief Remove an action and all its bindings.
/// @param name Borrowed action name to remove.
/// @return `1` if removed; `0` if not found.
int8_t rt_action_remove(rt_string name);

//=========================================================================
// Keyboard Bindings
//=========================================================================

/// @brief Bind a keyboard key to a button action.
/// @details Duplicate bindings are permitted and evaluated independently.
/// @param action Borrowed button-action name.
/// @param key Key code (ZANNA_KEY_*).
/// @return `1` on success; `0` if the action is missing/axis-style or
///         allocation fails.
int8_t rt_action_bind_key(rt_string action, int64_t key);

/// @brief Bind a keyboard key to an axis action with a specific value.
/// @details The configured contribution is added on every update while the key
///          is held. It is not validated or clamped before accumulation.
/// @param action Borrowed axis-action name.
/// @param key Key code.
/// @param value Axis contribution while held, conventionally in -1.0..1.0.
/// @return `1` on success; `0` if the action is missing/button-style or
///         allocation fails.
int8_t rt_action_bind_key_axis(rt_string action, int64_t key, double value);

/// @brief Unbind a keyboard key from an action.
/// @details Removes only the newest matching binding if duplicates exist.
/// @param action Borrowed button- or axis-action name.
/// @param key Key code.
/// @return `1` if unbound; `0` if the action or binding is not found.
int8_t rt_action_unbind_key(rt_string action, int64_t key);

//=========================================================================
// Mouse Bindings
//=========================================================================

/// @brief Bind a mouse button to a button action.
/// @param action Borrowed button-action name.
/// @param button Mouse button (ZANNA_MOUSE_BUTTON_*).
/// @return `1` on success; `0` if the action is missing/axis-style or
///         allocation fails.
int8_t rt_action_bind_mouse(rt_string action, int64_t button);

/// @brief Unbind a mouse button from an action.
/// @details Removes only the newest matching binding if duplicates exist.
/// @param action Borrowed action name.
/// @param button Mouse button.
/// @return `1` if unbound; `0` if the action or binding is not found.
int8_t rt_action_unbind_mouse(rt_string action, int64_t button);

/// @brief Bind mouse X delta to an axis action.
/// @param action Borrowed axis-action name.
/// @param sensitivity Unvalidated multiplier for per-frame horizontal pixel delta.
/// @return `1` on success; `0` if the action is missing/button-style or
///         allocation fails.
int8_t rt_action_bind_mouse_x(rt_string action, double sensitivity);

/// @brief Bind mouse Y delta to an axis action.
/// @param action Borrowed axis-action name.
/// @param sensitivity Unvalidated multiplier for per-frame vertical pixel delta.
/// @return `1` on success; `0` if the action is missing/button-style or
///         allocation fails.
int8_t rt_action_bind_mouse_y(rt_string action, double sensitivity);

/// @brief Bind mouse scroll X to an axis action.
/// @param action Borrowed axis-action name.
/// @param sensitivity Unvalidated multiplier for horizontal wheel delta.
/// @return `1` on success; `0` if the action is missing/button-style or
///         allocation fails.
int8_t rt_action_bind_scroll_x(rt_string action, double sensitivity);

/// @brief Bind mouse scroll Y to an axis action.
/// @param action Borrowed axis-action name.
/// @param sensitivity Unvalidated multiplier for vertical wheel delta.
/// @return `1` on success; `0` if the action is missing/button-style or
///         allocation fails.
int8_t rt_action_bind_scroll_y(rt_string action, double sensitivity);

//=========================================================================
// Gamepad Bindings
//=========================================================================

/// @brief Bind a gamepad button to a button action.
/// @param action Borrowed button-action name.
/// @param pad_index Controller index (0-3), or -1 for any controller.
/// @param button Gamepad button (ZANNA_PAD_*).
/// @return `1` on success; `0` if the action is missing/axis-style or
///         allocation fails.
int8_t rt_action_bind_pad_button(rt_string action, int64_t pad_index, int64_t button);

/// @brief Unbind a gamepad button from an action.
/// @details Controller index and button must exactly match the stored binding;
///          only the newest match is removed.
/// @param action Borrowed action name.
/// @param pad_index Stored controller index to match.
/// @param button Gamepad button.
/// @return `1` if unbound; `0` if the action or binding is not found.
int8_t rt_action_unbind_pad_button(rt_string action, int64_t pad_index, int64_t button);

/// @brief Bind a gamepad axis to an axis action.
/// @param action Borrowed axis-action name.
/// @param pad_index Controller index (0-3), or -1 for any controller.
/// @param axis Axis constant (ZANNA_AXIS_*).
/// @param scale Unvalidated multiplier for the raw value (use -1.0 to invert).
/// @return `1` on success; `0` if the action is missing/button-style or
///         allocation fails.
int8_t rt_action_bind_pad_axis(rt_string action, int64_t pad_index, int64_t axis, double scale);

/// @brief Unbind a gamepad axis from an action.
/// @details Controller index and axis must exactly match the stored binding;
///          only the newest match is removed.
/// @param action Borrowed action name.
/// @param pad_index Stored controller index to match.
/// @param axis Axis constant.
/// @return `1` if unbound; `0` if the action or binding is not found.
int8_t rt_action_unbind_pad_axis(rt_string action, int64_t pad_index, int64_t axis);

/// @brief Bind a gamepad button to an axis action with a specific value.
/// @details Adds @p value on each update while the selected button is held.
/// @param action Borrowed axis-action name.
/// @param pad_index Controller index (0-3), or -1 for any controller.
/// @param button Gamepad button.
/// @param value Unvalidated axis contribution while held.
/// @return `1` on success; `0` if the action is missing/button-style or
///         allocation fails.
int8_t rt_action_bind_pad_button_axis(rt_string action,
                                      int64_t pad_index,
                                      int64_t button,
                                      double value);

//=========================================================================
// Button Action State Queries
//=========================================================================

/// @brief Check if a button action was pressed this frame.
/// @details Any bound input edge can set the flag, even when a different
///          binding already holds the same action.
/// @param action Borrowed action name.
/// @return `1` if the most recent update observed a qualifying down edge;
///         otherwise `0`.
int8_t rt_action_pressed(rt_string action);

/// @brief Check if a button action was released this frame.
/// @details Set for a bound input's up edge or when the overall action changes
///          from held to unheld.
/// @param action Borrowed action name.
/// @return `1` if the most recent update observed or synthesized a release;
///         otherwise `0`.
int8_t rt_action_released(rt_string action);

/// @brief Check if a button action is currently held.
/// @param action Borrowed action name.
/// @return `1` if any bound button or complete chord is down; otherwise `0`.
int8_t rt_action_held(rt_string action);

/// @brief Get the "strength" of a button action.
/// @details Digital-only: button actions have no trigger-axis binding, so the
///          result is always 1.0 while held and 0.0 otherwise. Use axis actions
///          for analog input.
/// @param action Borrowed action name.
/// @return `1.0` if held; `0.0` for unheld, axis, or missing actions.
double rt_action_strength(rt_string action);

//=========================================================================
// Axis Action Queries
//=========================================================================

/// @brief Get the current value of an axis action.
/// @details Sources are summed during rt_action_update(). NaN contributions
///          remain NaN because the clamp only compares numeric bounds.
/// @param action Borrowed action name.
/// @return Combined value clamped to -1.0..1.0, or `0.0` if not found.
double rt_action_axis(rt_string action);

/// @brief Get the raw value of an axis action (not clamped).
/// @param action Borrowed action name.
/// @return Unclamped combined value, or `0.0` if not found.
double rt_action_axis_raw(rt_string action);

//=========================================================================
// Binding Introspection
//=========================================================================

/// @brief Get all defined action names.
/// @details Names are newly copied in newest-first registry order. The
///          returned sequence owns its string elements.
/// @return Owned runtime `seq<str>`.
void *rt_action_list(void);

/// @brief Get all bindings for an action as a human-readable string.
/// @details Emits a newest-first comma-separated description such as
///          `"Space, Mouse Left, Pad A, Ctrl+S"`.
/// @param action Borrowed action name.
/// @return Owned description string, or the immortal empty string if missing
///         or after a trapped allocation failure.
rt_string rt_action_bindings_str(rt_string action);

/// @brief Get the number of bindings for an action.
/// @param action Borrowed action name.
/// @return Number of bindings of every type, or `0` if not found.
int64_t rt_action_binding_count(rt_string action);

//=========================================================================
// Conflict Detection
//=========================================================================

/// @brief Check if a key is bound to any action.
/// @details Searches actions newest-first and includes key-to-axis bindings.
/// @param key Key code.
/// @return Newly owned action name if bound; otherwise the immortal empty string.
rt_string rt_action_key_bound_to(int64_t key);

/// @brief Check if a mouse button is bound to any action.
/// @param button Mouse button.
/// @return Newly owned newest matching action name; otherwise the immortal
///         empty string.
rt_string rt_action_mouse_bound_to(int64_t button);

/// @brief Check if a gamepad button is bound to any action.
/// @details Matches button and button-to-axis bindings. A binding stored for
///          pad `-1` matches any concrete controller query.
/// @param pad_index Controller index to search for.
/// @param button Gamepad button.
/// @return Newly owned newest matching action name; otherwise the immortal
///         empty string.
rt_string rt_action_pad_button_bound_to(int64_t pad_index, int64_t button);

//=========================================================================
// Persistence (Save/Load)
//=========================================================================

/// @brief Serialize all actions and bindings to a JSON string.
/// @details Preserves current newest-first action and binding order and emits
///          chord key arrays where applicable.
/// @return Newly owned JSON string representing the current configuration.
///         Builder failure returns an owned empty string when possible; final
///         string allocation failure can return `NULL`.
rt_string rt_action_save(void);

/// @brief Load actions and bindings from a JSON string.
/// @details Parses into a temporary registry and replaces existing actions only
///          after the complete document succeeds. Failure restores the prior
///          registry unchanged. Unknown object members and binding types are
///          skipped. Individual action/binding allocation failures can omit
///          entries without failing an otherwise valid parse.
/// @param json Borrowed JSON string, normally produced by rt_action_save().
/// @return `1` when a top-level actions array is fully loaded; `0` on null,
///         parse, shape, name/chord bound, parser, or pending-buffer failure.
int8_t rt_action_load(rt_string json);

//=========================================================================
// Key Chord/Combo Bindings
//=========================================================================

/// @brief Bind a key chord (multi-key combo) to a button action.
/// @details A chord triggers when ALL specified keys are held simultaneously.
///          At least one chord key must be newly pressed in that frame for the
///          action to register as pressed. Example: Ctrl+Shift+S.
/// @param action Borrowed button-action name.
/// @param keys Borrowed sequence containing two through eight boxed int64 key
///        codes in the order to preserve.
/// @return `1` on success; `0` if the action is missing/axis-style, the
///         sequence is invalid, or allocation fails.
int8_t rt_action_bind_chord(rt_string action, void *keys);

/// @brief Unbind a key chord from an action.
/// @details Removes the newest chord with exactly the same length and key
///          order.
/// @param action Borrowed action name.
/// @param keys Borrowed sequence of two through eight boxed key codes.
/// @return `1` if unbound; `0` if invalid or not found.
int8_t rt_action_unbind_chord(rt_string action, void *keys);

/// @brief Get the number of chord bindings for an action.
/// @param action Borrowed action name.
/// @return Number of chord bindings, or `0` if the action is missing.
int64_t rt_action_chord_count(rt_string action);

//=========================================================================
// Preset Loading
//=========================================================================

/// @brief Load a predefined set of actions with standard bindings.
/// @details Recognized presets overlay the existing registry rather than
///          clearing it. Existing names are reused, and calling a preset again
///          can append duplicate bindings. The function initializes the
///          registry lazily.
/// @param preset_name Borrowed name: `"standard_movement"`,
///        `"menu_navigation"`, `"platformer"`, `"topdown"`, or `"fps3d"`.
/// @return `1` when the preset name is recognized; `0` for null, empty, or
///         unknown names.
int8_t rt_action_load_preset(rt_string preset_name);

//=========================================================================
// Axis Constant Getters (for runtime.def)
//
/// @brief Runtime accessors returning the integer code for each analog axis.
///        The name is the axis (`left_x`/`left_y`/`right_x`/`right_y` sticks,
///        `left_trigger`/`right_trigger`); the code is what rt_action_axis()
///        and binding APIs expect.
//=========================================================================

/// @brief Return the left-stick horizontal-axis identifier.
/// @return `ZANNA_AXIS_LEFT_X`.
int64_t rt_action_axis_left_x(void);

/// @brief Return the left-stick vertical-axis identifier.
/// @return `ZANNA_AXIS_LEFT_Y`.
int64_t rt_action_axis_left_y(void);

/// @brief Return the right-stick horizontal-axis identifier.
/// @return `ZANNA_AXIS_RIGHT_X`.
int64_t rt_action_axis_right_x(void);

/// @brief Return the right-stick vertical-axis identifier.
/// @return `ZANNA_AXIS_RIGHT_Y`.
int64_t rt_action_axis_right_y(void);

/// @brief Return the left-trigger axis identifier.
/// @return `ZANNA_AXIS_LEFT_TRIGGER`.
int64_t rt_action_axis_left_trigger(void);

/// @brief Return the right-trigger axis identifier.
/// @return `ZANNA_AXIS_RIGHT_TRIGGER`.
int64_t rt_action_axis_right_trigger(void);

#ifdef __cplusplus
}
#endif
