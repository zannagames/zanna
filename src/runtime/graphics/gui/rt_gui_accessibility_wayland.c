//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_accessibility_wayland.c
/// @brief Implements Wayland-safe desktop preferences and AT-SPI accessibility forwarding.
///
/// @details
/// This adapter reads process environment hints and the desktop settings portal
/// without opening an X11 connection. Semantic-tree lifecycle, changes, and
/// live-region announcements are forwarded to the shared Linux AT-SPI bridge;
/// all native and toolkit handles remain borrowed.
///
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_accessibility_wayland.c
// Purpose: Display-neutral preference fallback for native Wayland GUI sessions.
// Key invariants:
//   - Queries never connect to X11 or initialize a second display stack.
//   - Missing environment/portal state returns the stable zero fallback.
// Ownership/Lifetime: No window or widget is retained; all arguments are borrowed.
// Links: src/runtime/graphics/gui/rt_gui_accessibility_platform.h,
//        docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#include "rt_gui_accessibility_platform.h"
#include "rt_gui_atspi_linux.h"
#include "rt_gui_linux_portal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/// @brief Search text for an ASCII substring without regard to letter case.
/// @param text NUL-terminated text to inspect; may be NULL.
/// @param needle Non-empty NUL-terminated substring; may be NULL.
/// @return 1 when @p needle occurs in @p text, otherwise 0.
static int rt_gui_wayland_contains_ascii(const char *text, const char *needle) {
    if (!text || !needle || !needle[0])
        return 0;
    size_t needle_length = strlen(needle);
    for (; *text; ++text) {
        size_t matched = 0;
        while (matched < needle_length && text[matched] &&
               (unsigned char)tolower((unsigned char)text[matched]) ==
                   (unsigned char)tolower((unsigned char)needle[matched])) {
            ++matched;
        }
        if (matched == needle_length)
            return 1;
    }
    return 0;
}

/// @brief Query high contrast from toolkit environment hints and the desktop portal.
/// @param window Borrowed Wayland window; unused because the settings are process-wide.
/// @return 1 when an available desktop hint requests high contrast, otherwise 0.
int32_t rt_gui_accessibility_platform_high_contrast(vgfx_window_t window) {
    (void)window;
    if (rt_gui_wayland_contains_ascii(getenv("GTK_THEME"), "contrast") ||
        rt_gui_wayland_contains_ascii(getenv("QT_STYLE_OVERRIDE"), "contrast"))
        return 1;
    int32_t contrast = 0;
    return rt_gui_linux_portal_read("org.freedesktop.appearance", "contrast", &contrast) &&
                   contrast == 1
               ? 1
               : 0;
}

/// @brief Query reduced motion from GTK's animation hint and the desktop portal.
/// @param window Borrowed Wayland window; unused because the settings are process-wide.
/// @return 1 when interface animations are explicitly disabled, otherwise 0.
int32_t rt_gui_accessibility_platform_reduced_motion(vgfx_window_t window) {
    (void)window;
    const char *value = getenv("GTK_ENABLE_ANIMATIONS");
    if (value && (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
                  strcmp(value, "FALSE") == 0))
        return 1;
    int32_t animations = 1;
    return rt_gui_linux_portal_read(
               "org.freedesktop.desktop.interface", "enable-animations", &animations) &&
                   animations == 0
               ? 1
               : 0;
}

/// @brief Query dark appearance from toolkit environment hints and the desktop portal.
/// @param window Borrowed Wayland window; unused because the settings are process-wide.
/// @return 1 when an available desktop hint requests dark appearance, otherwise 0.
int32_t rt_gui_accessibility_platform_prefers_dark(vgfx_window_t window) {
    (void)window;
    if (rt_gui_wayland_contains_ascii(getenv("GTK_THEME"), "dark") ||
        rt_gui_wayland_contains_ascii(getenv("QT_STYLE_OVERRIDE"), "dark"))
        return 1;
    int32_t color_scheme = 0;
    return rt_gui_linux_portal_read(
               "org.freedesktop.appearance", "color-scheme", &color_scheme) &&
                   color_scheme == 1
               ? 1
               : 0;
}

/// @brief Attach the shared Linux AT-SPI projection to a Wayland semantic tree.
/// @param window Borrowed Wayland window associated with the tree.
/// @param root Borrowed semantic root to project.
void rt_gui_accessibility_platform_attach(vgfx_window_t window, vg_widget_t *root) {
    rt_gui_atspi_linux_attach(window, root);
}

/// @brief Detach the shared Linux AT-SPI projection for a Wayland window.
/// @param window Borrowed Wayland window whose projection is removed.
void rt_gui_accessibility_platform_detach(vgfx_window_t window) {
    rt_gui_atspi_linux_detach(window);
}

/// @brief Forward a semantic-node change to the shared Linux AT-SPI projection.
/// @param window Borrowed Wayland window associated with the tree.
/// @param widget Borrowed semantic widget whose exported state changed.
void rt_gui_accessibility_platform_notify(vgfx_window_t window, vg_widget_t *widget) {
    rt_gui_atspi_linux_notify(window, widget);
}

/// @brief Reconcile the shared Linux AT-SPI projection with the semantic tree.
/// @param window Borrowed Wayland window associated with the tree.
/// @param root Borrowed semantic root to project.
void rt_gui_accessibility_platform_sync(vgfx_window_t window, vg_widget_t *root) {
    rt_gui_atspi_linux_sync(window, root);
}

/// @brief Forward a live-region announcement to the shared Linux AT-SPI projection.
/// @param window Borrowed Wayland window associated with the tree.
/// @param widget Borrowed semantic widget that originated the announcement.
/// @param text Borrowed UTF-8 announcement text.
/// @param mode Requested live-region urgency.
void rt_gui_accessibility_platform_announce(vgfx_window_t window,
                                            vg_widget_t *widget,
                                            const char *text,
                                            vg_live_region_mode_t mode) {
    rt_gui_atspi_linux_announce(window, widget, text, mode);
}
