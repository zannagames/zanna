//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaGFX macOS Cocoa Backend
//
// Platform-specific implementation using macOS Cocoa APIs (Objective-C).
// Provides window creation, event handling, framebuffer blitting, and timing
// functions for macOS systems.
//
// Architecture:
//   - NSWindow: Native window container
//   - VGFXView (NSView): Custom view for framebuffer display via CGImage
//   - VGFXWindowDelegate: Handles window lifecycle events (close, resize, focus)
//   - Event Translation: NSEvent → vgfx_event_t mapping
//   - Coordinate Conversion: Cocoa (bottom-left origin) → ZannaGFX (top-left origin)
//
// Key macOS Concepts:
//   - Autorelease pools: Required for Objective-C memory management
//   - NSApplication: Singleton application instance (NSApp)
//   - Event loop: nextEventMatchingMask with distantPast for non-blocking
//   - CGImage: Core Graphics image created from raw RGBA framebuffer
//   - mach_absolute_time: High-resolution monotonic timer
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief macOS Cocoa backend implementation for ZannaGFX.
/// @details Uses NSWindow, NSView, and Core Graphics to provide window
///          management and framebuffer presentation on macOS.

#include "../include/vgfx.h"
#include "vgfx_internal.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_vgfx_macos_cursor_hidden = 0;
static mach_timebase_info_data_t g_vgfx_macos_timebase = {0};
static pthread_once_t g_vgfx_macos_timebase_once = PTHREAD_ONCE_INIT;

static int vgfx_macos_env_flag_enabled(const char *name) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return 0;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 && strcmp(value, "FALSE") != 0 &&
           strcmp(value, "off") != 0 && strcmp(value, "OFF") != 0;
}

static int vgfx_macos_hide_windows(void) {
    return vgfx_macos_env_flag_enabled("ZANNA_GFX_HIDE_WINDOWS");
}

static int vgfx_macos_no_activate_on_create(void) {
    return vgfx_macos_env_flag_enabled("ZANNA_GFX_NO_ACTIVATE") || vgfx_macos_hide_windows();
}

/// @brief Allocate an aligned framebuffer buffer using POSIX allocation.
/// @details macOS exposes `posix_memalign`, and this Cocoa adapter is the only
///          layer that should use that platform API directly.  The core receives
///          an aligned block without carrying platform conditionals.
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
/// @details macOS `posix_memalign` allocations are released with `free`.
///          Passing NULL is a no-op.
/// @param ptr Pointer returned by `vgfx_platform_aligned_alloc()`, or NULL.
void vgfx_platform_aligned_free(void *ptr) {
    free(ptr);
}

/// @brief Initialize mach timebase conversion factors exactly once.
/// @details The graphics timer is called from event/update paths that may cross
///          threads in embedding applications.  pthread_once prevents a data
///          race on the cached conversion factors and normalizes unexpected
///          zero fields defensively.
static void vgfx_macos_init_timebase(void) {
    (void)mach_timebase_info(&g_vgfx_macos_timebase);
    if (g_vgfx_macos_timebase.numer == 0)
        g_vgfx_macos_timebase.numer = 1;
    if (g_vgfx_macos_timebase.denom == 0)
        g_vgfx_macos_timebase.denom = 1;
}

//===----------------------------------------------------------------------===//
// Forward Declarations
//===----------------------------------------------------------------------===//

/// @brief Custom NSView subclass for displaying the ZannaGFX framebuffer.
/// @details Overrides drawRect: to convert the raw RGBA framebuffer into a
///          CGImage and blit it to the screen.  Handles coordinate system
///          conversion (Cocoa uses bottom-left origin, we use top-left).
@interface VGFXView : NSView <NSTextInputClient>
/// @brief Pointer to the ZannaGFX window structure (backlink for rendering).
@property(nonatomic, assign) struct vgfx_window *vgfxWindow;
/// @brief Timestamp assigned by the platform pump before native key interpretation.
@property(nonatomic, assign) int64_t textInputTimestamp;
/// @brief ZannaGFX modifier mask assigned before native key interpretation.
@property(nonatomic, assign) int textInputModifiers;
/// @brief Current native marked string retained only while Cocoa owns preedit.
@property(nonatomic, copy) NSString *markedText;
/// @brief Whether a composition-start event has been emitted without a terminal boundary.
@property(nonatomic, assign) BOOL compositionActive;
@end

/// @brief Window delegate for handling lifecycle events.
/// @details Implements NSWindowDelegate protocol to receive notifications for
///          window close, resize, and focus change events.  Translates these
///          into vgfx_event_t and enqueues them.
@interface VGFXWindowDelegate : NSObject <NSWindowDelegate>
/// @brief Pointer to the ZannaGFX window structure (backlink for events).
@property(nonatomic, assign) struct vgfx_window *vgfxWindow;
@end

/// @brief Dispatcher for default macOS application-menu actions.
/// @details Keeps Cmd+Q on the same close path as the window close button so
///          Zanna apps still see a CLOSE event instead of being hard-terminated.
@interface VGFXMacAppMenuDispatcher : NSObject
+ (instancetype)shared;
- (void)quitApplication:(id)sender;
@end

//===----------------------------------------------------------------------===//
// Platform Data Structure
//===----------------------------------------------------------------------===//

/// @brief Platform-specific data for macOS windows.
/// @details Allocated and owned by the platform backend.  Stored in
///          vgfx_window->platform_data.  Contains references to the native
///          NSWindow, custom view, delegate, and close request flag.
///
/// @invariant window != nil implies view != nil && delegate != nil
typedef struct {
    NSWindow *window;               ///< Native NSWindow instance
    VGFXView *view;                 ///< Custom view for framebuffer display
    VGFXWindowDelegate *delegate;   ///< Delegate for window events
    int close_requested;            ///< 1 if user clicked close button, 0 otherwise
    int cursor_type;                ///< Current cursor type for persistent cursor rects
    int fullscreen_resizable_added; ///< 1 if fullscreen temporarily enabled resizable style
    NSSize windowed_content_size;   ///< Last windowed content size before native fullscreen
    int has_windowed_content_size;  ///< 1 if windowed_content_size is valid
} vgfx_macos_platform;

static BOOL g_finish_launching_called = NO;
static NSMenu *g_default_main_menu = nil;

static void macos_enqueue_text_input_events(struct vgfx_window *win,
                                            int64_t timestamp,
                                            NSString *characters,
                                            int modifiers);

/// @brief Convert a UTF-8 C string to an NSString with a safe fallback.
/// @details Cocoa returns nil from `stringWithUTF8String:` when the byte stream
///          is not valid UTF-8.  Window-title and menu APIs expect a non-nil
///          string, so this helper centralizes the fallback to an empty string.
/// @param text UTF-8 text, or NULL.
/// @return Autoreleased NSString containing `text`, or an empty string.
static NSString *vgfx_macos_string_or_empty(const char *text) {
    if (!text)
        return @ "";
    NSString *string = [NSString stringWithUTF8String:text];
    return string ? string : @ "";
}

/// @brief Duplicate a UTF-8 C string using portable C allocation.
/// @details Clipboard reads return caller-owned C strings.  This helper avoids
///          relying on POSIX `strdup` and keeps ownership explicit.
/// @param text Null-terminated string to duplicate.
/// @return Heap-allocated copy, or NULL on allocation failure.
static char *vgfx_macos_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

static NSCursor *macos_cursor_for_type(int32_t cursor_type) {
    switch (cursor_type) {
        case 1:
            return [NSCursor pointingHandCursor];
        case 2:
            return [NSCursor IBeamCursor];
        case 3:
            return [NSCursor resizeLeftRightCursor];
        case 4:
            return [NSCursor resizeUpDownCursor];
        case 5:
            return [NSCursor arrowCursor];
        case 6: /* resize NWSE: private selector unavailable; closest public */
        case 7: /* resize NESW */
            return [NSCursor crosshairCursor];
        case 8:
            return [NSCursor openHandCursor];
        case 9:
            return [NSCursor closedHandCursor];
        case 10:
            return [NSCursor crosshairCursor];
        case 11:
            return [NSCursor arrowCursor];
        case 12:
            return [NSCursor operationNotAllowedCursor];
        default:
            return [NSCursor arrowCursor];
    }
}

static CGDirectDisplayID macos_display_id_for_screen(NSScreen *screen) {
    if (!screen)
        return CGMainDisplayID();
    NSNumber *screen_number = [[screen deviceDescription] objectForKey:@ "NSScreenNumber"];
    return screen_number ? (CGDirectDisplayID)[screen_number unsignedIntValue] : CGMainDisplayID();
}

static int32_t macos_clamp_i32(int32_t value, int32_t min_value, int32_t max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static void macos_event_location_to_physical(struct vgfx_window *win,
                                             NSPoint location,
                                             NSRect content_rect,
                                             int32_t *out_x,
                                             int32_t *out_y) {
    float sf = (win && win->scale_factor > 0.0f) ? win->scale_factor : 1.0f;
    int32_t width_px = vgfx_internal_round_scaled((float)content_rect.size.width * sf);
    int32_t height_px = vgfx_internal_round_scaled((float)content_rect.size.height * sf);
    int32_t x = vgfx_internal_round_scaled((float)location.x * sf);
    int32_t y = height_px - 1 - vgfx_internal_round_scaled((float)location.y * sf);
    if (width_px > 0 && location.x >= 0.0 && location.x <= content_rect.size.width)
        x = macos_clamp_i32(x, 0, width_px - 1);
    if (height_px > 0 && location.y >= 0.0 && location.y <= content_rect.size.height)
        y = macos_clamp_i32(y, 0, height_px - 1);

    if (out_x)
        *out_x = x;
    if (out_y)
        *out_y = y;
}

/// @brief Translate a Cocoa mouse event into a Zanna mouse button.
/// @details Cocoa reports left and right mouse buttons through dedicated event
///          types.  Other mouse events carry a button number; ZannaGFX exposes
///          only left/right/middle, so extra side buttons are ignored instead of
///          being incorrectly reported as middle-button input.
/// @param event Cocoa mouse event to translate.
/// @param out_button Receives the Zanna button on success.
/// @return 1 when the event maps to a supported button; otherwise 0.
static int macos_mouse_button_from_event(NSEvent *event, vgfx_mouse_button_t *out_button) {
    if (!event || !out_button)
        return 0;
    switch ([event type]) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
            *out_button = VGFX_MOUSE_LEFT;
            return 1;
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
            *out_button = VGFX_MOUSE_RIGHT;
            return 1;
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
            if ([event buttonNumber] == 2) {
                *out_button = VGFX_MOUSE_MIDDLE;
                return 1;
            }
            return 0;
        default:
            return 0;
    }
}

