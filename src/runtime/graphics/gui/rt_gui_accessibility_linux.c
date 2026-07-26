//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_accessibility_linux.c
/// @brief Implements Linux desktop accessibility preferences and AT-SPI projection hooks.
///
/// @details
/// Preference queries combine environment overrides, the desktop portal, and
/// bounded XSettings parsing while retaining deterministic fallbacks. Semantic
/// tree lifecycle and announcement events are forwarded to the optional AT-SPI
/// adapter without transferring ownership of toolkit objects or native handles.
///
// File: src/runtime/graphics/gui/rt_gui_accessibility_linux.c
// Purpose: X11/XSettings accessibility-preference adapter for the Zanna GUI runtime.
//
// Key invariants:
//   - XSettings payloads are parsed with explicit byte order and strict bounds checks.
//   - At most 64 KiB is read from the X server for a preference query.
//   - Malformed settings, absent XSettings owners, and unavailable displays fall back to zero.
//
// Ownership/Lifetime:
//   - X11 Display and Window handles are borrowed from ZannaGFX.
//   - XGetWindowProperty buffers are always released with XFree before return.
//
// Links: src/runtime/graphics/gui/rt_gui_accessibility_platform.h,
//        src/lib/graphics/src/vgfx_platform_linux.c
//
//===----------------------------------------------------------------------===//

#include "rt_gui_accessibility_platform.h"
#include "rt_gui_atspi_linux.h"
#include "rt_gui_linux_portal.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RT_GUI_XSETTINGS_MAX_BYTES = 64 * 1024,
    RT_GUI_XSETTING_INTEGER = 0,
    RT_GUI_XSETTING_STRING = 1,
    RT_GUI_XSETTING_COLOR = 2,
};

typedef struct rt_gui_xsetting_value {
    int found;        ///< Non-zero when the requested setting was decoded.
    int32_t integer;  ///< Integer value for type-zero settings.
    char string[128]; ///< Truncated NUL-terminated type-one setting value.
} rt_gui_xsetting_value_t;

/// @brief Read a byte-order-aware unsigned 16-bit integer from an XSettings payload.
/// @param bytes Payload start; must contain at least two bytes.
/// @param msb_first Non-zero when the payload uses most-significant-byte-first encoding.
/// @return Decoded unsigned value.
static uint16_t rt_gui_xsettings_u16(const unsigned char *bytes, int msb_first) {
    if (msb_first)
        return (uint16_t)(((uint16_t)bytes[0] << 8u) | bytes[1]);
    return (uint16_t)(((uint16_t)bytes[1] << 8u) | bytes[0]);
}

/// @brief Read a byte-order-aware unsigned 32-bit integer from an XSettings payload.
/// @param bytes Payload start; must contain at least four bytes.
/// @param msb_first Non-zero when the payload uses most-significant-byte-first encoding.
/// @return Decoded unsigned value.
static uint32_t rt_gui_xsettings_u32(const unsigned char *bytes, int msb_first) {
    if (msb_first) {
        return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
               ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
    }
    return ((uint32_t)bytes[3] << 24u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[1] << 8u) |
           (uint32_t)bytes[0];
}

/// @brief Round one payload offset up to the four-byte XSettings alignment.
/// @param offset Unaligned byte offset.
/// @return Aligned offset, or SIZE_MAX when addition would overflow.
static size_t rt_gui_xsettings_align4(size_t offset) {
    return offset > SIZE_MAX - 3u ? SIZE_MAX : (offset + 3u) & ~(size_t)3u;
}

/// @brief Compare one non-NUL XSettings name with an ordinary C string.
/// @param bytes Name bytes in the payload.
/// @param length Exact payload name length.
/// @param expected NUL-terminated setting name.
/// @return Non-zero only for byte-exact equality.
static int rt_gui_xsettings_name_equals(const unsigned char *bytes,
                                        size_t length,
                                        const char *expected) {
    return expected && strlen(expected) == length && memcmp(bytes, expected, length) == 0;
}

