//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/input/rt_inputmgr.c
// Purpose: High-level input manager that aggregates keyboard, mouse, and gamepad
//   state from the lower-level input layers. Provides per-key debounce filtering
//   (configurable delay in frames) to suppress rapid re-trigger on held keys.
//   Acts as a convenience wrapper: most methods delegate directly to the global
//   rt_input / rt_pad state and apply debounce tracking on top.
//
// Key invariants:
//   - rt_inputmgr_update() must be called once per frame to decrement debounce
//     timers; failing to call it freezes debounce state.
//   - Debounce tracking is limited to MAX_DEBOUNCE_KEYS (32) simultaneous keys.
//     When full, the least-protected slot (expired first, otherwise smallest
//     remaining timer) is reused for the newly queried key.
//   - Debounce delay defaults to 12 frames (~200 ms at 60 fps); configurable
//     per instance via rt_inputmgr_set_debounce_delay().
//   - Non-debounced query methods (key_pressed, key_held, etc.) pass through
//     directly to the global input state without any filtering.
//
// Ownership/Lifetime:
//   - rt_inputmgr instances are allocated via rt_obj_new_i64 (GC heap);
//     rt_inputmgr_destroy is a no-op (GC handles reclamation).
//
// Links: src/runtime/graphics/input/rt_inputmgr.h (public API),
//        src/runtime/graphics/input/rt_input.h (keyboard/mouse/gamepad global state)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements debounced and unified high-level input queries.
 *
 * @details Each GC-managed instance tracks bounded per-key debounce timers,
 *          while ordinary keyboard, mouse, and gamepad queries delegate to
 *          frame-coherent global input state. Unified helpers merge digital
 *          navigation and analog stick input into common actions and axes.
 */

#include "rt_inputmgr.h"
#include "rt_input.h"
#include "rt_object.h"
#include "rt_trap.h"
#include <stdlib.h>
#include <string.h>

/// @brief Maximum number of simultaneous per-manager debounce entries.
#define MAX_DEBOUNCE_KEYS 32

/// @brief Private bounded debounce state for one InputManager.
struct rt_inputmgr_impl {
    int64_t debounce_delay;                     ///< Frames to wait for debounce.
    int64_t debounce_timers[MAX_DEBOUNCE_KEYS]; ///< Per-key debounce timers.
    int64_t debounce_keys[MAX_DEBOUNCE_KEYS];   ///< Key codes being debounced.
    int64_t debounce_count;                     ///< Number of keys being tracked.
};

/// @brief Create a new inputmgr object.
/// @return New GC-managed InputManager handle, or NULL after a recoverable allocation trap.
rt_inputmgr rt_inputmgr_new(void) {
    struct rt_inputmgr_impl *mgr =
        (struct rt_inputmgr_impl *)rt_obj_new_i64(0, (int64_t)sizeof(struct rt_inputmgr_impl));
    if (!mgr) {
        // rt_obj_new_i64 already traps on OOM, but rt_trap is not _Noreturn under
        // recoverable test hooks, so guard the NULL return the header documents
        // rather than dereferencing a null handle below.
        rt_trap("InputManager: allocation failed");
        return NULL;
    }

    mgr->debounce_delay = 12; // Default: 12 frames (~200ms at 60fps)
    mgr->debounce_count = 0;
    memset(mgr->debounce_timers, 0, sizeof(mgr->debounce_timers));
    memset(mgr->debounce_keys, 0, sizeof(mgr->debounce_keys));

    return mgr;
}

/// @brief Compatibility destroy operation for a GC-managed InputManager.
/// @details Reclamation remains owned by the runtime object manager, so this call is a no-op.
/// @param mgr InputManager handle; ignored.
void rt_inputmgr_destroy(rt_inputmgr mgr) {
    (void)mgr;
}

/// @brief Update the inputmgr state (called per frame/tick).
/// @details Decrements each positive debounce timer by one frame; NULL managers are ignored.
/// @param mgr InputManager handle.
void rt_inputmgr_update(rt_inputmgr mgr) {
    if (!mgr)
        return;

    // Decrement all debounce timers
    for (int64_t i = 0; i < mgr->debounce_count; i++) {
        if (mgr->debounce_timers[i] > 0) {
            mgr->debounce_timers[i]--;
        }
    }
}

//=============================================================================
// Keyboard
//=============================================================================

