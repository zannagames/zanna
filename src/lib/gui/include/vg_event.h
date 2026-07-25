//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_event.h
// Purpose: Event system for widget interaction — input, focus, and widget events.
//          Defines vg_event_t, all event type tags, modifier flags, key codes,
//          and the dispatch/translation API.
// Key invariants:
//   - Setting event->handled = true stops bubbling immediately.
//   - Mouse coordinates in the mouse payload are relative to the target widget.
//   - Events are value types; no heap allocation is required.
// Ownership/Lifetime:
//   - vg_event_t is stack-allocated by callers and passed by pointer to dispatch.
// Links: lib/gui/src/core/vg_event.c,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// @file
/// @brief Declares GUI event values, key and modifier codes, translation, and dispatch.
/// @details Events are self-contained values that can be routed through a widget tree. Mouse
/// dispatch performs hit testing, keyboard dispatch targets focus, and unhandled events bubble
/// through live ancestors while per-root interaction state remains isolated.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Forward Declarations
//=============================================================================

/// @brief Opaque widget type used as an event target and dispatch root.
typedef struct vg_widget vg_widget_t;

//=============================================================================
// Event Types
//=============================================================================

/// @brief Enumerates every kind of event the GUI system can produce.
///
/// @details Grouped into mouse events, keyboard events, focus events,
///          widget-specific value-change events, and window-level events.
typedef enum vg_event_type {
    VG_EVENT_NONE = -1, ///< No GUI event was produced.

    // Mouse events
    VG_EVENT_MOUSE_MOVE,   ///< Mouse cursor moved (no button state change).
    VG_EVENT_MOUSE_DOWN,   ///< A mouse button was pressed.
    VG_EVENT_MOUSE_UP,     ///< A mouse button was released.
    VG_EVENT_MOUSE_ENTER,  ///< Mouse cursor entered the widget's bounds.
    VG_EVENT_MOUSE_LEAVE,  ///< Mouse cursor left the widget's bounds.
    VG_EVENT_MOUSE_WHEEL,  ///< Mouse scroll wheel was rotated.
    VG_EVENT_CLICK,        ///< Single click detected (button down + up in same widget).
    VG_EVENT_DOUBLE_CLICK, ///< Double click detected within the system's threshold.
    VG_EVENT_TRIPLE_CLICK, ///< Triple click detected within the system's threshold.

    // Keyboard events
    VG_EVENT_KEY_DOWN,           ///< A key was pressed (physical key event).
    VG_EVENT_KEY_UP,             ///< A key was released (physical key event).
    VG_EVENT_KEY_CHAR,           ///< Character input after platform key translation (e.g. IME).
    VG_EVENT_COMPOSITION_START,  ///< Native IME began preedit over a replacement range.
    VG_EVENT_COMPOSITION_UPDATE, ///< Native IME changed visible preedit text or selection.
    VG_EVENT_COMPOSITION_COMMIT, ///< Native IME committed preedit as one logical edit.
    VG_EVENT_COMPOSITION_CANCEL, ///< Native IME discarded preedit and restored editor state.

    // Focus events
    VG_EVENT_FOCUS_IN,  ///< Widget gained keyboard focus.
    VG_EVENT_FOCUS_OUT, ///< Widget lost keyboard focus.

    // Widget-specific events
    VG_EVENT_VALUE_CHANGED,     ///< A widget's value changed (slider, checkbox, etc.).
    VG_EVENT_TEXT_CHANGED,      ///< Text content of a text input changed.
    VG_EVENT_SELECTION_CHANGED, ///< Selection in a list, tree, or text changed.
    VG_EVENT_SUBMIT,            ///< Enter/Return pressed in a text input.
    VG_EVENT_CANCEL,            ///< Escape pressed, cancelling the current operation.

    // Window events (bubbled to root)
    VG_EVENT_RESIZE, ///< The window was resized.
    VG_EVENT_CLOSE,  ///< The window close button was pressed.
} vg_event_type_t;

//=============================================================================
// Mouse Buttons
//=============================================================================

/// @brief Identifies which mouse button was involved in a mouse event.
typedef enum vg_mouse_button {
    VG_MOUSE_LEFT = 0,   ///< Primary (left) mouse button.
    VG_MOUSE_RIGHT = 1,  ///< Secondary (right) mouse button.
    VG_MOUSE_MIDDLE = 2, ///< Middle (wheel) mouse button.
} vg_mouse_button_t;

//=============================================================================
// Modifier Keys
//=============================================================================

