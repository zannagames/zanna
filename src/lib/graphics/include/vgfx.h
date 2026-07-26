//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaGFX Public API
//
// Provides a cross-platform software 2D graphics library with window
// management, pixel operations, drawing primitives, and event handling.
// The library implements a simple immediate-mode API where all drawing
// operations modify a software framebuffer that gets blitted to the native
// window surface on vgfx_update().
//
// Key design principles:
// - Pure software rendering (no GPU acceleration required)
// - Platform abstraction layer isolates OS-specific windowing code
// - Integer-only math for predictable, deterministic rendering
// - Direct framebuffer access for maximum flexibility
// - Thread-safe event queue for input produced by platform callbacks
//
// Supported platforms:
// - macOS (Cocoa/AppKit backend)
// - Linux (X11 backend)
// - Windows (Win32 backend)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Cross-platform software 2D graphics library public API.
/// @details Exposes window lifecycle management, drawing primitives (lines,
///          rectangles, circles), pixel operations, input polling, and event
///          handling.  All functions are safe to call from a single thread
///          (typically the main thread).  The platform backend handles OS
///          event translation and window rendering asynchronously.

#pragma once

#include "vgfx_config.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Library Version
//===----------------------------------------------------------------------===//

#define VGFX_VERSION_MAJOR 1
#define VGFX_VERSION_MINOR 0
#define VGFX_VERSION_PATCH 0

/// @brief Query the runtime library version as a packed integer.
/// @details Encodes the version as (major << 16) | (minor << 8) | patch.
///          Useful for runtime version checks and compatibility assertions.
/// @return Packed version number (e.g., 0x010000 for version 1.0.0).
uint32_t vgfx_version(void);

/// @brief Get the library version as a human-readable string.
/// @details Returns a static string in the format "major.minor.patch".
///          The string is owned by the library and remains valid for the
///          lifetime of the process.
/// @return Version string (e.g., "1.0.0"), never NULL.
const char *vgfx_version_string(void);

//===----------------------------------------------------------------------===//
// Core Data Types
//===----------------------------------------------------------------------===//

/// @brief Opaque handle to a platform window.
/// @details Internally points to a vgfx_window structure that contains the
///          framebuffer, event queue, input state, and platform-specific data.
///          All API functions accept this handle as their first parameter.
///          Windows are created via vgfx_create_window() and destroyed via
///          vgfx_destroy_window().
typedef struct vgfx_window *vgfx_window_t;

/// @brief Native window-system backend associated with a window handle.
/// @details Callers must inspect this discriminator before interpreting any
///          value returned in vgfx_native_handles_t.
typedef enum {
    VGFX_NATIVE_BACKEND_NONE = 0,
    VGFX_NATIVE_BACKEND_COCOA = 1,
    VGFX_NATIVE_BACKEND_WIN32 = 2,
    VGFX_NATIVE_BACKEND_X11 = 3,
    VGFX_NATIVE_BACKEND_WAYLAND = 4,
} vgfx_native_backend_t;

/// @brief Typed platform-native handles for graphics interoperability.
/// @details Handles are borrowed from the window and remain valid only until
///          vgfx_destroy_window(). A field is NULL when the selected backend
///          has no corresponding concept.
typedef struct {
    vgfx_native_backend_t backend; ///< Backend defining the remaining fields.
    void *display;                 ///< X11 Display* or Wayland wl_display*.
    void *surface;                 ///< Cocoa NSView*, Win32 HWND, or Wayland wl_surface*.
    uintptr_t window;              ///< Integer X11 Window; zero for pointer-handle backends.
} vgfx_native_handles_t;

/// @brief Backend features available for one live window.
/// @details Capability bits describe native behavior, not portable emulation. They may differ
///          between Wayland compositors because optional protocols are advertised at runtime.
typedef uint64_t vgfx_window_capabilities_t;

enum {
    VGFX_CAP_WINDOW_POSITION = UINT64_C(1) << 0,
    VGFX_CAP_FOCUS_REQUEST = UINT64_C(1) << 1,
    VGFX_CAP_CURSOR_WARP = UINT64_C(1) << 2,
    VGFX_CAP_RELATIVE_MOUSE = UINT64_C(1) << 3,
    VGFX_CAP_TEXT_COMPOSITION = UINT64_C(1) << 4,
    VGFX_CAP_FRACTIONAL_SCALE = UINT64_C(1) << 5,
    VGFX_CAP_SERVER_DECORATIONS = UINT64_C(1) << 6,
    VGFX_CAP_ACTIVATION = UINT64_C(1) << 7,
    VGFX_CAP_CLIPBOARD_TEXT = UINT64_C(1) << 8,
    VGFX_CAP_FILE_DROP = UINT64_C(1) << 9,
};

/// @brief 24-bit RGB color encoded in a 32-bit integer: 0x00RRGGBB.
/// @details The high byte is ignored.  Colors are internally converted to
///          32-bit RGBA with alpha = 0xFF (fully opaque) when written to the
///          framebuffer.  Use the vgfx_rgb() helper or predefined constants
///          (VGFX_RED, VGFX_GREEN, etc.) for convenience.
typedef uint32_t vgfx_color_t;

/// @brief Window creation parameters.
/// @details Configures the initial size, title, frame rate, and resizability
///          of a new window.  Invalid or zero values for width/height are
///          replaced with VGFX_DEFAULT_WIDTH and VGFX_DEFAULT_HEIGHT.
typedef struct {
    int32_t width;      ///< Window width in pixels (≤ 0 → use default)
    int32_t height;     ///< Window height in pixels (≤ 0 → use default)
    const char *title;  ///< Window title (UTF-8 string; NULL → use default)
    int32_t fps;        ///< Target FPS (< 0: unlimited, 0: default, > 0: limit)
    int32_t resizable;  ///< 0 = fixed size, non-zero = user-resizable
    int32_t fullscreen; ///< Non-zero = create fullscreen at desktop resolution
                        ///< (width/height ignored; no windowed flash)
} vgfx_window_params_t;

/// @brief Construct default window parameters.
/// @details Returns a parameter struct initialised to sensible defaults:
///          width = VGFX_DEFAULT_WIDTH, height = VGFX_DEFAULT_HEIGHT,
///          title = VGFX_DEFAULT_TITLE, fps = VGFX_DEFAULT_FPS, resizable = 0.
/// @return Default-initialised vgfx_window_params_t.
vgfx_window_params_t vgfx_window_params_default(void);

/// @brief Framebuffer descriptor for direct pixel access.
/// @details Provides raw access to the RGBA pixel buffer.  Each pixel is 4
///          bytes (RGBA order, 8 bits per channel).  The stride is always
///          width * 4.  Pixels are stored in row-major order with (0, 0) at
///          the top-left corner. The generation value changes whenever the
///          framebuffer storage is replaced, so callers that retain a pointer
///          across event/update calls can detect invalidation.
typedef struct {
    uint8_t *pixels;     ///< RGBA pixel data (4 bytes per pixel)
    int32_t width;       ///< Framebuffer width in pixels
    int32_t height;      ///< Framebuffer height in pixels
    int32_t stride;      ///< Bytes per row (always width * 4)
    uint64_t generation; ///< Monotonic storage generation; changes after resize/reallocation.
} vgfx_framebuffer_t;

/// @brief Logging callback function type.
/// @details When a log callback is installed via vgfx_set_log_callback, the
///          library forwards human-readable diagnostic messages to the client
///          for display or capture. The callback must be thread-safe.
/// @param msg Borrowed NUL-terminated diagnostic string valid only for the call.
typedef void (*vgfx_log_fn)(const char *msg);