/// @brief Get the screen frame used to normalize Cocoa window coordinates.
/// @details Cocoa window frames use a bottom-left origin.  Public ZannaGFX
///          positioning APIs use top-left coordinates, so conversions need the
///          containing screen's frame.  If the window is not yet attached to a
///          screen, the main screen is used as a stable fallback.
/// @param window Cocoa window whose screen should be used.
/// @return Screen frame in global Cocoa coordinates, or a zero rect if no screen exists.
static NSRect macos_screen_frame_for_window(NSWindow *window) {
    NSScreen *screen = window ? [window screen] : nil;
    if (!screen)
        screen = [NSScreen mainScreen];
    return screen ? [screen frame] : NSZeroRect;
}

static void macos_set_view_tracks_window(VGFXView *view) {
    if (!view)
        return;
    [view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
}

static void macos_sync_view_frame_to_content(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;

    vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
    if (!platform->window || !platform->view)
        return;

    macos_set_view_tracks_window(platform->view);

    NSRect content_rect = [platform->window contentRectForFrameRect:[platform->window frame]];
    if (content_rect.size.width <= 0.0 || content_rect.size.height <= 0.0)
        return;

    NSRect view_frame = [platform->view frame];
    if (fabs(view_frame.size.width - content_rect.size.width) <= 0.5 &&
        fabs(view_frame.size.height - content_rect.size.height) <= 0.5)
        return;

    view_frame.origin = NSZeroPoint;
    view_frame.size = content_rect.size;
    [platform->view setFrame:view_frame];
}

static void macos_sync_window_metrics(struct vgfx_window *win,
                                      int emit_resize_event,
                                      int invoke_resize_callback) {
    if (!win || !win->platform_data)
        return;

    vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
    if (!platform->window || !platform->view)
        return;

    macos_sync_view_frame_to_content(win);

    CGFloat backing_scale = [platform->window backingScaleFactor];
    vgfx_internal_refresh_scale_factor(win, (float)backing_scale);

    NSRect content_rect = [platform->view bounds];
    int32_t new_width =
        vgfx_internal_round_scaled((float)content_rect.size.width * win->scale_factor);
    int32_t new_height =
        vgfx_internal_round_scaled((float)content_rect.size.height * win->scale_factor);

    if (new_width <= 0 || new_height <= 0)
        return;

    if (new_width == win->width && new_height == win->height)
        return;

    if (!vgfx_internal_resize_framebuffer(win, new_width, new_height))
        return;

    if (emit_resize_event) {
        vgfx_event_t event = {0};
        vgfx_internal_init_resize_event(&event, win, vgfx_platform_now_ms(), new_width, new_height);
        vgfx_internal_enqueue_event(win, &event);
    }

    [platform->view setNeedsDisplay:YES];

    if (invoke_resize_callback && win->on_resize)
        win->on_resize(win->on_resize_userdata, new_width, new_height);
}

static NSString *vgfx_macos_app_name(const char *preferred_title) {
    if (preferred_title && preferred_title[0] != '\0') {
        NSString *title = vgfx_macos_string_or_empty(preferred_title);
        if (title.length > 0) {
            return title;
        }
    }

    NSString *bundle_name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@ "CFBundleName"];
    if (bundle_name.length > 0) {
        return bundle_name;
    }

    NSString *process_name = [[NSProcessInfo processInfo] processName];
    return process_name.length > 0 ? process_name : @ "Zanna";
}

static NSMenu *vgfx_macos_build_default_app_menu(NSString *app_name) {
    NSMenu *app_menu = [[NSMenu alloc] initWithTitle:app_name];
    [app_menu setAutoenablesItems:NO];

    NSMenuItem *about_item =
        [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@ "About %@", app_name]
                                   action:@selector(orderFrontStandardAboutPanel:)
                            keyEquivalent:@ ""];
    [about_item setTarget:NSApp];
    [app_menu addItem:about_item];

    [app_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *services_root = [[NSMenuItem alloc] initWithTitle:@ "Services"
                                                           action:nil
                                                    keyEquivalent:@ ""];
    NSMenu *services_menu = [[NSMenu alloc] initWithTitle:@ "Services"];
    [services_menu setAutoenablesItems:NO];
    [services_root setSubmenu:services_menu];
    [NSApp setServicesMenu:services_menu];
    [app_menu addItem:services_root];

    [app_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *hide_item =
        [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@ "Hide %@", app_name]
                                   action:@selector(hide:)
                            keyEquivalent:@ "h"];
    [hide_item setTarget:NSApp];
    [hide_item setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [app_menu addItem:hide_item];

    NSMenuItem *hide_others_item =
        [[NSMenuItem alloc] initWithTitle:@ "Hide Others"
                                   action:@selector(hideOtherApplications:)
                            keyEquivalent:@ "h"];
    [hide_others_item setTarget:NSApp];
    [hide_others_item
        setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
    [app_menu addItem:hide_others_item];

    NSMenuItem *show_all_item = [[NSMenuItem alloc] initWithTitle:@ "Show All"
                                                           action:@selector(unhideAllApplications:)
                                                    keyEquivalent:@ ""];
    [show_all_item setTarget:NSApp];
    [app_menu addItem:show_all_item];

    [app_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *quit_item =
        [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@ "Quit %@", app_name]
                                   action:@selector(quitApplication:)
                            keyEquivalent:@ "q"];
    [quit_item setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [quit_item setTarget:[VGFXMacAppMenuDispatcher shared]];
    [app_menu addItem:quit_item];

    return app_menu;
}

static NSMenu *vgfx_macos_build_default_window_menu(void) {
    NSMenu *window_menu = [[NSMenu alloc] initWithTitle:@ "Window"];
    [window_menu setAutoenablesItems:NO];

    NSMenuItem *minimize_item = [[NSMenuItem alloc] initWithTitle:@ "Minimize"
                                                           action:@selector(performMiniaturize:)
                                                    keyEquivalent:@ "m"];
    [minimize_item setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [minimize_item setTarget:nil];
    [minimize_item setEnabled:YES];
    [window_menu addItem:minimize_item];

    NSMenuItem *zoom_item = [[NSMenuItem alloc] initWithTitle:@ "Zoom"
                                                       action:@selector(performZoom:)
                                                keyEquivalent:@ ""];
    [zoom_item setTarget:nil];
    [zoom_item setEnabled:YES];
    [window_menu addItem:zoom_item];

    [window_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *bring_all_to_front_item =
        [[NSMenuItem alloc] initWithTitle:@ "Bring All to Front"
                                   action:@selector(arrangeInFront:)
                            keyEquivalent:@ ""];
    [bring_all_to_front_item setTarget:NSApp];
    [bring_all_to_front_item setEnabled:YES];
    [window_menu addItem:bring_all_to_front_item];

    [NSApp setWindowsMenu:window_menu];
    return window_menu;
}

static NSMenu *vgfx_macos_build_default_main_menu(const char *preferred_title) {
    NSString *app_name = vgfx_macos_app_name(preferred_title);
    NSMenu *main_menu = [[NSMenu alloc] initWithTitle:@ ""];
    [main_menu setAutoenablesItems:NO];

    // Preserve the authored name in the Cocoa menu model and submenu. macOS
    // renders the system application-menu label from process identity for raw
    // executables; product builds provide that identity through their staged
    // executable leaf name.
    NSMenuItem *app_root = [[NSMenuItem alloc] initWithTitle:app_name action:nil keyEquivalent:@ ""];
    [app_root setSubmenu:vgfx_macos_build_default_app_menu(app_name)];
    [main_menu addItem:app_root];

    NSMenuItem *window_root = [[NSMenuItem alloc] initWithTitle:@ "Window"
                                                         action:nil
                                                  keyEquivalent:@ ""];
    [window_root setSubmenu:vgfx_macos_build_default_window_menu()];
    [window_root setEnabled:YES];
    [main_menu addItem:window_root];

    return main_menu;
}

void vgfx_platform_macos_finish_launching_if_needed(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        if ([NSWindow respondsToSelector:@selector(setAllowsAutomaticWindowTabbing:)]) {
            [NSWindow setAllowsAutomaticWindowTabbing:NO];
        }
        if (!g_finish_launching_called) {
            [NSApp finishLaunching];
            g_finish_launching_called = YES;
        }
    }
}

void vgfx_platform_macos_install_default_main_menu(const char *preferred_title) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        vgfx_platform_macos_finish_launching_if_needed();
        g_default_main_menu = vgfx_macos_build_default_main_menu(preferred_title);
        [NSApp setMainMenu:g_default_main_menu];
    }
}

void vgfx_platform_macos_ensure_default_main_menu(const char *preferred_title) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSMenu *current_menu = [NSApp mainMenu];
        if (current_menu && current_menu != g_default_main_menu) {
            return;
        }
        vgfx_platform_macos_install_default_main_menu(preferred_title);
    }
}

@implementation VGFXMacAppMenuDispatcher

+ (instancetype)shared {
    static VGFXMacAppMenuDispatcher *shared = nil;
    if (!shared) {
        shared = [[VGFXMacAppMenuDispatcher alloc] init];
    }
    return shared;
}

- (void)quitApplication:(id)sender {
    (void)sender;

    NSWindow *target_window = [NSApp keyWindow];
    if (!target_window) {
        target_window = [NSApp mainWindow];
    }

    if (target_window) {
        [target_window performClose:nil];
        return;
    }

    [NSApp terminate:nil];
}

@end

//===----------------------------------------------------------------------===//
// Key Code Translation
//===----------------------------------------------------------------------===//

