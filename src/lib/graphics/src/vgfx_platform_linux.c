//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaGFX Linux X11 Backend
//
// Platform-specific implementation using X11 (Xlib) on Linux/Unix systems.
// Provides window creation, event handling, framebuffer blitting, and timing
// functions for X11-based systems.
//
// Architecture:
//   - Display: X11 connection to the X server
//   - Window: Native X11 window handle
//   - XImage: Wrapper for framebuffer data for efficient blitting
//   - GC (Graphics Context): X11 drawing context
//   - Atom: WM_DELETE_WINDOW protocol for close button handling
//
// Key X11 Concepts:
//   - XOpenDisplay: Establish connection to X server
//   - XCreateWindow: Create native window
//   - XImage: Wrap framebuffer for blitting with XPutImage
//   - XPending/XNextEvent: Non-blocking event polling
//   - ClientMessage: Window manager protocol messages (close, etc.)
//   - KeySym: X11 keyboard symbol mapping via XLookupKeysym
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Linux X11 backend implementation for ZannaGFX.
/// @details Uses Xlib to provide window management and framebuffer
///          presentation on Linux and Unix systems.

#include "vgfx_internal.h"

#if defined(__linux__)

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VGFX_X11_CLIPBOARD_MAX_BYTES (16u * 1024u * 1024u)
#define VGFX_X11_CLIPBOARD_WAIT_MS 1000

/// @brief Opaque GLX framebuffer-configuration handle used by the runtime loader.
/// @details Declared locally to avoid a build-time dependency on GLX headers.
typedef void *GLXFBConfig;

static pthread_mutex_t g_x11_global_mu = PTHREAD_MUTEX_INITIALIZER;

/// @brief Interpret a ZannaGFX environment variable as a Boolean opt-in flag.
/// @details Missing and empty values are disabled.  The common textual false
///          spellings `0`, `false`, and `off` are also disabled, case-insensitively
///          for the variants explicitly recognized here; every other non-empty
///          value enables the option.
/// @param name Environment-variable name to inspect.
/// @return 1 when the variable requests the option, otherwise 0.
static int vgfx_x11_env_flag_enabled(const char *name) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return 0;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 && strcmp(value, "FALSE") != 0 &&
           strcmp(value, "off") != 0 && strcmp(value, "OFF") != 0;
}

/// @brief Check whether newly created X11 windows should remain unmapped.
/// @return 1 when `ZANNA_GFX_HIDE_WINDOWS` is enabled, otherwise 0.
static int vgfx_x11_hide_windows(void) {
    return vgfx_x11_env_flag_enabled("ZANNA_GFX_HIDE_WINDOWS");
}

/// @brief Check whether creation should avoid activating the new X11 window.
/// @details Hidden windows necessarily avoid activation; otherwise the result
///          follows `ZANNA_GFX_NO_ACTIVATE`.
/// @return 1 when activation should be suppressed, otherwise 0.
static int vgfx_x11_no_activate_on_create(void) {
    return vgfx_x11_env_flag_enabled("ZANNA_GFX_NO_ACTIVATE") || vgfx_x11_hide_windows();
}

/// @brief Acquire the mutex protecting process-global X11 backend state.
/// @details This lock covers Zanna-owned globals such as the live-window list,
///          cached GLX library handle, and temporary X error handler changes.
///          It does not replace per-display Xlib locking.
static void x11_global_lock(void) {
    (void)pthread_mutex_lock(&g_x11_global_mu);
}

/// @brief Release the mutex protecting process-global X11 backend state.
static void x11_global_unlock(void) {
    (void)pthread_mutex_unlock(&g_x11_global_mu);
}

/// @brief Allocate an aligned framebuffer buffer using the POSIX allocator.
/// @details Linux/X11 is an approved platform adapter layer, so it owns the
///          direct `posix_memalign` call needed by the platform-neutral core.
///          The returned pointer must be released with
///          `vgfx_platform_aligned_free()`.
/// @param alignment Required byte alignment; POSIX requires a power-of-two
///                  multiple of `sizeof(void *)`.
/// @param size Number of bytes requested.
/// @return Aligned allocation on success, or NULL for invalid input/OOM.
void *vgfx_platform_aligned_alloc(size_t alignment, size_t size) {
    if (size == 0)
        return NULL;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0)
        return NULL;
    return ptr;
}

/// @brief Free a buffer returned by `vgfx_platform_aligned_alloc()`.
/// @details POSIX aligned allocations are released with regular `free`.
///          Passing NULL is permitted.
/// @param ptr Pointer returned by `vgfx_platform_aligned_alloc()`, or NULL.
void vgfx_platform_aligned_free(void *ptr) {
    free(ptr);
}

#define VGFX_GLX_USE_GL 1
#define VGFX_GLX_RGBA_BIT 0x00000001
#define VGFX_GLX_WINDOW_BIT 0x00000001
#define VGFX_GLX_DOUBLEBUFFER 5
#define VGFX_GLX_RED_SIZE 8
#define VGFX_GLX_GREEN_SIZE 9
#define VGFX_GLX_BLUE_SIZE 10
#define VGFX_GLX_ALPHA_SIZE 11
#define VGFX_GLX_DEPTH_SIZE 12
#define VGFX_GLX_RENDER_TYPE 0x8011
#define VGFX_GLX_DRAWABLE_TYPE 0x8010

//===----------------------------------------------------------------------===//
// Platform Data Structure
//===----------------------------------------------------------------------===//

/// @brief Platform-specific data for X11 windows.
/// @details Allocated and owned by the platform backend.  Stored in
///          vgfx_window->platform_data.  Contains X11 Display connection,
///          Window handle, XImage for blitting, and WM protocol atoms.
///
/// @invariant display != NULL implies window != 0 && gc != NULL
typedef struct {
    Display *display;       ///< X11 connection to server
    int screen;             ///< Screen number
    Window window;          ///< Native X11 window handle
    GC gc;                  ///< Graphics context for drawing
    Atom wm_delete_window;  ///< Atom for WM_DELETE_WINDOW protocol
    Atom event_wake;        ///< Private client message used to interrupt waits
    XImage *ximage;         ///< XImage wrapper for presentation buffer
    uint8_t *ximage_buf;    ///< BGRA presentation buffer (R↔B swizzled from win->pixels)
    Visual *visual;         ///< Visual used for window and XImage
    int depth;              ///< Depth matching visual (24 or 32)
    Colormap colormap;      ///< Colormap for the chosen visual (None if default)
    size_t ximage_buf_size; ///< Size of ximage_buf in bytes
    int width;              ///< Cached window width
    int height;             ///< Cached window height
    unsigned long resize_request_serial; ///< X request serial of the latest client resize
    int hidden;             ///< 1 if creation intentionally skipped mapping
    int close_requested;    ///< 1 if WM_DELETE_WINDOW received, 0 otherwise
    // XDND (drag-and-drop) atoms
    Atom xdnd_aware;                  ///< XdndAware atom
    Atom xdnd_enter;                  ///< XdndEnter atom
    Atom xdnd_position;               ///< XdndPosition atom
    Atom xdnd_status;                 ///< XdndStatus atom
    Atom xdnd_drop;                   ///< XdndDrop atom
    Atom xdnd_finished;               ///< XdndFinished atom
    Atom xdnd_selection;              ///< XdndSelection atom
    Atom xdnd_type_list;              ///< XdndTypeList atom
    Atom text_uri_list;               ///< text/uri-list MIME type atom
    Window xdnd_source;               ///< Source window for current drag
    Atom clipboard_atom;              ///< CLIPBOARD selection atom
    Atom utf8_string_atom;            ///< UTF8_STRING target atom
    Atom targets_atom;                ///< TARGETS target atom
    Atom incr_atom;                   ///< INCR target/property atom
    Atom clipboard_property_atom;     ///< Property used for selection conversion
    char *clipboard_text;             ///< Owned text while this window owns CLIPBOARD
    XIM xim;                          ///< Input method for UTF-8 text input
    XIC xic;                          ///< Input context for UTF-8 text input
    int xi_opcode;                    ///< XInput extension opcode for this Display connection
    uint32_t *ime_preedit;            ///< Owned Unicode-codepoint preedit buffer
    size_t ime_preedit_count;         ///< Live codepoints in ime_preedit
    size_t ime_preedit_capacity;      ///< Allocated codepoint slots
    size_t ime_caret;                 ///< Current preedit caret in codepoint units
    int ime_active;                   ///< 1 between XIM preedit start and terminal event
    int cursor_type;                  ///< Last requested cursor type
    int cursor_visible;               ///< 1 if cursor should be visible
    Cursor cursor_cache[13];          ///< Cached visible cursor handles by public cursor type
    Cursor blank_cursor;              ///< Cached invisible cursor
    struct vgfx_window *owner_window; ///< Backlink for multi-window global services
    struct vgfx_window *next_window;  ///< Intrusive list of live X11 windows
} vgfx_x11_data;

/// @brief Runtime-resolved signature of `glXChooseFBConfig`.
/// @param dpy Open X11 display.
/// @param screen X11 screen number.
/// @param attrib_list None-terminated GLX attribute/value list.
/// @param nelements Receives the number of returned configurations.
/// @return Xlib-allocated configuration array, or NULL.
typedef GLXFBConfig *(*vgfx_glx_choose_fb_config_fn)(Display *dpy,
                                                     int screen,
                                                     const int *attrib_list,
                                                     int *nelements);

/// @brief Runtime-resolved signature of `glXGetVisualFromFBConfig`.
/// @param dpy Open X11 display.
/// @param config Candidate GLX framebuffer configuration.
/// @return Xlib-allocated matching visual information, or NULL.
typedef XVisualInfo *(*vgfx_glx_get_visual_from_fb_config_fn)(Display *dpy, GLXFBConfig config);

static void *g_vgfx_glx_libgl_handle = NULL;
static vgfx_glx_choose_fb_config_fn g_vgfx_glx_choose_fb_config = NULL;
static vgfx_glx_get_visual_from_fb_config_fn g_vgfx_glx_get_visual_from_fb_config = NULL;
static pthread_once_t g_vgfx_glx_once = PTHREAD_ONCE_INIT;

/// @brief Load libGL and resolve the optional GLX visual-selection entry points.
/// @details Runs once through `pthread_once`.  It tries the versioned soname
///          first, then the generic soname, and leaves function pointers NULL
///          when GLX is unavailable so software presentation can still work.
static void x11_load_glx_library(void) {
    g_vgfx_glx_libgl_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_vgfx_glx_libgl_handle)
        g_vgfx_glx_libgl_handle = dlopen("libGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_vgfx_glx_libgl_handle)
        return;
    g_vgfx_glx_choose_fb_config =
        (vgfx_glx_choose_fb_config_fn)dlsym(g_vgfx_glx_libgl_handle, "glXChooseFBConfig");
    g_vgfx_glx_get_visual_from_fb_config = (vgfx_glx_get_visual_from_fb_config_fn)dlsym(
        g_vgfx_glx_libgl_handle, "glXGetVisualFromFBConfig");
}

/// @brief Try to select a double-buffered GLX visual for windows that may host GPU rendering.
///
/// ZannaGFX windows are created before Canvas3D selects its backend, so the Linux window adapter
/// must pick a visual that is compatible with both XImage software blits and GLX.  The OpenGL
/// backend renders to GL_BACK and swaps; using a generic TrueColor visual can leave GLX without a
/// matching double-buffered FBConfig, which produces a mapped title bar with a never-updated client
/// area on some Linux drivers.
/// @param x11 Platform state receiving the selected visual, depth, and colormap.
/// @param root Root window used to create the selected visual's colormap.
/// @return 1 when a compatible GLX visual and colormap were selected, otherwise 0.
static int x11_try_choose_glx_visual(vgfx_x11_data *x11, Window root) {
    GLXFBConfig *configs;
    XVisualInfo *visual_info = NULL;
    int fb_count = 0;
    const int fb_attribs[] = {
        VGFX_GLX_RENDER_TYPE,
        VGFX_GLX_RGBA_BIT,
        VGFX_GLX_DRAWABLE_TYPE,
        VGFX_GLX_WINDOW_BIT,
        VGFX_GLX_DOUBLEBUFFER,
        1,
        VGFX_GLX_RED_SIZE,
        8,
        VGFX_GLX_GREEN_SIZE,
        8,
        VGFX_GLX_BLUE_SIZE,
        8,
        VGFX_GLX_DEPTH_SIZE,
        24,
        None,
    };

    if (!x11 || !x11->display)
        return 0;
    if (pthread_once(&g_vgfx_glx_once, x11_load_glx_library) != 0)
        return 0;
    if (!g_vgfx_glx_choose_fb_config || !g_vgfx_glx_get_visual_from_fb_config)
        return 0;

    configs = g_vgfx_glx_choose_fb_config(x11->display, x11->screen, fb_attribs, &fb_count);
    if (!configs || fb_count <= 0) {
        if (configs)
            XFree(configs);
        return 0;
    }

    for (int i = 0; i < fb_count; i++) {
        visual_info = g_vgfx_glx_get_visual_from_fb_config(x11->display, configs[i]);
        if (visual_info)
            break;
    }
    XFree(configs);

    if (!visual_info)
        return 0;
    x11->visual = visual_info->visual;
    x11->depth = visual_info->depth;
    x11->colormap = XCreateColormap(x11->display, root, x11->visual, AllocNone);
    XFree(visual_info);
    return x11->colormap != None;
}

static struct vgfx_window *g_vgfx_cursor_window = NULL;
static struct vgfx_window *g_vgfx_clipboard_window = NULL;
static struct vgfx_window *g_vgfx_x11_windows = NULL;
static vgfx_atomic_flag_t g_x11_scale_lock;
static int g_x11_scale_cached = 0;
static float g_x11_scale_value = 1.0f;

/// @brief Initialize Xlib's thread support before opening any display.
/// @details XInitThreads must be called before other Xlib calls in a process
///          that may touch Xlib from multiple threads.  Calling it more than
///          once is harmless for this backend; the flag only avoids repeated
///          work on common single-threaded paths.
static pthread_once_t g_x11_threads_once = PTHREAD_ONCE_INIT;
static int g_x11_threads_available = 0;

/// @brief Perform the process-wide `XInitThreads` initialization call.
/// @details Stores whether Xlib accepted multithreaded access for subsequent
///          `pthread_once` callers.
static void x11_init_threads(void) {
    g_x11_threads_available = XInitThreads() != 0;
}

/// @brief Ensure Xlib thread support has been initialized exactly once.
/// @return 1 when `XInitThreads` succeeded, otherwise 0.
static int x11_init_threads_once(void) {
    if (pthread_once(&g_x11_threads_once, x11_init_threads) != 0)
        return 0;
    return g_x11_threads_available;
}

/// @brief Wait briefly for an X11 window to become viewable after `XMapWindow`.
/// @details X11 mapping is asynchronous. Canvas3D can create an OpenGL context immediately after
///          `vgfx_create_window`; if the drawable is still `IsUnmapped`, GLX accepts commands but
///          the default framebuffer can remain black. This helper synchronizes with the server and
///          polls the map state for a bounded interval so callers get a realized drawable without
///          blocking indefinitely under unusual window-manager behavior.
/// @param x11 Platform state containing the display and newly mapped window.
static void x11_wait_for_viewable(vgfx_x11_data *x11) {
    if (!x11 || !x11->display || x11->window == None)
        return;
    for (int attempt = 0; attempt < 50; attempt++) {
        XWindowAttributes attrs;
        XSync(x11->display, False);
        if (XGetWindowAttributes(x11->display, x11->window, &attrs) &&
            attrs.map_state == IsViewable) {
            return;
        }
        usleep(10000);
    }
}

enum {
    /*
     * X11/X.h standardizes Button1..Button5 only. Many servers report
     * horizontal wheel motion as raw button codes 6 and 7, so keep those
     * values local instead of relying on non-portable macros.
     */
    VGFX_X11_BUTTON_SCROLL_LEFT = 6,
    VGFX_X11_BUTTON_SCROLL_RIGHT = 7,
};

/// @brief Convert one ASCII hexadecimal digit to its numeric value.
/// @param c Candidate byte.
/// @return Value in [0, 15], or -1 when @p c is not a hexadecimal digit.
static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F')
        return (int)(c - 'A') + 10;
    return -1;
}