//===----------------------------------------------------------------------===//
// Event System
//===----------------------------------------------------------------------===//

/// @brief Event type enumeration.
/// @details Identifies the kind of event in a vgfx_event_t structure.
///          Events are generated by the platform backend and placed in a
///          synchronized ring buffer for consumption by the application.
typedef enum {
    VGFX_EVENT_NONE = 0,           ///< No event (queue empty)
    VGFX_EVENT_KEY_DOWN,           ///< Keyboard key pressed
    VGFX_EVENT_KEY_UP,             ///< Keyboard key released
    VGFX_EVENT_TEXT_INPUT,         ///< Translated text input/codepoint event
    VGFX_EVENT_COMPOSITION_START,  ///< Native IME preedit session began
    VGFX_EVENT_COMPOSITION_UPDATE, ///< Native IME preedit text or selection changed
    VGFX_EVENT_COMPOSITION_COMMIT, ///< Native IME committed one atomic text edit
    VGFX_EVENT_COMPOSITION_CANCEL, ///< Native IME discarded its active preedit
    VGFX_EVENT_MOUSE_MOVE,         ///< Mouse cursor moved
    VGFX_EVENT_MOUSE_DOWN,         ///< Mouse button pressed
    VGFX_EVENT_MOUSE_UP,           ///< Mouse button released
    VGFX_EVENT_RESIZE,             ///< Window resized (framebuffer reallocated and cleared)
    VGFX_EVENT_CLOSE,              ///< Window close requested by user
    VGFX_EVENT_FOCUS_GAINED,       ///< Window gained keyboard focus
    VGFX_EVENT_FOCUS_LOST,         ///< Window lost keyboard focus
    VGFX_EVENT_SCROLL,             ///< Scroll wheel or trackpad scroll
    VGFX_EVENT_FILE_DROP,          ///< File dropped onto window (one event per file)
    VGFX_EVENT_TOUCH_DOWN,         ///< Touch contact began
    VGFX_EVENT_TOUCH_MOVE,         ///< Touch contact moved
    VGFX_EVENT_TOUCH_UP,           ///< Touch contact ended
    VGFX_EVENT_TOUCH_CANCEL        ///< Compositor cancelled the active touch sequence
} vgfx_event_type_t;

/// @brief Keyboard key codes.
/// @details Maps common keys to integer constants.  The encoding is designed
///          to be compatible with ASCII for alphanumeric keys.  Special keys
///          use values >= 256.  Not all keys are represented; unmapped keys
///          report VGFX_KEY_UNKNOWN.
typedef enum {
    VGFX_KEY_UNKNOWN = 0,

    /* Printable ASCII keys (A-Z share values with uppercase ASCII) */
    VGFX_KEY_SPACE = ' ',
    VGFX_KEY_0 = '0',
    VGFX_KEY_1,
    VGFX_KEY_2,
    VGFX_KEY_3,
    VGFX_KEY_4,
    VGFX_KEY_5,
    VGFX_KEY_6,
    VGFX_KEY_7,
    VGFX_KEY_8,
    VGFX_KEY_9,
    VGFX_KEY_A = 'A',
    VGFX_KEY_B,
    VGFX_KEY_C,
    VGFX_KEY_D,
    VGFX_KEY_E,
    VGFX_KEY_F,
    VGFX_KEY_G,
    VGFX_KEY_H,
    VGFX_KEY_I,
    VGFX_KEY_J,
    VGFX_KEY_K,
    VGFX_KEY_L,
    VGFX_KEY_M,
    VGFX_KEY_N,
    VGFX_KEY_O,
    VGFX_KEY_P,
    VGFX_KEY_Q,
    VGFX_KEY_R,
    VGFX_KEY_S,
    VGFX_KEY_T,
    VGFX_KEY_U,
    VGFX_KEY_V,
    VGFX_KEY_W,
    VGFX_KEY_X,
    VGFX_KEY_Y,
    VGFX_KEY_Z,

    /* Special keys (values >= 256) */
    VGFX_KEY_ESCAPE = 256,
    VGFX_KEY_ENTER = 257,
    VGFX_KEY_LEFT = 258,
    VGFX_KEY_RIGHT = 259,
    VGFX_KEY_UP = 260,
    VGFX_KEY_DOWN = 261,
    VGFX_KEY_BACKSPACE = 262,
    VGFX_KEY_DELETE = 263,
    VGFX_KEY_TAB = 264,
    VGFX_KEY_HOME = 265,
    VGFX_KEY_END = 266,
    VGFX_KEY_PAGE_UP = 267,
    VGFX_KEY_PAGE_DOWN = 268,

    /* Function keys */
    VGFX_KEY_F1 = 269,
    VGFX_KEY_F2 = 270,
    VGFX_KEY_F3 = 271,
    VGFX_KEY_F4 = 272,
    VGFX_KEY_F5 = 273,
    VGFX_KEY_F6 = 274,
    VGFX_KEY_F7 = 275,
    VGFX_KEY_F8 = 276,
    VGFX_KEY_F9 = 277,
    VGFX_KEY_F10 = 278,
    VGFX_KEY_F11 = 279,
    VGFX_KEY_F12 = 280
} vgfx_key_t;

/// @brief Keyboard modifier flags
typedef enum {
    VGFX_MOD_SHIFT = 1 << 0,
    VGFX_MOD_CTRL = 1 << 1,
    VGFX_MOD_ALT = 1 << 2,
    VGFX_MOD_CMD = 1 << 3 // macOS Command key
} vgfx_mod_t;

/// @brief Mouse button identifiers.
/// @details Standard three-button mouse mapping.  Additional buttons may be
///          added in future versions.
typedef enum {
    VGFX_MOUSE_LEFT = 0,  ///< Left mouse button (primary)
    VGFX_MOUSE_RIGHT = 1, ///< Right mouse button (secondary)
    VGFX_MOUSE_MIDDLE = 2 ///< Middle mouse button (tertiary)
} vgfx_mouse_button_t;

/// @brief Semantic purpose of an editable text field for native input methods.
typedef enum {
    VGFX_TEXT_INPUT_NORMAL = 0,
    VGFX_TEXT_INPUT_PASSWORD,
    VGFX_TEXT_INPUT_EMAIL,
    VGFX_TEXT_INPUT_NUMBER,
    VGFX_TEXT_INPUT_PHONE,
    VGFX_TEXT_INPUT_URL
} vgfx_text_input_purpose_t;

/// @brief Current editable-text context published to a native input method.
/// @details All pointers are borrowed for the duration of the call. Cursor and anchor are UTF-8
///          byte offsets into surrounding_text. The cursor rectangle is in physical window
///          coordinates and should cover the visible caret; zero width/height are accepted.
typedef struct {
    const char *surrounding_text;      ///< NUL-terminated committed UTF-8, or NULL for unavailable
    int32_t cursor_byte;               ///< Active selection endpoint byte offset
    int32_t anchor_byte;               ///< Fixed selection endpoint byte offset
    int32_t cursor_x;                  ///< Caret rectangle X
    int32_t cursor_y;                  ///< Caret rectangle Y
    int32_t cursor_width;              ///< Caret rectangle width
    int32_t cursor_height;             ///< Caret rectangle height
    vgfx_text_input_purpose_t purpose; ///< Native keyboard/input specialization
} vgfx_text_input_state_t;

/// @brief Inline UTF-8 capacity of a native composition event, including its terminator.
/// @details IME preedit is deliberately stored in the value-type event so queue copies never
///          borrow platform memory. Platform adapters truncate oversized native preedit at a
///          complete UTF-8 codepoint boundary, set `composition.truncated`, and always append a
///          NUL terminator. The 4096-byte bound covers normal language input while keeping event
///          queues allocation-free and ABI-stable.
enum { VGFX_COMPOSITION_TEXT_CAPACITY = 4096 };