/// @brief Check whether a keyboard key was pressed this frame (edge-triggered).
/// @param mgr InputManager handle; unused because keyboard state is process-global.
/// @param key Public key code.
/// @return Global keyboard press edge for @p key.
int8_t rt_inputmgr_key_pressed(rt_inputmgr mgr, int64_t key) {
    (void)mgr; // Uses global keyboard state
    return rt_keyboard_was_pressed(key);
}

/// @brief Check whether a keyboard key was released this frame (edge-triggered).
/// @param mgr InputManager handle; unused because keyboard state is process-global.
/// @param key Public key code.
/// @return Global keyboard release edge for @p key.
int8_t rt_inputmgr_key_released(rt_inputmgr mgr, int64_t key) {
    (void)mgr;
    return rt_keyboard_was_released(key);
}

/// @brief Check whether a keyboard key is currently held down (continuous).
/// @param mgr InputManager handle; unused because keyboard state is process-global.
/// @param key Public key code.
/// @return One while @p key is held, otherwise zero.
int8_t rt_inputmgr_key_held(rt_inputmgr mgr, int64_t key) {
    (void)mgr;
    return rt_keyboard_is_down(key);
}

/// @brief Find the debounce slot index for `key`, creating or reusing a slot if none exists.
/// @details When the fixed table is full, an expired timer is reused first. If every tracked key
///          is still debounced, the slot with the smallest remaining timer is reused because it is
///          closest to becoming eligible again. This makes saturation deterministic and avoids the
///          older behavior where new keys could be ignored or the most-protected slot was evicted.
/// @param mgr Live InputManager handle.
/// @param key Public key code to find or begin tracking.
/// @return Debounce-table index assigned to @p key.
static int64_t find_or_create_debounce_slot(rt_inputmgr mgr, int64_t key) {
    // Look for existing slot
    for (int64_t i = 0; i < mgr->debounce_count; i++) {
        if (mgr->debounce_keys[i] == key) {
            return i;
        }
    }

    // Create new slot if space available
    if (mgr->debounce_count < MAX_DEBOUNCE_KEYS) {
        int64_t slot = mgr->debounce_count++;
        mgr->debounce_keys[slot] = key;
        mgr->debounce_timers[slot] = 0;
        return slot;
    }

    // No space - evict an expired slot, otherwise the one closest to expiry.
    int64_t reuse_slot = 0;
    int64_t shortest_time = mgr->debounce_timers[0];
    for (int64_t i = 1; i < mgr->debounce_count; i++) {
        if (mgr->debounce_timers[i] < shortest_time) {
            shortest_time = mgr->debounce_timers[i];
            reuse_slot = i;
        }
    }
    mgr->debounce_keys[reuse_slot] = key;
    mgr->debounce_timers[reuse_slot] = 0;
    return reuse_slot;
}

/// @brief Check whether a key was pressed with debounce filtering (prevents rapid re-triggers).
/// @details An eligible press starts the configured timer. Releasing the key resets its timer so a
///          later physical press can trigger immediately.
/// @param mgr InputManager handle.
/// @param key Public key code.
/// @return One for an eligible press edge, otherwise zero.
int8_t rt_inputmgr_key_pressed_debounced(rt_inputmgr mgr, int64_t key) {
    if (!mgr)
        return 0;

    int64_t slot = find_or_create_debounce_slot(mgr, key);

    // Check if debounce timer has expired and the key transitioned down this frame.
    if (mgr->debounce_timers[slot] == 0 && rt_keyboard_was_pressed(key)) {
        mgr->debounce_timers[slot] = mgr->debounce_delay;
        return 1;
    }

    // If key is released, reset timer so next press is immediate
    if (!rt_keyboard_is_down(key)) {
        mgr->debounce_timers[slot] = 0;
    }

    return 0;
}

/// @brief Set the debounce delay in frames (minimum frames between repeated key presses).
/// @param mgr InputManager handle; NULL is ignored.
/// @param frames Non-negative debounce duration; negative values are ignored.
void rt_inputmgr_set_debounce_delay(rt_inputmgr mgr, int64_t frames) {
    if (mgr && frames >= 0) {
        mgr->debounce_delay = frames;
    }
}

/// @brief Get the current debounce delay in frames.
/// @param mgr InputManager handle.
/// @return Configured frame count, or zero for NULL.
int64_t rt_inputmgr_get_debounce_delay(rt_inputmgr mgr) {
    return mgr ? mgr->debounce_delay : 0;
}

//=============================================================================
// Mouse
//=============================================================================