/// @brief Percent-decode an XDND URI path into bounded storage.
/// @details Copies ordinary bytes verbatim and replaces valid `%HH` sequences
///          with their byte value.  Malformed percent escapes are preserved.
///          Embedded NUL terminates the source early.  On capacity failure the
///          destination is reset to an empty string.
/// @param src Source bytes to decode.
/// @param len Maximum readable source length.
/// @param dst Destination character buffer.
/// @param dst_cap Destination capacity including the terminator.
/// @return 1 when the decoded path fit, otherwise 0.
static int percent_decode_path(const char *src, size_t len, char *dst, size_t dst_cap) {
    if (!dst || dst_cap == 0)
        return 0;

    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\0')
            break;
        if (out + 1 >= dst_cap) {
            dst[0] = '\0';
            return 0;
        }
        if (ch == '%' && i + 2 < len) {
            int hi = hex_value((unsigned char)src[i + 1]);
            int lo = hex_value((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        dst[out++] = (char)ch;
    }
    dst[out] = '\0';
    return 1;
}

/// @brief Translate one `text/uri-list` line into a file-drop event.
/// @details Trims CR/NUL suffixes, ignores blank and comment lines, removes
///          local `file://` authority syntax, discards non-local authority-only
///          URIs, percent-decodes the remaining path, and enqueues one bounded
///          `VGFX_EVENT_FILE_DROP`.  Oversized paths count as event overflow.
/// @param win Window receiving the file-drop event.
/// @param timestamp Monotonic event timestamp in milliseconds.
/// @param line Borrowed URI-list line bytes.
/// @param line_len Number of readable bytes in @p line.
static void enqueue_xdnd_uri_line(struct vgfx_window *win,
                                  int64_t timestamp,
                                  const char *line,
                                  size_t line_len) {
    while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == '\0'))
        line_len--;
    if (line_len == 0 || line[0] == '#')
        return;

    const char *path = line;
    size_t path_len = line_len;
    if (line_len >= 7 && strncmp(line, "file://", 7) == 0) {
        path = line + 7;
        path_len = line_len - 7;
        if (path_len >= 10 && strncmp(path, "localhost/", 10) == 0) {
            path += 9;
            path_len -= 9;
        } else if (path_len > 0 && path[0] != '/') {
            const char *slash = memchr(path, '/', path_len);
            if (!slash)
                return;
            path_len -= (size_t)(slash - path);
            path = slash;
        }
    }

    vgfx_event_t vgfx_event = {0};
    vgfx_event.type = VGFX_EVENT_FILE_DROP;
    vgfx_event.time_ms = timestamp;
    if (!percent_decode_path(path,
                             path_len,
                             vgfx_event.data.file_drop.path,
                             sizeof(vgfx_event.data.file_drop.path))) {
        vgfx_internal_note_event_overflow(win);
        return;
    }
    if (vgfx_event.data.file_drop.path[0] != '\0')
        vgfx_internal_enqueue_event(win, &vgfx_event);
}

/// @brief Parse an XDND `text/uri-list` payload into file-drop events.
/// @details Splits the bounded payload at newline bytes and delegates each line
///          independently, preserving the original drop timestamp.
/// @param win Window receiving parsed file-drop events.
/// @param timestamp Monotonic event timestamp in milliseconds.
/// @param data Borrowed URI-list payload.
/// @param len Number of readable bytes in @p data.
static void parse_xdnd_uri_list(struct vgfx_window *win,
                                int64_t timestamp,
                                const unsigned char *data,
                                size_t len) {
    size_t line_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            enqueue_xdnd_uri_line(win, timestamp, (const char *)data + line_start, i - line_start);
            line_start = i + 1;
        }
    }
}

//===----------------------------------------------------------------------===//
// Key Code Translation
//===----------------------------------------------------------------------===//

/// @brief Translate X11 KeySym to vgfx_key_t.
/// @details Maps X11 keysyms (obtained via XLookupKeysym) to ZannaGFX key
///          codes.  Handles A-Z, 0-9, Space, arrows, Enter, Escape.
///          Unrecognized keys return VGFX_KEY_UNKNOWN.
///
/// @param keysym X11 KeySym from XLookupKeysym()
/// @return Corresponding vgfx_key_t, or VGFX_KEY_UNKNOWN if not recognized
///
/// @details Key mapping:
///            - A-Z: Mapped to vgfx_key_t enum values (uppercase)
///            - 0-9: Mapped to vgfx_key_t enum values
///            - Space: VGFX_KEY_SPACE
///            - Arrows: VGFX_KEY_LEFT/RIGHT/UP/DOWN
///            - Enter/Return: VGFX_KEY_ENTER
///            - Escape: VGFX_KEY_ESCAPE
static vgfx_key_t translate_keysym(KeySym keysym) {
    /* Lowercase letters (convert to uppercase) */
    if (keysym >= XK_a && keysym <= XK_z) {
        return (vgfx_key_t)('A' + (keysym - XK_a));
    }

    /* Uppercase letters */
    if (keysym >= XK_A && keysym <= XK_Z) {
        return (vgfx_key_t)keysym;
    }

    /* Digits 0-9 */
    if (keysym >= XK_0 && keysym <= XK_9) {
        return (vgfx_key_t)keysym;
    }

    /* Other printable ASCII (punctuation/symbols): X11 Latin-1 keysyms equal their
       ASCII codes, so '=' (XK_equal) and '-' (XK_minus) — which back the zoom
       shortcuts — map straight through. Placed after the a-z block above so lowercase
       letters are still folded to uppercase; special keys live in the 0xff00+ range
       and are unaffected by this check. */
    if (keysym >= 0x20 && keysym <= 0x7e) {
        return (vgfx_key_t)keysym;
    }

    /* Special keys */
    switch (keysym) {
        case XK_space:
            return VGFX_KEY_SPACE;
        case XK_Return:
            return VGFX_KEY_ENTER;
        case XK_KP_Enter:
            return VGFX_KEY_ENTER;
        case XK_Escape:
            return VGFX_KEY_ESCAPE;
        case XK_BackSpace:
            return VGFX_KEY_BACKSPACE;
        case XK_Delete:
        case XK_KP_Delete:
            return VGFX_KEY_DELETE;
        case XK_Tab:
        case XK_ISO_Left_Tab:
        case XK_KP_Tab:
            return VGFX_KEY_TAB;
        case XK_Left:
        case XK_KP_Left:
            return VGFX_KEY_LEFT;
        case XK_Right:
        case XK_KP_Right:
            return VGFX_KEY_RIGHT;
        case XK_Up:
        case XK_KP_Up:
            return VGFX_KEY_UP;
        case XK_Down:
        case XK_KP_Down:
            return VGFX_KEY_DOWN;
        case XK_Home:
        case XK_KP_Home:
            return VGFX_KEY_HOME;
        case XK_End:
        case XK_KP_End:
            return VGFX_KEY_END;
        case XK_Page_Up:
        case XK_KP_Page_Up:
            return VGFX_KEY_PAGE_UP;
        case XK_Page_Down:
        case XK_KP_Page_Down:
            return VGFX_KEY_PAGE_DOWN;
        default:
            /* Function keys occupy a contiguous keysym range. */
            if (keysym >= XK_F1 && keysym <= XK_F12) {
                return (vgfx_key_t)(VGFX_KEY_F1 + (keysym - XK_F1));
            }
            return VGFX_KEY_UNKNOWN;
    }
}

/// @brief Translate X11 modifier-state bits to the public modifier mask.
/// @param state Native X11 state mask from a key, button, or motion event.
/// @return Bitwise combination of `VGFX_MOD_SHIFT`, `VGFX_MOD_CTRL`,
///         `VGFX_MOD_ALT`, and `VGFX_MOD_CMD`.
static int x11_modifiers(unsigned int state) {
    int mods = 0;
    if (state & ShiftMask)
        mods |= VGFX_MOD_SHIFT;
    if (state & ControlMask)
        mods |= VGFX_MOD_CTRL;
    if (state & Mod1Mask)
        mods |= VGFX_MOD_ALT;
    if (state & Mod4Mask)
        mods |= VGFX_MOD_CMD;
    return mods;
}

/// @brief Decode the first bounded UTF-8 sequence in a byte span.
/// @details Accepts canonical one- through four-byte sequences and rejects
///          overlong forms, UTF-16 surrogates, and scalars beyond U+10FFFF.
///          Malformed input consumes one byte with code point zero so callers
///          can make forward progress.
/// @param bytes Start of the bounded byte span.
/// @param len Number of readable bytes.
/// @param out_codepoint Receives the decoded scalar, or zero for malformed input.
/// @return Valid sequence length in [1, 4], one for malformed input, or zero
///         when arguments contain no decodable byte.
static int utf8_decode_codepoint(const char *bytes, int len, uint32_t *out_codepoint) {
    const unsigned char *s = (const unsigned char *)bytes;
    if (!s || len <= 0 || !out_codepoint)
        return 0;

    *out_codepoint = 0;
    if (s[0] < 0x80) {
        *out_codepoint = s[0];
        return 1;
    }
    if (len >= 2 && s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) {
        *out_codepoint = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if (len >= 3 && s[0] >= 0xE0 && s[0] <= 0xEF && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        if ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0))
            return 1;
        *out_codepoint = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
                         (uint32_t)(s[2] & 0x3F);
        return 3;
    }
    if (len >= 4 && s[0] >= 0xF0 && s[0] <= 0xF4 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        if ((s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] > 0x8F))
            return 1;
        *out_codepoint = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                         ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        return 4;
    }

    return 1;
}

/// @brief Decode committed X11 text and enqueue one event per textual scalar.
/// @details Walks the bounded UTF-8 lookup result, filters control/private-use
///          values and command-modifier combinations through the shared text
///          policy, and preserves the native timestamp and modifier state.
/// @param win Window receiving text-input events.
/// @param timestamp Monotonic event timestamp in milliseconds.
/// @param mods Translated bitwise `VGFX_MOD_*` mask.
/// @param text Borrowed UTF-8 lookup bytes.
/// @param text_len Number of readable bytes in @p text.
static void x11_enqueue_text_input_events(
    struct vgfx_window *win, int64_t timestamp, int mods, const char *text, int text_len) {
    int offset = 0;

    if (!win || !text || text_len <= 0)
        return;

    while (offset < text_len) {
        uint32_t codepoint = 0;
        int consumed = utf8_decode_codepoint(text + offset, text_len - offset, &codepoint);
        if (consumed <= 0)
            break;
        offset += consumed;

        if (vgfx_internal_should_emit_text_input(codepoint, mods)) {
            vgfx_event_t text_event = {.type = VGFX_EVENT_TEXT_INPUT,
                                       .time_ms = timestamp,
                                       .data.text = {.codepoint = codepoint, .modifiers = mods}};
            vgfx_internal_enqueue_event(win, &text_event);
        }
    }
}