/// @brief Unified event structure.
/// @details Contains the event type, timestamp, and type-specific data in a
///          tagged union. Events are retrieved via vgfx_poll_event() from a
///          synchronized ring buffer populated by the platform backend.
typedef struct {
    vgfx_event_type_t type; ///< Event discriminator
    int64_t time_ms;        ///< Event timestamp (milliseconds since epoch)

    /// @brief Event-specific data.
    /// @details Access the appropriate member based on the event type.
    union {
        /// @brief Key event data (KEY_DOWN, KEY_UP).
        struct {
            vgfx_key_t key; ///< Key code
            int is_repeat;  ///< 1 if key repeat, 0 if initial press
            int modifiers;  ///< Modifier flags (VGFX_MOD_SHIFT, VGFX_MOD_CTRL, etc.)
        } key;

        /// @brief Text input event data (TEXT_INPUT).
        struct {
            uint32_t codepoint; ///< Unicode codepoint after platform text translation
            int modifiers;      ///< Modifier flags active while the text was generated
        } text;

        /// @brief Native IME composition lifecycle payload.
        /// @details Used by COMPOSITION_START, COMPOSITION_UPDATE, COMPOSITION_COMMIT, and
        ///          COMPOSITION_CANCEL. Text is inline and byte-counted UTF-8. Selection offsets
        ///          are Unicode-codepoint indices within `text`. Replacement offsets are
        ///          Unicode-codepoint indices in the focused client's committed text; -1 means
        ///          replace the client's current selection. The GUI layer converts both kinds of
        ///          offsets to extended-grapheme boundaries before editing.
        struct {
            char text[VGFX_COMPOSITION_TEXT_CAPACITY]; ///< NUL-terminated preedit/commit UTF-8.
            uint32_t text_length;    ///< Bytes before the terminator, excluding any truncated tail.
            int32_t selection_start; ///< Preedit selection/caret start in Unicode codepoints.
            int32_t selection_length;   ///< Preedit selection length in Unicode codepoints.
            int32_t replacement_start;  ///< Committed codepoint start, or -1 for current selection.
            int32_t replacement_length; ///< Committed codepoint count, or -1 with sentinel start.
            int modifiers; ///< Modifier flags active during the native composition callback.
            int truncated; ///< Non-zero when native UTF-8 exceeded the inline event capacity.
        } composition;

        /// @brief Mouse movement event data (MOUSE_MOVE).
        struct {
            int32_t x;     ///< Mouse X coordinate (pixels from left edge)
            int32_t y;     ///< Mouse Y coordinate (pixels from top edge)
            int modifiers; ///< Modifier flags active during the move
        } mouse_move;

        /// @brief Mouse button event data (MOUSE_DOWN, MOUSE_UP).
        struct {
            int32_t x;                  ///< Mouse X at time of click
            int32_t y;                  ///< Mouse Y at time of click
            vgfx_mouse_button_t button; ///< Which button was pressed/released
            int modifiers;              ///< Modifier flags active during the button event
        } mouse_button;

        /// @brief Resize event data (RESIZE).
        /// @details The framebuffer has been reallocated and cleared to black.
        ///          `width`/`height` report the physical framebuffer size in
        ///          pixels. `logical_width`/`logical_height` report the
        ///          current public coordinate-space size after coord scaling
        ///          (matches what vgfx_get_size() returns immediately after
        ///          the event).
        struct {
            int32_t width;          ///< New framebuffer width in physical pixels
            int32_t height;         ///< New framebuffer height in physical pixels
            int32_t logical_width;  ///< New logical/public width
            int32_t logical_height; ///< New logical/public height
        } resize;

        /// @brief Scroll event data (SCROLL).
        struct {
            float delta_x; ///< Horizontal scroll delta (positive = right)
            float delta_y; ///< Vertical scroll delta (positive = down)
            int32_t x;     ///< Cursor X at time of scroll (physical pixels)
            int32_t y;     ///< Cursor Y at time of scroll (physical pixels)
            int modifiers; ///< Modifier flags active during the scroll event
        } scroll;

        /// @brief File drop event data (FILE_DROP).
        /// @details One event is enqueued per dropped file. The path is
        ///          NUL-terminated and copied into the event struct (not a
        ///          pointer to external memory). Platform backends drop paths
        ///          that do not fit and increment the event overflow counter
        ///          rather than enqueueing a truncated path.
        struct {
            char path[260]; ///< File path (NUL-terminated, max 259 chars)
        } file_drop;

        /// @brief Touch contact data (TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP).
        /// @details Contact IDs are stable only for the duration of one touch sequence. Shape and
        ///          orientation are supplied when the compositor supports wl_touch version 6;
        ///          otherwise major/minor are zero and orientation is unspecified as zero.
        struct {
            int32_t id;        ///< Compositor-assigned contact identifier
            int32_t x;         ///< Contact X in physical framebuffer pixels
            int32_t y;         ///< Contact Y in physical framebuffer pixels
            float major;       ///< Major-axis diameter in surface coordinates, or zero
            float minor;       ///< Minor-axis diameter in surface coordinates, or zero
            float orientation; ///< Ellipse orientation in degrees, or zero
        } touch;
    } data;
} vgfx_event_t;

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

/// @brief Error code enumeration.
/// @details Identifies the category of the last error that occurred in a
///          ZannaGFX API call.  Error details are stored in thread-local
///          storage and retrieved via vgfx_get_last_error().
typedef enum {
    VGFX_ERR_NONE = 0,     ///< No error
    VGFX_ERR_ALLOC,        ///< Memory allocation failed
    VGFX_ERR_PLATFORM,     ///< Platform-specific error (window creation, etc.)
    VGFX_ERR_INVALID_PARAM ///< Invalid parameter (out of range, NULL, etc.)
} vgfx_error_t;

/// @brief Retrieve the last error message.
/// @details Returns a descriptive error string for the most recent failure in
///          the current thread.  The string is stored in thread-local storage
///          and remains valid until another error replaces it, it is explicitly
///          cleared, or the thread terminates.
/// @return Error message string, or NULL if no error has occurred.
const char *vgfx_get_last_error(void);

/// @brief Retrieve the last error code.
/// @details Returns the thread-local error code set by the most recent failure
///          in the current thread, or VGFX_ERR_NONE if no error is pending.
/// @return Pending error category for the calling thread.
vgfx_error_t vgfx_last_error_code(void);

/// @brief Clear the last error state.
/// @details Resets the thread-local error code and message.  Useful for
///          recovering from non-fatal errors.
void vgfx_clear_error(void);

/// @brief Install or clear a logging callback.
/// @details Pass a non-null function pointer to receive diagnostic messages;
///          pass NULL to disable logging callbacks.
/// @param fn Callback function or NULL to disable.
void vgfx_set_log_callback(vgfx_log_fn fn);

//===----------------------------------------------------------------------===//
// Window Management
//===----------------------------------------------------------------------===//

/// @brief Create a new window with the specified parameters.
/// @details Allocates a framebuffer, initializes the event queue, and creates
///          a native OS window via the platform backend.  The window is
///          initially visible and ready for drawing.  Returns NULL on failure
///          (e.g., allocation failure, unsupported dimensions).
/// @param params Configuration for the new window (width, height, title, etc.)
/// @return Window handle on success, NULL on failure (check vgfx_get_last_error())
vgfx_window_t vgfx_create_window(const vgfx_window_params_t *params);