/// @brief Translate macOS virtual key codes to vgfx_key_t.
/// @details Attempts to use the character representation first (handles A-Z,
///          0-9, SPACE), then falls back to keycode-based lookup for special
///          keys (arrows, Enter, Escape).  Unrecognized keys return
///          VGFX_KEY_UNKNOWN.
///
/// @param keycode macOS virtual key code from NSEvent
/// @param chars   Character string from NSEvent (used for alphanumeric keys)
/// @param flags   Modifier flags from NSEvent (used for fn+arrow navigation)
/// @return Corresponding vgfx_key_t, or VGFX_KEY_UNKNOWN if not recognized
///
/// @details Key mapping:
///            - A-Z: Mapped to vgfx_key_t enum values (uppercase)
///            - 0-9: Mapped to vgfx_key_t enum values
///            - Space: VGFX_KEY_SPACE
///            - Arrows/page navigation: VGFX_KEY_LEFT/RIGHT/UP/DOWN/HOME/END/PAGE_UP/PAGE_DOWN
///            - Enter: VGFX_KEY_ENTER (both main and numpad)
///            - Escape: VGFX_KEY_ESCAPE
static vgfx_key_t translate_keycode(unsigned short keycode,
                                    NSString *chars,
                                    NSEventModifierFlags flags) {
    (void)flags;
    /* NOTE: Do NOT branch on NSEventModifierFlagFunction here. Cocoa sets
       this flag on every arrow-key press because the arrow keys are
       themselves classified as function keys in macOS's modifier scheme.
       A previous Fn+arrow→PageUp/PageDown translation that gated on this
       flag intercepted plain arrow keys and broke editor navigation +
       game input. Real Fn+arrow is handled below via the macOS-translated
       NSPageUpFunctionKey / NSHomeFunctionKey unichar values, which the
       OS emits when the user actually holds Fn on a compact keyboard. */

    /* Try to use character first. macOS sends function-key Unicode values for
       Fn+arrow and some compact keyboards, so handle those before ASCII. */
    if (chars && [chars length] > 0) {
        unichar raw = [chars characterAtIndex:0];
        switch (raw) {
            case NSHomeFunctionKey:
                return VGFX_KEY_HOME;
            case NSEndFunctionKey:
                return VGFX_KEY_END;
            case NSPageUpFunctionKey:
                return VGFX_KEY_PAGE_UP;
            case NSPageDownFunctionKey:
                return VGFX_KEY_PAGE_DOWN;
            case NSLeftArrowFunctionKey:
                return VGFX_KEY_LEFT;
            case NSRightArrowFunctionKey:
                return VGFX_KEY_RIGHT;
            case NSUpArrowFunctionKey:
                return VGFX_KEY_UP;
            case NSDownArrowFunctionKey:
                return VGFX_KEY_DOWN;
            case NSF1FunctionKey:
                return VGFX_KEY_F1;
            case NSF2FunctionKey:
                return VGFX_KEY_F2;
            case NSF3FunctionKey:
                return VGFX_KEY_F3;
            case NSF4FunctionKey:
                return VGFX_KEY_F4;
            case NSF5FunctionKey:
                return VGFX_KEY_F5;
            case NSF6FunctionKey:
                return VGFX_KEY_F6;
            case NSF7FunctionKey:
                return VGFX_KEY_F7;
            case NSF8FunctionKey:
                return VGFX_KEY_F8;
            case NSF9FunctionKey:
                return VGFX_KEY_F9;
            case NSF10FunctionKey:
                return VGFX_KEY_F10;
            case NSF11FunctionKey:
                return VGFX_KEY_F11;
            case NSF12FunctionKey:
                return VGFX_KEY_F12;
            default:
                break;
        }

        unichar c = [[chars uppercaseString] characterAtIndex:0];
        // Any printable ASCII key maps to its ASCII value — vgfx_key_t is ASCII-based
        // (VGFX_KEY_A == 'A', VGFX_KEY_SPACE == ' '). This covers letters, digits, and
        // crucially punctuation such as '=' and '-' that back keyboard shortcuts (zoom
        // in/out). `c` derives from charactersIgnoringModifiers, so Shift+'=' is '='.
        if (c >= 0x20 && c <= 0x7E)
            return (vgfx_key_t)c;
    }

    /* Special keys by keycode (macOS-specific virtual key codes) */
    switch (keycode) {
        case 0x24:
            return VGFX_KEY_ENTER; /* Return key */
        case 0x4C:
            return VGFX_KEY_ENTER; /* Enter key (numpad) */
        case 0x35:
            return VGFX_KEY_ESCAPE; /* Escape key */
        case 0x33:
            return VGFX_KEY_BACKSPACE; /* Backspace (delete backward) */
        case 0x75:
            return VGFX_KEY_DELETE; /* Forward delete */
        case 0x30:
            return VGFX_KEY_TAB; /* Tab key */
        case 0x73:
            return VGFX_KEY_HOME; /* Home key */
        case 0x77:
            return VGFX_KEY_END; /* End key */
        case 0x74:
            return VGFX_KEY_PAGE_UP; /* Page Up key */
        case 0x79:
            return VGFX_KEY_PAGE_DOWN; /* Page Down key */
        case 0x7B:
            return VGFX_KEY_LEFT; /* Left arrow */
        case 0x7C:
            return VGFX_KEY_RIGHT; /* Right arrow */
        case 0x7E:
            return VGFX_KEY_UP; /* Up arrow */
        case 0x7D:
            return VGFX_KEY_DOWN; /* Down arrow */
        default:
            return VGFX_KEY_UNKNOWN; /* Unrecognized key */
    }
}

static bool macos_should_check_menu_key_equivalent(vgfx_key_t key, NSEventModifierFlags flags) {
    if (!(flags & NSEventModifierFlagCommand))
        return false;

    switch (key) {
        case VGFX_KEY_LEFT:
        case VGFX_KEY_RIGHT:
        case VGFX_KEY_UP:
        case VGFX_KEY_DOWN:
        case VGFX_KEY_TAB:
        case VGFX_KEY_HOME:
        case VGFX_KEY_END:
        case VGFX_KEY_PAGE_UP:
        case VGFX_KEY_PAGE_DOWN:
        case VGFX_KEY_BACKSPACE:
        case VGFX_KEY_DELETE:
            return false;
        default:
            return key != VGFX_KEY_UNKNOWN;
    }
}

/// @brief Normalize Cocoa text-input callback values to a plain NSString.
/// @details NSTextInputClient permits NSString or NSAttributedString values. Unknown values use
///          their textual description, and nil becomes an empty string, so platform callbacks
///          never pass borrowed Objective-C object layouts into the C event queue.
/// @param value Cocoa text input value supplied by AppKit.
/// @return Autoreleased plain string suitable for UTF-8 conversion.
static NSString *macos_plain_input_string(id value) {
    if ([value isKindOfClass:[NSAttributedString class]])
        return [(NSAttributedString *)value string];
    if ([value isKindOfClass:[NSString class]])
        return (NSString *)value;
    return value ? [value description] : @ "";
}

/// @brief Replace embedded U+0000 values before crossing the C-string editor boundary.
/// @details Composition events are byte-counted, but lower retained text remains NUL-terminated.
///          Converting U+0000 to U+FFFD preserves all following native text instead of silently
///          truncating it at the first embedded zero.
/// @param value Plain native input string.
/// @return String containing no embedded U+0000 values.
static NSString *macos_sanitize_input_string(NSString *value) {
    if (!value || value.length == 0)
        return @ "";
    unichar nul = 0;
    NSString *nul_string = [NSString stringWithCharacters:&nul length:1];
    return [value stringByReplacingOccurrencesOfString:nul_string withString:@ "\uFFFD"];
}

/// @brief Count Unicode codepoints in an NSString UTF-16 prefix.
/// @details Valid surrogate pairs count once; lone surrogates count once and are later encoded by
///          NSString's lossy UTF-8 bridge. The UTF-16 limit is clamped to the string length.
/// @param value Native string whose prefix is measured.
/// @param utf16_limit Number of UTF-16 code units requested.
/// @return Unicode-codepoint count saturated to INT32_MAX.
static int32_t macos_codepoint_count_prefix(NSString *value, NSUInteger utf16_limit) {
    if (!value)
        return 0;
    NSUInteger length = [value length];
    if (utf16_limit > length)
        utf16_limit = length;
    NSUInteger index = 0;
    int64_t count = 0;
    while (index < utf16_limit) {
        unichar high = [value characterAtIndex:index++];
        if (high >= 0xD800 && high <= 0xDBFF && index < utf16_limit) {
            unichar low = [value characterAtIndex:index];
            if (low >= 0xDC00 && low <= 0xDFFF)
                index++;
        }
        if (count < INT32_MAX)
            count++;
    }
    return (int32_t)count;
}

/// @brief Enqueue one Cocoa NSTextInputClient composition lifecycle event.
/// @details Converts AppKit UTF-16 selection offsets to codepoint units, stores valid UTF-8 inline,
///          and uses the current-client-selection sentinel for replacement because ZannaGFX does
///          not retain application text. The focused GUI editor owns the authoritative selection.
/// @param view Native framebuffer view receiving AppKit text callbacks.
/// @param type Composition lifecycle discriminator.
/// @param value NSString or NSAttributedString callback value; nil becomes empty.
/// @param selection Native preedit selection in UTF-16 units.
static void macos_enqueue_composition_event(VGFXView *view,
                                            vgfx_event_type_t type,
                                            id value,
                                            NSRange selection) {
    if (!view || !view.vgfxWindow)
        return;
    NSString *text = macos_sanitize_input_string(macos_plain_input_string(value));
    NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:YES];
    const char *bytes = utf8.length > 0 ? (const char *)utf8.bytes : "";
    int32_t selection_start = selection.location == NSNotFound
                                  ? 0
                                  : macos_codepoint_count_prefix(text, selection.location);
    NSUInteger selection_end_utf16 = selection.location == NSNotFound ? 0 : selection.location;
    if (selection.location != NSNotFound) {
        if (selection.length <= NSUIntegerMax - selection_end_utf16)
            selection_end_utf16 += selection.length;
        else
            selection_end_utf16 = NSUIntegerMax;
    }
    int32_t selection_end = macos_codepoint_count_prefix(text, selection_end_utf16);
    vgfx_event_t event;
    if (vgfx_internal_init_composition_event(
            &event,
            type,
            view.textInputTimestamp,
            bytes,
            (size_t)utf8.length,
            selection_start,
            selection_end >= selection_start ? selection_end - selection_start : 0,
            -1,
            -1,
            view.textInputModifiers)) {
        vgfx_internal_enqueue_event(view.vgfxWindow, &event);
    }
}

//===----------------------------------------------------------------------===//
// Custom NSView for Framebuffer Display
//===----------------------------------------------------------------------===//

@implementation VGFXView

/// @brief Initialize the view with the given frame rectangle.
/// @details Creates an NSView subclass that will display the ZannaGFX
///          framebuffer.  Sets up the backlink to NULL (assigned later
///          by vgfx_platform_init_window).
///
/// @param frameRect Initial frame rectangle for the view (in window coordinates)
/// @return Initialized VGFXView instance, or nil on failure
- (id)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _vgfxWindow = NULL;
        _textInputTimestamp = 0;
        _textInputModifiers = 0;
        _markedText = @ "";
        _compositionActive = NO;
        [self setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
        // Register as a file drop target
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    }
    return self;
}

/// @brief Declare the view as opaque (optimization hint for Cocoa).
/// @details Tells the Cocoa drawing system that this view fills its entire
///          bounds with opaque content, enabling faster compositing.
///
/// @return YES (view is always opaque)
- (BOOL)isOpaque {
    return YES;
}

/// @brief Allow the view to become the first responder.
/// @details Required to receive keyboard events.  Without this, the view
///          won't receive key presses.
///
/// @return YES (view accepts first responder status)
- (BOOL)acceptsFirstResponder {
    return YES;
}

/// @brief Route a native key press through AppKit's text input manager.
/// @details Physical key state is queued by the outer platform pump first. `interpretKeyEvents:`
///          then invokes this view's NSTextInputClient methods for layout-aware text, dead keys,
///          dictation, and IME preedit without synthesizing characters from key codes.
/// @param event Native key-down event being interpreted.
- (void)keyDown:(NSEvent *)event {
    if (!event)
        return;
    [self interpretKeyEvents:@[ event ]];
}