/// @brief Append one Unicode scalar to a caller-provided UTF-8 buffer.
/// @details Invalid scalar/surrogate values are replaced with U+FFFD. The caller computes enough
///          storage for the worst-case four-byte encoding before invoking this helper.
/// @param codepoint Unicode scalar candidate.
/// @param output Destination byte buffer.
/// @return Number of bytes written in the inclusive range one through four.
static size_t x11_utf8_encode_codepoint(uint32_t codepoint, char *output) {
    if (!output)
        return 0;
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        codepoint = 0xFFFD;
    if (codepoint < 0x80) {
        output[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        output[0] = (char)(0xC0 | (codepoint >> 6));
        output[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        output[0] = (char)(0xE0 | (codepoint >> 12));
        output[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        output[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    output[0] = (char)(0xF0 | (codepoint >> 18));
    output[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    output[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    output[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

/// @brief Convert an XIM changed-text segment into owned Unicode codepoints.
/// @details Wide-character XIM values are copied as scalars. Multibyte values from the UTF-8 input
///          context are decoded with ZannaGFX's bounded decoder; malformed bytes advance and map
///          to U+FFFD so callback processing cannot stall. The caller frees the returned array.
/// @param text Borrowed XIM text segment; may be NULL for deletion-only draws.
/// @param out_count Receives the allocated codepoint count.
/// @return Owned codepoint array, NULL for an empty segment or allocation failure.
static uint32_t *x11_ime_segment_codepoints(const XIMText *text, size_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!text || !out_count || text->length == 0)
        return NULL;

    if (text->encoding_is_wchar) {
        if (!text->string.wide_char)
            return NULL;
        size_t count = (size_t)text->length;
        if (count > SIZE_MAX / sizeof(uint32_t))
            return NULL;
        uint32_t *result = (uint32_t *)malloc(count * sizeof(uint32_t));
        if (!result)
            return NULL;
        for (size_t index = 0; index < count; index++) {
            uint32_t codepoint = (uint32_t)text->string.wide_char[index];
            result[index] = codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)
                                ? 0xFFFD
                                : codepoint;
        }
        *out_count = count;
        return result;
    }

    if (!text->string.multi_byte)
        return NULL;
    size_t byte_count = strlen(text->string.multi_byte);
    if (byte_count == 0 || byte_count > SIZE_MAX / sizeof(uint32_t))
        return NULL;
    uint32_t *result = (uint32_t *)malloc(byte_count * sizeof(uint32_t));
    if (!result)
        return NULL;
    size_t offset = 0;
    size_t count = 0;
    while (offset < byte_count) {
        uint32_t codepoint = 0;
        int consumed = utf8_decode_codepoint(
            text->string.multi_byte + offset, (int)(byte_count - offset), &codepoint);
        if (consumed <= 0)
            consumed = 1;
        if (codepoint == 0)
            codepoint = 0xFFFD;
        result[count++] = codepoint;
        offset += (size_t)consumed;
    }
    *out_count = count;
    return result;
}

/// @brief Enqueue an XIM composition lifecycle boundary without text.
/// @param x11 Platform state whose owner window receives the event.
/// @param type COMPOSITION_START or COMPOSITION_CANCEL.
static void x11_ime_emit_boundary(vgfx_x11_data *x11, vgfx_event_type_t type) {
    if (!x11 || !x11->owner_window)
        return;
    vgfx_event_t event;
    if (vgfx_internal_init_composition_event(
            &event, type, vgfx_platform_now_ms(), "", 0, 0, 0, -1, -1, 0)) {
        vgfx_internal_enqueue_event(x11->owner_window, &event);
    }
}

/// @brief Encode and enqueue the complete current XIM preedit snapshot.
/// @details XIM draw callbacks are deltas, but ZannaGFX events intentionally carry full snapshots
///          so coalescing remains safe. The temporary UTF-8 buffer is released immediately after
///          the value event copies its bounded prefix.
/// @param x11 Platform state containing the current codepoint buffer and caret.
static void x11_ime_emit_update(vgfx_x11_data *x11) {
    if (!x11 || !x11->owner_window)
        return;
    if (x11->ime_preedit_count > (SIZE_MAX - 1u) / 4u) {
        vgfx_internal_note_event_overflow(x11->owner_window);
        return;
    }
    size_t capacity = x11->ime_preedit_count * 4u + 1u;
    char *utf8 = (char *)malloc(capacity);
    if (!utf8) {
        vgfx_internal_note_event_overflow(x11->owner_window);
        return;
    }
    size_t length = 0;
    for (size_t index = 0; index < x11->ime_preedit_count; index++)
        length += x11_utf8_encode_codepoint(x11->ime_preedit[index], utf8 + length);
    utf8[length] = '\0';
    size_t caret =
        x11->ime_caret < x11->ime_preedit_count ? x11->ime_caret : x11->ime_preedit_count;
    if (caret > (size_t)INT32_MAX)
        caret = (size_t)INT32_MAX;
    vgfx_event_t event;
    if (vgfx_internal_init_composition_event(&event,
                                             VGFX_EVENT_COMPOSITION_UPDATE,
                                             vgfx_platform_now_ms(),
                                             utf8,
                                             length,
                                             (int32_t)caret,
                                             0,
                                             -1,
                                             -1,
                                             0)) {
        vgfx_internal_enqueue_event(x11->owner_window, &event);
    }
    free(utf8);
}

/// @brief Emit committed XIM lookup text as one terminal composition event.
/// @details If an input method returns committed UTF-8 without first invoking the start callback,
///          an implicit start boundary is emitted so the GUI still creates one atomic history
///          record. The callback-owned preedit buffer is cleared only after queueing the commit.
/// @param x11 Platform input-method state.
/// @param text Borrowed committed UTF-8 bytes.
/// @param text_length Number of readable committed bytes.
/// @param modifiers Active X11 modifier mask translated to ZannaGFX flags.
/// @param timestamp Monotonic event timestamp in milliseconds.
static void x11_ime_emit_commit(
    vgfx_x11_data *x11, const char *text, size_t text_length, int modifiers, int64_t timestamp) {
    if (!x11 || !x11->owner_window || (!text && text_length != 0))
        return;
    if (!x11->ime_active) {
        x11_ime_emit_boundary(x11, VGFX_EVENT_COMPOSITION_START);
        x11->ime_active = 1;
    }
    vgfx_event_t event;
    if (vgfx_internal_init_composition_event(&event,
                                             VGFX_EVENT_COMPOSITION_COMMIT,
                                             timestamp,
                                             text ? text : "",
                                             text_length,
                                             0,
                                             0,
                                             -1,
                                             -1,
                                             modifiers)) {
        vgfx_internal_enqueue_event(x11->owner_window, &event);
    }
    x11->ime_active = 0;
    x11->ime_preedit_count = 0;
    x11->ime_caret = 0;
}

/// @brief Begin one XIM preedit session and reset prior callback state.
/// @details Xlib uses the integer return as the client preedit length limit; -1 requests no
///          implementation-defined restriction while the ZannaGFX event layer enforces its safe
///          inline payload bound.
/// @param input_context XIM input context invoking the callback.
/// @param client_data Borrowed vgfx_x11_data pointer registered at XIC creation.
/// @param call_data Unused XIM callback data.
/// @return -1 to advertise no smaller native preedit limit.
static int x11_ime_preedit_start(XIC input_context, XPointer client_data, XPointer call_data) {
    (void)input_context;
    (void)call_data;
    vgfx_x11_data *x11 = (vgfx_x11_data *)client_data;
    if (!x11)
        return -1;
    if (x11->ime_active)
        x11_ime_emit_boundary(x11, VGFX_EVENT_COMPOSITION_CANCEL);
    x11->ime_preedit_count = 0;
    x11->ime_caret = 0;
    x11->ime_active = 1;
    x11_ime_emit_boundary(x11, VGFX_EVENT_COMPOSITION_START);
    return -1;
}

/// @brief Apply one XIM preedit delta and publish the resulting full snapshot.
/// @details `chg_first`/`chg_length` are character indices, which correspond to the maintained
///          codepoint array. Invalid ranges clamp safely; allocation failure preserves the prior
///          preedit and records overflow rather than partially applying the delta.
/// @param input_context XIM input context invoking the callback.
/// @param client_data Borrowed vgfx_x11_data pointer.
/// @param call_data Borrowed XIMPreeditDrawCallbackStruct pointer.
static void x11_ime_preedit_draw(XIC input_context, XPointer client_data, XPointer call_data) {
    (void)input_context;
    vgfx_x11_data *x11 = (vgfx_x11_data *)client_data;
    XIMPreeditDrawCallbackStruct *draw = (XIMPreeditDrawCallbackStruct *)call_data;
    if (!x11 || !draw)
        return;
    if (!x11->ime_active)
        (void)x11_ime_preedit_start(input_context, client_data, NULL);

    size_t changed_start = draw->chg_first > 0 ? (size_t)draw->chg_first : 0;
    if (changed_start > x11->ime_preedit_count)
        changed_start = x11->ime_preedit_count;
    size_t changed_length = draw->chg_length > 0 ? (size_t)draw->chg_length : 0;
    if (changed_length > x11->ime_preedit_count - changed_start)
        changed_length = x11->ime_preedit_count - changed_start;

    size_t segment_count = 0;
    uint32_t *segment = x11_ime_segment_codepoints(draw->text, &segment_count);
    if (draw->text && draw->text->length > 0 && !segment) {
        if (x11->owner_window)
            vgfx_internal_note_event_overflow(x11->owner_window);
        return;
    }
    size_t retained = x11->ime_preedit_count - changed_length;
    if (segment_count > SIZE_MAX - retained) {
        free(segment);
        return;
    }
    size_t new_count = retained + segment_count;
    if (new_count > x11->ime_preedit_capacity) {
        size_t new_capacity = x11->ime_preedit_capacity ? x11->ime_preedit_capacity : 16u;
        while (new_capacity < new_count && new_capacity <= SIZE_MAX / 2u)
            new_capacity *= 2u;
        if (new_capacity < new_count || new_capacity > SIZE_MAX / sizeof(uint32_t)) {
            free(segment);
            return;
        }
        uint32_t *resized = (uint32_t *)realloc(x11->ime_preedit, new_capacity * sizeof(uint32_t));
        if (!resized) {
            free(segment);
            if (x11->owner_window)
                vgfx_internal_note_event_overflow(x11->owner_window);
            return;
        }
        x11->ime_preedit = resized;
        x11->ime_preedit_capacity = new_capacity;
    }
    size_t tail_start = changed_start + changed_length;
    size_t tail_count = x11->ime_preedit_count - tail_start;
    memmove(x11->ime_preedit + changed_start + segment_count,
            x11->ime_preedit + tail_start,
            tail_count * sizeof(uint32_t));
    if (segment_count > 0)
        memcpy(x11->ime_preedit + changed_start, segment, segment_count * sizeof(uint32_t));
    free(segment);
    x11->ime_preedit_count = new_count;
    x11->ime_caret = draw->caret > 0 ? (size_t)draw->caret : 0;
    x11_ime_emit_update(x11);
}

/// @brief Publish an XIM caret-only change as a full coalescible update.
/// @param input_context XIM input context invoking the callback.
/// @param client_data Borrowed vgfx_x11_data pointer.
/// @param call_data Borrowed XIMPreeditCaretCallbackStruct pointer.
static void x11_ime_preedit_caret(XIC input_context, XPointer client_data, XPointer call_data) {
    (void)input_context;
    vgfx_x11_data *x11 = (vgfx_x11_data *)client_data;
    XIMPreeditCaretCallbackStruct *caret = (XIMPreeditCaretCallbackStruct *)call_data;
    if (!x11 || !caret || !x11->ime_active)
        return;
    x11->ime_caret = caret->position > 0 ? (size_t)caret->position : 0;
    x11_ime_emit_update(x11);
}

/// @brief End an XIM preedit session that did not already emit committed lookup text.
/// @param input_context XIM input context invoking the callback.
/// @param client_data Borrowed vgfx_x11_data pointer.
/// @param call_data Unused XIM callback data.
static void x11_ime_preedit_done(XIC input_context, XPointer client_data, XPointer call_data) {
    (void)input_context;
    (void)call_data;
    vgfx_x11_data *x11 = (vgfx_x11_data *)client_data;
    if (!x11 || !x11->ime_active)
        return;
    x11_ime_emit_boundary(x11, VGFX_EVENT_COMPOSITION_CANCEL);
    x11->ime_active = 0;
    x11->ime_preedit_count = 0;
    x11->ime_caret = 0;
}

/// @brief Create the richest XIM input context supported by the current input method.
/// @details Prefers callback preedit so ZannaGUI can render marked text. If the input method does
///          not advertise that style or creation fails, falls back to the existing preedit-nothing
///          context, preserving committed UTF-8 input on minimal X servers.
/// @param x11 Initialized X11 platform state with open display, window, and input method.
/// @return Created XIC, or NULL when no input context is available.
static XIC x11_create_input_context(vgfx_x11_data *x11) {
    if (!x11 || !x11->xim)
        return NULL;
    bool supports_callbacks = false;
    XIMStyles *styles = NULL;
    if (XGetIMValues(x11->xim, XNQueryInputStyle, &styles, NULL) == NULL && styles) {
        for (unsigned short index = 0; index < styles->count_styles; index++) {
            XIMStyle style = styles->supported_styles[index];
            if ((style & XIMPreeditCallbacks) != 0 && (style & XIMStatusNothing) != 0) {
                supports_callbacks = true;
                break;
            }
        }
    }
    if (styles)
        XFree(styles);

    if (supports_callbacks) {
        XIMCallback start = {(XPointer)x11, (XIMProc)x11_ime_preedit_start};
        XIMCallback done = {(XPointer)x11, (XIMProc)x11_ime_preedit_done};
        XIMCallback draw = {(XPointer)x11, (XIMProc)x11_ime_preedit_draw};
        XIMCallback caret = {(XPointer)x11, (XIMProc)x11_ime_preedit_caret};
        XVaNestedList preedit = XVaCreateNestedList(0,
                                                    XNPreeditStartCallback,
                                                    &start,
                                                    XNPreeditDoneCallback,
                                                    &done,
                                                    XNPreeditDrawCallback,
                                                    &draw,
                                                    XNPreeditCaretCallback,
                                                    &caret,
                                                    NULL);
        if (preedit) {
            XIC context = XCreateIC(x11->xim,
                                    XNInputStyle,
                                    XIMPreeditCallbacks | XIMStatusNothing,
                                    XNClientWindow,
                                    x11->window,
                                    XNFocusWindow,
                                    x11->window,
                                    XNPreeditAttributes,
                                    preedit,
                                    NULL);
            XFree(preedit);
            if (context)
                return context;
        }
    }

    return XCreateIC(x11->xim,
                     XNInputStyle,
                     XIMPreeditNothing | XIMStatusNothing,
                     XNClientWindow,
                     x11->window,
                     XNFocusWindow,
                     x11->window,
                     NULL);
}

/// @brief Convert a logical window dimension to X11 physical pixels.
/// @param win Window supplying the backing display scale, or NULL for 1.0.
/// @param logical Logical coordinate or extent.
/// @return Rounded, saturated physical-pixel value.
static int32_t x11_logical_to_physical(const struct vgfx_window *win, int32_t logical) {
    float scale = win ? vgfx_internal_sanitize_scale(win->scale_factor) : 1.0f;
    return vgfx_internal_scale_up_i32(logical, scale);
}

/// @brief Test whether a window has usable native X11 state.
/// @param win Window to inspect.
/// @return 1 when platform data, display, and native window are present;
///         otherwise 0.
static int x11_window_usable(const struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    const vgfx_x11_data *x11 = (const vgfx_x11_data *)win->platform_data;
    return x11 && x11->display && x11->window;
}

/// @brief Ignore an X11 error raised while destroying a possibly stale window.
/// @param display Display that reported the error.
/// @param event Error event being suppressed.
/// @return Zero, as required for a non-terminating Xlib error handler.
static int x11_ignore_bad_window_error(Display *display, XErrorEvent *event) {
    (void)display;
    (void)event;
    return 0;
}

/// @brief Publish a fully initialized window to process-global X11 services.
/// @details Adds the window to the intrusive live list and chooses it as the
///          initial cursor and clipboard service window when those slots are
///          empty.  Updates are serialized by the global mutex.
/// @param win Initialized window to register; invalid input is ignored.
static void x11_register_window(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    x11_global_lock();
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    x11->owner_window = win;
    x11->next_window = g_vgfx_x11_windows;
    g_vgfx_x11_windows = win;
    if (!g_vgfx_cursor_window)
        g_vgfx_cursor_window = win;
    if (!g_vgfx_clipboard_window)
        g_vgfx_clipboard_window = win;
    x11_global_unlock();
}

/// @brief Remove a window from process-global X11 service state.
/// @details Unlinks the window and redirects cursor/clipboard service slots to
///          the next live window when necessary.
/// @param win Window being destroyed.
static void x11_unregister_window(struct vgfx_window *win) {
    x11_global_lock();
    struct vgfx_window **cursor = &g_vgfx_x11_windows;
    while (*cursor) {
        vgfx_x11_data *x11 = (vgfx_x11_data *)(*cursor)->platform_data;
        if (*cursor == win) {
            *cursor = x11 ? x11->next_window : NULL;
            break;
        }
        if (!x11)
            break;
        cursor = &x11->next_window;
    }
    if (g_vgfx_cursor_window == win)
        g_vgfx_cursor_window = g_vgfx_x11_windows;
    if (g_vgfx_clipboard_window == win)
        g_vgfx_clipboard_window = g_vgfx_x11_windows;
    x11_global_unlock();
}

/// @brief Select a usable window for X11 clipboard selection traffic.
/// @details Keeps the cached clipboard window when valid, otherwise prefers a
///          focused live window, then the cursor-service window, then the first
///          live window.  Returns NULL when no native window is usable.
/// @return Borrowed window pointer valid only while the global mutex remains held.
/// @pre g_x11_global_mu is held by the caller until it finishes using the result.
static struct vgfx_window *x11_clipboard_window_locked(void) {
    if (x11_window_usable(g_vgfx_clipboard_window)) {
        return g_vgfx_clipboard_window;
    }

    for (struct vgfx_window *win = g_vgfx_x11_windows; win;) {
        vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
        struct vgfx_window *next = x11 ? x11->next_window : NULL;
        vgfx_internal_event_lock(win);
        int focused = win->is_focused;
        vgfx_internal_event_unlock(win);
        if (x11_window_usable(win) && focused) {
            g_vgfx_clipboard_window = win;
            return win;
        }
        win = next;
    }

    if (x11_window_usable(g_vgfx_cursor_window)) {
        g_vgfx_clipboard_window = g_vgfx_cursor_window;
        return g_vgfx_clipboard_window;
    }
    if (x11_window_usable(g_vgfx_x11_windows)) {
        g_vgfx_clipboard_window = g_vgfx_x11_windows;
        return g_vgfx_clipboard_window;
    }

    g_vgfx_clipboard_window = NULL;
    return NULL;
}

/// @brief Acquire the X11 display-scale cache lock.
static void x11_scale_cache_lock(void) {
    while (vgfx_atomic_flag_test_and_set(&g_x11_scale_lock))
        vgfx_internal_event_wait();
}

/// @brief Release the X11 display-scale cache lock.
static void x11_scale_cache_unlock(void) {
    vgfx_atomic_flag_clear(&g_x11_scale_lock);
}

/// @brief Set modern and legacy X11 title properties from UTF-8 text.
/// @details EWMH-aware window managers prefer _NET_WM_NAME/_NET_WM_ICON_NAME
///          with UTF8_STRING. XStoreName/XSetIconName are still updated as a
///          fallback for older window managers and tools.
/// @param display Open X11 display connection.
/// @param window Native window whose title properties should be changed.
/// @param title NUL-terminated UTF-8 title.
static void x11_set_window_title_utf8(Display *display, Window window, const char *title) {
    if (!display || !window || !title)
        return;

    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom net_wm_icon_name = XInternAtom(display, "_NET_WM_ICON_NAME", False);
    size_t len = strlen(title);
    int x_len = len > (size_t)INT_MAX ? INT_MAX : (int)len;
    if (utf8 != None && net_wm_name != None) {
        XChangeProperty(display,
                        window,
                        net_wm_name,
                        utf8,
                        8,
                        PropModeReplace,
                        (const unsigned char *)title,
                        x_len);
    }
    if (utf8 != None && net_wm_icon_name != None) {
        XChangeProperty(display,
                        window,
                        net_wm_icon_name,
                        utf8,
                        8,
                        PropModeReplace,
                        (const unsigned char *)title,
                        x_len);
    }

    XStoreName(display, window, title);
    XSetIconName(display, window, title);
}

/// @brief Validate that an XGetWindowProperty result is an Atom list.
/// @details Several EWMH queries request `_NET_WM_STATE` as `XA_ATOM`.  Window
///          managers or proxying X servers may still return an unexpected type
///          or format.  This helper centralizes the check before the byte buffer
///          is cast to `Atom *`.
/// @param actual_type Type returned by XGetWindowProperty.
/// @param actual_format Element width returned by XGetWindowProperty.
/// @param data Returned property payload.
/// @return 1 when `data` may be treated as an array of Atom values; otherwise 0.
static int x11_is_atom_list_property(Atom actual_type,
                                     int actual_format,
                                     const unsigned char *data) {
    return data && actual_type == XA_ATOM && actual_format == 32;
}

/// @brief Allocate an XImage and its independent native presentation buffer.
/// @details Validates the physical dimensions, allocates zeroed row storage,
///          creates an XImage around it using the window's selected visual, and
///          transfers both allocations through output parameters.  Ownership
///          remains with the caller on success.
/// @param x11 Initialized platform state containing display, visual, and depth.
/// @param width Physical image width in pixels.
/// @param height Physical image height in pixels.
/// @param stride Row stride in bytes.
/// @param out_image Receives the new XImage.
/// @param out_buf Receives its separately owned data buffer.
/// @param out_size Receives the data-buffer size in bytes.
/// @return 1 on success, or 0 after setting an internal error on invalid
///         dimensions, allocation failure, or XImage creation failure.
static int x11_create_ximage_resources(vgfx_x11_data *x11,
                                       int32_t width,
                                       int32_t height,
                                       int32_t stride,
                                       XImage **out_image,
                                       uint8_t **out_buf,
                                       size_t *out_size) {
    if (!x11 || !out_image || !out_buf || !out_size)
        return 0;

    *out_image = NULL;
    *out_buf = NULL;
    *out_size = 0;

    if (!x11->display)
        return 0;

    if (height <= 0 || stride <= 0 || (size_t)height > SIZE_MAX / (size_t)stride) {
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM, "Invalid XImage buffer dimensions");
        return 0;
    }
    size_t buf_size = (size_t)height * (size_t)stride;
    uint8_t *buf = (uint8_t *)calloc(1, buf_size);
    if (!buf) {
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to allocate XImage buffer");
        return 0;
    }

    XImage *image = XCreateImage(
        x11->display, x11->visual, x11->depth, ZPixmap, 0, (char *)buf, width, height, 32, stride);
    if (!image) {
        free(buf);
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to create XImage");
        return 0;
    }

    image->byte_order = ImageByteOrder(x11->display);
    *out_image = image;
    *out_buf = buf;
    *out_size = buf_size;
    return 1;
}

/// @brief Replace a window's current XImage and presentation buffer.
/// @details Destroys the prior XImage without allowing it to free the separately
///          managed data pointer, frees the old buffer, and adopts all new
///          resources supplied by the caller.
/// @param x11 Platform state that takes ownership of the replacement resources.
/// @param new_image Replacement XImage, or NULL.
/// @param new_buf Replacement presentation buffer, or NULL.
/// @param new_size Size of @p new_buf in bytes.
static void x11_replace_ximage(vgfx_x11_data *x11,
                               XImage *new_image,
                               uint8_t *new_buf,
                               size_t new_size) {
    if (!x11)
        return;

    if (x11->ximage) {
        x11->ximage->data = NULL;
        XDestroyImage(x11->ximage);
    }
    free(x11->ximage_buf);
    x11->ximage = new_image;
    x11->ximage_buf = new_buf;
    x11->ximage_buf_size = new_size;
}

/// @brief Recreate presentation resources for the window's current framebuffer.
/// @param win Window whose physical dimensions and stride should be mirrored.
/// @return 1 when new resources were installed, otherwise 0.
static int x11_recreate_ximage(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    XImage *new_image = NULL;
    uint8_t *new_buf = NULL;
    size_t new_size = 0;
    if (!x11_create_ximage_resources(
            x11, win->width, win->height, win->stride, &new_image, &new_buf, &new_size)) {
        return 0;
    }

    x11_replace_ximage(x11, new_image, new_buf, new_size);
    x11->width = win->width;
    x11->height = win->height;
    return 1;
}

/// @brief Resize both the core framebuffer and X11 presentation backing store.
/// @details Validates physical limits and allocates replacement XImage resources
///          before changing the shared framebuffer, so allocation failure leaves
///          existing state intact.  On success it adopts the new presentation
///          storage, refreshes cached dimensions, and optionally queues a resize
///          event containing physical and logical extents.
/// @param win Window whose backing stores should be resized.
/// @param new_w New physical width in pixels.
/// @param new_h New physical height in pixels.
/// @param timestamp Monotonic timestamp for an optional resize event.
/// @param emit_event Non-zero to enqueue `VGFX_EVENT_RESIZE`.
/// @return 1 on complete success, otherwise 0.
static int x11_resize_backing_store(
    struct vgfx_window *win, int32_t new_w, int32_t new_h, int64_t timestamp, int emit_event) {
    if (!win || !win->platform_data)
        return 0;
    if (new_w <= 0 || new_h <= 0 || new_w > VGFX_MAX_WIDTH || new_h > VGFX_MAX_HEIGHT ||
        new_w > INT32_MAX / 4) {
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM, "X11 resize exceeds framebuffer limits");
        return 0;
    }

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    int32_t new_stride = new_w * 4;
    XImage *new_image = NULL;
    uint8_t *new_buf = NULL;
    size_t new_size = 0;
    if (!x11_create_ximage_resources(
            x11, new_w, new_h, new_stride, &new_image, &new_buf, &new_size)) {
        return 0;
    }

    if (!vgfx_internal_resize_framebuffer(win, new_w, new_h)) {
        if (new_image) {
            new_image->data = NULL;
            XDestroyImage(new_image);
        }
        free(new_buf);
        return 0;
    }

    x11_replace_ximage(x11, new_image, new_buf, new_size);
    x11->width = new_w;
    x11->height = new_h;

    if (emit_event) {
        vgfx_event_t event = {0};
        vgfx_internal_init_resize_event(&event, win, timestamp, new_w, new_h);
        vgfx_internal_enqueue_event(win, &event);
    }

    return 1;
}

/// @brief Release all X11 resources owned by a window's platform state.
/// @details Unregisters global service references, destroys presentation and
///          input-method resources, cursor handles, graphics context, colormap,
///          native window, and display connection, then clears platform_data.
///          Partial initialization and NULL input are accepted.
/// @param win Window whose platform resources should be released.
static void x11_cleanup_platform(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;

    x11_unregister_window(win);

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;

    if (x11->ximage) {
        x11->ximage->data = NULL;
        XDestroyImage(x11->ximage);
        x11->ximage = NULL;
    }

    free(x11->ximage_buf);
    x11->ximage_buf = NULL;
    x11->ximage_buf_size = 0;

    free(x11->clipboard_text);
    x11->clipboard_text = NULL;
    free(x11->ime_preedit);
    x11->ime_preedit = NULL;
    x11->ime_preedit_count = 0;
    x11->ime_preedit_capacity = 0;

    if (x11->display) {
        if (x11->gc) {
            XFreeGC(x11->display, x11->gc);
            x11->gc = NULL;
        }
        if (x11->blank_cursor) {
            XFreeCursor(x11->display, x11->blank_cursor);
            x11->blank_cursor = 0;
        }
        for (size_t i = 0; i < sizeof(x11->cursor_cache) / sizeof(x11->cursor_cache[0]); i++) {
            if (x11->cursor_cache[i]) {
                XFreeCursor(x11->display, x11->cursor_cache[i]);
                x11->cursor_cache[i] = 0;
            }
        }
        if (x11->xic) {
            XDestroyIC(x11->xic);
            x11->xic = NULL;
        }
        if (x11->xim) {
            XCloseIM(x11->xim);
            x11->xim = NULL;
        }
        if (x11->colormap && x11->colormap != DefaultColormap(x11->display, x11->screen)) {
            XFreeColormap(x11->display, x11->colormap);
            x11->colormap = 0;
        }
        if (x11->window) {
            x11_global_lock();
            int (*old_handler)(Display *, XErrorEvent *) =
                XSetErrorHandler(x11_ignore_bad_window_error);
            XDestroyWindow(x11->display, x11->window);
            XSync(x11->display, False);
            XSetErrorHandler(old_handler);
            x11_global_unlock();
            x11->window = 0;
        }
        XCloseDisplay(x11->display);
        x11->display = NULL;
    }

    free(x11);
    win->platform_data = NULL;
}

//===----------------------------------------------------------------------===//
// Platform API Implementation
//===----------------------------------------------------------------------===//

/// @brief Query the HiDPI backing scale factor for the X11 display.
/// @details Tries environment and X11 sources in priority order:
///
///   1. GDK_SCALE env var — set by GNOME/Mutter on both Wayland and X11.
///      Example: GDK_SCALE=2 on a HiDPI GNOME desktop.
///
///   2. QT_SCALE_FACTOR env var — set by KDE Plasma.
///      Example: QT_SCALE_FACTOR=1.5 on a KDE 150% display.
///
///   3. Xft.dpi from the X11 resource database (XResourceManagerString).
///      Standard 96 DPI → scale 1.0; 192 DPI → scale 2.0.
///
///   4. Fallback: 1.0 (standard 96 DPI display).
///
/// @note X11 always reports physical pixel coordinates in events and
///       XConfigureNotify, so no additional scaling is needed in the
///       event handlers or resize handler on Linux.
///
/// @return Scale factor ≥ 1.0
float vgfx_platform_get_display_scale(void) {
    /* Priority 1: GDK_SCALE env var (GNOME/Mutter on Wayland and X11) */
    const char *gdk = getenv("GDK_SCALE");
    if (gdk) {
        char *end = NULL;
        errno = 0;
        float s = strtof(gdk, &end);
        if (errno == 0 && end && *end == '\0' && s >= 1.0f)
            return vgfx_internal_sanitize_scale(s);
    }

    /* Priority 2: QT_SCALE_FACTOR (KDE Plasma) */
    const char *qt = getenv("QT_SCALE_FACTOR");
    if (qt) {
        char *end = NULL;
        errno = 0;
        float s = strtof(qt, &end);
        if (errno == 0 && end && *end == '\0' && s >= 1.0f)
            return vgfx_internal_sanitize_scale(s);
    }

    x11_scale_cache_lock();
    if (g_x11_scale_cached) {
        float scale = g_x11_scale_value;
        x11_scale_cache_unlock();
        return scale;
    }
    x11_scale_cache_unlock();

    /* Priority 3: Xft.dpi from the X11 resource database.  The temporary
     * display connection is cached after the first successful query so repeated
     * scale reads do not continually connect to the X server. */
    x11_init_threads_once();
    Display *dpy = XOpenDisplay(NULL);
    float scale = 1.0f;
    if (dpy) {
        const char *rms = XResourceManagerString(dpy);
        if (rms) {
            /* Search for "Xft.dpi:\t96" or "Xft.dpi: 192" etc. */
            const char *pos = strstr(rms, "Xft.dpi:");
            if (pos) {
                pos += 8; /* skip "Xft.dpi:" */
                while (*pos == ' ' || *pos == '\t')
                    pos++;
                char *end = NULL;
                errno = 0;
                float dpi = strtof(pos, &end);
                if (errno == 0 && end != pos && dpi >= 96.0f)
                    scale = dpi / 96.0f;
            }
        }
        XCloseDisplay(dpy);
    }

    scale = vgfx_internal_sanitize_scale(scale);
    x11_scale_cache_lock();
    g_x11_scale_value = scale;
    g_x11_scale_cached = 1;
    x11_scale_cache_unlock();
    return scale;
}

/// @brief Query the primary display's logical dimensions.
/// @details X11 reports physical pixels; divide by the display scale for
///          vgfx logical units. Uses a short-lived display connection (called
///          once per fullscreen window creation).
/// @param out_w Receives the primary display width in logical units when non-NULL.
/// @param out_h Receives the primary display height in logical units when non-NULL.
/// @return 1 on success, 0 when no X display is reachable.
int vgfx_platform_get_display_logical_size(int32_t *out_w, int32_t *out_h) {
    x11_init_threads_once();
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 0;
    int screen = DefaultScreen(dpy);
    int phys_w = DisplayWidth(dpy, screen);
    int phys_h = DisplayHeight(dpy, screen);
    XCloseDisplay(dpy);
    if (phys_w <= 0 || phys_h <= 0)
        return 0;
    float scale = vgfx_platform_get_display_scale();
    if (scale < 1.0f)
        scale = 1.0f;
    if (out_w)
        *out_w = (int32_t)((float)phys_w / scale);
    if (out_h)
        *out_h = (int32_t)((float)phys_h / scale);
    return 1;
}

/// @brief Initialize platform-specific window resources for X11.
/// @details Opens connection to X server, creates X11 window with appropriate
///          attributes, sets up WM_DELETE_WINDOW protocol for close button,
///          creates XImage wrapper for framebuffer, and makes window visible.
///
/// @param win    Pointer to the ZannaGFX window structure (framebuffer already allocated)
/// @param params Window creation parameters (title, dimensions, resizable flag)
/// @return 1 on success, 0 on failure
///
/// @pre  win != NULL
/// @pre  params != NULL
/// @pre  win->pixels != NULL (framebuffer allocated by vgfx_create_window)
/// @post On success: X11 window created and visible, platform_data allocated
/// @post On failure: platform_data NULL, error set
///
/// @details The window is:
///            - Has a title bar
///            - Can be closed (intercepts WM_DELETE_WINDOW)
///            - Receives keyboard and mouse input
///            - 32-bit depth for direct RGBA rendering
///            - Made visible unless ZANNA_GFX_HIDE_WINDOWS is set
///            - Hints that the window should not take focus when ZANNA_GFX_NO_ACTIVATE is set
int vgfx_platform_init_window(struct vgfx_window *win, const vgfx_window_params_t *params) {
    if (!win || !params)
        return 0;

    int hide_window = vgfx_x11_hide_windows();
    int no_activate = vgfx_x11_no_activate_on_create();

    /* Allocate platform data structure */
    vgfx_x11_data *x11 = (vgfx_x11_data *)calloc(1, sizeof(vgfx_x11_data));
    if (!x11) {
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to allocate X11 platform data");
        return 0;
    }

    win->platform_data = x11;
    x11->close_requested = 0;
    x11->xi_opcode = -1;
    x11->cursor_type = 0;
    x11->cursor_visible = 1;
    x11->width = win->width;
    x11->height = win->height;
    x11->hidden = hide_window;

    /* Open connection to X server */
    if (!x11_init_threads_once()) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to initialize thread-safe Xlib access");
        x11_cleanup_platform(win);
        return 0;
    }
    x11->display = XOpenDisplay(NULL);
    if (!x11->display) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to open X11 display");
        x11_cleanup_platform(win);
        return 0;
    }

    x11->screen = DefaultScreen(x11->display);
    Window root = RootWindow(x11->display, x11->screen);

    /* Find a visual that works for both GPU and software presentation.  Prefer
     * a double-buffered GLX visual because Canvas3D enables GPU presentation
     * after the window already exists.  Fall back to the original XImage path:
     * a 32-bit TrueColor visual first, then the screen default visual. */
    XVisualInfo vinfo;
    if (x11_try_choose_glx_visual(x11, root)) {
        /* Fields were filled by the GLX visual helper. */
    } else if (XMatchVisualInfo(x11->display, x11->screen, 32, TrueColor, &vinfo)) {
        x11->visual = vinfo.visual;
        x11->depth = 32;
        x11->colormap = XCreateColormap(x11->display, root, x11->visual, AllocNone);
    } else {
        x11->visual = DefaultVisual(x11->display, x11->screen);
        x11->depth = DefaultDepth(x11->display, x11->screen);
        x11->colormap = DefaultColormap(x11->display, x11->screen);
    }

    XSetWindowAttributes attrs;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.colormap = x11->colormap;
    attrs.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | ExposureMask | FocusChangeMask | StructureNotifyMask |
                       PropertyChangeMask;

    x11->window = XCreateWindow(x11->display,
                                root,
                                0,
                                0, /* x, y position (will be overridden) */
                                (unsigned int)win->width,
                                (unsigned int)win->height,
                                0,           /* border width */
                                x11->depth,  /* depth matching our visual */
                                InputOutput, /* class */
                                x11->visual, /* visual matching our depth */
                                CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
                                &attrs);

    if (!x11->window) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to create X11 window");
        x11_cleanup_platform(win);
        return 0;
    }

    /* Set window title */
    x11_set_window_title_utf8(x11->display, x11->window, params->title);

    x11->xim = XOpenIM(x11->display, NULL, NULL, NULL);
    if (x11->xim)
        x11->xic = x11_create_input_context(x11);

    /* Set window size hints (prevents resizing if not resizable) */
    XSizeHints *size_hints = XAllocSizeHints();
    if (size_hints) {
        size_hints->flags = PSize | PMinSize | PMaxSize;
        size_hints->width = win->width;
        size_hints->height = win->height;
        size_hints->min_width = params->resizable ? 1 : win->width;
        size_hints->min_height = params->resizable ? 1 : win->height;
        size_hints->max_width = params->resizable ? 16384 : win->width;
        size_hints->max_height = params->resizable ? 16384 : win->height;
        XSetWMNormalHints(x11->display, x11->window, size_hints);
        XFree(size_hints);
    }

    if (no_activate) {
        XWMHints *wm_hints = XAllocWMHints();
        if (wm_hints) {
            wm_hints->flags = InputHint;
            wm_hints->input = False;
            XSetWMHints(x11->display, x11->window, wm_hints);
            XFree(wm_hints);
        }

        Atom net_wm_user_time = XInternAtom(x11->display, "_NET_WM_USER_TIME", False);
        if (net_wm_user_time != None) {
            unsigned long user_time = 0;
            XChangeProperty(x11->display,
                            x11->window,
                            net_wm_user_time,
                            XA_CARDINAL,
                            32,
                            PropModeReplace,
                            (unsigned char *)&user_time,
                            1);
        }
        vgfx_internal_set_focus_state(win, 0);
    }

    /* Set up WM_DELETE_WINDOW protocol (intercept close button) */
    x11->wm_delete_window = XInternAtom(x11->display, "WM_DELETE_WINDOW", False);
    x11->event_wake = XInternAtom(x11->display, "_ZANNA_EVENT_WAKE", False);
    XSetWMProtocols(x11->display, x11->window, &x11->wm_delete_window, 1);

    /* Set up XDND (drag-and-drop) protocol */
    x11->xdnd_aware = XInternAtom(x11->display, "XdndAware", False);
    x11->xdnd_enter = XInternAtom(x11->display, "XdndEnter", False);
    x11->xdnd_position = XInternAtom(x11->display, "XdndPosition", False);
    x11->xdnd_status = XInternAtom(x11->display, "XdndStatus", False);
    x11->xdnd_drop = XInternAtom(x11->display, "XdndDrop", False);
    x11->xdnd_finished = XInternAtom(x11->display, "XdndFinished", False);
    x11->xdnd_selection = XInternAtom(x11->display, "XdndSelection", False);
    x11->xdnd_type_list = XInternAtom(x11->display, "XdndTypeList", False);
    x11->text_uri_list = XInternAtom(x11->display, "text/uri-list", False);
    x11->clipboard_atom = XInternAtom(x11->display, "CLIPBOARD", False);
    x11->utf8_string_atom = XInternAtom(x11->display, "UTF8_STRING", False);
    x11->targets_atom = XInternAtom(x11->display, "TARGETS", False);
    x11->incr_atom = XInternAtom(x11->display, "INCR", False);
    x11->clipboard_property_atom = XInternAtom(x11->display, "ZANNAGFX_CLIPBOARD", False);
    if (x11->wm_delete_window == None || x11->event_wake == None ||
        x11->xdnd_aware == None || x11->xdnd_enter == None ||
        x11->xdnd_position == None || x11->xdnd_status == None || x11->xdnd_drop == None ||
        x11->xdnd_finished == None || x11->xdnd_selection == None || x11->xdnd_type_list == None ||
        x11->text_uri_list == None || x11->clipboard_atom == None ||
        x11->utf8_string_atom == None || x11->targets_atom == None || x11->incr_atom == None ||
        x11->clipboard_property_atom == None) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to initialize X11 protocol atoms");
        x11_cleanup_platform(win);
        return 0;
    }
    x11->xdnd_source = 0;
    {
        /* Advertise XDND version 5 support */
        Atom xdnd_version = 5;
        XChangeProperty(x11->display,
                        x11->window,
                        x11->xdnd_aware,
                        XA_ATOM,
                        32,
                        PropModeReplace,
                        (unsigned char *)&xdnd_version,
                        1);
    }

    /* Create graphics context */
    x11->gc = XCreateGC(x11->display, x11->window, 0, NULL);
    if (!x11->gc) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to create X11 GC");
        x11_cleanup_platform(win);
        return 0;
    }

    /* Allocate the presentation buffer/XImage at the framebuffer size in
     * physical pixels so present and resize stay consistent with win->pixels. */
    if (!x11_recreate_ximage(win)) {
        x11_cleanup_platform(win);
        return 0;
    }

    /* Map (show) the window unless tests explicitly request hidden graphics windows. */
    if (!hide_window)
        XMapWindow(x11->display, x11->window);
    XFlush(x11->display);
    if (!hide_window)
        x11_wait_for_viewable(x11);

    /* Publish only fully initialized windows to process-global services. */
    x11_register_window(win);

    return 1;
}