/// @brief Destroy a window and free all associated resources.
/// @details Closes the native OS window, deallocates the framebuffer and event
///          queue, and frees the window structure.  The handle becomes invalid
///          and must not be used after this call.  It is safe to pass NULL.
/// @param window Window handle to destroy (may be NULL)
void vgfx_destroy_window(vgfx_window_t window);

/// @brief Update the window display and process pending events.
/// @details Blits the framebuffer to the native window surface, polls OS
///          events (translating them to ZannaGFX events), and applies FPS
///          limiting if configured.  Must be called regularly in the main
///          loop to keep the window responsive.
/// @param window Window handle
/// @return 1 on success, 0 on fatal error
int vgfx_update(vgfx_window_t window);

/// @brief Apply frame-rate pacing for a window without presenting a frame.
/// @details Runs the same deadline-based sleep vgfx_update() performs after
///          presenting. With a positive FPS cap it sleeps until the next frame
///          deadline (advanced additively to avoid drift). With no FPS cap
///          (fps <= 0) it sleeps @p min_idle_sleep_ms milliseconds when that is
///          positive, else returns immediately. GUI event loops call this on
///          frames that needed no repaint so an idle window does not busy-loop.
/// @param window Window handle (NULL is a no-op).
/// @param min_idle_sleep_ms Anti-spin floor applied only when fps <= 0.
void vgfx_frame_pace(vgfx_window_t window, int32_t min_idle_sleep_ms);

/// @brief Pump pending OS events without presenting the framebuffer.
/// @details Polls the native event queue and enqueues translated ZannaGFX
///          events for later consumption via vgfx_poll_event(). Use this when
///          input must be processed before rendering or presenting a frame.
/// @param window Window handle
/// @return 1 on success, 0 on fatal error
int vgfx_pump_events(vgfx_window_t window);

/// @brief Block until an OS event is available for the window, or the timeout
///        elapses, without dequeuing anything.
/// @details A hint, not a contract: spurious wakeups are fine, so callers pump
///          normally afterwards. The timeout is clamped to [0, 1000] ms so a
///          bug can never hang the UI. Use in event loops to sleep while idle
///          instead of busy-polling.
/// @param window Window handle.
/// @param timeout_ms Maximum wait in milliseconds (0 returns immediately).
/// @return 1 if events are (probably) available, 0 on timeout.
int vgfx_wait_events(vgfx_window_t window, int32_t timeout_ms);

/// @brief Enable or disable native text input for the focused editor in a window.
/// @details Disabling cancels any active native preedit. This does not affect raw key events.
/// @param window Window whose native text-input session is updated.
/// @param enabled 1 to enable native text input or 0 to disable it.
/// @return 1 when accepted; zero for an invalid window or invalid enabled value.
int vgfx_set_text_input_enabled(vgfx_window_t window, int32_t enabled);

/// @brief Publish surrounding text, selection, content purpose, and caret geometry to the IME.
/// @details Backends without an explicit surrounding-text protocol safely ignore this state while
///          retaining their existing native text behavior.
/// @param window Window whose active input method receives the state.
/// @param state Borrowed text-input context consumed during the call.
/// @return 1 when the state is valid and accepted, otherwise zero.
int vgfx_set_text_input_state(vgfx_window_t window, const vgfx_text_input_state_t *state);

/// @brief Get the current window dimensions.
/// @details Retrieves the current drawable size. When coord scaling is
///          enabled, this returns logical dimensions; otherwise it returns the
///          framebuffer size in physical pixels. Dimensions may change when a
///          resizable window is resized.
/// @param window Window handle
/// @param out_width Pointer to receive width (may be NULL)
/// @param out_height Pointer to receive height (may be NULL)
/// @return 1 on success, 0 if window is NULL
int vgfx_get_size(vgfx_window_t window, int32_t *out_width, int32_t *out_height);

/// @brief Set the global default FPS for subsequently-created windows.
/// @details Positive values are clamped to the supported range. Negative
///          values mean unlimited FPS. Passing 0 restores an unlimited default.
/// @param fps New process-wide default frame-rate cap.
void vgfx_set_default_fps(int32_t fps);

/// @brief Get the global default FPS used when create params specify fps == 0.
/// @return Current process-wide default; a negative value denotes unlimited pacing.
int32_t vgfx_get_default_fps(void);

/// @brief Set the target frame rate for the window.
/// @details Configures FPS limiting for the next vgfx_update() call.  The
///          library will sleep to throttle rendering if frames complete faster
///          than the target rate.  Pass 0 to disable FPS limiting (unlimited).
/// @param window Window handle
/// @param fps Target FPS (< 0: unlimited, 0: unlimited, > 0: limit to fps)
void vgfx_set_fps(vgfx_window_t window, int32_t fps);

/// @brief Get the configured target frame rate for the window.
/// @details Returns the current FPS setting for the window. Negative values
///          indicate unlimited, zero should not occur after initialization.
/// @param window Window handle
/// @return Current FPS setting, or -1 if window is NULL
int32_t vgfx_get_fps(vgfx_window_t window);

/// @brief Get the duration of the most recent vgfx_update() call in ms.
/// @param window Window whose last measured update duration is requested.
/// @return Last frame duration in milliseconds, or -1 if window is NULL.
int32_t vgfx_frame_time_ms(vgfx_window_t window);

/// @brief Set the window title.
/// @details Changes the window's title bar text at runtime. The title string
///          is copied internally, so the caller's string can be freed after
///          the call returns.
/// @param window Window handle
/// @param title New window title (UTF-8 string; NULL restores default)
void vgfx_set_title(vgfx_window_t window, const char *title);

/// @brief Register a callback invoked immediately on window resize.
/// @details On macOS, the Cocoa live-resize modal loop blocks the main
///          thread while the user drags the resize handle.  Registering a
///          callback here allows the application to re-render on each resize
///          notification, preventing the window from going blank.
///          On other platforms the callback is stored but never called
///          (resize events arrive via the normal poll loop instead).
/// @param window   Window handle
/// @param callback Function called with (userdata, new_width, new_height)
/// @param userdata Opaque pointer passed back to the callback
void vgfx_set_resize_callback(vgfx_window_t window,
                              void (*callback)(void *userdata, int32_t w, int32_t h),
                              void *userdata);

/// @brief Set the window to fullscreen or windowed mode.
/// @details Toggles the window between fullscreen and windowed modes. In
///          fullscreen mode, the window covers the entire screen with no
///          title bar or borders. The framebuffer is resized to match the
///          screen dimensions, and a RESIZE event is generated.
/// @param window Window handle
/// @param fullscreen 1 for fullscreen, 0 for windowed mode
void vgfx_set_fullscreen(vgfx_window_t window, int fullscreen);

/// @brief Check if the window is in fullscreen mode.
/// @param window Window handle
/// @return 1 if fullscreen, 0 if windowed, -1 if window is NULL
int vgfx_is_fullscreen(vgfx_window_t window);

/// @brief Minimize (iconify) the window.
/// @param window Window to minimize; NULL is ignored.
void vgfx_minimize(vgfx_window_t window);

/// @brief Maximize (zoom) the window.
/// @param window Window to maximize; NULL is ignored.
void vgfx_maximize(vgfx_window_t window);

/// @brief Restore the window from minimized or maximized state.
/// @param window Window to restore; NULL is ignored.
void vgfx_restore(vgfx_window_t window);

/// @brief Check if the window is currently minimized.
/// @param window Window to inspect.
/// @return 1 if minimized, 0 otherwise or for NULL.
int32_t vgfx_is_minimized(vgfx_window_t window);