/// @brief Parse one named integer or string from a bounded XSettings payload.
/// @details Unknown setting types are skipped according to the XSettings protocol. Every offset
///          and value length is checked before access, and a malformed record terminates parsing.
/// @param bytes Complete property payload.
/// @param length Payload size in bytes.
/// @param requested Exact setting name to locate.
/// @return Decoded value record with `found=0` when absent or malformed.
static rt_gui_xsetting_value_t rt_gui_xsettings_parse(const unsigned char *bytes,
                                                      size_t length,
                                                      const char *requested) {
    rt_gui_xsetting_value_t result = {0};
    if (!bytes || length < 12u || !requested)
        return result;
    if (bytes[0] != LSBFirst && bytes[0] != MSBFirst)
        return result;
    int msb_first = bytes[0] == MSBFirst;
    uint32_t setting_count = rt_gui_xsettings_u32(bytes + 8u, msb_first);
    size_t offset = 12u;
    for (uint32_t index = 0; index < setting_count && offset <= length; ++index) {
        if (length - offset < 4u)
            return result;
        unsigned type = bytes[offset];
        size_t name_length = rt_gui_xsettings_u16(bytes + offset + 2u, msb_first);
        offset += 4u;
        if (name_length > length - offset)
            return result;
        const unsigned char *name = bytes + offset;
        int matches = rt_gui_xsettings_name_equals(name, name_length, requested);
        offset = rt_gui_xsettings_align4(offset + name_length);
        if (offset == SIZE_MAX || offset > length || length - offset < 4u)
            return result;
        offset += 4u; /* last-change serial */

        if (type == RT_GUI_XSETTING_INTEGER) {
            if (length - offset < 4u)
                return result;
            if (matches) {
                result.found = 1;
                result.integer = (int32_t)rt_gui_xsettings_u32(bytes + offset, msb_first);
                return result;
            }
            offset += 4u;
        } else if (type == RT_GUI_XSETTING_STRING) {
            if (length - offset < 4u)
                return result;
            size_t value_length = rt_gui_xsettings_u32(bytes + offset, msb_first);
            offset += 4u;
            if (value_length > length - offset)
                return result;
            if (matches) {
                size_t copy_length = value_length < sizeof(result.string) - 1u
                                         ? value_length
                                         : sizeof(result.string) - 1u;
                memcpy(result.string, bytes + offset, copy_length);
                result.string[copy_length] = '\0';
                result.found = 1;
                return result;
            }
            offset = rt_gui_xsettings_align4(offset + value_length);
            if (offset == SIZE_MAX || offset > length)
                return result;
        } else if (type == RT_GUI_XSETTING_COLOR) {
            if (length - offset < 8u)
                return result;
            offset += 8u;
        } else {
            return result;
        }
    }
    return result;
}

/// @brief Decode an integer XSetting from a supplied payload for parser conformance checks.
/// @param bytes Complete XSettings property payload.
/// @param length Payload size in bytes.
/// @param requested Exact setting name to locate.
/// @param integer_out Optional destination for the decoded integer value.
/// @return 1 when the requested setting was found, otherwise 0.
int rt_gui_linux_test_parse_xsetting(const unsigned char *bytes,
                                     size_t length,
                                     const char *requested,
                                     int32_t *integer_out) {
    rt_gui_xsetting_value_t value = rt_gui_xsettings_parse(bytes, length, requested);
    if (!value.found)
        return 0;
    if (integer_out)
        *integer_out = value.integer;
    return 1;
}