/// @brief Destroy platform-specific window resources for X11.
/// @details Destroys XImage wrapper, closes X11 window, frees graphics
///          context, closes display connection, and frees platform data.
///          Safe to call even if init failed.
///
/// @param win Pointer to the ZannaGFX window structure
///
/// @pre  win != NULL
/// @post platform_data freed and set to NULL
/// @post X11 window destroyed and display connection closed (if existed)
void vgfx_platform_destroy_window(struct vgfx_window *win) {
    x11_cleanup_platform(win);
}

/// @brief Duplicate nullable clipboard text into owned storage.
/// @details Treats NULL as an empty string and includes the trailing NUL byte.
/// @param text Source text, or NULL.
/// @return Newly allocated copy, or NULL on allocation failure.
static char *x11_strdup_text(const char *text) {
    const char *src = text ? text : "";
    size_t len = strlen(src);
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, src, len + 1u);
    return copy;
}

/// @brief Answer an ICCCM clipboard selection request owned by this window.
/// @details Advertises supported targets or publishes stored text as UTF8_STRING
///          or XA_STRING, then sends the required SelectionNotify response.  An
///          unsupported request is answered with a `None` property.
/// @param x11 Clipboard-owning platform state.
/// @param request Borrowed native selection request.
static void x11_handle_selection_request(vgfx_x11_data *x11, XSelectionRequestEvent *request) {
    if (!x11 || !x11->display || !request)
        return;

    XSelectionEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.type = SelectionNotify;
    reply.display = request->display;
    reply.requestor = request->requestor;
    reply.selection = request->selection;
    reply.target = request->target;
    reply.time = request->time;
    reply.property = None;

    Atom property = request->property != None ? request->property : request->target;
    if (request->selection == x11->clipboard_atom && x11->clipboard_text) {
        if (request->target == x11->targets_atom) {
            Atom targets[] = {x11->targets_atom, x11->utf8_string_atom, XA_STRING};
            XChangeProperty(x11->display,
                            request->requestor,
                            property,
                            XA_ATOM,
                            32,
                            PropModeReplace,
                            (const unsigned char *)targets,
                            (int)(sizeof(targets) / sizeof(targets[0])));
            reply.property = property;
        } else if (request->target == x11->utf8_string_atom || request->target == XA_STRING) {
            const unsigned char *text = (const unsigned char *)x11->clipboard_text;
            size_t len = strlen(x11->clipboard_text);
            if (len > INT32_MAX)
                len = INT32_MAX;
            XChangeProperty(x11->display,
                            request->requestor,
                            property,
                            request->target,
                            8,
                            PropModeReplace,
                            text,
                            (int)len);
            reply.property = property;
        }
    }

    XSendEvent(x11->display, request->requestor, False, 0, (XEvent *)&reply);
    XFlush(x11->display);
}