/// @brief Check if the window is currently maximized.
/// @param window Window to inspect.
/// @return 1 if maximized, 0 otherwise or for NULL.
int32_t vgfx_is_maximized(vgfx_window_t window);

/// @brief Get the window's current screen position.
/// @param window Window handle
/// @param out_x Pointer to receive X coordinate (may be NULL)
/// @param out_y Pointer to receive Y coordinate (may be NULL)
void vgfx_get_position(vgfx_window_t window, int32_t *out_x, int32_t *out_y);

/// @brief Move the window to a new screen position.
/// @param window Window handle
/// @param x New X coordinate
/// @param y New Y coordinate
void vgfx_set_position(vgfx_window_t window, int32_t x, int32_t y);

/// @brief Bring the window to the front and give it keyboard focus.
/// @param window Window to focus; NULL is ignored.
void vgfx_focus(vgfx_window_t window);

/// @brief Request foreground application activation for the window.
/// @details Stronger than vgfx_focus on platforms with app/window separation:
///          macOS makes the process a regular foreground app, installs its
///          menu bar, makes the window key/main, and activates NSApp. Other
///          platforms map this to their best foreground/focus request.
/// @param window Window handle.
void vgfx_request_foreground(vgfx_window_t window);

/// @brief Check if the window currently has keyboard focus.
/// @param window Window to inspect.
/// @return 1 if focused, 0 otherwise or for NULL.
int32_t vgfx_is_focused(vgfx_window_t window);

/// @brief Control whether clicking the close button closes the window.
/// @details When prevented, the close event is still delivered so the
///          application can prompt or clean up before later allowing close.
/// @param window Window handle
/// @param prevent 1 to block close, 0 to allow
void vgfx_set_prevent_close(vgfx_window_t window, int32_t prevent);

/// @brief Cursor type constants for vgfx_set_cursor().
/// DEFAULT=0, POINTER=1, TEXT=2, RESIZE_H=3, RESIZE_V=4, WAIT=5
typedef enum {
    VGFX_CURSOR_DEFAULT = 0,     ///< Standard arrow cursor
    VGFX_CURSOR_POINTER = 1,     ///< Hand/pointer cursor (links, buttons)
    VGFX_CURSOR_TEXT = 2,        ///< I-beam text cursor
    VGFX_CURSOR_RESIZE_H = 3,    ///< Horizontal resize cursor
    VGFX_CURSOR_RESIZE_V = 4,    ///< Vertical resize cursor
    VGFX_CURSOR_WAIT = 5,        ///< Busy/spinner cursor
    VGFX_CURSOR_RESIZE_NWSE = 6, ///< Diagonal resize (top-left/bottom-right)
    VGFX_CURSOR_RESIZE_NESW = 7, ///< Diagonal resize (top-right/bottom-left)
    VGFX_CURSOR_GRAB = 8,        ///< Open-hand grab cursor (draggable)
    VGFX_CURSOR_GRABBING = 9,    ///< Closed-hand grabbing cursor (dragging)
    VGFX_CURSOR_CROSSHAIR = 10,  ///< Precision crosshair cursor
    VGFX_CURSOR_HELP = 11,       ///< Help cursor (question mark)
    VGFX_CURSOR_NOT_ALLOWED = 12 ///< Action-not-allowed cursor
} vgfx_cursor_type_t;

/// @brief Set the mouse cursor shape.
/// @param window Window handle
/// @param cursor_type Cursor type (VGFX_CURSOR_* constant)
void vgfx_set_cursor(vgfx_window_t window, int32_t cursor_type);

/// @brief Show or hide the mouse cursor.
/// @param window Window handle
/// @param visible 1 to show, 0 to hide
void vgfx_set_cursor_visible(vgfx_window_t window, int32_t visible);

/// @brief Get the current monitor's screen dimensions.
/// @details Queries the size of the monitor containing the window when the
///          backend can determine it, otherwise falls back to the primary or
///          default screen. Reported dimensions are physical pixels.
/// @param window Window handle (may be NULL to query primary monitor)
/// @param out_w Pointer to receive monitor width (may be NULL)
/// @param out_h Pointer to receive monitor height (may be NULL)
void vgfx_get_monitor_size(vgfx_window_t window, int32_t *out_w, int32_t *out_h);

/// @brief Resize the native OS window.
/// @details Changes the window's logical client/content area dimensions. The
///          native backend performs any frame/content conversion it needs. The
///          resulting resize is reported asynchronously through
///          VGFX_EVENT_RESIZE after the framebuffer has been reallocated.
/// @param window Window handle
/// @param w New logical window width in pixels (must be > 0)
/// @param h New logical window height in pixels (must be > 0)
void vgfx_set_window_size(vgfx_window_t window, int32_t w, int32_t h);

/// @brief Set the minimum native client/content size for a resizable window.
/// @details The dimensions use the same logical coordinate space as
///          @ref vgfx_set_window_size. Future programmatic resize requests are
///          clamped to this floor, and supported desktop window managers are
///          told to enforce it during interactive resizing. Values below one
///          are normalized to one, which restores the effectively unconstrained
///          default.
/// @param window Window handle.
/// @param w Minimum logical client/content width.
/// @param h Minimum logical client/content height.
void vgfx_set_window_min_size(vgfx_window_t window, int32_t w, int32_t h);

/// @brief Query the HiDPI backing scale factor for a window.
/// @details Returns the current ratio of physical pixels to logical points.
///          On a 2× macOS Retina display this returns 2.0; on a standard
///          96 DPI display it returns 1.0. The value may change if the window
///          moves between displays with different scale factors.
///          Use this value to scale logical coordinates to physical pixels or
///          to adjust font/UI element sizes for crisp rendering.
/// @param window Window handle (may be NULL → returns 1.0)
/// @return Scale factor (≥ 1.0)
float vgfx_window_get_scale(vgfx_window_t window);

/// @brief Enable coordinate-space scaling for the drawing API.
/// @details When set to a value > 1.0, all public drawing functions
///          (vgfx_pset, vgfx_line, vgfx_fill_rect, etc.) automatically
///          multiply input coordinates by this factor.  Mouse positions
///          returned by vgfx_mouse_pos() are divided by it.  vgfx_get_size()
///          returns logical (divided) dimensions.
///          Intended for the Canvas (game) API so apps draw in logical
///          pixels while the framebuffer is at physical resolution.
///          The GUI widget layer leaves this at the default 1.0.
/// @param window Window handle
/// @param scale  Coordinate scale (typically vgfx_window_get_scale(window))
void vgfx_set_coord_scale(vgfx_window_t window, float scale);

/// @brief Get the physical pixel width of the window framebuffer.
/// @details Returns win->width, which equals (logical_width × scale_factor)
///          after vgfx_create_window().  Use for framebuffer operations.
/// @param window Window handle
/// @return Physical width in pixels, or 0 if window is NULL
int32_t vgfx_window_get_width(vgfx_window_t window);

/// @brief Get the physical pixel height of the window framebuffer.
/// @details Returns win->height, which equals (logical_height × scale_factor)
///          after vgfx_create_window().  Use for framebuffer operations.
/// @param window Window handle
/// @return Physical height in pixels, or 0 if window is NULL
int32_t vgfx_window_get_height(vgfx_window_t window);

/// @brief Get direct access to the framebuffer.
/// @details Returns a descriptor with pointers to the raw RGBA pixel data.
///          The framebuffer is always stored in row-major order with 4 bytes
///          per pixel (RGBA, 8 bits per channel).  Direct writes are visible
///          after the next vgfx_update() call. Any API that pumps native events
///          can resize the window and invalidate a previously returned pixels
///          pointer; compare vgfx_framebuffer_t::generation after such calls.
/// @param window Window handle
/// @param out_fb Pointer to receive framebuffer descriptor
/// @return 1 on success, 0 if window or out_fb is NULL
int vgfx_get_framebuffer(vgfx_window_t window, vgfx_framebuffer_t *out_fb);