/// @brief Check whether a mouse button was pressed this frame.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @param button Public mouse-button code.
/// @return Global mouse press edge for @p button.
int8_t rt_inputmgr_mouse_pressed(rt_inputmgr mgr, int64_t button) {
    (void)mgr;
    return rt_mouse_was_pressed(button);
}

/// @brief Check whether a mouse button was released this frame.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @param button Public mouse-button code.
/// @return Global mouse release edge for @p button.
int8_t rt_inputmgr_mouse_released(rt_inputmgr mgr, int64_t button) {
    (void)mgr;
    return rt_mouse_was_released(button);
}

/// @brief Check whether a mouse button is currently held down.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @param button Public mouse-button code.
/// @return One while @p button is held, otherwise zero.
int8_t rt_inputmgr_mouse_held(rt_inputmgr mgr, int64_t button) {
    (void)mgr;
    return rt_mouse_is_down(button);
}

/// @brief Get the current mouse X position in window coordinates.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Current logical canvas X coordinate.
int64_t rt_inputmgr_mouse_x(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_x();
}

/// @brief Get the current mouse Y position in window coordinates.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Current logical canvas Y coordinate.
int64_t rt_inputmgr_mouse_y(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_y();
}

/// @brief Get the mouse X movement since the last frame.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Horizontal integer delta in logical canvas pixels.
int64_t rt_inputmgr_mouse_delta_x(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_delta_x();
}

/// @brief Get the mouse Y movement since the last frame.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Vertical integer delta in logical canvas pixels.
int64_t rt_inputmgr_mouse_delta_y(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_delta_y();
}

/// @brief Get the vertical scroll wheel delta this frame.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Whole-step vertical wheel delta.
int64_t rt_inputmgr_scroll_y(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_wheel_y();
}

/// @brief Get the horizontal scroll wheel delta this frame.
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Whole-step horizontal wheel delta.
int64_t rt_inputmgr_scroll_x(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_wheel_x();
}

/// @brief Return the vertical scroll delta this frame as a float (positive = scroll up).
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Full-precision vertical wheel delta.
double rt_inputmgr_scroll_yf(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_wheel_yf();
}

/// @brief Return the horizontal scroll delta this frame as a float (positive = scroll right).
/// @param mgr InputManager handle; unused because mouse state is process-global.
/// @return Full-precision horizontal wheel delta.
double rt_inputmgr_scroll_xf(rt_inputmgr mgr) {
    (void)mgr;
    return rt_mouse_wheel_xf();
}

//=============================================================================
// Gamepad
//=============================================================================

/// @brief Check whether a gamepad button was pressed this frame.
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot, or -1 to query every connected controller.
/// @param button Public gamepad-button code.
/// @return One when the requested button gained its down state, otherwise zero.
int8_t rt_inputmgr_pad_pressed(rt_inputmgr mgr, int64_t pad, int64_t button) {
    (void)mgr;

    if (pad == -1) {
        // Check any gamepad
        for (int64_t i = 0; i < 4; i++) {
            if (rt_pad_is_connected(i) && rt_pad_was_pressed(i, button)) {
                return 1;
            }
        }
        return 0;
    }

    return rt_pad_was_pressed(pad, button);
}

/// @brief Check whether a gamepad button was released this frame.
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot, or -1 to query every connected controller.
/// @param button Public gamepad-button code.
/// @return One when the requested button lost its down state, otherwise zero.
int8_t rt_inputmgr_pad_released(rt_inputmgr mgr, int64_t pad, int64_t button) {
    (void)mgr;

    if (pad == -1) {
        for (int64_t i = 0; i < 4; i++) {
            if (rt_pad_is_connected(i) && rt_pad_was_released(i, button)) {
                return 1;
            }
        }
        return 0;
    }

    return rt_pad_was_released(pad, button);
}

/// @brief Check whether a gamepad button is currently held down.
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot, or -1 to query every connected controller.
/// @param button Public gamepad-button code.
/// @return One while the requested button is held, otherwise zero.
int8_t rt_inputmgr_pad_held(rt_inputmgr mgr, int64_t pad, int64_t button) {
    (void)mgr;

    if (pad == -1) {
        for (int64_t i = 0; i < 4; i++) {
            if (rt_pad_is_connected(i) && rt_pad_is_down(i, button)) {
                return 1;
            }
        }
        return 0;
    }

    return rt_pad_is_down(pad, button);
}

/// @brief Get the gamepad left stick X axis value.
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot.
/// @return Deadzone-adjusted left-stick X component, or zero when unavailable.
double rt_inputmgr_pad_left_x(rt_inputmgr mgr, int64_t pad) {
    (void)mgr;
    return rt_pad_left_x(pad);
}