/// @brief Match clipboard SelectionNotify events for a waiting requestor.
/// @param display Display passed by Xlib; not otherwise consulted.
/// @param event Candidate event.
/// @param arg Borrowed `vgfx_x11_data` pointer identifying requestor and selection.
/// @return True only for the expected window's CLIPBOARD SelectionNotify.
static Bool x11_clipboard_selection_notify_predicate(Display *display,
                                                     XEvent *event,
                                                     XPointer arg) {
    (void)display;
    vgfx_x11_data *x11 = (vgfx_x11_data *)arg;
    return x11 && event && event->type == SelectionNotify &&
           event->xselection.requestor == x11->window &&
           event->xselection.selection == x11->clipboard_atom;
}

/// @brief Predicate context for waiting on one incremental clipboard property.
typedef struct {
    vgfx_x11_data *x11;
    Atom property;
} x11_property_wait_t;

/// @brief Match a new-value notification for an incremental clipboard property.
/// @param display Display passed by Xlib; not otherwise consulted.
/// @param event Candidate event.
/// @param arg Borrowed `x11_property_wait_t` identifying window and property.
/// @return True only for a matching `PropertyNewValue` notification.
static Bool x11_property_new_value_predicate(Display *display, XEvent *event, XPointer arg) {
    (void)display;
    x11_property_wait_t *wait = (x11_property_wait_t *)arg;
    return wait && wait->x11 && event && event->type == PropertyNotify &&
           event->xproperty.window == wait->x11->window &&
           event->xproperty.atom == wait->property && event->xproperty.state == PropertyNewValue;
}

/// @brief Append a clipboard chunk to a bounded growable byte string.
/// @details Enforces the 16 MiB clipboard ceiling, reserves a terminator, grows
///          geometrically without overflowing `size_t`, and leaves the caller's
///          accumulated state unchanged when allocation fails.
/// @param result Address of the owned destination allocation.
/// @param len Address of its current payload length.
/// @param cap Address of its allocated capacity.
/// @param data Borrowed bytes to append.
/// @param nitems Number of bytes to append.
/// @return 1 when the chunk was appended or empty, otherwise 0.
static int x11_append_bytes(
    char **result, size_t *len, size_t *cap, const unsigned char *data, size_t nitems) {
    if (!result || !len || !cap)
        return 0;
    if (nitems == 0)
        return 1;
    if (*len > VGFX_X11_CLIPBOARD_MAX_BYTES || nitems > VGFX_X11_CLIPBOARD_MAX_BYTES - *len)
        return 0;
    if (nitems > SIZE_MAX - *len - 1u)
        return 0;
    size_t needed = *len + nitems + 1u;
    if (needed > *cap) {
        size_t new_cap = *cap ? *cap : 4096u;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2u) {
                new_cap = needed;
                break;
            }
            new_cap *= 2u;
        }
        char *next = (char *)realloc(*result, new_cap);
        if (!next)
            return 0;
        *result = next;
        *cap = new_cap;
    }
    memcpy(*result + *len, data, nitems);
    *len += nitems;
    (*result)[*len] = '\0';
    return 1;
}

/// @brief Receive an ICCCM INCR clipboard transfer into one owned string.
/// @details Deletes the property to acknowledge transfer start, waits for
///          successive property chunks, validates target and eight-bit format,
///          bounds total size, and resets a one-second inactivity timeout after
///          each chunk.  The zero-length terminal chunk completes the transfer.
/// @param x11 Platform state acting as the selection requestor.
/// @param property Property used for incremental delivery.
/// @param requested_target Requested UTF8_STRING or XA_STRING target.
/// @return NUL-terminated owned text, including an allocated empty string for
///         an empty transfer, or NULL on timeout/protocol/allocation failure.
static char *x11_read_incr_text_property(vgfx_x11_data *x11, Atom property, Atom requested_target) {
    if (!x11 || !x11->display || property == None)
        return NULL;

    XDeleteProperty(x11->display, x11->window, property);
    XFlush(x11->display);

    char *result = NULL;
    size_t len = 0;
    size_t cap = 0;
    int64_t start = vgfx_platform_now_ms();
    x11_property_wait_t wait = {x11, property};

    while (vgfx_platform_now_ms() - start < 1000) {
        XEvent prop_event;
        if (!XCheckIfEvent(
                x11->display, &prop_event, x11_property_new_value_predicate, (XPointer)&wait)) {
            usleep(1000);
            continue;
        }

        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(x11->display,
                                        x11->window,
                                        property,
                                        0,
                                        262144,
                                        True,
                                        AnyPropertyType,
                                        &actual_type,
                                        &actual_format,
                                        &nitems,
                                        &bytes_after,
                                        &data);
        if (status != Success) {
            if (data)
                XFree(data);
            free(result);
            return NULL;
        }

        if (nitems == 0 && bytes_after == 0) {
            if (data)
                XFree(data);
            if (!result) {
                result = (char *)malloc(1u);
                if (result)
                    result[0] = '\0';
            }
            return result;
        }

        if (actual_format != 8 ||
            !(actual_type == requested_target || actual_type == x11->utf8_string_atom ||
              actual_type == XA_STRING)) {
            if (data)
                XFree(data);
            free(result);
            return NULL;
        }

        if (!x11_append_bytes(&result, &len, &cap, data, (size_t)nitems)) {
            if (data)
                XFree(data);
            free(result);
            return NULL;
        }
        if (data)
            XFree(data);
        start = vgfx_platform_now_ms();
    }

    free(result);
    return NULL;
}

/// @brief Read a text selection property, including ICCCM incremental transfers.
/// @details Fetches ordinary eight-bit chunks until `bytes_after` reaches zero,
///          delegates INCR properties to the incremental reader, validates the
///          advertised type, deletes the temporary property, and returns bounded
///          NUL-terminated owned storage.
/// @param x11 Platform state containing display, requestor window, and atoms.
/// @param property Selection-conversion property to read.
/// @param requested_target Requested UTF8_STRING or XA_STRING target.
/// @return Owned text (possibly empty), or NULL on invalid input, protocol
///         mismatch, Xlib error, excessive size, or allocation failure.
static char *x11_read_text_property(vgfx_x11_data *x11, Atom property, Atom requested_target) {
    if (!x11 || !x11->display || !x11->window || property == None)
        return NULL;

    char *result = NULL;
    size_t len = 0;
    size_t cap = 0;
    long offset = 0;
    unsigned long bytes_after = 0;

    do {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned char *data = NULL;
        int status = XGetWindowProperty(x11->display,
                                        x11->window,
                                        property,
                                        offset,
                                        262144,
                                        False,
                                        AnyPropertyType,
                                        &actual_type,
                                        &actual_format,
                                        &nitems,
                                        &bytes_after,
                                        &data);

        if (status != Success) {
            if (data)
                XFree(data);
            free(result);
            XDeleteProperty(x11->display, x11->window, property);
            return NULL;
        }

        if (actual_type == x11->incr_atom) {
            if (data)
                XFree(data);
            free(result);
            return x11_read_incr_text_property(x11, property, requested_target);
        }

        if (actual_format != 8 ||
            !(actual_type == requested_target || actual_type == x11->utf8_string_atom ||
              actual_type == XA_STRING)) {
            if (data)
                XFree(data);
            free(result);
            XDeleteProperty(x11->display, x11->window, property);
            return NULL;
        }

        if (nitems > 0) {
            if (!x11_append_bytes(&result, &len, &cap, data, (size_t)nitems)) {
                if (data)
                    XFree(data);
                free(result);
                XDeleteProperty(x11->display, x11->window, property);
                return NULL;
            }
        }

        if (data)
            XFree(data);
        offset += (long)((nitems + 3ul) / 4ul);
    } while (bytes_after > 0);

    XDeleteProperty(x11->display, x11->window, property);

    if (!result) {
        result = (char *)malloc(1u);
        if (result)
            result[0] = '\0';
    }
    return result;
}

/* Relative-mouse helpers implemented after the XInput2 loader at the bottom
 * of this file (see "Relative (raw) mouse mode" section). */
/// @brief Decode an XInput2 generic event when native relative mode is active.
/// @param win Window receiving any decoded raw-motion delta.
/// @param event Borrowed generic X11 event.
static void x11_handle_generic_event(struct vgfx_window *win, XEvent *event);

/// @brief Apply or release the pointer grab used by relative mouse mode.
/// @param win Window whose pointer grab should change.
/// @param enable Non-zero to grab and confine, zero to release.
/// @return 1 when the requested state was applied, otherwise 0.
static int x11_relative_apply_grab(struct vgfx_window *win, int enable);

/// @brief Wait for X11 connection activity without dispatching events.
/// @details Returns immediately when Xlib already buffers events; otherwise
///          polls the display connection descriptor, retrying interrupted waits.
///          Event decoding remains the responsibility of
///          `vgfx_platform_process_events()`.
/// @param win Window selecting the display connection.
/// @param timeout_ms Maximum positive wait in milliseconds.
/// @return 1 when the connection became readable or events were already
///         buffered, otherwise 0 for timeout, invalid state, or poll failure.
int vgfx_platform_wait_events(struct vgfx_window *win, int32_t timeout_ms) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display)
        return 0;
    if (timeout_ms <= 0)
        return 0;
    /* Events already buffered client-side: return immediately. */
    if (XPending(x11->display) > 0)
        return 1;
    /* Block on the X connection fd until data arrives or the timeout elapses.
       Events are left for vgfx_platform_process_events to read. */
    struct pollfd pfd;
    pfd.fd = ConnectionNumber(x11->display);
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    return r > 0 && (pfd.revents & POLLIN) != 0 ? 1 : 0;
}

/// @copydoc vgfx_platform_wake_events
int vgfx_platform_wake_events(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return 0;
    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.display = x11->display;
    event.xclient.window = x11->window;
    event.xclient.message_type = x11->event_wake;
    event.xclient.format = 32;
    int sent = XSendEvent(x11->display, x11->window, False, NoEventMask, &event);
    XFlush(x11->display);
    return sent != 0 ? 1 : 0;
}