/// @brief Bit-field flags for modifier keys held during an event.
typedef enum vg_modifiers {
    VG_MOD_NONE = 0,       ///< No modifier keys held.
    VG_MOD_SHIFT = 1 << 0, ///< Shift key held.
    VG_MOD_CTRL = 1 << 1,  ///< Control key held.
    VG_MOD_ALT = 1 << 2,   ///< Alt / Option key held.
    VG_MOD_SUPER = 1 << 3, ///< Super key held (Cmd on macOS, Win on Windows).
} vg_modifiers_t;

//=============================================================================
// Key Codes
//=============================================================================

/// @brief Virtual key codes for keyboard events.
///
/// @details Printable ASCII keys use their ASCII values; function and
///          navigation keys start at 256. Special-key values are not identical
///          to VGFX key codes; use vg_key_from_vgfx_key() or
///          vg_event_from_platform() when translating platform input.
typedef enum vg_key {
    VG_KEY_UNKNOWN = -1, ///< Unknown or unmapped key.

    // Printable ASCII
    VG_KEY_SPACE = 32,         ///< Space bar.
    VG_KEY_APOSTROPHE = 39,    ///< Apostrophe / single quote.
    VG_KEY_COMMA = 44,         ///< Comma.
    VG_KEY_MINUS = 45,         ///< Minus / hyphen.
    VG_KEY_PERIOD = 46,        ///< Period / full stop.
    VG_KEY_SLASH = 47,         ///< Forward slash.
    VG_KEY_0 = 48,             ///< Digit 0.
    VG_KEY_1 = 49,             ///< Digit 1.
    VG_KEY_2 = 50,             ///< Digit 2.
    VG_KEY_3 = 51,             ///< Digit 3.
    VG_KEY_4 = 52,             ///< Digit 4.
    VG_KEY_5 = 53,             ///< Digit 5.
    VG_KEY_6 = 54,             ///< Digit 6.
    VG_KEY_7 = 55,             ///< Digit 7.
    VG_KEY_8 = 56,             ///< Digit 8.
    VG_KEY_9 = 57,             ///< Digit 9.
    VG_KEY_SEMICOLON = 59,     ///< Semicolon.
    VG_KEY_EQUAL = 61,         ///< Equals sign.
    VG_KEY_A = 65,             ///< Letter A.
    VG_KEY_B = 66,             ///< Letter B.
    VG_KEY_C = 67,             ///< Letter C.
    VG_KEY_D = 68,             ///< Letter D.
    VG_KEY_E = 69,             ///< Letter E.
    VG_KEY_F = 70,             ///< Letter F.
    VG_KEY_G = 71,             ///< Letter G.
    VG_KEY_H = 72,             ///< Letter H.
    VG_KEY_I = 73,             ///< Letter I.
    VG_KEY_J = 74,             ///< Letter J.
    VG_KEY_K = 75,             ///< Letter K.
    VG_KEY_L = 76,             ///< Letter L.
    VG_KEY_M = 77,             ///< Letter M.
    VG_KEY_N = 78,             ///< Letter N.
    VG_KEY_O = 79,             ///< Letter O.
    VG_KEY_P = 80,             ///< Letter P.
    VG_KEY_Q = 81,             ///< Letter Q.
    VG_KEY_R = 82,             ///< Letter R.
    VG_KEY_S = 83,             ///< Letter S.
    VG_KEY_T = 84,             ///< Letter T.
    VG_KEY_U = 85,             ///< Letter U.
    VG_KEY_V = 86,             ///< Letter V.
    VG_KEY_W = 87,             ///< Letter W.
    VG_KEY_X = 88,             ///< Letter X.
    VG_KEY_Y = 89,             ///< Letter Y.
    VG_KEY_Z = 90,             ///< Letter Z.
    VG_KEY_LEFT_BRACKET = 91,  ///< Left square bracket.
    VG_KEY_BACKSLASH = 92,     ///< Backslash.
    VG_KEY_RIGHT_BRACKET = 93, ///< Right square bracket.
    VG_KEY_GRAVE = 96,         ///< Grave accent / backtick.

    // Function keys
    VG_KEY_ESCAPE = 256,        ///< Escape key.
    VG_KEY_ENTER = 257,         ///< Enter / Return key.
    VG_KEY_TAB = 258,           ///< Tab key.
    VG_KEY_BACKSPACE = 259,     ///< Backspace key.
    VG_KEY_INSERT = 260,        ///< Insert key.
    VG_KEY_DELETE = 261,        ///< Delete key.
    VG_KEY_RIGHT = 262,         ///< Right arrow key.
    VG_KEY_LEFT = 263,          ///< Left arrow key.
    VG_KEY_DOWN = 264,          ///< Down arrow key.
    VG_KEY_UP = 265,            ///< Up arrow key.
    VG_KEY_PAGE_UP = 266,       ///< Page Up key.
    VG_KEY_PAGE_DOWN = 267,     ///< Page Down key.
    VG_KEY_HOME = 268,          ///< Home key.
    VG_KEY_END = 269,           ///< End key.
    VG_KEY_CAPS_LOCK = 280,     ///< Caps Lock key.
    VG_KEY_SCROLL_LOCK = 281,   ///< Scroll Lock key.
    VG_KEY_NUM_LOCK = 282,      ///< Num Lock key.
    VG_KEY_PRINT_SCREEN = 283,  ///< Print Screen key.
    VG_KEY_PAUSE = 284,         ///< Pause / Break key.
    VG_KEY_F1 = 290,            ///< F1 function key.
    VG_KEY_F2 = 291,            ///< F2 function key.
    VG_KEY_F3 = 292,            ///< F3 function key.
    VG_KEY_F4 = 293,            ///< F4 function key.
    VG_KEY_F5 = 294,            ///< F5 function key.
    VG_KEY_F6 = 295,            ///< F6 function key.
    VG_KEY_F7 = 296,            ///< F7 function key.
    VG_KEY_F8 = 297,            ///< F8 function key.
    VG_KEY_F9 = 298,            ///< F9 function key.
    VG_KEY_F10 = 299,           ///< F10 function key.
    VG_KEY_F11 = 300,           ///< F11 function key.
    VG_KEY_F12 = 301,           ///< F12 function key.
    VG_KEY_LEFT_SHIFT = 340,    ///< Left Shift key.
    VG_KEY_LEFT_CONTROL = 341,  ///< Left Control key.
    VG_KEY_LEFT_ALT = 342,      ///< Left Alt / Option key.
    VG_KEY_LEFT_SUPER = 343,    ///< Left Super / Cmd / Win key.
    VG_KEY_RIGHT_SHIFT = 344,   ///< Right Shift key.
    VG_KEY_RIGHT_CONTROL = 345, ///< Right Control key.
    VG_KEY_RIGHT_ALT = 346,     ///< Right Alt / Option key.
    VG_KEY_RIGHT_SUPER = 347,   ///< Right Super / Cmd / Win key.
} vg_key_t;