/// @brief Get the gamepad left stick Y axis value.
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot.
/// @return Deadzone-adjusted left-stick Y component, or zero when unavailable.
double rt_inputmgr_pad_left_y(rt_inputmgr mgr, int64_t pad) {
    (void)mgr;
    return rt_pad_left_y(pad);
}

/// @brief Get the gamepad right stick X axis value.
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot.
/// @return Deadzone-adjusted right-stick X component, or zero when unavailable.
double rt_inputmgr_pad_right_x(rt_inputmgr mgr, int64_t pad) {
    (void)mgr;
    return rt_pad_right_x(pad);
}

/// @brief Get the gamepad right stick Y axis value.
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot.
/// @return Deadzone-adjusted right-stick Y component, or zero when unavailable.
double rt_inputmgr_pad_right_y(rt_inputmgr mgr, int64_t pad) {
    (void)mgr;
    return rt_pad_right_y(pad);
}

/// @brief Get the gamepad left trigger value (0.0–1.0).
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot.
/// @return Normalized left-trigger value, or zero when unavailable.
double rt_inputmgr_pad_left_trigger(rt_inputmgr mgr, int64_t pad) {
    (void)mgr;
    return rt_pad_left_trigger(pad);
}

/// @brief Get the gamepad right trigger value (0.0–1.0).
/// @param mgr InputManager handle; unused because gamepad state is process-global.
/// @param pad Controller slot.
/// @return Normalized right-trigger value, or zero when unavailable.
double rt_inputmgr_pad_right_trigger(rt_inputmgr mgr, int64_t pad) {
    (void)mgr;
    return rt_pad_right_trigger(pad);
}

//=============================================================================
// Unified Direction Input
//=============================================================================

/// @brief Check unified "up" input (arrow key, W, D-pad up, or left stick up).
/// @param mgr InputManager handle; currently unused.
/// @return One while any configured upward input is active, otherwise zero.
int8_t rt_inputmgr_up(rt_inputmgr mgr) {
    (void)mgr;

    // Keyboard: Up arrow or W
    if (rt_keyboard_is_down(ZANNA_KEY_UP) || rt_keyboard_is_down(ZANNA_KEY_W)) {
        return 1;
    }

    // Gamepad: D-pad up or left stick up
    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i)) {
            if (rt_pad_is_down(i, ZANNA_PAD_UP))
                return 1;
            if (rt_pad_left_y(i) < -0.5)
                return 1;
        }
    }

    return 0;
}

/// @brief Check unified "down" input (arrow key, S, D-pad down, or left stick down).
/// @param mgr InputManager handle; currently unused.
/// @return One while any configured downward input is active, otherwise zero.
int8_t rt_inputmgr_down(rt_inputmgr mgr) {
    (void)mgr;

    if (rt_keyboard_is_down(ZANNA_KEY_DOWN) || rt_keyboard_is_down(ZANNA_KEY_S)) {
        return 1;
    }

    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i)) {
            if (rt_pad_is_down(i, ZANNA_PAD_DOWN))
                return 1;
            if (rt_pad_left_y(i) > 0.5)
                return 1;
        }
    }

    return 0;
}

/// @brief Check unified "left" input (arrow key, A, D-pad left, or left stick left).
/// @param mgr InputManager handle; currently unused.
/// @return One while any configured leftward input is active, otherwise zero.
int8_t rt_inputmgr_left(rt_inputmgr mgr) {
    (void)mgr;

    if (rt_keyboard_is_down(ZANNA_KEY_LEFT) || rt_keyboard_is_down(ZANNA_KEY_A)) {
        return 1;
    }

    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i)) {
            if (rt_pad_is_down(i, ZANNA_PAD_LEFT))
                return 1;
            if (rt_pad_left_x(i) < -0.5)
                return 1;
        }
    }

    return 0;
}

/// @brief Check unified "right" input (arrow key, D, D-pad right, or left stick right).
/// @param mgr InputManager handle; currently unused.
/// @return One while any configured rightward input is active, otherwise zero.
int8_t rt_inputmgr_right(rt_inputmgr mgr) {
    (void)mgr;

    if (rt_keyboard_is_down(ZANNA_KEY_RIGHT) || rt_keyboard_is_down(ZANNA_KEY_D)) {
        return 1;
    }

    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i)) {
            if (rt_pad_is_down(i, ZANNA_PAD_RIGHT))
                return 1;
            if (rt_pad_left_x(i) > 0.5)
                return 1;
        }
    }

    return 0;
}