/// @brief Process pending X11 events and translate them to ZannaGFX state.
/// @details Drains Xlib's buffered events without blocking, giving XIM first
///          access and translating keyboard, committed text/composition, mouse,
///          wheel, close, XDND, clipboard, focus, XInput2 raw motion, and resize
///          activity.  Sticky polling state and relative-mode grabs are updated
///          alongside the public event queue.
/// @param win Window whose display queue should be drained.
/// @return 1 after processing all pending events, or 0 for invalid platform state.
/// @post On success, all currently pending X events have been consumed or
///       filtered and corresponding public state/events have been published.
int vgfx_platform_process_events(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display)
        return 0;

    /* Process all pending events without blocking */
    while (XPending(x11->display) > 0) {
        XEvent event;
        XNextEvent(x11->display, &event);

        /* Give the active XIM first access so preedit callbacks can publish
           lifecycle events and consume implementation-private messages. */
        if (XFilterEvent(&event, x11->window))
            continue;

        int64_t timestamp = vgfx_platform_now_ms();

        switch (event.type) {
            case KeyPress: {
                KeySym keysym = NoSymbol;
                int mods = x11_modifiers(event.xkey.state);
                char text_buf[16];
                char *text_storage = text_buf;
                int text_len = 0;
                if (x11->xic) {
                    Status status = 0;
                    text_len = Xutf8LookupString(x11->xic,
                                                 &event.xkey,
                                                 text_storage,
                                                 (int)sizeof(text_buf),
                                                 &keysym,
                                                 &status);
                    if (status == XBufferOverflow && text_len > 0 && text_len < INT_MAX) {
                        int text_cap = text_len + 1;
                        text_storage = (char *)malloc((size_t)text_cap);
                        if (text_storage) {
                            text_len = Xutf8LookupString(
                                x11->xic, &event.xkey, text_storage, text_cap, &keysym, &status);
                            if (status == XBufferOverflow)
                                text_len = 0;
                        } else {
                            text_storage = text_buf;
                            text_len = 0;
                        }
                    }
                    if (status == XLookupNone)
                        text_len = 0;
                } else {
                    text_len = XLookupString(
                        &event.xkey, text_storage, (int)sizeof(text_buf), &keysym, NULL);
                }
                if (keysym == NoSymbol)
                    keysym = XLookupKeysym(&event.xkey, 0);
                vgfx_key_t key = translate_keysym(keysym);

                if (key != VGFX_KEY_UNKNOWN && key < 512) {
                    int is_repeat = vgfx_key_down(win, key);
                    vgfx_internal_set_key_state(win, key, 1);

                    vgfx_event_t vgfx_event = {
                        .type = VGFX_EVENT_KEY_DOWN,
                        .time_ms = timestamp,
                        .data.key = {.key = key, .is_repeat = is_repeat, .modifiers = mods}};
                    vgfx_internal_enqueue_event(win, &vgfx_event);
                }
                if (x11->ime_active && text_len > 0) {
                    x11_ime_emit_commit(x11, text_storage, (size_t)text_len, mods, timestamp);
                } else {
                    x11_enqueue_text_input_events(win, timestamp, mods, text_storage, text_len);
                }
                if (text_storage != text_buf)
                    free(text_storage);
                break;
            }

            case KeyRelease: {
                /* X11 generates repeated KeyRelease/KeyPress pairs for key repeat.
                 * We detect true release by checking if there's an immediate KeyPress. */
                if (XEventsQueued(x11->display, QueuedAfterReading)) {
                    XEvent next_event;
                    XPeekEvent(x11->display, &next_event);

                    /* If next event is KeyPress for same key, it's a repeat - ignore release */
                    if (next_event.type == KeyPress && next_event.xkey.time == event.xkey.time &&
                        next_event.xkey.keycode == event.xkey.keycode) {
                        break; /* Ignore this release event */
                    }
                }

                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                vgfx_key_t key = translate_keysym(keysym);

                if (key != VGFX_KEY_UNKNOWN && key < 512) {
                    vgfx_internal_set_key_state(win, key, 0);

                    vgfx_event_t vgfx_event = {
                        .type = VGFX_EVENT_KEY_UP,
                        .time_ms = timestamp,
                        .data.key = {.key = key,
                                     .is_repeat = 0,
                                     .modifiers = x11_modifiers(event.xkey.state)}};
                    vgfx_internal_enqueue_event(win, &vgfx_event);
                }
                break;
            }

            case MotionNotify: {
                int32_t x = event.xmotion.x;
                int32_t y = event.xmotion.y;

                vgfx_internal_set_mouse_position(win, x, y);

                vgfx_event_t vgfx_event = {
                    .type = VGFX_EVENT_MOUSE_MOVE,
                    .time_ms = timestamp,
                    .data.mouse_move = {
                        .x = x, .y = y, .modifiers = x11_modifiers(event.xmotion.state)}};
                vgfx_internal_enqueue_coalesced_event(win, &vgfx_event);
                break;
            }

            case ButtonPress: {
                int32_t x = event.xbutton.x;
                int32_t y = event.xbutton.y;
                vgfx_internal_set_mouse_position(win, x, y);

                /* X11 mouse button mapping:
                 *   Button1 = Left (1)
                 *   Button2 = Middle (2)
                 *   Button3 = Right (3)
                 *   Button4/5 = Vertical scroll wheel
                 *   Button6/7 = Horizontal scroll wheel
                 */
                vgfx_mouse_button_t button = VGFX_MOUSE_LEFT;
                if (event.xbutton.button == Button1) {
                    button = VGFX_MOUSE_LEFT;
                } else if (event.xbutton.button == Button2) {
                    button = VGFX_MOUSE_MIDDLE;
                } else if (event.xbutton.button == Button3) {
                    button = VGFX_MOUSE_RIGHT;
                } else if (event.xbutton.button == Button4 || event.xbutton.button == Button5 ||
                           event.xbutton.button == VGFX_X11_BUTTON_SCROLL_LEFT ||
                           event.xbutton.button == VGFX_X11_BUTTON_SCROLL_RIGHT) {
                    float dx = 0.0f;
                    float dy = 0.0f;
                    if (event.xbutton.button == Button4)
                        dy = -1.0f;
                    else if (event.xbutton.button == Button5)
                        dy = 1.0f;
                    else if (event.xbutton.button == VGFX_X11_BUTTON_SCROLL_LEFT)
                        dx = -1.0f;
                    else
                        dx = 1.0f;
                    vgfx_event_t scroll_event = {
                        .type = VGFX_EVENT_SCROLL,
                        .time_ms = timestamp,
                        .data.scroll = {.delta_x = dx,
                                        .delta_y = dy,
                                        .x = x,
                                        .y = y,
                                        .modifiers = x11_modifiers(event.xbutton.state)}};
                    vgfx_internal_enqueue_event(win, &scroll_event);
                    break;
                } else {
                    break; /* Ignore extra buttons */
                }

                vgfx_internal_set_mouse_button_state(win, (int32_t)button, 1);

                vgfx_event_t vgfx_event = {
                    .type = VGFX_EVENT_MOUSE_DOWN,
                    .time_ms = timestamp,
                    .data.mouse_button = {.x = x,
                                          .y = y,
                                          .button = button,
                                          .modifiers = x11_modifiers(event.xbutton.state)}};
                vgfx_internal_enqueue_event(win, &vgfx_event);
                break;
            }

            case ButtonRelease: {
                int32_t x = event.xbutton.x;
                int32_t y = event.xbutton.y;
                vgfx_internal_set_mouse_position(win, x, y);

                vgfx_mouse_button_t button = VGFX_MOUSE_LEFT;
                if (event.xbutton.button == Button1) {
                    button = VGFX_MOUSE_LEFT;
                } else if (event.xbutton.button == Button2) {
                    button = VGFX_MOUSE_MIDDLE;
                } else if (event.xbutton.button == Button3) {
                    button = VGFX_MOUSE_RIGHT;
                } else {
                    break; /* Ignore scroll wheel and extra buttons */
                }

                vgfx_internal_set_mouse_button_state(win, (int32_t)button, 0);

                vgfx_event_t vgfx_event = {
                    .type = VGFX_EVENT_MOUSE_UP,
                    .time_ms = timestamp,
                    .data.mouse_button = {.x = x,
                                          .y = y,
                                          .button = button,
                                          .modifiers = x11_modifiers(event.xbutton.state)}};
                vgfx_internal_enqueue_event(win, &vgfx_event);
                break;
            }

            case ClientMessage: {
                /* Handle WM_DELETE_WINDOW (window close button clicked) */
                if ((Atom)event.xclient.data.l[0] == x11->wm_delete_window) {
                    vgfx_internal_event_lock(win);
                    int prevent_close = win->prevent_close;
                    vgfx_internal_event_unlock(win);
                    if (!prevent_close) {
                        x11->close_requested = 1;
                        vgfx_internal_set_close_requested(win, 1);
                    }

                    vgfx_event_t vgfx_event = {.type = VGFX_EVENT_CLOSE, .time_ms = timestamp};
                    vgfx_internal_enqueue_event(win, &vgfx_event);
                }
                /* XDND: drag entered our window */
                else if (event.xclient.message_type == x11->xdnd_enter) {
                    x11->xdnd_source = (Window)event.xclient.data.l[0];
                }
                /* XDND: drag positioned over our window — accept */
                else if (event.xclient.message_type == x11->xdnd_position) {
                    XEvent reply = {0};
                    reply.type = ClientMessage;
                    reply.xclient.window = x11->xdnd_source;
                    reply.xclient.message_type = x11->xdnd_status;
                    reply.xclient.format = 32;
                    reply.xclient.data.l[0] = (long)x11->window;
                    reply.xclient.data.l[1] = 1; /* Accept drop */
                    reply.xclient.data.l[4] =
                        (long)XInternAtom(x11->display, "XdndActionCopy", False);
                    XSendEvent(x11->display, x11->xdnd_source, False, NoEventMask, &reply);
                    XFlush(x11->display);
                }
                /* XDND: drop completed — request selection data */
                else if (event.xclient.message_type == x11->xdnd_drop) {
                    XConvertSelection(x11->display,
                                      x11->xdnd_selection,
                                      x11->text_uri_list,
                                      x11->xdnd_selection,
                                      x11->window,
                                      CurrentTime);
                }
                break;
            }

            case SelectionNotify: {
                /* XDND: received selection data (file paths as text/uri-list) */
                if (event.xselection.property == x11->xdnd_selection) {
                    char *data =
                        x11_read_text_property(x11, x11->xdnd_selection, x11->text_uri_list);
                    if (data && data[0] != '\0')
                        parse_xdnd_uri_list(
                            win, timestamp, (const unsigned char *)data, strlen(data));
                    free(data);

                    /* Send XdndFinished to complete the protocol */
                    XEvent reply = {0};
                    reply.type = ClientMessage;
                    reply.xclient.window = x11->xdnd_source;
                    reply.xclient.message_type = x11->xdnd_finished;
                    reply.xclient.format = 32;
                    reply.xclient.data.l[0] = (long)x11->window;
                    reply.xclient.data.l[1] = 1; /* Accepted */
                    reply.xclient.data.l[2] =
                        (long)XInternAtom(x11->display, "XdndActionCopy", False);
                    XSendEvent(x11->display, x11->xdnd_source, False, NoEventMask, &reply);
                    XFlush(x11->display);
                    x11->xdnd_source = 0;
                }
                break;
            }

            case SelectionRequest:
                x11_handle_selection_request(x11, &event.xselectionrequest);
                break;

            case SelectionClear:
                if (event.xselectionclear.selection == x11->clipboard_atom) {
                    free(x11->clipboard_text);
                    x11->clipboard_text = NULL;
                }
                break;

            case FocusIn: {
                if (x11->xic)
                    XSetICFocus(x11->xic);
                vgfx_internal_set_focus_state(win, 1);
                x11_global_lock();
                g_vgfx_cursor_window = win;
                g_vgfx_clipboard_window = win;
                x11_global_unlock();
                /* Re-confine the cursor while relative (raw) mouse is on. */
                if (win->relative_mouse_enabled)
                    x11_relative_apply_grab(win, 1);
                vgfx_event_t vgfx_event = {.type = VGFX_EVENT_FOCUS_GAINED, .time_ms = timestamp};
                vgfx_internal_enqueue_event(win, &vgfx_event);
                break;
            }

            case FocusOut: {
                if (x11->xic)
                    XUnsetICFocus(x11->xic);
                if (x11->ime_active) {
                    x11_ime_emit_boundary(x11, VGFX_EVENT_COMPOSITION_CANCEL);
                    x11->ime_active = 0;
                    x11->ime_preedit_count = 0;
                    x11->ime_caret = 0;
                }
                vgfx_internal_set_focus_state(win, 0);
                vgfx_internal_clear_input_state(win);
                /* Release the pointer grab while unfocused (re-applied on
                 * FocusIn) so the desktop stays usable. */
                if (win->relative_mouse_enabled)
                    x11_relative_apply_grab(win, 0);
                vgfx_event_t vgfx_event = {.type = VGFX_EVENT_FOCUS_LOST, .time_ms = timestamp};
                vgfx_internal_enqueue_event(win, &vgfx_event);
                break;
            }

            case GenericEvent: {
                /* XInput2 raw motion for relative (FPS mouse-look) mode. */
                x11_handle_generic_event(win, &event);
                break;
            }

            case ConfigureNotify: {
                /* Ignore configure notifications generated before the most
                 * recent client resize request. Without this ordering guard,
                 * a queued creation-time configure can overwrite the backing
                 * store dimensions synchronously published by SetWindowSize. */
                if (x11->resize_request_serial != 0 &&
                    event.xconfigure.serial < x11->resize_request_serial) {
                    break;
                }
                x11->resize_request_serial = 0;
                if (event.xconfigure.width > 0 && event.xconfigure.height > 0 &&
                    (event.xconfigure.width != x11->width ||
                     event.xconfigure.height != x11->height)) {
                    int32_t new_w = event.xconfigure.width;
                    int32_t new_h = event.xconfigure.height;
                    (void)x11_resize_backing_store(win, new_w, new_h, timestamp, 1);
                }
                break;
            }

            case Expose: {
                /* Window needs redraw - just note it, vgfx_present will handle */
                break;
            }

            default:
                /* Ignore unhandled event types */
                break;
        }
    }

    return 1;
}

/// @brief Locate an eight-bit color channel within a 32-bit X11 pixel mask.
/// @details Accepts only contiguous `0xFF` channel masks aligned to a byte
///          boundary, matching the conversion routine's byte-addressed writes.
/// @param mask Native visual channel mask.
/// @return Byte index in [0, 3], or -1 for zero/non-byte-aligned masks.
static int x11_mask_byte_index(unsigned long mask) {
    if (!mask)
        return -1;

    int shift = 0;
    while ((mask & 1ul) == 0ul) {
        mask >>= 1;
        shift++;
    }
    if (mask != 0xFFul || (shift % 8) != 0)
        return -1;

    int index = shift / 8;
    return (index >= 0 && index < 4) ? index : -1;
}

/// @brief Convert the RGBA framebuffer to the selected X11 visual's byte layout.
/// @details Derives byte indices from the visual masks, rejects unsupported or
///          overlapping channel layouts, copies directly when native order is
///          RGBA, and otherwise swizzles every pixel into the independent XImage
///          presentation buffer.  The unused byte, when present, receives alpha.
/// @param win Window supplying the source RGBA framebuffer and dimensions.
/// @param x11 Platform state supplying visual masks and destination storage.
/// @return 1 after conversion, otherwise 0 with an error set for unsupported masks.
static int x11_convert_rgba_to_native32(struct vgfx_window *win, vgfx_x11_data *x11) {
    if (!win || !x11 || !x11->visual || !win->pixels || !x11->ximage_buf)
        return 0;

    int r_index = x11_mask_byte_index(x11->visual->red_mask);
    int g_index = x11_mask_byte_index(x11->visual->green_mask);
    int b_index = x11_mask_byte_index(x11->visual->blue_mask);
    if (r_index < 0 || g_index < 0 || b_index < 0 || r_index == g_index || r_index == b_index ||
        g_index == b_index) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Unsupported X11 visual color masks");
        return 0;
    }

    int a_index = -1;
    for (int i = 0; i < 4; i++) {
        if (i != r_index && i != g_index && i != b_index) {
            a_index = i;
            break;
        }
    }

    const size_t pixel_count = (size_t)win->width * (size_t)win->height;
    const size_t byte_count = pixel_count * 4u;
    if (r_index == 0 && g_index == 1 && b_index == 2 && a_index == 3 &&
        byte_count <= x11->ximage_buf_size) {
        memcpy(x11->ximage_buf, win->pixels, byte_count);
        return 1;
    }

    const uint8_t *src = win->pixels;
    uint8_t *dst = x11->ximage_buf;
    for (size_t i = 0; i < pixel_count; ++i) {
        dst[0] = 0;
        dst[1] = 0;
        dst[2] = 0;
        dst[3] = 0;
        dst[r_index] = src[0];
        dst[g_index] = src[1];
        dst[b_index] = src[2];
        if (a_index >= 0)
            dst[a_index] = src[3];
        src += 4;
        dst += 4;
    }
    return 1;
}

/// @brief Present (blit) the framebuffer to the X11 window.
/// @details Converts the ZannaGFX RGBA framebuffer into the selected visual's
///          byte order in an independent XImage buffer, then sends it with
///          XPutImage and flushes the display.  Hidden windows and windows whose
///          GPU backend suppresses software presentation succeed without a blit.
///
/// @param win Pointer to the ZannaGFX window structure
/// @return 1 on success, 0 on failure
///
/// @pre  win != NULL
/// @pre  win->pixels != NULL (framebuffer valid)
/// @pre  win->platform_data != NULL
/// @post Framebuffer contents visible in X11 window
int vgfx_platform_present(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    if (win->skip_software_present)
        return 1;

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window || !x11->ximage)
        return 0;
    if (x11->hidden)
        return 1;

    if (!x11_convert_rgba_to_native32(win, x11))
        return 0;

    /* Blit presentation buffer to window using XPutImage */
    XPutImage(x11->display,
              x11->window,
              x11->gc,
              x11->ximage,
              0,
              0, /* src x, y */
              0,
              0, /* dst x, y */
              win->width,
              win->height);

    /* Flush to ensure immediate display */
    XFlush(x11->display);

    return 1;
}

/// @brief Get current high-resolution timestamp in milliseconds.
/// @details Returns a monotonic timestamp using CLOCK_MONOTONIC with
///          millisecond precision.  Never decreases, used for frame timing.
///
/// @return Milliseconds since arbitrary epoch (monotonic)
///
/// @post Return value >= previous calls within the same process
int64_t vgfx_platform_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/// @brief Sleep for the specified duration in milliseconds.
/// @details Uses nanosleep() for accurate sub-second delays.  If ms <= 0,
///          returns immediately without sleeping.  Used for FPS limiting.
///
/// @param ms Duration to sleep in milliseconds
void vgfx_platform_sleep_ms(int32_t ms) {
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000;
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        }
    }
}

/// @brief Yield the calling thread to the POSIX scheduler.
/// @details Used for short internal spin waits without adding a millisecond
///          sleep to event processing paths.
void vgfx_platform_yield(void) {
    sched_yield();
}

//===----------------------------------------------------------------------===//
// Window Title and Fullscreen
//===----------------------------------------------------------------------===//

/// @brief Set the window title.
/// @details Updates EWMH UTF-8 title properties and legacy title fallbacks.
///
/// @param win   Pointer to the window structure
/// @param title New title string (UTF-8)
void vgfx_platform_set_title(struct vgfx_window *win, const char *title) {
    if (!win || !win->platform_data || !title)
        return;

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;

    x11_set_window_title_utf8(x11->display, x11->window, title);
    XFlush(x11->display);
}

/// @copydoc vgfx_platform_set_icon
/// @details ADR 0317: publishes the EWMH `_NET_WM_ICON` property (width, height, then ARGB
///          words, each in a 32-bit "long" slot as the X protocol packs format-32 data).
void vgfx_platform_set_icon(struct vgfx_window *win,
                            const uint32_t *rgba,
                            int32_t width,
                            int32_t height) {
    if (!win || !win->platform_data || !rgba || width <= 0 || height <= 0)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;
    size_t count = 2u + (size_t)width * (size_t)height;
    if (count > (size_t)INT_MAX)
        return;
    long *buffer = (long *)malloc(count * sizeof(long));
    if (!buffer)
        return;
    buffer[0] = (long)width;
    buffer[1] = (long)height;
    for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
        uint32_t w = rgba[i];
        uint32_t r = (w >> 24) & 0xFFu;
        uint32_t g = (w >> 16) & 0xFFu;
        uint32_t b = (w >> 8) & 0xFFu;
        uint32_t a = w & 0xFFu;
        buffer[2 + i] = (long)(unsigned long)((a << 24) | (r << 16) | (g << 8) | b);
    }
    Atom net_wm_icon = XInternAtom(x11->display, "_NET_WM_ICON", False);
    Atom cardinal = XInternAtom(x11->display, "CARDINAL", False);
    if (net_wm_icon != None && cardinal != None) {
        XChangeProperty(x11->display,
                        x11->window,
                        net_wm_icon,
                        cardinal,
                        32,
                        PropModeReplace,
                        (const unsigned char *)buffer,
                        (int)count);
        XFlush(x11->display);
    }
    free(buffer);
}

/// @brief Set the window to fullscreen or windowed mode.
/// @details Uses the EWMH _NET_WM_STATE_FULLSCREEN hint to toggle fullscreen.
///          This is the standard way to request fullscreen on modern X11
///          window managers (GNOME, KDE, etc.).
///
/// @param win        Pointer to the window structure
/// @param fullscreen 1 for fullscreen, 0 for windowed
/// @return 1 on success, 0 on failure
int vgfx_platform_set_fullscreen(struct vgfx_window *win, int fullscreen) {
    if (!win || !win->platform_data)
        return 0;

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return 0;

    /* Get the EWMH atoms for fullscreen state */
    Atom wm_state = XInternAtom(x11->display, "_NET_WM_STATE", False);
    Atom wm_fullscreen = XInternAtom(x11->display, "_NET_WM_STATE_FULLSCREEN", False);

    if (wm_state == None || wm_fullscreen == None)
        return 0;

    /* Send a client message to the window manager to change fullscreen state */
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.type = ClientMessage;
    event.xclient.window = x11->window;
    event.xclient.message_type = wm_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = fullscreen ? 1 : 0; /* _NET_WM_STATE_ADD or _NET_WM_STATE_REMOVE */
    event.xclient.data.l[1] = (long)wm_fullscreen;
    event.xclient.data.l[2] = 0; /* No second property */
    event.xclient.data.l[3] = 1; /* Source indication: normal application */

    XSendEvent(x11->display,
               DefaultRootWindow(x11->display),
               False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               &event);

    XFlush(x11->display);
    return 1;
}