/// @brief Inline UTF-8 capacity of a toolkit composition event, including its terminator.
/// @details This matches the ZannaGFX platform event bound. Keeping composition text inline makes
///          `vg_event_t` a self-contained value with no allocation or borrowed lifetime.
enum { VG_COMPOSITION_TEXT_CAPACITY = 4096 };

//=============================================================================
// Event Structure
//=============================================================================

/// @brief A single GUI event carrying type, target, and type-specific payload.
///
/// @details The payload is stored in an anonymous union; only the member
///          corresponding to the event type is valid. Mouse events use the
///          `mouse` member, wheel events use `wheel`, keyboard events use
///          `key`, value-change events use `value`, and resize events use
///          `resize`.
typedef struct vg_event {
    vg_event_type_t type; ///< Discriminator for the event kind.
    vg_widget_t *target;  ///< Widget that generated or first received the event.
    bool handled;         ///< Set to true by a handler to stop further propagation.
    uint32_t modifiers;   ///< Bitwise OR of vg_modifiers_t flags active during the event.
    uint64_t timestamp;   ///< Event timestamp in milliseconds since an unspecified epoch.

    union {
        /// @brief Payload for mouse button and movement events.
        struct {
            float x, y;               ///< Cursor position relative to the target widget.
            float screen_x, screen_y; ///< Cursor position in screen (root) coordinates.
            vg_mouse_button_t button; ///< Which mouse button was pressed/released.
            int click_count;          ///< Number of rapid clicks (1 = single, 2 = double).
        } mouse;

        /// @brief Payload for mouse wheel (scroll) events.
        struct {
            float delta_x;            ///< Horizontal scroll amount (positive = right).
            float delta_y;            ///< Vertical scroll amount (positive = up / away from user).
            float screen_x, screen_y; ///< Cursor position in screen (root) coordinates.
        } wheel;

        /// @brief Payload for keyboard events.
        struct {
            vg_key_t key;       ///< Virtual key code.
            uint32_t codepoint; ///< Unicode codepoint (valid for VG_EVENT_KEY_CHAR).
            bool repeat;        ///< true if this is a key-repeat event (key held down).
        } key;

        /// @brief Payload for native IME composition lifecycle events.
        /// @details Text is byte-counted UTF-8 copied from ZannaGFX. Selection offsets are Unicode
        ///          codepoint indices within preedit text; replacement offsets are codepoint
        ///          indices within committed text. A replacement start of -1 instructs the
        ///          focused editor to use its current selection. TextInput converts all offsets to
        ///          extended-grapheme boundaries before applying them.
        struct {
            char text[VG_COMPOSITION_TEXT_CAPACITY]; ///< NUL-terminated preedit/commit UTF-8.
            uint32_t text_length;                    ///< UTF-8 byte count excluding the terminator.
            int32_t selection_start;    ///< Preedit selection/caret start in codepoints.
            int32_t selection_length;   ///< Preedit selected codepoint count.
            int32_t replacement_start;  ///< Committed codepoint start, or -1 for selection.
            int32_t replacement_length; ///< Committed codepoint count, or -1 with sentinel.
            bool truncated;             ///< true when the native payload exceeded inline capacity.
        } composition;

        /// @brief Payload for value-changed events from sliders, checkboxes, etc.
        struct {
            int int_value;     ///< Integer value representation.
            float float_value; ///< Floating-point value representation.
            bool bool_value;   ///< Boolean value representation.
        } value;

        /// @brief Payload for window resize events.
        struct {
            int width;  ///< New window width in pixels.
            int height; ///< New window height in pixels.
        } resize;
    };
} vg_event_t;