/// @brief Check unified "confirm" input (Enter, Space, or gamepad A button).
/// @param mgr InputManager handle; currently unused.
/// @return One on any configured confirm press edge, otherwise zero.
int8_t rt_inputmgr_confirm(rt_inputmgr mgr) {
    (void)mgr;

    // Keyboard: Enter or Space
    if (rt_keyboard_was_pressed(ZANNA_KEY_ENTER) || rt_keyboard_was_pressed(ZANNA_KEY_SPACE)) {
        return 1;
    }

    // Gamepad: A button
    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i) && rt_pad_was_pressed(i, ZANNA_PAD_A)) {
            return 1;
        }
    }

    return 0;
}

/// @brief Check unified "cancel" input (Escape or gamepad B button).
/// @param mgr InputManager handle; currently unused.
/// @return One on any configured cancel press edge, otherwise zero.
int8_t rt_inputmgr_cancel(rt_inputmgr mgr) {
    (void)mgr;

    // Keyboard: Escape
    if (rt_keyboard_was_pressed(ZANNA_KEY_ESCAPE)) {
        return 1;
    }

    // Gamepad: B button
    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i) && rt_pad_was_pressed(i, ZANNA_PAD_B)) {
            return 1;
        }
    }

    return 0;
}

/// @brief Get the unified horizontal axis (-1.0 to 1.0 from keyboard WASD/arrows or left stick).
/// @details Uses the most extreme keyboard, D-pad, or connected left-stick contribution.
/// @param mgr InputManager handle; currently unused.
/// @return Unified horizontal value clamped to [-1,1].
double rt_inputmgr_axis_x(rt_inputmgr mgr) {
    (void)mgr;

    double value = 0.0;

    // Keyboard contribution
    if (rt_keyboard_is_down(ZANNA_KEY_LEFT) || rt_keyboard_is_down(ZANNA_KEY_A)) {
        value -= 1.0;
    }
    if (rt_keyboard_is_down(ZANNA_KEY_RIGHT) || rt_keyboard_is_down(ZANNA_KEY_D)) {
        value += 1.0;
    }

    // Gamepad contribution (use first connected pad with significant input)
    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i)) {
            double pad_x = rt_pad_left_x(i);
            if (pad_x < -0.1 || pad_x > 0.1) {
                // Use gamepad value if more extreme than keyboard
                if ((pad_x < 0 && pad_x < value) || (pad_x > 0 && pad_x > value)) {
                    value = pad_x;
                }
            }
            // D-pad
            if (rt_pad_is_down(i, ZANNA_PAD_LEFT) && value > -1.0)
                value = -1.0;
            if (rt_pad_is_down(i, ZANNA_PAD_RIGHT) && value < 1.0)
                value = 1.0;
        }
    }

    // Clamp
    if (value < -1.0)
        value = -1.0;
    if (value > 1.0)
        value = 1.0;

    return value;
}

/// @brief Get the unified vertical axis (-1.0 to 1.0 from keyboard WASD/arrows or left stick).
/// @details Uses the most extreme keyboard, D-pad, or connected left-stick contribution.
/// @param mgr InputManager handle; currently unused.
/// @return Unified vertical value clamped to [-1,1], with negative values meaning up.
double rt_inputmgr_axis_y(rt_inputmgr mgr) {
    (void)mgr;

    double value = 0.0;

    // Keyboard contribution
    if (rt_keyboard_is_down(ZANNA_KEY_UP) || rt_keyboard_is_down(ZANNA_KEY_W)) {
        value -= 1.0;
    }
    if (rt_keyboard_is_down(ZANNA_KEY_DOWN) || rt_keyboard_is_down(ZANNA_KEY_S)) {
        value += 1.0;
    }

    // Gamepad contribution
    for (int64_t i = 0; i < 4; i++) {
        if (rt_pad_is_connected(i)) {
            double pad_y = rt_pad_left_y(i);
            if (pad_y < -0.1 || pad_y > 0.1) {
                if ((pad_y < 0 && pad_y < value) || (pad_y > 0 && pad_y > value)) {
                    value = pad_y;
                }
            }
            if (rt_pad_is_down(i, ZANNA_PAD_UP) && value > -1.0)
                value = -1.0;
            if (rt_pad_is_down(i, ZANNA_PAD_DOWN) && value < 1.0)
                value = 1.0;
        }
    }

    if (value < -1.0)
        value = -1.0;
    if (value > 1.0)
        value = 1.0;

    return value;
}