/// @brief Check if the window is in fullscreen mode.
/// @details Queries the _NET_WM_STATE property to check for fullscreen.
///
/// @param win Pointer to the window structure
/// @return 1 if fullscreen, 0 if windowed
int vgfx_platform_is_fullscreen(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;

    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return 0;

    Atom wm_state = XInternAtom(x11->display, "_NET_WM_STATE", False);
    Atom wm_fullscreen = XInternAtom(x11->display, "_NET_WM_STATE_FULLSCREEN", False);

    if (wm_state == None || wm_fullscreen == None)
        return 0;

    /* Query the _NET_WM_STATE property */
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(x11->display,
                                    x11->window,
                                    wm_state,
                                    0,
                                    1024,
                                    False,
                                    XA_ATOM,
                                    &actual_type,
                                    &actual_format,
                                    &nitems,
                                    &bytes_after,
                                    &data);

    if (status != Success || !x11_is_atom_list_property(actual_type, actual_format, data)) {
        if (data)
            XFree(data);
        return 0;
    }

    /* Check if _NET_WM_STATE_FULLSCREEN is in the list */
    int is_fullscreen = 0;
    Atom *atoms = (Atom *)data;
    for (unsigned long i = 0; i < nitems; i++) {
        if (atoms[i] == wm_fullscreen) {
            is_fullscreen = 1;
            break;
        }
    }

    XFree(data);
    return is_fullscreen;
}