/// @brief Get the platform-specific native view handle.
/// @details On macOS, returns the NSView* (as void*). On Linux, returns
///          an X11 Window (as void*, cast from unsigned long) or a Wayland
///          wl_surface*. On Windows, returns the HWND (as void*). New code
///          should prefer vgfx_get_native_handles() and inspect its discriminator.
/// @param window Window handle
/// @return Native view handle, or NULL if unavailable
void *vgfx_get_native_view(vgfx_window_t window);

/// @brief Tell vgfx that a GPU backend owns display for this window.
/// @details When set, vgfx_platform_present skips the software framebuffer blit
///          so the GPU backend's presented content is not overwritten.
/// @param window Window whose presentation ownership changes.
/// @param enabled Non-zero when a GPU presenter owns the surface; zero restores software blits.
void vgfx_set_gpu_present(vgfx_window_t window, int32_t enabled);

/// @brief Get the platform-specific native display/connection handle.
/// @details On Linux, returns the active X11 Display* or Wayland wl_display*.
///          Returns NULL on macOS, Windows, and mock backends. New code should
///          prefer vgfx_get_native_handles() before interpreting this pointer.
/// @param window Window handle
/// @return Native display handle, or NULL if unavailable
void *vgfx_get_native_display(vgfx_window_t window);

/// @brief Callback consulted for selected native platform window messages.
/// @details Installed by higher layers that must answer native protocol
///          messages arriving at the platform window procedure (today:
///          Windows WM_GETOBJECT for the UI Automation accessibility bridge).
///          The hook runs on the thread that owns the native window while the
///          platform pumps events. Set @p handled non-zero and fill
///          @p result to consume the message; leave it zero to fall through
///          to default handling.
/// @param user Opaque pointer supplied at registration.
/// @param native_window Native window handle (HWND on Windows).
/// @param msg Native message identifier.
/// @param wparam Native message word parameter.
/// @param lparam Native message long parameter.
/// @param result Receives the message result when handled.
/// @param handled Set non-zero when the hook fully handled the message.
typedef void (*vgfx_native_msg_hook_t)(void *user,
                                       void *native_window,
                                       uint32_t msg,
                                       uintptr_t wparam,
                                       intptr_t lparam,
                                       intptr_t *result,
                                       int32_t *handled);

/// @brief Install (or clear) the native message hook for one window.
/// @details At most one hook is stored per window; passing NULL clears it.
///          Only platform backends that route native protocol messages
///          (currently Win32) consult the hook; elsewhere it is stored but
///          never invoked, which keeps callers platform-neutral.
/// @param window Window handle; NULL is a no-op.
/// @param hook Hook function, or NULL to remove the current hook.
/// @param user Opaque pointer passed back to the hook.
void vgfx_set_native_msg_hook(vgfx_window_t window, vgfx_native_msg_hook_t hook, void *user);

/// @brief Query typed native handles without assuming that Linux uses X11.
/// @param window Window whose borrowed native handles are requested.
/// @param out_handles Receives a fully initialized native-handle descriptor.
/// @return 1 when window and out_handles are valid, otherwise 0. A valid
///         headless window returns 1 with backend VGFX_NATIVE_BACKEND_NONE.
int vgfx_get_native_handles(vgfx_window_t window, vgfx_native_handles_t *out_handles);

/// @brief Query native capabilities for one live window.
/// @param window Window whose selected backend is queried.
/// @return A bitwise OR of `VGFX_CAP_*`, or zero for an invalid/headless window.
vgfx_window_capabilities_t vgfx_get_window_capabilities(vgfx_window_t window);

//===----------------------------------------------------------------------===//
// Clipping
//===----------------------------------------------------------------------===//

/// @brief Set the clipping rectangle for all drawing operations.
/// @details When a clipping rectangle is set, all subsequent drawing operations
///          are constrained to render only within the specified region. Pixels
///          outside the clipping rectangle are not modified. The clipping region
///          is intersected with the window bounds.
/// @param window Window handle
/// @param x Left edge X coordinate of clip rect
/// @param y Top edge Y coordinate of clip rect
/// @param w Width of clip rect (pixels)
/// @param h Height of clip rect (pixels)
/// @note The clip rect persists until cleared with vgfx_clear_clip().
/// @note A zero or negative width/height results in no drawing.
void vgfx_set_clip(vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h);

/// @brief Clear the clipping rectangle, restoring full-window drawing.
/// @details After calling this function, drawing operations can affect any
///          pixel within the window bounds. Equivalent to setting the clip
///          rectangle to the full window size.
/// @param window Window handle
void vgfx_clear_clip(vgfx_window_t window);

/// @brief Query the current effective framebuffer clipping rectangle.
/// @details Writes the active physical-pixel clip bounds. When no explicit clip
///          is enabled, the returned rectangle covers the full framebuffer.
///          Pass NULL for any output pointer that is not needed.
/// @param window Window handle.
/// @param out_x Destination for clip left edge in physical pixels.
/// @param out_y Destination for clip top edge in physical pixels.
/// @param out_w Destination for clip width in physical pixels.
/// @param out_h Destination for clip height in physical pixels.
/// @return 1 when an explicit clip is active, 0 when drawing is unclipped or
///         when @p window is NULL.
int vgfx_get_clip(
    vgfx_window_t window, int32_t *out_x, int32_t *out_y, int32_t *out_w, int32_t *out_h);

/// @brief Establish a temporary upper bound that ordinary clip operations cannot escape.
/// @details Captures the current clip, intersects it with the requested rectangle, and makes the
///          result an immutable limit until the matching @ref vgfx_pop_clip_limit. While a limit is
///          active, @ref vgfx_set_clip intersects each requested clip with the innermost limit and
///          @ref vgfx_clear_clip restores that limit instead of enabling full-window drawing. This
///          lets retained compositors impose a damage rectangle — and internally clipping
///          containers such as scroll views impose their content viewport — while descendants
///          continue using the existing set/clear clip API. The operation performs no allocation.
///          Scopes nest to a fixed depth: each nested limit is intersected with the enclosing one,
///          and a push beyond the maximum depth leaves all state unchanged and returns zero so the
///          caller can fall back to a plain clip rectangle. Coordinates use the same
///          drawing-coordinate space and coordinate scaling as @ref vgfx_set_clip. A non-positive
///          extent establishes an empty limit.
/// @param window Window whose drawing state is constrained; NULL is rejected without side effects.
/// @param x Left edge of the requested limit in drawing coordinates.
/// @param y Top edge of the requested limit in drawing coordinates.
/// @param w Width of the requested limit; non-positive values suppress all drawing in the scope.
/// @param h Height of the requested limit; non-positive values suppress all drawing in the scope.
/// @return 1 when the scope was established, or 0 for a NULL window or exhausted nesting depth.
/// @post On success, exactly one matching @ref vgfx_pop_clip_limit restores the captured clip.
int vgfx_push_clip_limit(vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h);

/// @brief End the innermost clip-limit scope and restore the clip captured by its push.
/// @details Restores both the enabled state and exact effective rectangle that were active before
///          the matching @ref vgfx_push_clip_limit. Calling this function with NULL or without an
///          active scope is a no-op. The function performs no allocation and never changes
///          framebuffer pixels.
/// @param window Window whose innermost clip-limit scope should end; may be NULL.
/// @post A successful prior push is balanced and the enclosing scope (or ordinary set/clear clip
///       semantics) resumes.
void vgfx_pop_clip_limit(vgfx_window_t window);