/// @brief Accept committed native text from AppKit.
/// @details Active marked text becomes one composition commit. Ordinary single-character input is
///          emitted through the legacy codepoint event path so all existing text-capable widgets
///          remain compatible. Multi-character replacement/dictation is wrapped in start/commit
///          boundaries to retain one logical undo record.
/// @param value NSString or NSAttributedString committed by AppKit.
/// @param replacementRange Native replacement range; current GUI selection remains authoritative.
- (void)insertText:(id)value replacementRange:(NSRange)replacementRange {
    NSString *text = macos_sanitize_input_string(macos_plain_input_string(value));
    if (_compositionActive) {
        macos_enqueue_composition_event(
            self, VGFX_EVENT_COMPOSITION_COMMIT, text, NSMakeRange(0, 0));
        _compositionActive = NO;
        _markedText = @ "";
        return;
    }

    const BOOL is_replacement = replacementRange.location != NSNotFound;
    const BOOL is_multi_codepoint = macos_codepoint_count_prefix(text, text.length) > 1;
    if (is_replacement || is_multi_codepoint) {
        macos_enqueue_composition_event(self, VGFX_EVENT_COMPOSITION_START, @ "", NSMakeRange(0, 0));
        macos_enqueue_composition_event(
            self, VGFX_EVENT_COMPOSITION_COMMIT, text, NSMakeRange(0, 0));
        return;
    }

    /* Option-modified characters are already interpreted by AppKit. Clear
       only the shortcut modifier on the resulting text event so legitimate
       macOS Option glyph entry is not rejected as an Alt shortcut. */
    macos_enqueue_text_input_events(
        _vgfxWindow, _textInputTimestamp, text, _textInputModifiers & ~VGFX_MOD_ALT);
}

/// @brief Update the visible native IME preedit string and internal selection.
/// @details The first update emits an explicit start boundary. Later updates replace only the
///          separate marked buffer; committed application text is untouched until insertText or
///          unmarkText commits it.
/// @param value NSString or NSAttributedString marked value supplied by AppKit.
/// @param selectedRange Selection/caret within marked text in UTF-16 units.
/// @param replacementRange Native committed-text replacement; GUI current selection is used.
- (void)setMarkedText:(id)value
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString *text = macos_sanitize_input_string(macos_plain_input_string(value));
    if (!_compositionActive) {
        macos_enqueue_composition_event(self, VGFX_EVENT_COMPOSITION_START, @ "", NSMakeRange(0, 0));
        _compositionActive = YES;
    }
    _markedText = text;
    macos_enqueue_composition_event(self, VGFX_EVENT_COMPOSITION_UPDATE, text, selectedRange);
}

/// @brief End native marking while preserving AppKit's intended text result.
/// @details In the NSTextInputClient model marked text already lives in the document. Zanna keeps
///          it separate, so a non-empty marked value is committed here; an empty marked value is
///          an explicit cancellation. Exactly one terminal event is emitted.
- (void)unmarkText {
    if (!_compositionActive)
        return;
    if (_markedText.length > 0) {
        macos_enqueue_composition_event(
            self, VGFX_EVENT_COMPOSITION_COMMIT, _markedText, NSMakeRange(0, 0));
    } else {
        macos_enqueue_composition_event(
            self, VGFX_EVENT_COMPOSITION_CANCEL, @ "", NSMakeRange(0, 0));
    }
    _compositionActive = NO;
    _markedText = @ "";
}

/// @brief Report whether AppKit currently owns non-terminal marked text.
/// @return YES between composition start and commit/cancel.
- (BOOL)hasMarkedText {
    return _compositionActive && _markedText.length > 0;
}

/// @brief Return the UTF-16 range occupied by the native marked value.
/// @return Zero-based range within the view's lightweight marked-text client, or NSNotFound.
- (NSRange)markedRange {
    return _compositionActive ? NSMakeRange(0, _markedText.length) : NSMakeRange(NSNotFound, 0);
}

/// @brief Return the native committed-text selection placeholder.
/// @details The retained GUI editor owns authoritative grapheme selection. AppKit callbacks use
///          the current-selection replacement sentinel, so this lightweight client reports an
///          empty range rather than duplicating application text state.
/// @return Empty UTF-16 range.
- (NSRange)selectedRange {
    return NSMakeRange(0, 0);
}

/// @brief Decline attributed committed-text substring queries.
/// @details ZannaGFX intentionally does not retain widget text. Returning nil instructs input
///          methods to use the current-selection replacement path without stale native copies.
/// @param range Proposed native UTF-16 range.
/// @param actualRange Receives NSNotFound when non-NULL.
/// @return nil because committed text belongs to the GUI layer.
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actualRange {
    (void)range;
    if (actualRange)
        *actualRange = NSMakeRange(NSNotFound, 0);
    return nil;
}

/// @brief Return supported marked-text attributes.
/// @return Empty array because ZannaGUI derives preedit visuals from its theme.
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
    return @[];
}

/// @brief Provide a stable candidate-window anchor in global screen coordinates.
/// @details Until the focused widget publishes a finer caret rectangle, the top-left of the
///          framebuffer view is used. This keeps candidate UI attached to the correct window and
///          screen rather than falling back to an unrelated desktop origin.
/// @param range Requested native character range.
/// @param actualRange Receives an empty range when non-NULL.
/// @return One-point-wide screen rectangle anchored at the view's top-left.
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    if (actualRange)
        *actualRange = NSMakeRange(0, 0);
    NSRect local = NSMakeRect(0.0, NSMaxY([self bounds]), 1.0, 1.0);
    NSRect window_rect = [self convertRect:local toView:nil];
    return self.window ? [self.window convertRectToScreen:window_rect] : NSZeroRect;
}

/// @brief Map a native screen point to the lightweight client's character index.
/// @param point Screen point supplied by AppKit.
/// @return Zero because committed document geometry belongs to the GUI widget.
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

/// @brief Handle command selectors produced during native key interpretation.
/// @details Physical navigation/edit keys are already queued separately. Escape-style cancellation
///          terminates active preedit here; all other selectors remain available to the GUI event.
/// @param selector AppKit command selector.
- (void)doCommandBySelector:(SEL)selector {
    if (selector == @selector(cancelOperation:) && _compositionActive) {
        macos_enqueue_composition_event(
            self, VGFX_EVENT_COMPOSITION_CANCEL, @ "", NSMakeRange(0, 0));
        _compositionActive = NO;
        _markedText = @ "";
    }
}

/// @brief Accept mouse clicks even when the window is not focused.
/// @details Without this, the first click on an unfocused window only gives
///          focus without generating a mouse event.
///
/// @param event The mouse-down event (unused)
/// @return YES (always accept first mouse click)
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}

- (void)resetCursorRects {
    [super resetCursorRects];
    if (!_vgfxWindow || !_vgfxWindow->platform_data)
        return;
    vgfx_macos_platform *platform = (vgfx_macos_platform *)_vgfxWindow->platform_data;
    [self addCursorRect:[self bounds] cursor:macos_cursor_for_type(platform->cursor_type)];
}

- (void)cursorUpdate:(NSEvent *)event {
    (void)event;
    if (!_vgfxWindow || !_vgfxWindow->platform_data)
        return;
    vgfx_macos_platform *platform = (vgfx_macos_platform *)_vgfxWindow->platform_data;
    [macos_cursor_for_type(platform->cursor_type) set];
}

/// @brief Render the framebuffer to the view.
/// @details Called by Cocoa when the view needs to be redrawn (triggered by
///          setNeedsDisplay:YES in vgfx_platform_present).  Creates a CGImage
///          from the raw RGBA framebuffer and draws it to the view's graphics
///          context, handling coordinate system conversion (flip Y axis).
///
/// @param dirtyRect The rectangle that needs redrawing (ignored - we redraw all)
///
/// @details Rendering process:
///            1. Create CGColorSpace (device RGB)
///            2. Create CGDataProvider from framebuffer pointer (zero-copy)
///            3. Create CGImage with format info (8-bit RGBA, 32 bpp)
///            4. Flip coordinate system (Cocoa bottom-left → top-left)
///            5. Draw CGImage to view's CGContext
///            6. Restore coordinate system and cleanup
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;

    if (!_vgfxWindow || !_vgfxWindow->pixels) {
        /* No framebuffer available - clear to black */
        [[NSColor blackColor] setFill];
        NSRectFill(self.bounds);
        return;
    }


    /* Create CGImage from framebuffer (zero-copy via CGDataProvider) */
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL,                                      /* info (release callback context) */
        _vgfxWindow->pixels,                       /* data pointer (framebuffer) */
        _vgfxWindow->stride * _vgfxWindow->height, /* data size in bytes */
        NULL);                                     /* release callback (none, we own the buffer) */

    CGImageRef image = NULL;
    if (colorSpace && provider) {
        image = CGImageCreate(_vgfxWindow->width,  /* width in pixels */
                              _vgfxWindow->height, /* height in pixels */
                              8,                   /* bits per component (R/G/B/A each 8 bits) */
                              32,                  /* bits per pixel (4 * 8 = 32) */
                              _vgfxWindow->stride, /* bytes per row */
                              colorSpace,          /* color space (RGB) */
                              kCGBitmapByteOrderDefault | kCGImageAlphaLast, /* format (RGBA) */
                              provider,                                      /* data provider */
                              NULL,                       /* decode array (none) */
                              false,                      /* should interpolate (no, pixel art) */
                              kCGRenderingIntentDefault); /* rendering intent */
    }

    /* Get the view's Core Graphics context */
    CGContextRef context = [[NSGraphicsContext currentContext] CGContext];

    /* Use view bounds (logical points) for the destination rect.
     * The framebuffer is allocated at PHYSICAL pixel dimensions (win->width ×
     * win->height), which is larger than the view bounds on Retina.  CoreGraphics
     * automatically maps each physical framebuffer pixel 1:1 to a physical screen
     * pixel when drawing a physical-size CGImage into the logical view rect. */
    CGFloat view_height = self.bounds.size.height;
    CGFloat view_width = self.bounds.size.width;

    /*
     * Note: We deliberately DON'T flip the coordinate system here.
     * CGImage has origin at top-left (row 0 at top), and Cocoa's default
     * coordinate system has origin at bottom-left. When we draw without
     * flipping, CGContextDrawImage will flip the image for us (image appears
     * "upside down" from its data perspective, but correct visually).
     *
     * This is because CGContextDrawImage interprets the image with its
     * origin at the rect's origin, and the image fills the rect upward from there.
     */
    if (context && image) {
        CGRect rect = CGRectMake(0, 0, view_width, view_height);
        CGContextDrawImage(context, rect, image);
    }

    /* Cleanup (release Core Graphics objects) */
    if (image)
        CGImageRelease(image);
    if (provider)
        CGDataProviderRelease(provider);
    if (colorSpace)
        CGColorSpaceRelease(colorSpace);
}