/// @brief Send a _NET_WM_STATE client message to the window manager.
/// @details Publishes an EWMH state add, remove, or toggle request to the root
///          window and flushes it immediately.
/// @param x11 Platform state containing the target window and display.
/// @param action EWMH action: zero removes, one adds, and two toggles.
/// @param atom1 First state atom to modify.
/// @param atom2 Optional second state atom, or None.
static void x11_send_wm_state(vgfx_x11_data *x11, int action, Atom atom1, Atom atom2) {
    // action: 0 = remove, 1 = add, 2 = toggle
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.type = ClientMessage;
    event.xclient.window = x11->window;
    event.xclient.message_type = XInternAtom(x11->display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = action;
    event.xclient.data.l[1] = (long)atom1;
    event.xclient.data.l[2] = (long)atom2;
    event.xclient.data.l[3] = 1; // source indication: normal application
    XSendEvent(x11->display,
               DefaultRootWindow(x11->display),
               False,
               SubstructureNotifyMask | SubstructureRedirectMask,
               &event);
    XFlush(x11->display);
}

/// @brief Ask the X11 window manager to iconify a window.
/// @param win Window to minimize; invalid native state is ignored.
void vgfx_platform_minimize(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (x11->display && x11->window) {
        XIconifyWindow(x11->display, x11->window, x11->screen);
        XFlush(x11->display);
    }
}

/// @brief Ask an EWMH window manager to maximize both window dimensions.
/// @param win Window to maximize; invalid native state is ignored.
void vgfx_platform_maximize(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;
    Atom hz = XInternAtom(x11->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom vt = XInternAtom(x11->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    x11_send_wm_state(x11, 1, hz, vt);
}

/// @brief Remove maximized state and deiconify an X11 window.
/// @details Sends the EWMH horizontal/vertical maximize removal request, maps
///          the window in case it was minimized, and flushes both operations.
/// @param win Window to restore; invalid native state is ignored.
void vgfx_platform_restore(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;
    // Remove maximized state
    Atom hz = XInternAtom(x11->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom vt = XInternAtom(x11->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    x11_send_wm_state(x11, 0, hz, vt);
    // Deiconify if minimized
    XMapWindow(x11->display, x11->window);
    XFlush(x11->display);
}

/// @brief Query the EWMH hidden state of an X11 window.
/// @param win Window whose state property should be inspected.
/// @return 1 when `_NET_WM_STATE_HIDDEN` is present, otherwise 0.
int32_t vgfx_platform_is_minimized(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return 0;
    Atom wm_state_atom = XInternAtom(x11->display, "_NET_WM_STATE", False);
    Atom hidden = XInternAtom(x11->display, "_NET_WM_STATE_HIDDEN", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(x11->display,
                                    x11->window,
                                    wm_state_atom,
                                    0,
                                    1024,
                                    False,
                                    XA_ATOM,
                                    &actual_type,
                                    &actual_format,
                                    &nitems,
                                    &bytes_after,
                                    &data);
    if (status != Success || !x11_is_atom_list_property(actual_type, actual_format, data)) {
        if (data)
            XFree(data);
        return 0;
    }
    int found = 0;
    Atom *atoms = (Atom *)data;
    for (unsigned long i = 0; i < nitems; i++) {
        if (atoms[i] == hidden) {
            found = 1;
            break;
        }
    }
    XFree(data);
    return found;
}

/// @brief Query whether both EWMH maximized state atoms are active.
/// @param win Window whose state property should be inspected.
/// @return 1 only when horizontal and vertical maximization are both present,
///         otherwise 0.
int32_t vgfx_platform_is_maximized(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return 0;
    Atom wm_state_atom = XInternAtom(x11->display, "_NET_WM_STATE", False);
    Atom hz = XInternAtom(x11->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom vt = XInternAtom(x11->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(x11->display,
                                    x11->window,
                                    wm_state_atom,
                                    0,
                                    1024,
                                    False,
                                    XA_ATOM,
                                    &actual_type,
                                    &actual_format,
                                    &nitems,
                                    &bytes_after,
                                    &data);
    if (status != Success || !x11_is_atom_list_property(actual_type, actual_format, data)) {
        if (data)
            XFree(data);
        return 0;
    }
    int found_hz = 0;
    int found_vt = 0;
    Atom *atoms = (Atom *)data;
    for (unsigned long i = 0; i < nitems; i++) {
        if (atoms[i] == hz)
            found_hz = 1;
        else if (atoms[i] == vt)
            found_vt = 1;
    }
    XFree(data);
    return found_hz && found_vt;
}

/// @brief Get an X11 window's root-relative position.
/// @details Translates the client origin to its root window.  Invalid top-level
///          input initializes requested outputs to zero; unavailable native
///          handles leave already supplied outputs unchanged.
/// @param win Window whose position should be queried.
/// @param out_x Receives root-relative X when non-NULL.
/// @param out_y Receives root-relative Y when non-NULL.
void vgfx_platform_get_position(struct vgfx_window *win, int32_t *out_x, int32_t *out_y) {
    if (!win || !win->platform_data) {
        if (out_x)
            *out_x = 0;
        if (out_y)
            *out_y = 0;
        return;
    }
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;
    Window child;
    int x = 0, y = 0;
    XWindowAttributes attrs;
    XGetWindowAttributes(x11->display, x11->window, &attrs);
    XTranslateCoordinates(x11->display, x11->window, attrs.root, 0, 0, &x, &y, &child);
    if (out_x)
        *out_x = (int32_t)x;
    if (out_y)
        *out_y = (int32_t)y;
}

/// @brief Move an X11 window to a root-relative position.
/// @param win Window to move.
/// @param x Requested X coordinate.
/// @param y Requested Y coordinate.
void vgfx_platform_set_position(struct vgfx_window *win, int32_t x, int32_t y) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (x11->display && x11->window) {
        XMoveWindow(x11->display, x11->window, (int)x, (int)y);
        XFlush(x11->display);
    }
}

/// @brief Assign X11 keyboard focus directly to a visible window.
/// @details Intentionally does nothing for backend-hidden windows.
/// @param win Window that should receive focus.
void vgfx_platform_focus(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (x11->hidden)
        return;
    if (x11->display && x11->window) {
        XSetInputFocus(x11->display, x11->window, RevertToParent, CurrentTime);
        XFlush(x11->display);
    }
}

/// @brief Raise and focus a visible X11 window.
/// @details Provides the backend's foreground-activation request using
///          XRaiseWindow followed by XSetInputFocus.
/// @param win Window to activate.
void vgfx_platform_request_foreground(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (x11->hidden)
        return;
    if (x11->display && x11->window) {
        XRaiseWindow(x11->display, x11->window);
        XSetInputFocus(x11->display, x11->window, RevertToParent, CurrentTime);
        XFlush(x11->display);
    }
}

/// @brief Read the backend-maintained focus flag under the event lock.
/// @param win Window whose focus state should be queried.
/// @return 1 when focused, otherwise 0.
int32_t vgfx_platform_is_focused(struct vgfx_window *win) {
    if (!win)
        return 0;
    vgfx_internal_event_lock(win);
    int32_t focused = win->is_focused;
    vgfx_internal_event_unlock(win);
    return focused;
}

/// @brief Update whether native close requests should be intercepted.
/// @param win Window whose close policy should change.
/// @param prevent Non-zero to preserve the window after close requests.
void vgfx_platform_set_prevent_close(struct vgfx_window *win, int32_t prevent) {
    vgfx_internal_set_prevent_close(win, prevent);
}

/// @brief Normalize a public ZannaGFX cursor type to a cache index.
/// @param cursor_type Public cursor type value.
/// @return Cache index in the range [0, 5].
static int x11_cursor_index_for_type(int32_t cursor_type) {
    return (cursor_type >= 0 && cursor_type < 13) ? (int)cursor_type : 0;
}

/// @brief Map a normalized public cursor type to an X cursor-font glyph.
/// @param cursor_type Public `VGFX_CURSOR_*` value.
/// @return Matching `XC_*` cursor-font shape, defaulting to the left pointer.
static unsigned int x11_cursor_shape_for_type(int32_t cursor_type) {
    switch (x11_cursor_index_for_type(cursor_type)) {
        case 1:
            return XC_hand2;
        case 2:
            return XC_xterm;
        case 3:
            return XC_sb_h_double_arrow;
        case 4:
            return XC_sb_v_double_arrow;
        case 5:
            return XC_watch;
        case 6:
            return XC_bottom_right_corner;
        case 7:
            return XC_bottom_left_corner;
        case 8:
            return XC_hand1;
        case 9:
            return XC_fleur;
        case 10:
            return XC_crosshair;
        case 11:
            return XC_question_arrow;
        case 12:
            return XC_pirate;
        default:
            return XC_left_ptr;
    }
}

/// @brief Return a cached X11 cursor for a public cursor type.
/// @details XCreateFontCursor allocates server resources.  Cursor changes can
///          happen repeatedly during hover and drag tracking, so each window
///          caches the small fixed set of cursor handles and frees them during
///          backend cleanup.
/// @param x11 X11 platform data for one ZannaGFX window.
/// @param cursor_type Public cursor type value.
/// @return X11 cursor handle, or None on allocation failure.
static Cursor x11_cached_cursor_for_type(vgfx_x11_data *x11, int32_t cursor_type) {
    if (!x11 || !x11->display)
        return None;
    int index = x11_cursor_index_for_type(cursor_type);
    if (!x11->cursor_cache[index]) {
        x11->cursor_cache[index] =
            XCreateFontCursor(x11->display, x11_cursor_shape_for_type(cursor_type));
    }
    return x11->cursor_cache[index];
}

/// @brief Install the window's selected visible cursor handle.
/// @param x11 Platform state whose native cursor should be updated.
static void x11_apply_visible_cursor(vgfx_x11_data *x11) {
    if (!x11 || !x11->display || !x11->window)
        return;
    Cursor cursor = x11_cached_cursor_for_type(x11, x11->cursor_type);
    if (!cursor)
        return;
    XDefineCursor(x11->display, x11->window, cursor);
}

/// @brief Change the cursor shape used by one X11 window.
/// @details Normalizes and caches the public cursor type.  When the cursor is
///          currently hidden the choice is remembered and applied when shown.
/// @param win Window whose cursor shape should change.
/// @param cursor_type Public `VGFX_CURSOR_*` value.
void vgfx_platform_set_cursor(struct vgfx_window *win, int32_t cursor_type) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;

    x11->cursor_type = x11_cursor_index_for_type(cursor_type);
    if (!x11->cursor_visible)
        return;

    x11_apply_visible_cursor(x11);
    XFlush(x11->display);
}

/// @brief Lazily create the transparent cursor used for hidden-cursor mode.
/// @param x11 Platform state that owns the cached server cursor.
/// @return Cached invisible cursor, or zero on invalid state/allocation failure.
static Cursor x11_blank_cursor(vgfx_x11_data *x11) {
    if (!x11 || !x11->display || !x11->window)
        return 0;
    if (x11->blank_cursor)
        return x11->blank_cursor;

    Pixmap blank = XCreatePixmap(x11->display, x11->window, 1, 1, 1);
    if (!blank)
        return 0;
    XColor dummy;
    memset(&dummy, 0, sizeof(dummy));
    x11->blank_cursor = XCreatePixmapCursor(x11->display, blank, blank, &dummy, &dummy, 0, 0);
    XFreePixmap(x11->display, blank);
    return x11->blank_cursor;
}

/// @brief Show or hide a window's X11 cursor.
/// @details Applies the selected visible cursor or a cached transparent pixmap
///          cursor and flushes the display.
/// @param win Window whose cursor visibility should change.
/// @param visible Non-zero to show, zero to hide.
void vgfx_platform_set_cursor_visible(struct vgfx_window *win, int32_t visible) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;

    x11->cursor_visible = visible ? 1 : 0;
    if (visible) {
        x11_apply_visible_cursor(x11);
    } else {
        Cursor invisible = x11_blank_cursor(x11);
        if (!invisible)
            return;
        XDefineCursor(x11->display, x11->window, invisible);
    }
    XFlush(x11->display);
}

/// @brief Hide the cursor for the process-global active cursor window.
/// @details Serializes access to the global window selection and is a no-op
///          when no live cursor-service window exists.
void vgfx_platform_hide_cursor(void) {
    x11_global_lock();
    struct vgfx_window *win = g_vgfx_cursor_window;
    if (win)
        vgfx_platform_set_cursor_visible(win, 0);
    x11_global_unlock();
}

/// @brief Show the cursor for the process-global active cursor window.
/// @copydetails vgfx_platform_hide_cursor
void vgfx_platform_show_cursor(void) {
    x11_global_lock();
    struct vgfx_window *win = g_vgfx_cursor_window;
    if (win)
        vgfx_platform_set_cursor_visible(win, 1);
    x11_global_unlock();
}

/// @brief Query physical dimensions of the window's X11 screen.
/// @details Reuses the window display when available; otherwise opens a
///          temporary default-display connection.  Outputs are initialized to
///          zero when no display can be reached.
/// @param win Optional window selecting a display and screen.
/// @param out_w Receives physical screen width when non-NULL.
/// @param out_h Receives physical screen height when non-NULL.
void vgfx_platform_get_monitor_size(struct vgfx_window *win, int32_t *out_w, int32_t *out_h) {
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;

    Display *display = NULL;
    int screen = 0;
    int close_display = 0;

    if (win && win->platform_data) {
        vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
        display = x11->display;
        screen = x11->screen;
    } else {
        x11_init_threads_once();
        display = XOpenDisplay(NULL);
        close_display = 1;
        if (display)
            screen = DefaultScreen(display);
    }

    if (!display)
        return;
    if (out_w)
        *out_w = (int32_t)DisplayWidth(display, screen);
    if (out_h)
        *out_h = (int32_t)DisplayHeight(display, screen);
    if (close_display)
        XCloseDisplay(display);
}

/// @brief Resize an X11 window from logical client dimensions.
/// @details Converts through the backing scale, synchronously replaces the core
///          and XImage backing stores, emits a resize event, then sends the
///          native resize request.  Its request serial suppresses stale queued
///          ConfigureNotify events.
/// @param win Window to resize.
/// @param w Requested logical width.
/// @param h Requested logical height.
void vgfx_platform_set_window_size(struct vgfx_window *win, int32_t w, int32_t h) {
    if (!win || !win->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;
    int32_t physical_w = x11_logical_to_physical(win, w);
    int32_t physical_h = x11_logical_to_physical(win, h);
    if (physical_w <= 0 || physical_h <= 0)
        return;
    if (!x11_resize_backing_store(win, physical_w, physical_h, vgfx_platform_now_ms(), 1))
        return;
    x11->resize_request_serial = NextRequest(x11->display);
    XResizeWindow(x11->display, x11->window, (unsigned int)physical_w, (unsigned int)physical_h);
    XFlush(x11->display);
}

/// @brief Publish scaled minimum-size hints to the X11 window manager.
/// @details Preserves existing normal hints, converts logical dimensions to
///          physical pixels, and pins non-resizable windows to their current
///          framebuffer size.
/// @param win Window whose normal hints should be updated.
/// @param w Minimum logical client width.
/// @param h Minimum logical client height.
void vgfx_platform_set_window_min_size(struct vgfx_window *win, int32_t w, int32_t h) {
    if (!win || !win->platform_data || w <= 0 || h <= 0)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return;
    int32_t physical_w = x11_logical_to_physical(win, w);
    int32_t physical_h = x11_logical_to_physical(win, h);
    if (physical_w <= 0 || physical_h <= 0)
        return;
    XSizeHints *hints = XAllocSizeHints();
    if (!hints)
        return;
    long supplied = 0;
    if (!XGetWMNormalHints(x11->display, x11->window, hints, &supplied))
        hints->flags = 0;
    hints->flags |= PMinSize;
    hints->min_width = win->resizable ? physical_w : win->width;
    hints->min_height = win->resizable ? physical_h : win->height;
    XSetWMNormalHints(x11->display, x11->window, hints);
    XFree(hints);
    XFlush(x11->display);
}

// ============================================================================
// Clipboard (ICCCM CLIPBOARD selection)
// ============================================================================

/// @brief Check whether the X11 clipboard currently contains non-empty text.
/// @details Only `VGFX_CLIPBOARD_TEXT` is supported.  The query obtains and
///          frees a normal clipboard copy, so it may wait for an external owner.
/// @param format Clipboard format to test.
/// @return 1 for available non-empty text, otherwise 0.
int vgfx_clipboard_has_format(vgfx_clipboard_format_t format) {
    char *text = NULL;
    int has_text = 0;

    if (format != VGFX_CLIPBOARD_TEXT)
        return 0;

    text = vgfx_clipboard_get_text();
    has_text = text && text[0] != '\0';
    free(text);
    return has_text ? 1 : 0;
}

/// @brief Retrieve text from the process or external X11 clipboard owner.
/// @details Uses a live registered window as the selection requestor.  Locally
///          owned text is copied directly; external data is requested first as
///          UTF8_STRING and then XA_STRING, with bounded waits and support for
///          ICCCM INCR transfers.  Global clipboard/window state remains locked
///          throughout the synchronous exchange.
/// @return Heap-allocated NUL-terminated text for the caller to free, or NULL
///         when unavailable, timed out, invalid, or allocation failed.
char *vgfx_clipboard_get_text(void) {
    vgfx_x11_data *x11 = NULL;

    x11_global_lock();
    struct vgfx_window *owner = x11_clipboard_window_locked();
    if (!owner || !owner->platform_data) {
        x11_global_unlock();
        return NULL;
    }

    x11 = (vgfx_x11_data *)owner->platform_data;
    if (!x11 || !x11->display || !x11->window) {
        x11_global_unlock();
        return NULL;
    }

    if (x11->clipboard_text &&
        XGetSelectionOwner(x11->display, x11->clipboard_atom) == x11->window) {
        char *copy = x11_strdup_text(x11->clipboard_text);
        x11_global_unlock();
        return copy;
    }

    Atom targets[2] = {x11->utf8_string_atom, XA_STRING};
    for (size_t target_index = 0; target_index < 2; target_index++) {
        Atom target = targets[target_index];
        XDeleteProperty(x11->display, x11->window, x11->clipboard_property_atom);
        XConvertSelection(x11->display,
                          x11->clipboard_atom,
                          target,
                          x11->clipboard_property_atom,
                          x11->window,
                          CurrentTime);
        XFlush(x11->display);

        int64_t start = vgfx_platform_now_ms();
        int target_done = 0;
        while (!target_done && vgfx_platform_now_ms() - start < VGFX_X11_CLIPBOARD_WAIT_MS) {
            XEvent event;
            if (XCheckIfEvent(x11->display,
                              &event,
                              x11_clipboard_selection_notify_predicate,
                              (XPointer)x11)) {
                if (event.xselection.property == None) {
                    target_done = 1;
                    continue;
                }

                char *copy = x11_read_text_property(x11, x11->clipboard_property_atom, target);
                if (copy) {
                    x11_global_unlock();
                    return copy;
                }
                target_done = 1;
            } else {
                usleep(1000);
            }
        }
    }

    x11_global_unlock();
    return NULL;
}

/// @brief Claim the X11 CLIPBOARD selection with copied text.
/// @details Selects a usable registered window, copies nullable input as an
///          empty-or-NUL-terminated string, publishes selection ownership, and
///          retains the bytes for future SelectionRequest events.  If ownership
///          cannot be confirmed, the stored copy is discarded.
/// @param text UTF-8 text to publish, or NULL for an empty clipboard value.
void vgfx_clipboard_set_text(const char *text) {
    vgfx_x11_data *x11 = NULL;

    x11_global_lock();
    struct vgfx_window *owner = x11_clipboard_window_locked();
    if (!owner || !owner->platform_data) {
        x11_global_unlock();
        return;
    }

    x11 = (vgfx_x11_data *)owner->platform_data;
    if (!x11 || !x11->display || !x11->window) {
        x11_global_unlock();
        return;
    }

    char *copy = x11_strdup_text(text);
    if (!copy) {
        x11_global_unlock();
        return;
    }

    free(x11->clipboard_text);
    x11->clipboard_text = copy;
    XSetSelectionOwner(x11->display, x11->clipboard_atom, x11->window, CurrentTime);
    if (XGetSelectionOwner(x11->display, x11->clipboard_atom) != x11->window) {
        free(x11->clipboard_text);
        x11->clipboard_text = NULL;
    }
    XFlush(x11->display);
    x11_global_unlock();
}

/// @brief Replace the X11 text clipboard with an empty string.
void vgfx_clipboard_clear(void) {
    vgfx_clipboard_set_text("");
}

/// @brief Expose the native X11 Window identifier through the generic view API.
/// @param window Window whose native identifier should be queried.
/// @return X11 Window converted through `uintptr_t`, or NULL for invalid state.
void *vgfx_get_native_view(vgfx_window_t window) {
    if (!window)
        return NULL;
    vgfx_x11_data *x11 = (vgfx_x11_data *)window->platform_data;
    if (!x11)
        return NULL;
    return (void *)(uintptr_t)x11->window; /* X11 Window is unsigned long */
}

/// @brief Expose the native X11 Display connection.
/// @param window Window whose display should be queried.
/// @return Borrowed Display pointer, or NULL for invalid state.
void *vgfx_get_native_display(vgfx_window_t window) {
    if (!window)
        return NULL;
    vgfx_x11_data *x11 = (vgfx_x11_data *)window->platform_data;
    if (!x11)
        return NULL;
    return (void *)x11->display;
}

/// @brief Fill the cross-platform native-handle descriptor for an X11 window.
/// @param window Window whose handles should be exported.
/// @param out_handles Receives backend kind, Display pointer, and Window value.
/// @return 1 when handles were written, otherwise 0.
int vgfx_get_native_handles(vgfx_window_t window, vgfx_native_handles_t *out_handles) {
    if (!window || !window->platform_data || !out_handles)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)window->platform_data;
    *out_handles = (vgfx_native_handles_t){.backend = VGFX_NATIVE_BACKEND_X11,
                                           .display = (void *)x11->display,
                                           .surface = NULL,
                                           .window = (uintptr_t)x11->window};
    return 1;
}

/// @brief Report features implemented by the X11 window backend.
/// @param window Window whose initialized backend should be inspected.
/// @return Bitwise `VGFX_CAP_*` mask, or zero for invalid platform state.
vgfx_window_capabilities_t vgfx_get_window_capabilities(vgfx_window_t window) {
    if (!window || !window->platform_data)
        return 0;
    return VGFX_CAP_WINDOW_POSITION | VGFX_CAP_FOCUS_REQUEST | VGFX_CAP_CURSOR_WARP |
           VGFX_CAP_RELATIVE_MOUSE | VGFX_CAP_TEXT_COMPOSITION | VGFX_CAP_SERVER_DECORATIONS |
           VGFX_CAP_ACTIVATION | VGFX_CAP_CLIPBOARD_TEXT | VGFX_CAP_FILE_DROP;
}

/// @brief Warp the X11 pointer to a logical client coordinate.
/// @details Converts coordinates through the active public coordinate scale,
///          targets the client window, and flushes the request.
/// @param window Window receiving the pointer.
/// @param x Logical client X coordinate.
/// @param y Logical client Y coordinate.
void vgfx_platform_warp_cursor(vgfx_window_t window, int32_t x, int32_t y) {
    if (!window || !window->platform_data)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)window->platform_data;
    if (!x11->display || !x11->window)
        return;
    float cs = vgfx_internal_coord_scale(window);
    XWarpPointer(x11->display,
                 None,
                 x11->window,
                 0,
                 0,
                 0,
                 0,
                 vgfx_internal_scale_up_i32(x, cs),
                 vgfx_internal_scale_up_i32(y, cs));
    XFlush(x11->display);
}

//===----------------------------------------------------------------------===//
// Relative (raw) mouse mode — XInput2 raw motion
//===----------------------------------------------------------------------===//
// libXi is loaded at runtime via dlopen (same policy as the GLX loader above)
// so no build-time XInput2 headers or link dependency is introduced. The tiny
// ABI subset we need has been stable since XInput 2.0 (2009) and is declared
// locally below.

#define VGFX_XI_RAW_MOTION 17
#define VGFX_XI_ALL_MASTER_DEVICES 1

/// @brief Header-compatible subset of XInput2's `XIEventMask`.
typedef struct {
    int deviceid;
    int mask_len;
    unsigned char *mask;
} vgfx_xi_event_mask_t;

/* Layout of XIRawEvent (XInput 2.0 stable ABI). Only the fields up to
 * raw_values are accessed. */
/// @brief Header-compatible prefix of XInput2's stable `XIRawEvent` ABI.
/// @details Only fields through `raw_values` are declared and accessed, keeping
///          XInput2 optional at both compile time and runtime.
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    Display *display;
    int extension;
    int evtype;
    unsigned long time;
    int deviceid;
    int sourceid;
    int detail;
    int flags;

    struct {
        int mask_len;
        unsigned char *mask;
        double *values;
    } valuators;

    double *raw_values;
} vgfx_xi_raw_event_t;

/// @brief Runtime-resolved signature of `XIQueryVersion`.
typedef int (*vgfx_xi_query_version_fn)(Display *, int *, int *);

/// @brief Runtime-resolved signature of `XISelectEvents`.
typedef int (*vgfx_xi_select_events_fn)(Display *, Window, vgfx_xi_event_mask_t *, int);

static void *g_vgfx_xi_handle = NULL;
static vgfx_xi_query_version_fn g_vgfx_xi_query_version = NULL;
static vgfx_xi_select_events_fn g_vgfx_xi_select_events = NULL;
static pthread_once_t g_vgfx_xi_once = PTHREAD_ONCE_INIT;

/// @brief Load libXi and resolve the optional XInput2 entry points.
/// @details Runs once through `pthread_once`, preferring the versioned soname
///          and leaving entry points NULL when the library or symbols are absent.
static void x11_xi2_load_library(void) {
    g_vgfx_xi_handle = dlopen("libXi.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!g_vgfx_xi_handle)
        g_vgfx_xi_handle = dlopen("libXi.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_vgfx_xi_handle)
        return;
    g_vgfx_xi_query_version = (vgfx_xi_query_version_fn)dlsym(g_vgfx_xi_handle, "XIQueryVersion");
    g_vgfx_xi_select_events = (vgfx_xi_select_events_fn)dlsym(g_vgfx_xi_handle, "XISelectEvents");
}

/// @brief Lazily load libXi and resolve the XInput2 entry points we use.
/// @param x11 Platform state containing the display to query and receiving the
///            XInput extension opcode.
/// @return 1 when XInput2 >= 2.0 is available on `x11->display`, 0 otherwise.
static int x11_xi2_load(vgfx_x11_data *x11) {
    if (!x11 || !x11->display)
        return 0;
    if (pthread_once(&g_vgfx_xi_once, x11_xi2_load_library) != 0)
        return 0;
    if (!g_vgfx_xi_query_version || !g_vgfx_xi_select_events)
        return 0;

    int opcode = 0;
    int event_base = 0;
    int error_base = 0;
    if (!XQueryExtension(x11->display, "XInputExtension", &opcode, &event_base, &error_base))
        return 0;

    int major = 2;
    int minor = 0;
    if (g_vgfx_xi_query_version(x11->display, &major, &minor) != Success)
        return 0;

    x11->xi_opcode = opcode;
    return 1;
}

/// @brief Decode a GenericEvent cookie and accumulate raw mouse motion.
/// @details Only consumes XI raw-motion events while relative mode is active;
///          everything else is ignored. Raw valuator 0/1 presence is checked
///          via the event's valuator mask (devices may report axes sparsely).
/// @param win Window receiving logical relative-motion deltas.
/// @param event Borrowed X11 GenericEvent whose cookie may contain XI2 data.
static void x11_handle_generic_event(struct vgfx_window *win, XEvent *event) {
    if (!win || !event || !win->relative_mouse_enabled || !win->relative_mouse_native)
        return;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11 || !x11->display)
        return;
    if (x11->xi_opcode < 0)
        return;

    XGenericEventCookie *cookie = &event->xcookie;
    if (cookie->extension != x11->xi_opcode)
        return;
    if (!XGetEventData(x11->display, cookie))
        return;

    if (cookie->evtype == VGFX_XI_RAW_MOTION && cookie->data) {
        const vgfx_xi_raw_event_t *raw = (const vgfx_xi_raw_event_t *)cookie->data;
        double dx = 0.0;
        double dy = 0.0;
        int value_index = 0;
        const unsigned char *mask = raw->valuators.mask;
        const int mask_len = raw->valuators.mask_len;
        if (mask && raw->raw_values) {
            if (mask_len > 0 && (mask[0] & 0x01u))
                dx = raw->raw_values[value_index++];
            if (mask_len > 0 && (mask[0] & 0x02u))
                dy = raw->raw_values[value_index++];
        }
        if (dx != 0.0 || dy != 0.0) {
            double cs = (double)vgfx_internal_coord_scale(win);
            if (cs <= 0.0)
                cs = 1.0;
            vgfx_internal_add_relative_delta(win, dx / cs, dy / cs);
        }
    }

    XFreeEventData(x11->display, cookie);
}

/// @brief Select or deselect XI raw-motion events on the root window.
/// @param x11 Platform state with resolved XInput2 entry points.
/// @param enable Non-zero to select XI_RawMotion, zero to clear the mask.
/// @return 1 when XISelectEvents succeeded, otherwise 0.
static int x11_xi2_select_raw_motion(vgfx_x11_data *x11, int enable) {
    unsigned char mask_bits[3] = {0, 0, 0};
    if (enable)
        mask_bits[VGFX_XI_RAW_MOTION >> 3] |= (unsigned char)(1u << (VGFX_XI_RAW_MOTION & 7));

    vgfx_xi_event_mask_t mask;
    mask.deviceid = VGFX_XI_ALL_MASTER_DEVICES;
    mask.mask_len = (int)sizeof(mask_bits);
    mask.mask = mask_bits;

    Window root = RootWindow(x11->display, x11->screen);
    return g_vgfx_xi_select_events(x11->display, root, &mask, 1) == Success;
}

/// @brief Confine the pointer to the window while relative mode is active.
/// @details Raw motion keeps flowing regardless of the cursor position; the
///          grab only prevents the (hidden) cursor from drifting onto other
///          windows/workspaces and stealing a click.
/// @param win Window whose pointer should be confined or released.
/// @param enable Non-zero to grab/confine, zero to ungrab.
/// @return 1 when the requested state was applied, otherwise 0.
static int x11_relative_apply_grab(struct vgfx_window *win, int enable) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return 0;
    if (enable) {
        int result = XGrabPointer(x11->display,
                                  x11->window,
                                  True,
                                  ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                  GrabModeAsync,
                                  GrabModeAsync,
                                  x11->window,
                                  None,
                                  CurrentTime);
        if (result != GrabSuccess)
            return 0;
    } else {
        XUngrabPointer(x11->display, CurrentTime);
    }
    XFlush(x11->display);
    return 1;
}

/// @brief Enable or disable native XInput2 relative mouse delivery.
/// @details Enabling requires XInput2 raw-motion selection and a successful
///          pointer grab; a failed grab rolls selection back.  Disabling clears
///          selection when available and releases the grab.  Core state is
///          updated by the caller only after this function succeeds.
/// @param win Window whose native relative mode should change.
/// @param enabled Non-zero to enable, zero to disable.
/// @return 1 when the native mode transition succeeded, otherwise 0 so the
///         caller can use warp-to-center fallback.
int vgfx_platform_set_relative_mouse(struct vgfx_window *win, int enabled) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (!x11->display || !x11->window)
        return 0;

    if (enabled) {
        if (!x11_xi2_load(x11))
            return 0; /* No XInput2 — caller falls back to warp-to-center. */
        if (!x11_xi2_select_raw_motion(x11, 1))
            return 0;
        if (!x11_relative_apply_grab(win, 1)) {
            (void)x11_xi2_select_raw_motion(x11, 0);
            return 0;
        }
    } else {
        if (g_vgfx_xi_select_events && x11->xi_opcode >= 0)
            (void)x11_xi2_select_raw_motion(x11, 0);
        (void)x11_relative_apply_grab(win, 0);
    }
    XFlush(x11->display);
    return 1;
}

/// @brief Focus or unfocus the window's XIM input context.
/// @details A missing input context is treated as a supported no-op because
///          ordinary key lookup remains available.
/// @param win Window whose input method should change.
/// @param enabled Non-zero to focus the XIC, zero to unfocus it.
/// @return 1 for valid platform state, otherwise 0.
int vgfx_platform_set_text_input_enabled(struct vgfx_window *win, int32_t enabled) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_x11_data *x11 = (vgfx_x11_data *)win->platform_data;
    if (x11->xic) {
        if (enabled)
            XSetICFocus(x11->xic);
        else
            XUnsetICFocus(x11->xic);
    }
    return 1;
}

/// @brief Accept surrounding-text state for the X11 text-input bridge.
/// @details The current XIM integration does not expose a portable surrounding
///          text API, so validated state is acknowledged without native work.
/// @param win Window whose X11 platform state must be valid.
/// @param state Validated call-scoped text-input snapshot.
/// @return 1 when both platform state and snapshot are present, otherwise 0.
int vgfx_platform_set_text_input_state(struct vgfx_window *win,
                                       const vgfx_text_input_state_t *state) {
    return win && win->platform_data && state;
}

#endif /* __linux__ */