//===----------------------------------------------------------------------===//
// Drawing Primitives
//===----------------------------------------------------------------------===//

/// @brief Clear the entire window to a solid color.
/// @details Fills the active clipping rectangle, or every pixel in the
///          framebuffer when no clip is active.
/// @param window Window handle
/// @param color Fill color (24-bit RGB: 0x00RRGGBB)
void vgfx_cls(vgfx_window_t window, vgfx_color_t color);

/// @brief Set a single pixel to a color.
/// @details Writes directly to the framebuffer at (x, y).  Out-of-bounds
///          coordinates are silently ignored (no error).  Coordinates are
///          measured from the top-left corner (0, 0). Active clipping set via
///          vgfx_set_clip() is honored.
/// @param window Window handle
/// @param x X coordinate (pixels from left edge)
/// @param y Y coordinate (pixels from top edge)
/// @param color Pixel color (24-bit RGB)
void vgfx_pset(vgfx_window_t window, int32_t x, int32_t y, vgfx_color_t color);

/// @brief Plot a single pixel using source-over alpha blending.
/// @details Composites src_color (0xAARRGGBB) over the existing framebuffer
///          pixel using the Porter-Duff source-over formula:
///            dst.rgb = src.rgb * (src.a/255) + dst.rgb * (1 - src.a/255)
///          If src_color is fully opaque (alpha == 0xFF), this is identical to
///          vgfx_pset. Pixels outside window bounds or the active clip rectangle
///          are silently discarded.
/// @param window Window handle
/// @param x      X coordinate (pixels from left edge)
/// @param y      Y coordinate (pixels from top edge)
/// @param color  Source color with alpha (0xAARRGGBB)
void vgfx_pset_alpha(vgfx_window_t window, int32_t x, int32_t y, uint32_t color);

/// @brief Read the color of a single pixel.
/// @details Retrieves the current color at (x, y) from the framebuffer.
///          Returns 0 (failure) if coordinates are out of bounds or window is
///          NULL.  The output color is only written on success.
/// @param window Window handle
/// @param x X coordinate
/// @param y Y coordinate
/// @param out_color Pointer to receive pixel color
/// @return 1 on success, 0 if out of bounds or window/out_color is NULL
int vgfx_point(vgfx_window_t window, int32_t x, int32_t y, vgfx_color_t *out_color);

/// @brief Draw a line from (x1, y1) to (x2, y2).
/// @details Uses Bresenham's integer-only line algorithm for deterministic,
///          pixel-perfect rendering.  Pixels outside the window bounds are
///          clipped per-pixel (no error).  The line includes both endpoints.
/// @param window Window handle
/// @param x1 Starting X coordinate
/// @param y1 Starting Y coordinate
/// @param x2 Ending X coordinate
/// @param y2 Ending Y coordinate
/// @param color Line color
void vgfx_line(
    vgfx_window_t window, int32_t x1, int32_t y1, int32_t x2, int32_t y2, vgfx_color_t color);

/// @brief Draw a rectangle outline.
/// @details Draws the four edges of a rectangle with top-left corner at (x, y)
///          and dimensions w × h.  Only the perimeter is drawn; the interior
///          is left unchanged.  Negative or zero dimensions are rejected.
/// @param window Window handle
/// @param x Left edge X coordinate
/// @param y Top edge Y coordinate
/// @param w Rectangle width (must be > 0)
/// @param h Rectangle height (must be > 0)
/// @param color Outline color
void vgfx_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color);

/// @brief Draw a filled rectangle.
/// @details Fills a solid rectangle with top-left corner at (x, y) and
///          dimensions w × h.  Uses optimized scanline filling.  The rectangle
///          is clipped to window bounds; out-of-bounds regions are ignored.
/// @param window Window handle
/// @param x Left edge X coordinate
/// @param y Top edge Y coordinate
/// @param w Rectangle width (must be > 0)
/// @param h Rectangle height (must be > 0)
/// @param color Fill color
void vgfx_fill_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color);

/// @brief Draw a circle outline.
/// @details Draws the perimeter of a circle centered at (cx, cy) with the
///          specified radius using the midpoint circle algorithm (8-way
///          symmetry, integer-only math).  Negative radius is rejected.
/// @param window Window handle
/// @param cx Center X coordinate
/// @param cy Center Y coordinate
/// @param radius Circle radius in pixels (must be >= 0)
/// @param color Outline color
void vgfx_circle(vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color);

/// @brief Draw a filled circle.
/// @details Fills a solid circle centered at (cx, cy) with the specified
///          radius using scanline-based filling derived from the midpoint
///          algorithm.  Negative radius is rejected.
/// @param window Window handle
/// @param cx Center X coordinate
/// @param cy Center Y coordinate
/// @param radius Circle radius in pixels (must be >= 0)
/// @param color Fill color
void vgfx_fill_circle(
    vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color);

//===----------------------------------------------------------------------===//
// Input Polling
//===----------------------------------------------------------------------===//

/// @brief Check if a keyboard key is currently pressed.
/// @details Queries the key state array maintained by the platform backend.
///          Key state is updated during vgfx_update() when processing OS
///          events.  Returns 0 for unknown keys or NULL window.
/// @param window Window handle
/// @param key Key code to query
/// @return 1 if key is pressed, 0 otherwise
int vgfx_key_down(vgfx_window_t window, vgfx_key_t key);

/// @brief Get the current mouse position.
/// @details Retrieves the last reported mouse coordinates relative to the
///          window. When a window coordinate scale is active, coordinates are
///          returned in logical pixels. Coordinates are updated during
///          vgfx_update(). Returns 0 if the mouse is outside the window bounds,
///          but still writes the coordinates (which may be negative or exceed
///          window dimensions).
/// @param window Window handle
/// @param out_x Pointer to receive X coordinate (may be NULL)
/// @param out_y Pointer to receive Y coordinate (may be NULL)
/// @return 1 if mouse is in bounds, 0 if out of bounds or window is NULL
int vgfx_mouse_pos(vgfx_window_t window, int32_t *out_x, int32_t *out_y);

/// @brief Check if a mouse button is currently pressed.
/// @details Queries the button state array maintained by the platform backend.
///          Button state is updated during vgfx_update() when processing OS
///          events.  Returns 0 for invalid buttons or NULL window.
/// @param window Window handle
/// @param button Button identifier (left, right, middle)
/// @return 1 if button is pressed, 0 otherwise
int vgfx_mouse_button(vgfx_window_t window, vgfx_mouse_button_t button);

/// @brief Warp the mouse cursor to the specified position within the window.
/// @details Used for FPS-style mouse capture — warp to center each frame.
/// @param window Window handle
/// @param x Target X coordinate (logical pixels)
/// @param y Target Y coordinate (logical pixels)
void vgfx_warp_cursor(vgfx_window_t window, int32_t x, int32_t y);

/// @brief Query the primary display's logical (point) dimensions.
/// @details Used to size fullscreen windows. Falls back to the default window
///          size when the platform cannot report a display size (mock/headless).
/// @param out_w Receives the display width in logical pixels (may be NULL)
/// @param out_h Receives the display height in logical pixels (may be NULL)
void vgfx_get_display_size(int32_t *out_w, int32_t *out_h);