/// @brief Fetch and parse one setting from the XSettings manager on the window's screen.
/// @param window Borrowed ZannaGFX window supplying the shared X11 Display connection.
/// @param name Exact XSettings key.
/// @return Decoded setting record with `found=0` when XSettings is unavailable.
static rt_gui_xsetting_value_t rt_gui_xsettings_query(vgfx_window_t window, const char *name) {
    rt_gui_xsetting_value_t result = {0};
    vgfx_native_handles_t handles = {0};
    if (!vgfx_get_native_handles(window, &handles) || handles.backend != VGFX_NATIVE_BACKEND_X11)
        return result;
    Display *display = (Display *)handles.display;
    if (!display || !name)
        return result;

    char selection_name[32];
    int selection_length =
        snprintf(selection_name, sizeof(selection_name), "_XSETTINGS_S%d", DefaultScreen(display));
    if (selection_length <= 0 || (size_t)selection_length >= sizeof(selection_name))
        return result;
    Atom selection = XInternAtom(display, selection_name, True);
    Atom property = XInternAtom(display, "_XSETTINGS_SETTINGS", True);
    if (selection == None || property == None)
        return result;
    Window owner = XGetSelectionOwner(display, selection);
    if (owner == None)
        return result;

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *bytes = NULL;
    int status = XGetWindowProperty(display,
                                    owner,
                                    property,
                                    0,
                                    RT_GUI_XSETTINGS_MAX_BYTES / 4,
                                    False,
                                    AnyPropertyType,
                                    &actual_type,
                                    &actual_format,
                                    &item_count,
                                    &bytes_after,
                                    &bytes);
    if (status == Success && actual_type != None && actual_format == 8 && bytes_after == 0 &&
        item_count <= RT_GUI_XSETTINGS_MAX_BYTES) {
        result = rt_gui_xsettings_parse(bytes, (size_t)item_count, name);
    }
    if (bytes)
        XFree(bytes);
    return result;
}

/// @brief Test whether text contains `contrast` while ignoring ASCII case and separators.
/// @param text NUL-terminated desktop theme name; may be NULL.
/// @return Non-zero when the normalized text contains the word contrast.
static int rt_gui_theme_requests_high_contrast(const char *text) {
    static const char needle[] = "contrast";
    if (!text)
        return 0;
    size_t matched = 0;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor) {
        unsigned char ch = (unsigned char)tolower(*cursor);
        if (ch == (unsigned char)needle[matched]) {
            if (++matched == sizeof(needle) - 1u)
                return 1;
        } else {
            matched = ch == (unsigned char)needle[0] ? 1u : 0u;
        }
    }
    return 0;
}

/// @brief Test whether desktop-theme text contains `dark` case-insensitively.
/// @details GTK and Qt theme names conventionally append or include `dark`; separators and other
///          punctuation are ignored while matching. NULL and empty input return zero.
/// @param text Borrowed desktop-theme name or style override.
/// @return One when the normalized text contains `dark`, otherwise zero.
static int rt_gui_theme_requests_dark(const char *text) {
    static const char needle[] = "dark";
    if (!text)
        return 0;
    size_t matched = 0;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor) {
        unsigned char ch = (unsigned char)tolower(*cursor);
        if (ch == (unsigned char)needle[matched]) {
            ++matched;
            if (matched == sizeof(needle) - 1u)
                return 1;
        } else {
            matched = ch == (unsigned char)needle[0] ? 1u : 0u;
        }
    }
    return 0;
}

/// @brief Query high contrast from toolkit environment hints and XSettings theme state.
/// @param window Borrowed ZannaGFX window supplying an X11 display when available.
/// @return One when a high-contrast desktop theme is selected, otherwise zero.
int32_t rt_gui_accessibility_platform_high_contrast(vgfx_window_t window) {
    const char *gtk_theme = getenv("GTK_THEME");
    const char *qt_theme = getenv("QT_STYLE_OVERRIDE");
    if (rt_gui_theme_requests_high_contrast(gtk_theme) ||
        rt_gui_theme_requests_high_contrast(qt_theme)) {
        return 1;
    }
    int32_t contrast = 0;
    if (rt_gui_linux_portal_read("org.freedesktop.appearance", "contrast", &contrast))
        return contrast == 1 ? 1 : 0;
    rt_gui_xsetting_value_t theme = rt_gui_xsettings_query(window, "Net/ThemeName");
    return theme.found && rt_gui_theme_requests_high_contrast(theme.string) ? 1 : 0;
}