//===------------------------------------------------------------------===//
// NSDraggingDestination — File Drop Support
//===------------------------------------------------------------------===//

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (!_vgfxWindow)
        return NO;

    NSPasteboard *pb = [sender draggingPasteboard];
    NSArray<NSURL *> *urls =
        [pb readObjectsForClasses:@[ [NSURL class] ]
                          options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];

    int64_t timestamp = vgfx_platform_now_ms();
    for (NSURL *url in urls) {
        const char *path = [[url path] UTF8String];
        if (!path)
            continue;

        vgfx_event_t event = {0};
        event.type = VGFX_EVENT_FILE_DROP;
        event.time_ms = timestamp;
        size_t len = strlen(path);
        if (len >= sizeof(event.data.file_drop.path)) {
            vgfx_internal_note_event_overflow(_vgfxWindow);
            continue;
        }
        memcpy(event.data.file_drop.path, path, len);
        event.data.file_drop.path[len] = '\0';
        vgfx_internal_enqueue_event(_vgfxWindow, &event);
    }
    return YES;
}

@end

//===----------------------------------------------------------------------===//
// Window Delegate for Events
//===----------------------------------------------------------------------===//

@implementation VGFXWindowDelegate

/// @brief Handle window close button click.
/// @details Called when the user clicks the red close button (⊗).  We don't
///          actually close the window here - instead, we set a flag and enqueue
///          a CLOSE event for the application to handle.  This gives the app
///          a chance to save state, prompt the user, etc.  Returns NO to prevent
///          Cocoa from closing the window automatically.
///
/// @param sender The NSWindow that is attempting to close (unused)
/// @return NO (don't close the window automatically, let app handle it)
///
/// @post close_requested flag set to 1
/// @post CLOSE event enqueued in the event queue
- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;

    if (!_vgfxWindow)
        return NO;

    vgfx_macos_platform *platform = (vgfx_macos_platform *)_vgfxWindow->platform_data;
    if (!platform)
        return NO;

    vgfx_internal_event_lock(_vgfxWindow);
    int prevent_close = _vgfxWindow->prevent_close;
    vgfx_internal_event_unlock(_vgfxWindow);

    if (!prevent_close) {
        /* Mark close as requested (can be checked by the application) */
        platform->close_requested = 1;
        vgfx_internal_set_close_requested(_vgfxWindow, 1);

        /* Enqueue CLOSE event for the application to handle */
        vgfx_event_t event = {.type = VGFX_EVENT_CLOSE, .time_ms = vgfx_platform_now_ms()};
        vgfx_internal_enqueue_event(_vgfxWindow, &event);
    }

    /* Don't actually close the window - let the application decide */
    return NO;
}

/// @brief Handle window resize.
/// @details Called when the window's size changes (user drag, maximize, etc.).
///          Updates the ZannaGFX window dimensions and reallocates the
///          framebuffer to match the new size.  Enqueues a RESIZE event.
///
/// @param notification Notification object with resize details (unused)
///
/// @post Window dimensions updated to match new size
/// @post Framebuffer reallocated and cleared to black
/// @post RESIZE event enqueued in the event queue
///
/// @warning Framebuffer reallocation may fail (memory allocation).  In that
///          case, the window will have a NULL framebuffer and rendering will
///          display black (handled in drawRect:).
- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    macos_sync_window_metrics(_vgfxWindow, 1, 1);
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    (void)notification;
    macos_sync_window_metrics(_vgfxWindow, 1, 0);
}

- (void)windowDidChangeScreen:(NSNotification *)notification {
    (void)notification;
    macos_sync_window_metrics(_vgfxWindow, 1, 0);
}

- (void)windowWillEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    if (!_vgfxWindow || !_vgfxWindow->platform_data)
        return;
    vgfx_macos_platform *platform = (vgfx_macos_platform *)_vgfxWindow->platform_data;
    if (platform->view)
        platform->windowed_content_size = [platform->view bounds].size;
    platform->has_windowed_content_size = platform->view != nil ? 1 : 0;
    macos_set_view_tracks_window(platform->view);
}

- (void)windowDidEnterFullScreen:(NSNotification *)notification {
    (void)notification;
    macos_sync_window_metrics(_vgfxWindow, 1, 1);
}

- (void)windowDidExitFullScreen:(NSNotification *)notification {
    (void)notification;

    if (_vgfxWindow && _vgfxWindow->platform_data) {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)_vgfxWindow->platform_data;
        if (platform->window && platform->fullscreen_resizable_added && !_vgfxWindow->resizable) {
            [platform->window
                setStyleMask:([platform->window styleMask] & ~NSWindowStyleMaskResizable)];
            platform->fullscreen_resizable_added = 0;
        }

        if (platform->window && platform->view && platform->has_windowed_content_size) {
            [platform->window setContentSize:platform->windowed_content_size];
            [platform->view setFrame:NSMakeRect(0.0,
                                                0.0,
                                                platform->windowed_content_size.width,
                                                platform->windowed_content_size.height)];
            platform->has_windowed_content_size = 0;
        }
    }

    macos_sync_window_metrics(_vgfxWindow, 1, 1);
}

- (NSSize)window:(NSWindow *)window willUseFullScreenContentSize:(NSSize)proposedSize {
    (void)window;
    return proposedSize;
}

/// @brief Handle window gaining focus.
/// @details Called when the window becomes the key window (receives keyboard
///          input).  Enqueues a FOCUS_GAINED event.
///
/// @param notification Notification object (unused)
///
/// @post FOCUS_GAINED event enqueued in the event queue
- (void)windowDidBecomeKey:(NSNotification *)notification {
    (void)notification;

    if (!_vgfxWindow)
        return;

    vgfx_internal_set_focus_state(_vgfxWindow, 1);

    /* Re-engage relative mouse mode (cursor dissociation is a global,
     * per-process setting — only hold it while we own focus). */
    if (_vgfxWindow->relative_mouse_enabled)
        CGAssociateMouseAndMouseCursorPosition(false);

    vgfx_event_t event = {.type = VGFX_EVENT_FOCUS_GAINED, .time_ms = vgfx_platform_now_ms()};
    vgfx_internal_enqueue_event(_vgfxWindow, &event);
}

/// @brief Handle window losing focus.
/// @details Called when the window resigns key window status (loses keyboard
///          input).  Enqueues a FOCUS_LOST event.
///
/// @param notification Notification object (unused)
///
/// @post FOCUS_LOST event enqueued in the event queue
- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;

    if (!_vgfxWindow)
        return;

    vgfx_macos_platform *platform = (vgfx_macos_platform *)_vgfxWindow->platform_data;
    if (platform && platform->view.compositionActive) {
        platform->view.textInputTimestamp = vgfx_platform_now_ms();
        [platform->view doCommandBySelector:@selector(cancelOperation:)];
    }
    vgfx_internal_set_focus_state(_vgfxWindow, 0);
    vgfx_internal_clear_input_state(_vgfxWindow);

    /* Suspend relative mouse mode while unfocused so the rest of the desktop
     * gets a normal cursor back (resumed in windowDidBecomeKey). */
    if (_vgfxWindow->relative_mouse_enabled)
        CGAssociateMouseAndMouseCursorPosition(true);

    vgfx_event_t event = {.type = VGFX_EVENT_FOCUS_LOST, .time_ms = vgfx_platform_now_ms()};
    vgfx_internal_enqueue_event(_vgfxWindow, &event);
}

@end

//===----------------------------------------------------------------------===//
// Platform API Implementation
//===----------------------------------------------------------------------===//

/// @brief Query the HiDPI backing scale factor from the macOS screen.
/// @details Queries [NSScreen mainScreen].backingScaleFactor, which returns
///          2.0 on Retina displays and 1.0 on standard (96 DPI) displays.
///          NSApplication is initialized first to ensure screen properties
///          are fully available.
///
/// @return Scale factor ≥ 1.0 (2.0 on Retina, 1.0 on standard displays)
float vgfx_platform_get_display_scale(void) {
    @autoreleasepool {
        /* Ensure NSApp is initialized before querying screen properties.
         * Calling sharedApplication multiple times is safe (singleton). */
        [NSApplication sharedApplication];

        NSScreen *main = [NSScreen mainScreen];
        if (!main)
            return 1.0f;

        CGFloat s = [main backingScaleFactor];
        return (s >= 1.0) ? (float)s : 1.0f;
    }
}

/// @brief Query the primary display's logical (point) dimensions.
/// @details NSScreen frames are already in points, which matches vgfx's
///          logical coordinate space. Used to size fullscreen windows.
/// @return 1 on success, 0 when no screen is available.
int vgfx_platform_get_display_logical_size(int32_t *out_w, int32_t *out_h) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSScreen *main = [NSScreen mainScreen];
        if (!main)
            return 0;
        NSRect frame = [main frame];
        if (out_w)
            *out_w = (int32_t)frame.size.width;
        if (out_h)
            *out_h = (int32_t)frame.size.height;
        return frame.size.width > 0 && frame.size.height > 0;
    }
}