/// @brief Enable or disable relative (raw) mouse mode for FPS mouse-look.
/// @details While enabled, platform backends that support raw motion deliver
///          unbounded, sub-pixel motion deltas (drained via
///          vgfx_get_relative_deltas()) instead of the cursor tracking
///          absolute positions. Backends without raw motion support (e.g.
///          X11 without XInput2) return 0 and callers should fall back to
///          warp-to-center capture. Disabling always restores normal cursor
///          behavior and clears any accumulated deltas.
/// @param window Window handle
/// @param enabled Non-zero to enable, zero to disable
/// @return 1 when the platform delivers native raw deltas, 0 otherwise
int32_t vgfx_set_relative_mouse(vgfx_window_t window, int32_t enabled);

/// @brief Query whether native raw deltas are currently being delivered.
/// @param window Window whose relative-input state is queried.
/// @return 1 when relative mode is enabled AND the platform is native, else 0
int32_t vgfx_relative_mouse_native(vgfx_window_t window);

/// @brief Drain the accumulated relative mouse deltas (read-and-clear).
/// @details Returns the motion accumulated since the previous call. Values
///          are doubles in logical units and may be sub-pixel. Both output
///          pointers may be NULL. Returns zeros when relative mode is off.
/// @param window Window handle
/// @param out_dx Receives horizontal motion (may be NULL)
/// @param out_dy Receives vertical motion, positive = down (may be NULL)
void vgfx_get_relative_deltas(vgfx_window_t window, double *out_dx, double *out_dy);

/// @brief Hide the OS mouse cursor.
void vgfx_hide_cursor(void);

/// @brief Show the OS mouse cursor.
void vgfx_show_cursor(void);

//===----------------------------------------------------------------------===//
// Event Queue
//===----------------------------------------------------------------------===//

/// @brief Append a caller-authored event to a window's synchronized queue.
/// @details Copies the complete value-type event into the same bounded queue used by
///          native platform adapters. The event is subsequently observed through
///          vgfx_poll_event() in normal FIFO order and is subject to the queue's usual
///          overflow and state-repair policy. This is intended for deterministic test
///          automation and embedders; it does not update platform input state directly.
///          Injection accepts KEY_DOWN through FILE_DROP; NONE, touch lifecycle
///          events, and values outside that range are rejected.
/// @param window Window whose event queue receives the copied event.
/// @param event Complete caller-owned event value; no pointer within it is retained.
/// @return 1 when the event was queued, otherwise 0 for invalid input, a destroying
///         window, or an event that could not be retained under overflow pressure.
int vgfx_post_event(vgfx_window_t window, const vgfx_event_t *event);

/// @brief Poll the next event from the queue.
/// @details Retrieves and removes the oldest event from the synchronized event
///          queue. Events are generated by the platform backend during
///          vgfx_update() and queued for application consumption. Returns 0 if
///          the queue is empty.
/// @param window Window handle
/// @param out_event Pointer to receive event data
/// @return 1 if event was retrieved, 0 if queue is empty or window/out_event is NULL
int vgfx_poll_event(vgfx_window_t window, vgfx_event_t *out_event);

/// @brief Peek at the next event without removing it.
/// @details Returns the oldest event without dequeuing it.  Useful for
///          inspecting event types before deciding whether to consume them.
/// @param window Window handle
/// @param out_event Pointer to receive event data
/// @return 1 if event was peeked, 0 if queue is empty or window/out_event is NULL
int vgfx_peek_event(vgfx_window_t window, vgfx_event_t *out_event);

/// @brief Discard non-critical events from the queue and report how many were removed.
/// @details Useful for ignoring accumulated input after a menu or dialog.  Close,
///          key/button release, and focus-lost events remain queued so applications
///          can still observe state-repair transitions.
/// @param window Window handle
/// @return Number of non-critical events discarded, or 0 if window is NULL
int32_t vgfx_flush_events(vgfx_window_t window);

/// @brief Clear non-critical events from the queue.
/// @details Compatibility wrapper around vgfx_flush_events().
/// @param window Window handle
void vgfx_clear_events(vgfx_window_t window);

/// @brief Get and reset the event overflow counter.
/// @details Returns the number of events that were dropped due to queue
///          overflow since the last call to this function.  The counter is
///          reset to zero after reading.  Overflow occurs when more than
///          VGFX_EVENT_QUEUE_SIZE events are generated between updates.
/// @param window Window handle
/// @return Number of dropped events (0 if none)
int32_t vgfx_event_overflow_count(vgfx_window_t window);

/// @brief Check whether the window close button has been pressed.
/// @details Returns non-zero if the platform backend received a close
///          request (e.g. WM_CLOSE on Win32, windowShouldClose on macOS,
///          WM_DELETE_WINDOW on X11).  This flag is sticky — once set it
///          remains set for the lifetime of the window.
/// @param window Window handle
/// @return Non-zero if close was requested, 0 otherwise
int32_t vgfx_close_requested(vgfx_window_t window);

//===----------------------------------------------------------------------===//
// Color Utilities
//===----------------------------------------------------------------------===//

/// @brief Construct a color from RGB components.
/// @details Packs 8-bit red, green, and blue components into a 24-bit color
///          value: 0x00RRGGBB without allocation or color-space conversion.
/// @param r Red component (0-255)
/// @param g Green component (0-255)
/// @param b Blue component (0-255)
/// @return Packed color value
static inline vgfx_color_t vgfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/// @brief Alias for vgfx_rgb() using uppercase macro style.
#define VGFX_RGB(r, g, b) vgfx_rgb((r), (g), (b))

/* Common color constants */
#define VGFX_BLACK 0x000000
#define VGFX_WHITE 0xFFFFFF
#define VGFX_RED 0xFF0000
#define VGFX_GREEN 0x00FF00
#define VGFX_BLUE 0x0000FF
#define VGFX_YELLOW 0xFFFF00
#define VGFX_CYAN 0x00FFFF
#define VGFX_MAGENTA 0xFF00FF
#define VGFX_GRAY 0x808080

/// @brief Decompose a packed color into RGB components.
/// @details Writes the red, green, and blue bytes to the provided pointers
///          when non-NULL. Useful for debugging and UI integrations.
/// @param color Packed color value (0x00RRGGBB)
/// @param r Optional pointer to receive red component
/// @param g Optional pointer to receive green component
/// @param b Optional pointer to receive blue component
void vgfx_color_to_rgb(vgfx_color_t color, uint8_t *r, uint8_t *g, uint8_t *b);

//===----------------------------------------------------------------------===//
// Clipboard Operations
//===----------------------------------------------------------------------===//

/// @brief Clipboard format types
typedef enum {
    VGFX_CLIPBOARD_TEXT,  ///< Plain text (UTF-8)
    VGFX_CLIPBOARD_HTML,  ///< HTML formatted text
    VGFX_CLIPBOARD_IMAGE, ///< Image data (not yet supported)
    VGFX_CLIPBOARD_FILES  ///< File paths (not yet supported)
} vgfx_clipboard_format_t;

/// @brief Check if the clipboard contains data in the specified format.
/// @param format Clipboard format to check for
/// @return 1 if data is available, 0 otherwise
int vgfx_clipboard_has_format(vgfx_clipboard_format_t format);

/// @brief Get text from the clipboard.
/// @details Returns a malloc'd UTF-8 string containing the clipboard text.
///          The caller is responsible for freeing the returned string.
/// @return Clipboard text (caller must free), or NULL if not available
char *vgfx_clipboard_get_text(void);

/// @brief Set text to the clipboard.
/// @details Copies the specified UTF-8 string to the system clipboard.
/// @param text Text to copy (NULL clears text from clipboard)
void vgfx_clipboard_set_text(const char *text);

/// @brief Clear all clipboard contents.
void vgfx_clipboard_clear(void);

#ifdef __cplusplus
}
#endif