/// @brief Query reduced motion from GTK's animation hint and XSettings animation state.
/// @param window Borrowed ZannaGFX window supplying an X11 display when available.
/// @return One when desktop interface animations are explicitly disabled, otherwise zero.
int32_t rt_gui_accessibility_platform_reduced_motion(vgfx_window_t window) {
    const char *gtk_animations = getenv("GTK_ENABLE_ANIMATIONS");
    if (gtk_animations &&
        (strcmp(gtk_animations, "0") == 0 || strcmp(gtk_animations, "false") == 0 ||
         strcmp(gtk_animations, "FALSE") == 0)) {
        return 1;
    }
    int32_t portal_animations = 1;
    if (rt_gui_linux_portal_read(
            "org.freedesktop.desktop.interface", "enable-animations", &portal_animations))
        return portal_animations == 0 ? 1 : 0;
    rt_gui_xsetting_value_t animations = rt_gui_xsettings_query(window, "Gtk/EnableAnimations");
    if (!animations.found)
        animations = rt_gui_xsettings_query(window, "Net/EnableAnimations");
    return animations.found && animations.integer == 0 ? 1 : 0;
}

/// @brief Query common Linux desktop theme hints for a dark application preference.
/// @details GTK and Qt environment overrides win, followed by XSettings' `Net/ThemeName`. As a
///          terminal-compatible fallback, `COLORFGBG` values ending in ANSI colors 0-6 or 8 are
///          treated as dark. Missing/malformed hints return the stable light fallback.
/// @param window Borrowed ZannaGFX window supplying an X11 display when available.
/// @return One when an available desktop hint requests dark mode, otherwise zero.
int32_t rt_gui_accessibility_platform_prefers_dark(vgfx_window_t window) {
    const char *gtk_theme = getenv("GTK_THEME");
    const char *qt_theme = getenv("QT_STYLE_OVERRIDE");
    if (rt_gui_theme_requests_dark(gtk_theme) || rt_gui_theme_requests_dark(qt_theme))
        return 1;

    int32_t color_scheme = 0;
    if (rt_gui_linux_portal_read(
            "org.freedesktop.appearance", "color-scheme", &color_scheme))
        return color_scheme == 1 ? 1 : 0;

    rt_gui_xsetting_value_t theme = rt_gui_xsettings_query(window, "Net/ThemeName");
    if (theme.found)
        return rt_gui_theme_requests_dark(theme.string) ? 1 : 0;

    const char *colorfgbg = getenv("COLORFGBG");
    if (colorfgbg) {
        const char *last = strrchr(colorfgbg, ';');
        const char *number = last ? last + 1 : colorfgbg;
        char *end = NULL;
        long background = strtol(number, &end, 10);
        if (end != number && *end == '\0')
            return (background >= 0 && background <= 6) || background == 8 ? 1 : 0;
    }
    return 0;
}

/// @brief Attach the optional AT-SPI projection to a window's semantic tree.
/// @param window Borrowed native window associated with the tree.
/// @param root Borrowed semantic root to project.
void rt_gui_accessibility_platform_attach(vgfx_window_t window, vg_widget_t *root) {
    rt_gui_atspi_linux_attach(window, root);
}

/// @brief Detach the optional Linux native accessibility projection.
/// @param window Borrowed native window whose AT-SPI projection is removed.
void rt_gui_accessibility_platform_detach(vgfx_window_t window) {
    rt_gui_atspi_linux_detach(window);
}

/// @brief Notify the optional Linux native accessibility projection of a changed node.
/// @param window Borrowed native window associated with the tree.
/// @param widget Borrowed semantic widget whose exported state changed.
void rt_gui_accessibility_platform_notify(vgfx_window_t window, vg_widget_t *widget) {
    rt_gui_atspi_linux_notify(window, widget);
}

/// @brief Reconcile the optional Linux native accessibility projection with the semantic tree.
/// @param window Borrowed native window associated with the tree.
/// @param root Borrowed semantic root to project.
void rt_gui_accessibility_platform_sync(vgfx_window_t window, vg_widget_t *root) {
    rt_gui_atspi_linux_sync(window, root);
}

/// @brief Project a live-region announcement when a Linux native bridge is installed.
/// @param window Borrowed native window associated with the tree.
/// @param widget Borrowed semantic widget that originated the announcement.
/// @param text Borrowed UTF-8 announcement text.
/// @param mode Requested live-region urgency.
void rt_gui_accessibility_platform_announce(vgfx_window_t window,
                                            vg_widget_t *widget,
                                            const char *text,
                                            vg_live_region_mode_t mode) {
    rt_gui_atspi_linux_announce(window, widget, text, mode);
}