//=============================================================================
// Event Dispatch
//=============================================================================

/// @brief Dispatch an event into the widget tree with hit-test and bubbling.
///
/// @details For mouse events the deepest widget under the cursor is found via
///          hit testing. The event is delivered to that widget and then bubbles
///          up through ancestors until a handler sets event->handled = true or
///          the root is reached. For keyboard events the focused widget
///          receives the event first. Runtime state such as focus, modal roots,
///          input capture, hover, and click tracking is isolated per recently
///          dispatched root widget.
///
/// @param root  The root of the widget tree.
/// @param event The event to dispatch (modified in place: target and handled may change).
/// @return true if the event was handled by any widget.
bool vg_event_dispatch(vg_widget_t *root, vg_event_t *event);

/// @brief Send an event directly to a widget.
///
/// @details The event is localized to @p widget, delivered to its handler, and
///          bubbles through live ancestors until handled. Use
///          `vg_event_dispatch` for normal root-based routing and per-root
///          runtime state activation.
///
/// @param widget The widget to deliver the event to.
/// @param event  The event to send.
/// @return true if the widget handled the event.
bool vg_event_send(vg_widget_t *widget, vg_event_t *event);

/// @brief Construct a mouse event from raw parameters.
///
/// @param type      One of the VG_EVENT_MOUSE_* or VG_EVENT_CLICK types.
/// @param x         Cursor X coordinate in screen space.
/// @param y         Cursor Y coordinate in screen space.
/// @param button    The mouse button involved (or VG_MOUSE_LEFT for moves).
/// @param modifiers Bitwise OR of active modifier keys.
/// @return A fully initialised vg_event_t value.
vg_event_t vg_event_mouse(
    vg_event_type_t type, float x, float y, vg_mouse_button_t button, uint32_t modifiers);

/// @brief Construct a keyboard event from raw parameters.
///
/// @param type      One of VG_EVENT_KEY_DOWN, VG_EVENT_KEY_UP, or VG_EVENT_KEY_CHAR.
/// @param key       The virtual key code.
/// @param codepoint Unicode codepoint (meaningful only for VG_EVENT_KEY_CHAR).
/// @param modifiers Bitwise OR of active modifier keys.
/// @return A fully initialised vg_event_t value.
vg_event_t vg_event_key(vg_event_type_t type, vg_key_t key, uint32_t codepoint, uint32_t modifiers);

/// @brief Translate a VGFX key code into a GUI key code.
///
/// @details Printable ASCII codes are returned unchanged. Navigation and other
///          special keys are mapped from the VGFX numbering to vg_key_t.
///
/// @param vgfx_key Key code from a vgfx_event_t keyboard event.
/// @return Matching GUI key code, or VG_KEY_UNKNOWN for unknown sentinel input.
vg_key_t vg_key_from_vgfx_key(int32_t vgfx_key);

/// @brief Translate a platform-level event into a GUI-level vg_event_t.
///
/// @details Reads from the opaque VGFX platform event structure and maps its
///          fields into the corresponding vg_event_t representation.
///
/// @param platform_event Pointer to the platform event (e.g. vgfx_event_t*).
/// @return A fully initialised vg_event_t value.
vg_event_t vg_event_from_platform(void *platform_event);

/// @brief Clear cached dispatch state for a widget subtree.
///
/// @details This is used by the widget lifetime code when a subtree is hidden,
///          detached, or destroyed so per-root event snapshots cannot restore
///          stale focus/capture/hover pointers later.
///
/// @param widget Root of the subtree to forget.
void vg_event_forget_widget_subtree(vg_widget_t *widget);

#ifdef __cplusplus
}
#endif