/// @brief Initialize platform-specific window resources for macOS.
/// @details Creates an NSWindow with a custom VGFXView for framebuffer display.
///          Initializes NSApplication (required for any Cocoa window), creates
///          the window with appropriate style mask, sets up the delegate, and
///          makes the window visible.
///
/// @param win    Pointer to the ZannaGFX window structure (framebuffer already allocated)
/// @param params Window creation parameters (title, dimensions, resizable flag)
/// @return 1 on success, 0 on failure
///
/// @pre  win != NULL
/// @pre  params != NULL
/// @pre  win->pixels != NULL (framebuffer allocated by vgfx_create_window)
/// @post On success: NSWindow created and visible, platform_data allocated
/// @post On failure: platform_data NULL, error set
///
/// @details The window is:
///            - Titled (has a title bar)
///            - Closable (red ⊗ button)
///            - Miniaturizable (yellow ⊖ button)
///            - Resizable (optional, based on params->resizable)
///            - Centered on screen
///            - Made visible unless ZANNA_GFX_HIDE_WINDOWS is set
///            - Normally focused unless ZANNA_GFX_NO_ACTIVATE is set
int vgfx_platform_init_window(struct vgfx_window *win, const vgfx_window_params_t *params) {
    if (!win || !params)
        return 0;

    @autoreleasepool {
        int hide_window = vgfx_macos_hide_windows();
        int no_activate = vgfx_macos_no_activate_on_create();

        /* Ensure NSApplication is initialized (singleton) */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:no_activate ? NSApplicationActivationPolicyAccessory
                                               : NSApplicationActivationPolicyRegular];
        vgfx_platform_macos_ensure_default_main_menu(params->title);

        /* Allocate platform data structure */
        vgfx_macos_platform *platform =
            (vgfx_macos_platform *)calloc(1, sizeof(vgfx_macos_platform));
        if (!platform) {
            vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to allocate platform data");
            return 0;
        }

        win->platform_data = platform;
        platform->close_requested = 0;

        /* Create window style mask (controls which buttons appear in title bar) */
        NSWindowStyleMask styleMask =
            NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;

        if (params->resizable) {
            styleMask |= NSWindowStyleMaskResizable;
        }

        /* Create NSWindow */
        NSRect contentRect = NSMakeRect(0, 0, params->width, params->height);
        platform->window =
            [[NSWindow alloc] initWithContentRect:contentRect
                                        styleMask:styleMask
                                          backing:NSBackingStoreBuffered /* Double-buffered */
                                            defer:NO];                   /* Create immediately */

        if (!platform->window) {
            free(platform);
            win->platform_data = NULL;
            vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to create NSWindow");
            return 0;
        }

        /* Set window title */
        [platform->window setTitle:vgfx_macos_string_or_empty(params->title)];
        if ([platform->window respondsToSelector:@selector(setTabbingMode:)]) {
            [platform->window setTabbingMode:NSWindowTabbingModeDisallowed];
        }
        [platform->window setCollectionBehavior:([platform->window collectionBehavior] |
                                                 NSWindowCollectionBehaviorFullScreenPrimary)];

        /* Create custom view for framebuffer display */
        platform->view = [[VGFXView alloc] initWithFrame:contentRect];
        platform->view.vgfxWindow = win; /* Backlink for rendering */

        /* Set view as the window's content view */
        [platform->window setContentView:platform->view];

        /* Create and set window delegate */
        platform->delegate = [[VGFXWindowDelegate alloc] init];
        platform->delegate.vgfxWindow = win; /* Backlink for events */
        [platform->window setDelegate:platform->delegate];

        /* Center window on screen and make it visible */
        [platform->window center];
        if (hide_window) {
            [platform->window orderOut:nil];
            vgfx_internal_set_focus_state(win, 0);
        } else if (no_activate) {
            [platform->window orderBack:nil];
            vgfx_internal_set_focus_state(win, 0);
        } else {
            [platform->window makeKeyAndOrderFront:nil];
        }
        macos_sync_window_metrics(win, 0, 0);

        /* Activate the application (bring to foreground) */
        if (!no_activate)
            [NSApp activateIgnoringOtherApps:YES];

        return 1;
    }
}

/// @brief Destroy platform-specific window resources for macOS.
/// @details Closes the NSWindow, releases the delegate and view, and frees
///          the platform data structure.  Safe to call even if init failed.
///
/// @param win Pointer to the ZannaGFX window structure
///
/// @pre  win != NULL
/// @post platform_data freed and set to NULL
/// @post NSWindow closed and released (if it existed)
void vgfx_platform_destroy_window(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;

    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;

        if (platform->delegate)
            platform->delegate.vgfxWindow = NULL;
        if (platform->view)
            platform->view.vgfxWindow = NULL;

        /* Close and release window */
        if (platform->window) {
            [platform->window setDelegate:nil]; /* Clear delegate to avoid stale pointer */
            [platform->window close];
            platform->window = nil;
        }

        /* Release delegate (ARC will deallocate) */
        if (platform->delegate) {
            platform->delegate = nil;
        }

        /* Release view (ARC will deallocate) */
        if (platform->view) {
            platform->view = nil;
        }

        /* Free platform data */
        free(platform);
        win->platform_data = NULL;
    }
}

/// @brief Process pending macOS events and translate to ZannaGFX events.
/// @details Polls the NSApplication event queue in non-blocking mode
///          (distantPast = don't wait).  For each NSEvent, sends it to the
///          application for normal processing, then translates it to a
///          vgfx_event_t and enqueues it.
///
///          Handles:
///            - Keyboard: NSEventTypeKeyDown/KeyUp → KEY_DOWN/KEY_UP
///            - Mouse move: NSEventTypeMouseMoved/Dragged → MOUSE_MOVE
///            - Mouse buttons: NSEventTypeLeftMouse* → MOUSE_DOWN/MOUSE_UP
///
/// @param win Pointer to the ZannaGFX window structure
/// @return 1 on success, 0 on failure
///
/// @pre  win != NULL
/// @pre  win->platform_data != NULL
/// @post All pending NSEvents processed and translated
/// @post win->key_state and win->mouse_* updated to reflect current input state
/// @post Corresponding vgfx_event_t enqueued for each NSEvent
///
/// @details Coordinate conversion:
///            - NSEvent coordinates: origin at bottom-left
///            - ZannaGFX coordinates: origin at top-left
///            - Conversion: vgfx_y = (view_height - ns_y - 1)
static int macos_modifiers(NSEventModifierFlags flags) {
    int mods = 0;
    if (flags & NSEventModifierFlagShift)
        mods |= VGFX_MOD_SHIFT;
    if (flags & NSEventModifierFlagControl)
        mods |= VGFX_MOD_CTRL;
    if (flags & NSEventModifierFlagOption)
        mods |= VGFX_MOD_ALT;
    if (flags & NSEventModifierFlagCommand)
        mods |= VGFX_MOD_CMD;
    return mods;
}

static uint32_t macos_codepoint_at(NSString *string, NSUInteger *index) {
    if (!string || !index || *index >= [string length])
        return 0;

    unichar high = [string characterAtIndex:*index];
    (*index)++;
    if (high >= 0xD800 && high <= 0xDBFF) {
        if (*index < [string length]) {
            unichar low = [string characterAtIndex:*index];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                (*index)++;
                return 0x10000 + ((((uint32_t)high - 0xD800) << 10) | ((uint32_t)low - 0xDC00));
            }
        }
        return 0xFFFD;
    }
    if (high >= 0xDC00 && high <= 0xDFFF)
        return 0xFFFD;
    return (uint32_t)high;
}

static void macos_enqueue_text_input_events(struct vgfx_window *win,
                                            int64_t timestamp,
                                            NSString *characters,
                                            int modifiers) {
    if (!win || !characters)
        return;

    NSUInteger index = 0;
    NSUInteger length = [characters length];
    while (index < length) {
        uint32_t codepoint = macos_codepoint_at(characters, &index);
        if (vgfx_internal_should_emit_text_input(codepoint, modifiers)) {
            vgfx_event_t text_event = {
                .type = VGFX_EVENT_TEXT_INPUT,
                .time_ms = timestamp,
                .data.text = {.codepoint = codepoint, .modifiers = modifiers}};
            vgfx_internal_enqueue_event(win, &text_event);
        }
    }
}

int vgfx_platform_wait_events(struct vgfx_window *win, int32_t timeout_ms) {
    if (!win || !win->platform_data)
        return 0;
    if (timeout_ms <= 0)
        return 0;

    @autoreleasepool {
        /* Block up to timeout for the next event, but leave it queued
           (dequeue:NO) so vgfx_platform_process_events handles it normally. */
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:(double)timeout_ms / 1000.0];
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:deadline
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:NO];
        return event != nil ? 1 : 0;
    }
}

int vgfx_platform_process_events(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;

    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;

        /* Process all pending events without blocking (non-blocking poll) */
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast] /* Don't wait */
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES])) { /* Remove from queue */

            NSEventType eventType = [event type];
            if (eventType == NSEventTypeKeyDown) {
                /* Let the native app menu consume real Command-based shortcuts
                   before we translate the event into Zanna input. Plain
                   navigation keys (arrows, tab, home/end, delete) must always
                   reach the game/runtime input path. */
                NSEventModifierFlags flags = [event modifierFlags];
                vgfx_key_t key =
                    translate_keycode([event keyCode], [event charactersIgnoringModifiers], flags);
                NSMenu *mainMenu = [NSApp mainMenu];
                if (mainMenu && macos_should_check_menu_key_equivalent(key, flags) &&
                    [mainMenu performKeyEquivalent:event]) {
                    continue;
                }
            }

            int dispatch_key_after_zanna =
                (eventType == NSEventTypeKeyDown || eventType == NSEventTypeKeyUp);
            if (!dispatch_key_after_zanna) {
                [NSApp sendEvent:event];
            }

            /* Translate to ZannaGFX events */
            int64_t timestamp = vgfx_platform_now_ms();

            switch ([event type]) {
                case NSEventTypeKeyDown: {
                    NSEventModifierFlags flags = [event modifierFlags];
                    int mods = macos_modifiers(flags);
                    vgfx_key_t key = translate_keycode(
                        [event keyCode], [event charactersIgnoringModifiers], flags);
                    if (key != VGFX_KEY_UNKNOWN && key < 512) {
                        vgfx_internal_set_key_state(win, key, 1);

                        vgfx_event_t vgfx_event = {
                            .type = VGFX_EVENT_KEY_DOWN,
                            .time_ms = timestamp,
                            .data.key = {.key = key,
                                         .is_repeat = [event isARepeat] ? 1 : 0,
                                         .modifiers = mods}};
                        vgfx_internal_enqueue_event(win, &vgfx_event);
                    }
                    platform->view.textInputTimestamp = timestamp;
                    platform->view.textInputModifiers = mods;
                    break;
                }

                case NSEventTypeKeyUp: {
                    NSEventModifierFlags flags = [event modifierFlags];
                    vgfx_key_t key = translate_keycode(
                        [event keyCode], [event charactersIgnoringModifiers], flags);
                    if (key != VGFX_KEY_UNKNOWN && key < 512) {
                        vgfx_internal_set_key_state(win, key, 0);

                        vgfx_event_t vgfx_event = {
                            .type = VGFX_EVENT_KEY_UP,
                            .time_ms = timestamp,
                            .data.key = {.key = key,
                                         .is_repeat = 0,
                                         .modifiers = macos_modifiers([event modifierFlags])}};
                        vgfx_internal_enqueue_event(win, &vgfx_event);
                    }
                    break;
                }

                case NSEventTypeMouseMoved:
                case NSEventTypeLeftMouseDragged:
                case NSEventTypeRightMouseDragged:
                case NSEventTypeOtherMouseDragged: {
                    NSPoint location = [event locationInWindow];
                    NSRect contentRect = [platform->view bounds];

                    int32_t x, y;
                    macos_event_location_to_physical(win, location, contentRect, &x, &y);

                    /* Relative (raw) mouse mode: while the hardware mouse is
                     * dissociated from the cursor, deltaX/deltaY keep flowing
                     * unbounded. Accumulate them in logical units so relative
                     * deltas match the coordinate space of the warp-to-center
                     * fallback (points * backing scale / coord scale). */
                    if (win->relative_mouse_enabled && win->relative_mouse_native) {
                        double sf = win->scale_factor > 0.0f ? (double)win->scale_factor : 1.0;
                        double cs = (double)vgfx_internal_coord_scale(win);
                        if (cs <= 0.0)
                            cs = 1.0;
                        double k = sf / cs;
                        vgfx_internal_add_relative_delta(
                            win, (double)[event deltaX] * k, (double)[event deltaY] * k);
                    }

                    vgfx_internal_set_mouse_position(win, x, y);

                    vgfx_event_t vgfx_event = {
                        .type = VGFX_EVENT_MOUSE_MOVE,
                        .time_ms = timestamp,
                        .data.mouse_move = {
                            .x = x, .y = y, .modifiers = macos_modifiers([event modifierFlags])}};
                    vgfx_internal_enqueue_event(win, &vgfx_event);
                    break;
                }

                case NSEventTypeLeftMouseDown:
                case NSEventTypeRightMouseDown:
                case NSEventTypeOtherMouseDown: {
                    NSPoint location = [event locationInWindow];
                    NSRect contentRect = [platform->view bounds];

                    int32_t x, y;
                    macos_event_location_to_physical(win, location, contentRect, &x, &y);

                    vgfx_mouse_button_t button = VGFX_MOUSE_LEFT;
                    if (!macos_mouse_button_from_event(event, &button))
                        break;
                    vgfx_internal_set_mouse_button_state(win, (int32_t)button, 1);
                    vgfx_internal_set_mouse_position(win, x, y);

                    vgfx_event_t vgfx_event = {
                        .type = VGFX_EVENT_MOUSE_DOWN,
                        .time_ms = timestamp,
                        .data.mouse_button = {.x = x,
                                              .y = y,
                                              .button = button,
                                              .modifiers = macos_modifiers([event modifierFlags])}};
                    vgfx_internal_enqueue_event(win, &vgfx_event);
                    break;
                }

                case NSEventTypeLeftMouseUp:
                case NSEventTypeRightMouseUp:
                case NSEventTypeOtherMouseUp: {
                    NSPoint location = [event locationInWindow];
                    NSRect contentRect = [platform->view bounds];

                    int32_t x, y;
                    macos_event_location_to_physical(win, location, contentRect, &x, &y);

                    vgfx_mouse_button_t button = VGFX_MOUSE_LEFT;
                    if (!macos_mouse_button_from_event(event, &button))
                        break;
                    vgfx_internal_set_mouse_button_state(win, (int32_t)button, 0);
                    vgfx_internal_set_mouse_position(win, x, y);

                    vgfx_event_t vgfx_event = {
                        .type = VGFX_EVENT_MOUSE_UP,
                        .time_ms = timestamp,
                        .data.mouse_button = {.x = x,
                                              .y = y,
                                              .button = button,
                                              .modifiers = macos_modifiers([event modifierFlags])}};
                    vgfx_internal_enqueue_event(win, &vgfx_event);
                    break;
                }

                case NSEventTypeScrollWheel: {
                    NSPoint location = [event locationInWindow];
                    NSRect contentRect = [platform->view bounds];
                    float sf = win->scale_factor;
                    int32_t x, y;
                    macos_event_location_to_physical(win, location, contentRect, &x, &y);
                    vgfx_internal_set_mouse_position(win, x, y);

                    float dx, dy;
                    if ([event hasPreciseScrollingDeltas]) {
                        /* Trackpad: values are in points — convert to pixels */
                        dx = (float)[event scrollingDeltaX] * sf;
                        dy = -(float)[event scrollingDeltaY] * sf;
                    } else {
                        /* Traditional scroll wheel: deltaY is in lines (typically ±1/3) */
                        dx = (float)[event deltaX];
                        dy = -(float)[event deltaY];
                    }

                    vgfx_event_t vgfx_event = {
                        .type = VGFX_EVENT_SCROLL,
                        .time_ms = timestamp,
                        .data.scroll = {.delta_x = dx,
                                        .delta_y = dy,
                                        .x = x,
                                        .y = y,
                                        .modifiers = macos_modifiers([event modifierFlags])}};
                    vgfx_internal_enqueue_event(win, &vgfx_event);
                    break;
                }

                default:
                    /* Ignore remaining unhandled event types (gestures, etc.) */
                    break;
            }

            if (dispatch_key_after_zanna) {
                [NSApp sendEvent:event];
            }
        }

        return 1;
    }
}

/// @brief Present (blit) the framebuffer to the window.
/// @details Marks the VGFXView as needing display, then forces an immediate
///          redraw via displayIfNeeded.  This triggers the view's drawRect:
///          method, which converts the framebuffer to a CGImage and blits it
///          to the screen.
///
/// @param win Pointer to the ZannaGFX window structure
/// @return 1 on success, 0 on failure
///
/// @pre  win != NULL
/// @pre  win->pixels != NULL (framebuffer valid)
/// @pre  win->platform_data != NULL
/// @post Framebuffer contents visible on screen unless ZANNA_GFX_HIDE_WINDOWS is set
int vgfx_platform_present(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    if (win->skip_software_present)
        return 1;

    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;

        if (!platform->view)
            return 0;
        if (vgfx_macos_hide_windows()) {
            [platform->window orderOut:nil];
            return 1;
        }

        /* Mark view as needing display (queues redraw) */
        [platform->view setNeedsDisplay:YES];

        /* Force immediate redraw (calls drawRect: synchronously) */
        [platform->view displayIfNeeded];

        return 1;
    }
}

/// @brief Get high-resolution timestamp in milliseconds.
/// @details Uses mach_absolute_time() for a monotonic high-resolution timer.
///          The epoch is arbitrary but consistent within a process.  Converts
///          mach time units to nanoseconds using the timebase, then to
///          milliseconds.
///
/// @return Milliseconds since arbitrary epoch (monotonic, never decreases)
///
/// @details mach_absolute_time():
///            - Returns CPU time base units (hardware-specific)
///            - mach_timebase_info() provides numer/denom for conversion
///            - nanoseconds = mach_time * numer / denom
///            - milliseconds = nanoseconds / 1000000
int64_t vgfx_platform_now_ms(void) {
    pthread_once(&g_vgfx_macos_timebase_once, vgfx_macos_init_timebase);

    uint64_t now = mach_absolute_time(); /* Get current time in mach units */
    double ms = ((double)now * (double)g_vgfx_macos_timebase.numer) /
                ((double)g_vgfx_macos_timebase.denom * 1000000.0);
    if (ms >= (double)INT64_MAX)
        return INT64_MAX;
    return (int64_t)ms;
}

/// @brief Sleep for the specified duration in milliseconds.
/// @details Uses nanosleep() for sub-second precision.  If ms <= 0, returns
///          immediately without sleeping.  Used by vgfx_update() for frame
///          rate limiting.
///
/// @param ms Duration to sleep in milliseconds
///
/// @post Thread sleeps for approximately ms milliseconds (may be longer)
void vgfx_platform_sleep_ms(int32_t ms) {
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;              /* Whole seconds */
        ts.tv_nsec = (ms % 1000) * 1000000; /* Fractional seconds in nanoseconds */
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        }
    }
}

/// @brief Yield the calling thread to the Darwin scheduler.
/// @details Used for short internal spin waits without advancing frame pacing
///          timers or sleeping for a full millisecond.
void vgfx_platform_yield(void) {
    sched_yield();
}

//===----------------------------------------------------------------------===//
// Clipboard Operations
//===----------------------------------------------------------------------===//

/// @brief Check if the clipboard contains data in the specified format.
/// @param format Clipboard format to check for
/// @return 1 if data is available, 0 otherwise
int vgfx_clipboard_has_format(vgfx_clipboard_format_t format) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

        switch (format) {
            case VGFX_CLIPBOARD_TEXT:
                return [pasteboard availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil ? 1
                                                                                              : 0;
            case VGFX_CLIPBOARD_HTML:
                return [pasteboard availableTypeFromArray:@[ NSPasteboardTypeHTML ]] != nil ? 1 : 0;
            case VGFX_CLIPBOARD_IMAGE:
                return [pasteboard
                           availableTypeFromArray:@[ NSPasteboardTypePNG, NSPasteboardTypeTIFF ]] !=
                               nil
                           ? 1
                           : 0;
            case VGFX_CLIPBOARD_FILES:
                return [pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL ]] != nil ? 1
                                                                                               : 0;
            default:
                return 0;
        }
    }
}

/// @brief Get text from the clipboard.
/// @details Returns a malloc'd UTF-8 string containing the clipboard text.
///          The caller is responsible for freeing the returned string.
/// @return Clipboard text (caller must free), or NULL if not available
char *vgfx_clipboard_get_text(void) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSString *string = [pasteboard stringForType:NSPasteboardTypeString];

        if (!string)
            return NULL;

        const char *utf8 = [string UTF8String];
        if (!utf8)
            return NULL;

        return vgfx_macos_strdup(utf8);
    }
}

/// @brief Set text to the clipboard.
/// @details Copies the specified UTF-8 string to the system clipboard.
/// @param text Text to copy (NULL clears text from clipboard)
void vgfx_clipboard_set_text(const char *text) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];

        if (text) {
            NSString *string = vgfx_macos_string_or_empty(text);
            if (string) {
                [pasteboard setString:string forType:NSPasteboardTypeString];
            }
        }
    }
}

/// @brief Clear all clipboard contents.
void vgfx_clipboard_clear(void) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
    }
}

//===----------------------------------------------------------------------===//
// Window Title and Fullscreen
//===----------------------------------------------------------------------===//

/// @brief Set the window title.
/// @details Updates the NSWindow's title bar text.
///
/// @param win   Pointer to the window structure
/// @param title New title string (UTF-8)
void vgfx_platform_set_title(struct vgfx_window *win, const char *title) {
    if (!win || !win->platform_data || !title)
        return;

    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;

        if (platform->window) {
            [platform->window setTitle:vgfx_macos_string_or_empty(title)];
        }

        vgfx_platform_macos_ensure_default_main_menu(title);
    }
}

/// @brief Set the window to fullscreen or windowed mode.
/// @details Toggles the NSWindow between fullscreen and windowed modes using
///          macOS's native fullscreen API (toggleFullScreen:).
///
/// @param win        Pointer to the window structure
/// @param fullscreen 1 for fullscreen, 0 for windowed
/// @return 1 on success, 0 on failure
int vgfx_platform_set_fullscreen(struct vgfx_window *win, int fullscreen) {
    if (!win || !win->platform_data)
        return 0;

    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;

        if (!platform->window)
            return 0;

        /* Check current fullscreen state */
        BOOL is_currently_fullscreen =
            ([platform->window styleMask] & NSWindowStyleMaskFullScreen) != 0;

        /* Only toggle if state needs to change */
        if (fullscreen && !is_currently_fullscreen) {
            [platform->window setCollectionBehavior:([platform->window collectionBehavior] |
                                                     NSWindowCollectionBehaviorFullScreenPrimary)];
            if (([platform->window styleMask] & NSWindowStyleMaskResizable) == 0) {
                [platform->window
                    setStyleMask:([platform->window styleMask] | NSWindowStyleMaskResizable)];
                platform->fullscreen_resizable_added = 1;
            }
            [platform->window makeKeyAndOrderFront:nil];
            if (!vgfx_macos_no_activate_on_create())
                [NSApp activateIgnoringOtherApps:YES];
            [platform->window toggleFullScreen:nil];
        } else if (!fullscreen && is_currently_fullscreen) {
            [platform->window toggleFullScreen:nil];
        } else if (!fullscreen && platform->fullscreen_resizable_added && !win->resizable) {
            [platform->window
                setStyleMask:([platform->window styleMask] & ~NSWindowStyleMaskResizable)];
            platform->fullscreen_resizable_added = 0;
        }

        return 1;
    }
}

/// @brief Check if the window is in fullscreen mode.
/// @param win Pointer to the window structure
/// @return 1 if fullscreen, 0 if windowed
int vgfx_platform_is_fullscreen(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;

    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;

        if (!platform->window)
            return 0;

        return ([platform->window styleMask] & NSWindowStyleMaskFullScreen) != 0 ? 1 : 0;
    }
}

void vgfx_platform_minimize(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (platform->window)
            [platform->window miniaturize:nil];
    }
}

void vgfx_platform_maximize(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (platform->window && ![platform->window isZoomed])
            [platform->window zoom:nil];
    }
}

void vgfx_platform_restore(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (!platform->window)
            return;
        if ([platform->window isMiniaturized])
            [platform->window deminiaturize:nil];
        else if ([platform->window isZoomed])
            [platform->window zoom:nil];
    }
}

int32_t vgfx_platform_is_minimized(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        return (platform->window && [platform->window isMiniaturized]) ? 1 : 0;
    }
}

int32_t vgfx_platform_is_maximized(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        return (platform->window && [platform->window isZoomed]) ? 1 : 0;
    }
}

void vgfx_platform_get_position(struct vgfx_window *win, int32_t *out_x, int32_t *out_y) {
    if (!win || !win->platform_data) {
        if (out_x)
            *out_x = 0;
        if (out_y)
            *out_y = 0;
        return;
    }
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (!platform->window)
            return;
        NSRect frame = [platform->window frame];
        NSRect screen_frame = macos_screen_frame_for_window(platform->window);
        if (out_x)
            *out_x = (int32_t)frame.origin.x;
        if (out_y) {
            CGFloat top_y = NSMaxY(screen_frame) - NSMaxY(frame);
            *out_y = (int32_t)top_y;
        }
    }
}

void vgfx_platform_set_position(struct vgfx_window *win, int32_t x, int32_t y) {
    if (!win || !win->platform_data)
        return;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (platform->window) {
            NSRect frame = [platform->window frame];
            NSRect screen_frame = macos_screen_frame_for_window(platform->window);
            CGFloat cocoa_y = NSMaxY(screen_frame) - (CGFloat)y - frame.size.height;
            [platform->window setFrameOrigin:NSMakePoint((CGFloat)x, cocoa_y)];
        }
    }
}

void vgfx_platform_focus(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_platform_request_foreground(win);
}

void vgfx_platform_request_foreground(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (platform->window) {
            if (vgfx_macos_hide_windows()) {
                [platform->window orderOut:nil];
                vgfx_internal_set_focus_state(win, 0);
                return;
            }
            if (vgfx_macos_no_activate_on_create()) {
                [platform->window orderBack:nil];
                vgfx_internal_set_focus_state(win, 0);
                return;
            }
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            vgfx_platform_macos_finish_launching_if_needed();
            NSString *window_title = [platform->window title];
            const char *preferred_title =
                window_title && [window_title length] > 0 ? [window_title UTF8String] : NULL;
            vgfx_platform_macos_ensure_default_main_menu(preferred_title);
            [platform->window makeKeyAndOrderFront:nil];
            [platform->window makeMainWindow];
            [platform->window orderFrontRegardless];
            [NSApp activateIgnoringOtherApps:YES];
        }
    }
}

int32_t vgfx_platform_is_focused(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        return (platform->window && [platform->window isKeyWindow]) ? 1 : 0;
    }
}

void vgfx_platform_set_prevent_close(struct vgfx_window *win, int32_t prevent) {
    vgfx_internal_set_prevent_close(win, prevent);
}

void vgfx_platform_set_cursor(struct vgfx_window *win, int32_t cursor_type) {
    @autoreleasepool {
        if (!win || !win->platform_data)
            return;
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        platform->cursor_type = (cursor_type >= 0 && cursor_type < 6) ? cursor_type : 0;
        if (platform->window && platform->view)
            [platform->window invalidateCursorRectsForView:platform->view];
        [macos_cursor_for_type(platform->cursor_type) set];
    }
}

void vgfx_platform_set_cursor_visible(struct vgfx_window *win, int32_t visible) {
    (void)win;
    @autoreleasepool {
        if (visible)
            vgfx_platform_show_cursor();
        else
            vgfx_platform_hide_cursor();
    }
}

void vgfx_platform_get_monitor_size(struct vgfx_window *win, int32_t *out_w, int32_t *out_h) {
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;
    @autoreleasepool {
        NSScreen *screen = nil;
        if (win && win->platform_data) {
            vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
            screen = platform->window ? [platform->window screen] : nil;
        }
        if (!screen)
            screen = [NSScreen mainScreen];
        NSRect frame = [screen convertRectToBacking:[screen frame]];
        if (out_w)
            *out_w = vgfx_internal_round_scaled((float)frame.size.width);
        if (out_h)
            *out_h = vgfx_internal_round_scaled((float)frame.size.height);
    }
}

void vgfx_platform_set_window_size(struct vgfx_window *win, int32_t w, int32_t h) {
    if (!win || !win->platform_data)
        return;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (!platform->window)
            return;
        [platform->window setContentSize:NSMakeSize((CGFloat)w, (CGFloat)h)];
        macos_sync_window_metrics(win, 1, 1);
    }
}

void vgfx_platform_set_window_min_size(struct vgfx_window *win, int32_t w, int32_t h) {
    if (!win || !win->platform_data || w <= 0 || h <= 0)
        return;
    @autoreleasepool {
        vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
        if (!platform->window)
            return;
        [platform->window setContentMinSize:NSMakeSize((CGFloat)w, (CGFloat)h)];
    }
}

void *vgfx_get_native_display(vgfx_window_t window) {
    (void)window;
    return NULL; /* macOS doesn't have a separate display connection */
}

void *vgfx_get_native_view(vgfx_window_t window) {
    if (!window)
        return NULL;
    vgfx_macos_platform *platform = (vgfx_macos_platform *)window->platform_data;
    if (!platform)
        return NULL;
    return (__bridge void *)platform->view;
}

int vgfx_get_native_handles(vgfx_window_t window, vgfx_native_handles_t *out_handles) {
    if (!window || !window->platform_data || !out_handles)
        return 0;
    vgfx_macos_platform *platform = (vgfx_macos_platform *)window->platform_data;
    *out_handles = (vgfx_native_handles_t){.backend = VGFX_NATIVE_BACKEND_COCOA,
                                           .display = NULL,
                                           .surface = (__bridge void *)platform->view,
                                           .window = 0};
    return 1;
}

vgfx_window_capabilities_t vgfx_get_window_capabilities(vgfx_window_t window) {
    if (!window || !window->platform_data)
        return 0;
    return VGFX_CAP_WINDOW_POSITION | VGFX_CAP_FOCUS_REQUEST | VGFX_CAP_CURSOR_WARP |
           VGFX_CAP_RELATIVE_MOUSE | VGFX_CAP_TEXT_COMPOSITION | VGFX_CAP_SERVER_DECORATIONS |
           VGFX_CAP_ACTIVATION | VGFX_CAP_CLIPBOARD_TEXT | VGFX_CAP_FILE_DROP;
}

void vgfx_platform_warp_cursor(struct vgfx_window *win, int32_t x, int32_t y) {
    if (!win || !win->platform_data)
        return;
    vgfx_macos_platform *platform = (vgfx_macos_platform *)win->platform_data;
    if (!platform->window)
        return;

    /* Only warp if our window is the key window (has focus) */
    if (![platform->window isKeyWindow])
        return;

    NSScreen *screen = [platform->window screen];
    if (!screen)
        screen = [NSScreen mainScreen];

    NSRect frame = [platform->window frame];
    NSRect content = [platform->window contentRectForFrameRect:frame];
    NSRect screen_frame = [screen frame];
    CGDirectDisplayID display_id = macos_display_id_for_screen(screen);

    CGFloat display_scale = win->scale_factor > 0.0f ? (CGFloat)win->scale_factor : 1.0;
    CGFloat coord_scale = vgfx_internal_coord_scale(win);
    CGFloat physical_x = (CGFloat)vgfx_internal_scale_up_i32(x, coord_scale);
    CGFloat physical_y = (CGFloat)vgfx_internal_scale_up_i32(y, coord_scale);
    CGFloat point_x = physical_x / display_scale;
    CGFloat point_y = physical_y / display_scale;

    CGFloat local_x_points = (content.origin.x - screen_frame.origin.x) + point_x;
    CGFloat local_y_points_from_bottom =
        (content.origin.y - screen_frame.origin.y) + (content.size.height - point_y);

    /* Cursor positions use the same point-space coordinate system as NSWindow
     * geometry. Multiplying by the Retina backing scale moves the pointer
     * toward the bottom-right on HiDPI displays even though drawing uses
     * physical backing pixels. */
    CGPoint display_point = CGPointMake(
        vgfx_internal_round_scaled((float)local_x_points),
        vgfx_internal_round_scaled((float)(screen_frame.size.height - local_y_points_from_bottom)));

    CGDisplayMoveCursorToPoint(display_id, display_point);
    CGAssociateMouseAndMouseCursorPosition(true);
}

int vgfx_platform_set_relative_mouse(struct vgfx_window *win, int enabled) {
    (void)win;
    /* Decouple the hardware mouse from the on-screen cursor. While
     * dissociated the cursor stays parked (no edge pinning) and NSEvent
     * mouse-moved events continue reporting deltaX/deltaY, which the event
     * pump accumulates into the window's relative delta accumulators. */
    CGAssociateMouseAndMouseCursorPosition(enabled ? false : true);
    return 1;
}

int vgfx_platform_set_text_input_enabled(struct vgfx_window *win, int32_t enabled) {
    (void)enabled;
    return win != NULL;
}

int vgfx_platform_set_text_input_state(struct vgfx_window *win,
                                       const vgfx_text_input_state_t *state) {
    return win && state;
}

void vgfx_platform_hide_cursor(void) {
    if (!g_vgfx_macos_cursor_hidden) {
        [NSCursor hide];
        g_vgfx_macos_cursor_hidden = 1;
    }
}

void vgfx_platform_show_cursor(void) {
    if (g_vgfx_macos_cursor_hidden) {
        [NSCursor unhide];
        g_vgfx_macos_cursor_hidden = 0;
    }
}

#endif /* __APPLE__ */

//===----------------------------------------------------------------------===//
// End of macOS Backend
//===----------------------------------------------------------------------===//
