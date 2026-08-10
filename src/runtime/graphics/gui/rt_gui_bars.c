//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_bars.c
/// @brief Implements runtime bindings for status bars, toolbars, and their item handles.
///
/// @details
/// The graphics-enabled path validates wrapper ownership, converts runtime
/// strings and pixels at the ABI boundary, and forwards retained bar mutations
/// to ZannaGUI. Graphics-disabled definitions preserve the same API with
/// deterministic empty values and no side effects.
///
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_bars.c
// Purpose: Status-bar and tool-bar GUI widgets (and their items) for the Zanna
//          runtime. Split out of rt_gui_menus.c; shares widget types and the
//          status-bar/tool-bar cast + icon helpers via rt_gui_internal.h.
//
// Key invariants:
//   - Widget handles are validated through the rt_*_checked cast helpers before
//     use, so a wrong-typed or stale handle is rejected rather than misread.
//   - Item helpers operate relative to their owning bar; a detached item is a
//     no-op rather than a crash.
//   - Mirrors rt_gui_menus.c's ZANNA_ENABLE_GRAPHICS guard: real widgets when
//     graphics is enabled, no-op stubs otherwise.
//
// Ownership/Lifetime:
//   - Widgets are owned by the GUI widget tree; this layer borrows them.
//
// Links: src/runtime/graphics/gui/rt_gui_menus.c (menu widgets + shared helpers),
//        src/runtime/graphics/gui/rt_gui_internal.h (shared GUI types + API)
//
//===----------------------------------------------------------------------===//

#include "rt_gui_internal.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "vg_icon_vector.h"

#include <string.h>

#ifdef ZANNA_ENABLE_GRAPHICS

void rt_gui_set_clicked_statusbar_item(void *item);

/// @brief Status-bar item click callback: record the clicked item for the next poll.
/// @details Matches the GUI library's callback signature; @p user_data is unused
///          because the clicked item is surfaced through global poll state instead.
/// @param item Borrowed item reported by the toolkit callback.
/// @param user_data Unused callback context.
static void rt_statusbar_button_clicked(vg_statusbar_item_t *item, void *user_data) {
    (void)user_data;
    rt_gui_set_clicked_statusbar_item(item);
}

//=============================================================================
// StatusBar Widget (Phase 3)
//=============================================================================

/// @brief Create a new status bar widget (typically placed at the bottom of a window).
/// @details Creates a vg_statusbar_t with three zones (left, center, right) for
///          displaying status text, buttons, progress indicators, and separators.
///          Items are added to specific zones and rendered in the status strip.
/// @param parent Parent container or app handle.
/// @return Opaque status bar widget handle, or NULL on failure.
void *rt_statusbar_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_from_handle(parent);
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_statusbar_t *sb = vg_statusbar_create(parent_widget);
    if (sb) {
        if (app)
            rt_gui_activate_app(app);
        // Apply the active app's default font regardless of whether `parent` was an
        // app handle or a layout container. rt_gui_app_from_handle() returns NULL for
        // container parents (the IDE parents its status bar to a VBox), so the old
        // `if (app && app->default_font)` path was skipped and sb->font stayed NULL —
        // which made statusbar_paint early-return and the whole strip invisible. This
        // resolves the font via the active app, matching every other widget constructor.
        rt_gui_apply_default_font((vg_widget_t *)sb);
    }
    return sb;
}

/// @brief Release resources and destroy the statusbar.
/// @param bar StatusBar widget handle; invalid handles are ignored.
void rt_statusbar_destroy(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (sb)
        rt_widget_destroy(sb);
}

/// @brief Return the first text-type item in the given status-bar zone (or NULL).
///
/// Used by the convenience setters (`set_left_text` etc.) so they
/// can update an existing label rather than appending a new one
/// each time. Walking the zone is fine — zones rarely hold more
/// than a handful of items.
/// @param sb Borrowed StatusBar widget.
/// @param zone Zone to search.
/// @return Borrowed first text item, or NULL when the zone has none.
static vg_statusbar_item_t *get_zone_text_item(vg_statusbar_t *sb, vg_statusbar_zone_t zone) {
    vg_statusbar_item_t **items = NULL;
    size_t count = 0;
    switch (zone) {
        case VG_STATUSBAR_ZONE_LEFT:
            items = sb->left_items;
            count = sb->left_count;
            break;
        case VG_STATUSBAR_ZONE_CENTER:
            items = sb->center_items;
            count = sb->center_count;
            break;
        case VG_STATUSBAR_ZONE_RIGHT:
            items = sb->right_items;
            count = sb->right_count;
            break;
    }
    for (size_t i = 0; i < count; i++) {
        if (items[i] && items[i]->type == VG_STATUSBAR_ITEM_TEXT) {
            return items[i];
        }
    }
    return NULL;
}

/// @brief Set the left text of the statusbar.
/// @param bar StatusBar widget handle.
/// @param text Runtime string copied into the left text item.
void rt_statusbar_set_left_text(void *bar, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return;
    vg_statusbar_item_t *item = get_zone_text_item(sb, VG_STATUSBAR_ZONE_LEFT);
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    if (item) {
        vg_statusbar_item_set_text(item, ctext);
    } else {
        vg_statusbar_add_text(sb, VG_STATUSBAR_ZONE_LEFT, ctext);
    }
    free(ctext);
}

/// @brief Set the center text of the statusbar.
/// @param bar StatusBar widget handle.
/// @param text Runtime string copied into the center text item.
void rt_statusbar_set_center_text(void *bar, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return;
    vg_statusbar_item_t *item = get_zone_text_item(sb, VG_STATUSBAR_ZONE_CENTER);
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    if (item) {
        vg_statusbar_item_set_text(item, ctext);
    } else {
        vg_statusbar_add_text(sb, VG_STATUSBAR_ZONE_CENTER, ctext);
    }
    free(ctext);
}

/// @brief Set the right text of the statusbar.
/// @param bar StatusBar widget handle.
/// @param text Runtime string copied into the right text item.
void rt_statusbar_set_right_text(void *bar, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return;
    vg_statusbar_item_t *item = get_zone_text_item(sb, VG_STATUSBAR_ZONE_RIGHT);
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    if (item) {
        vg_statusbar_item_set_text(item, ctext);
    } else {
        vg_statusbar_add_text(sb, VG_STATUSBAR_ZONE_RIGHT, ctext);
    }
    free(ctext);
}

/// @brief Get the left text of the statusbar.
/// @param bar StatusBar widget handle.
/// @return Owned left-zone text, or an empty runtime string when unavailable.
rt_string rt_statusbar_get_left_text(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return rt_str_empty();
    vg_statusbar_item_t *item = get_zone_text_item(sb, VG_STATUSBAR_ZONE_LEFT);
    if (item && item->text) {
        return rt_gui_string_from_cstr_bounded(item->text);
    }
    return rt_str_empty();
}

/// @brief Get the center text of the statusbar.
/// @param bar StatusBar widget handle.
/// @return Owned center-zone text, or an empty runtime string when unavailable.
rt_string rt_statusbar_get_center_text(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return rt_str_empty();
    vg_statusbar_item_t *item = get_zone_text_item(sb, VG_STATUSBAR_ZONE_CENTER);
    if (item && item->text) {
        return rt_gui_string_from_cstr_bounded(item->text);
    }
    return rt_str_empty();
}

/// @brief Get the right text of the statusbar.
/// @param bar StatusBar widget handle.
/// @return Owned right-zone text, or an empty runtime string when unavailable.
rt_string rt_statusbar_get_right_text(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return rt_str_empty();
    vg_statusbar_item_t *item = get_zone_text_item(sb, VG_STATUSBAR_ZONE_RIGHT);
    if (item && item->text) {
        return rt_gui_string_from_cstr_bounded(item->text);
    }
    return rt_str_empty();
}

// ===========================================================================
// Status-bar zone-targeted item builders. `zone` is one of
// `VG_STATUSBAR_ZONE_LEFT/CENTER/RIGHT` (passed in as int64 from
// the language layer). Each returns the new item handle so callers
// can capture it for later updates (e.g. progress value).
// ===========================================================================

/// @brief Append a text label to the given status-bar zone.
/// @param bar StatusBar widget handle.
/// @param text Runtime string copied into the new item.
/// @param zone Target status-bar zone.
/// @return Wrapped item handle, or NULL for invalid input/allocation failure.
void *rt_statusbar_add_text(void *bar, rt_string text, int64_t zone) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    vg_statusbar_zone_t checked_zone = VG_STATUSBAR_ZONE_LEFT;
    if (!sb || !rt_statusbar_zone_checked(zone, &checked_zone))
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return NULL;
    vg_statusbar_item_t *item = vg_statusbar_add_text(sb, checked_zone, ctext);
    free(ctext);
    return rt_gui_wrap_statusbar_item(item);
}

/// @brief Append a clickable button to a status-bar zone.
/// @param bar StatusBar widget handle.
/// @param text Runtime string copied into the button.
/// @param zone Target status-bar zone.
/// @return Wrapped button-item handle, or NULL on failure.
void *rt_statusbar_add_button(void *bar, rt_string text, int64_t zone) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    vg_statusbar_zone_t checked_zone = VG_STATUSBAR_ZONE_LEFT;
    if (!sb || !rt_statusbar_zone_checked(zone, &checked_zone))
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return NULL;
    vg_statusbar_item_t *item =
        vg_statusbar_add_button(sb, checked_zone, ctext, rt_statusbar_button_clicked, NULL);
    free(ctext);
    return rt_gui_wrap_statusbar_item(item);
}

/// @brief Append a progress bar to a status-bar zone (drive via `rt_statusbaritem_set_progress`).
/// @param bar StatusBar widget handle.
/// @param zone Target status-bar zone.
/// @return Wrapped progress-item handle, or NULL on failure.
void *rt_statusbar_add_progress(void *bar, int64_t zone) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    vg_statusbar_zone_t checked_zone = VG_STATUSBAR_ZONE_LEFT;
    return sb && rt_statusbar_zone_checked(zone, &checked_zone)
               ? rt_gui_wrap_statusbar_item(vg_statusbar_add_progress(sb, checked_zone))
               : NULL;
}

/// @brief Append a vertical separator line to a status-bar zone.
/// @param bar StatusBar widget handle.
/// @param zone Target status-bar zone.
/// @return Wrapped separator-item handle, or NULL on failure.
void *rt_statusbar_add_separator(void *bar, int64_t zone) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    vg_statusbar_zone_t checked_zone = VG_STATUSBAR_ZONE_LEFT;
    return sb && rt_statusbar_zone_checked(zone, &checked_zone)
               ? rt_gui_wrap_statusbar_item(vg_statusbar_add_separator(sb, checked_zone))
               : NULL;
}

/// @brief Append a flexible spacer (consumes free space within the zone).
/// @param bar StatusBar widget handle.
/// @param zone Target status-bar zone.
/// @return Wrapped spacer-item handle, or NULL on failure.
void *rt_statusbar_add_spacer(void *bar, int64_t zone) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    vg_statusbar_zone_t checked_zone = VG_STATUSBAR_ZONE_LEFT;
    return sb && rt_statusbar_zone_checked(zone, &checked_zone)
               ? rt_gui_wrap_statusbar_item(vg_statusbar_add_spacer(sb, checked_zone))
               : NULL;
}

/// @brief Remove an item from the status bar.
/// @param bar Owning StatusBar widget handle.
/// @param item Wrapped item handle; foreign or stale items are ignored.
void rt_statusbar_remove_item(void *bar, void *item) {
    RT_ASSERT_MAIN_THREAD();
    if (!bar || !item)
        return;
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sb || !sbi || sbi->owner != sb)
        return;
    rt_gui_app_t *app = rt_gui_app_from_widget(&sb->base);
    if (app && app->last_statusbar_clicked == sbi)
        app->last_statusbar_clicked = NULL;
    vg_statusbar_remove_item(sb, sbi);
    rt_gui_collect_retired_subhandles(&sb->base);
}

/// @brief Remove all items from all status bar zones.
/// @param bar StatusBar widget handle.
void rt_statusbar_clear(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return;
    rt_gui_app_t *app = rt_gui_app_from_widget(&sb->base);
    if (app)
        app->last_statusbar_clicked = NULL;
    vg_statusbar_clear_zone(sb, VG_STATUSBAR_ZONE_LEFT);
    vg_statusbar_clear_zone(sb, VG_STATUSBAR_ZONE_CENTER);
    vg_statusbar_clear_zone(sb, VG_STATUSBAR_ZONE_RIGHT);
    rt_gui_collect_retired_subhandles(&sb->base);
}

/// @brief Show or hide the status bar.
/// @param bar StatusBar widget handle.
/// @param visible Non-zero to show the bar; zero to hide it.
void rt_statusbar_set_visible(void *bar, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    if (!sb)
        return;
    vg_widget_set_visible(&sb->base, visible != 0);
}

/// @brief Check whether the status bar is currently visible.
/// @param bar StatusBar widget handle.
/// @return 1 when visible, otherwise 0.
int64_t rt_statusbar_is_visible(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_t *sb = rt_statusbar_checked(bar);
    return sb && sb->base.visible ? 1 : 0;
}

//=============================================================================
// StatusBarItem Widget (Phase 3)
//=============================================================================

/// @brief Set the text of the statusbaritem.
/// @param item StatusBarItem handle.
/// @param text Runtime string copied into the item.
void rt_statusbaritem_set_text(void *item, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    vg_statusbar_item_set_text(sbi, ctext);
    free(ctext);
}

/// @brief Set the text color of the statusbaritem.
/// @param item StatusBarItem handle.
/// @param color Packed ARGB text color.
void rt_statusbaritem_set_text_color(void *item, int64_t color) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return;
    vg_statusbar_item_set_text_color(sbi, (uint32_t)color);
}

/// @brief Set (or clear) a named scalable vector icon on a status bar item (ADR 0137).
/// @details Unknown names are ignored so callers can probe icon availability;
///          an empty name clears the current vector icon.
/// @param item StatusBarItem handle.
/// @param name Stable vector icon name, or an empty string to clear it.
void rt_statusbaritem_set_icon_name(void *item, rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return;
    char *cname = rt_string_to_gui_cstr(name);
    if (!cname || !cname[0]) {
        vg_statusbar_item_set_icon_vector(sbi, -1);
        free(cname);
        return;
    }
    int32_t icon_id = vg_icon_vector_find(cname);
    free(cname);
    if (icon_id != VG_ICON_VECTOR_INVALID)
        vg_statusbar_item_set_icon_vector(sbi, icon_id);
}

/// @brief Get the text of the statusbaritem.
/// @param item StatusBarItem handle.
/// @return Owned item text, or an empty runtime string when unavailable.
rt_string rt_statusbaritem_get_text(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return rt_str_empty();
    if (sbi->text) {
        return rt_gui_string_from_cstr_bounded(sbi->text);
    }
    return rt_str_empty();
}

/// @brief Set the tooltip of the statusbaritem.
/// @param item StatusBarItem handle.
/// @param tooltip Runtime string copied into the tooltip.
void rt_statusbaritem_set_tooltip(void *item, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return;
    char *ctext = rt_string_to_gui_cstr(tooltip);
    if (!ctext)
        return;
    vg_statusbar_item_set_tooltip(sbi, ctext);
    free(ctext);
}

/// @brief Set the progress of the statusbaritem.
/// @param item Progress StatusBarItem handle.
/// @param value Progress value clamped to `[0,1]`; non-finite values become 0.
void rt_statusbaritem_set_progress(void *item, double value) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return;
    double sanitized = rt_gui_double_is_finite(value) ? value : 0.0;
    vg_statusbar_item_set_progress(sbi, (float)rt_gui_clamp_f64(sanitized, 0.0, 1.0));
}

/// @brief Get the progress of the statusbaritem.
/// @param item Progress StatusBarItem handle.
/// @return Current normalized progress, or 0 for an invalid handle.
double rt_statusbaritem_get_progress(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return 0.0;
    return rt_gui_finite_clamped((double)sbi->progress, 0.0, 1.0, 0.0);
}

/// @brief Show or hide a status bar item.
/// @param item StatusBarItem handle.
/// @param visible Non-zero to show the item; zero to hide it.
void rt_statusbaritem_set_visible(void *item, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return;
    vg_statusbar_item_set_visible(sbi, visible != 0);
}

/// @brief Record which status bar item was clicked (for frame-based polling).
/// @param item Borrowed live status-bar item reported by the toolkit.
void rt_gui_set_clicked_statusbar_item(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = (vg_statusbar_item_t *)item;
    if (!vg_statusbar_item_is_live(sbi) || !sbi->owner)
        return;
    rt_gui_app_t *app = rt_gui_app_from_widget(&sbi->owner->base);
    if (app)
        app->last_statusbar_clicked = sbi;
}

/// @brief Check if a status bar item was clicked this frame (edge-triggered).
/// @param item StatusBarItem handle.
/// @return 1 once when this item matches the app's pending click, otherwise 0.
int64_t rt_statusbaritem_was_clicked(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_statusbar_item_t *sbi = rt_statusbaritem_checked(item);
    if (!sbi)
        return 0;
    if (!sbi->owner)
        return 0;
    rt_gui_app_t *app = rt_gui_app_from_widget(&sbi->owner->base);
    if (!app || app->last_statusbar_clicked != sbi)
        return 0;
    app->last_statusbar_clicked = NULL;
    return 1;
}

//=============================================================================
// Toolbar Widget (Phase 3)
//=============================================================================

/// @brief Map a semantic toolbar icon name to the vector-icon codepoint key.
/// @details The underlying GUI toolbar already knows how to draw a compact set of
///          codepoint-keyed vector icons. This bridge gives Zia and BASIC callers
///          stable names instead of exposing those private glyph choices as UI
///          text. Unknown names return 0, which callers convert to VG_ICON_NONE.
/// @param name Lowercase semantic icon name; may be NULL.
/// @return Unicode codepoint used by ZannaGUI vector toolbar drawing, or 0.
static uint32_t rt_toolbar_builtin_icon_codepoint(const char *name) {
    if (!name || name[0] == '\0')
        return 0;
    if (strcmp(name, "new") == 0 || strcmp(name, "new-file") == 0)
        return 0x2Bu;
    if (strcmp(name, "open") == 0 || strcmp(name, "open-folder") == 0)
        return 0x25A4u;
    if (strcmp(name, "save") == 0)
        return 0x25BCu;
    if (strcmp(name, "save-all") == 0 || strcmp(name, "saveall") == 0)
        return 0x21D3u;
    if (strcmp(name, "build") == 0)
        return 0x25A3u;
    if (strcmp(name, "run") == 0 || strcmp(name, "play") == 0)
        return 0x25B6u;
    if (strcmp(name, "stop") == 0)
        return 0x25A0u;
    if (strcmp(name, "debug") == 0)
        return 0x25C7u;
    if (strcmp(name, "continue") == 0 || strcmp(name, "debug-continue") == 0)
        return 0x25B7u;
    if (strcmp(name, "step-over") == 0 || strcmp(name, "step") == 0)
        return 0x2192u;
    if (strcmp(name, "find") == 0 || strcmp(name, "search") == 0)
        return 0x3Fu;
    if (strcmp(name, "explorer") == 0 || strcmp(name, "files") == 0)
        return 0x25A6u;
    if (strcmp(name, "source-control") == 0 || strcmp(name, "scm") == 0 || strcmp(name, "git") == 0)
        return 0x2387u;
    return 0;
}

/// @brief Convert a runtime string semantic icon name into a GUI icon value.
/// @param icon_name Runtime string containing a built-in icon name.
/// @return Glyph icon on known names, otherwise VG_ICON_NONE.
static vg_icon_t rt_toolbar_icon_from_name(rt_string icon_name) {
    vg_icon_t icon = {0};
    char *cname = rt_string_to_cstr_no_nul(icon_name);
    if (icon_name && !cname)
        return icon;
    // Scalable vector icons take precedence (ADR 0137); the legacy builtin
    // codepoint table remains the fallback for unmapped names.
    int32_t vector_id = vg_icon_vector_find(cname);
    if (vector_id != VG_ICON_VECTOR_INVALID) {
        free(cname);
        return vg_icon_from_vector(vector_id);
    }
    uint32_t cp = rt_toolbar_builtin_icon_codepoint(cname);
    free(cname);
    if (cp != 0)
        icon = vg_icon_from_glyph(cp);
    return icon;
}

/// @brief Create a new horizontal toolbar widget.
/// @details Creates a vg_toolbar_t for displaying icon buttons, toggles,
///          separators, and dropdown items in a horizontal strip. Typically
///          placed below the menu bar. Buttons can carry both icons and labels.
/// @param parent Parent container or app handle.
/// @return Opaque toolbar widget handle, or NULL on failure.
void *rt_toolbar_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_from_handle(parent);
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_toolbar_t *tb = vg_toolbar_create(parent_widget, VG_TOOLBAR_HORIZONTAL);
    if (tb) {
        if (app)
            rt_gui_activate_app(app);
        rt_gui_apply_default_font((vg_widget_t *)tb);
    }
    return tb;
}

/// @brief Create a new vertical toolbar widget.
/// @details Like rt_toolbar_new but arranged vertically (e.g., sidebar tool palette).
/// @param parent Parent container or app handle.
/// @return Opaque toolbar widget handle, or NULL on failure.
void *rt_toolbar_new_vertical(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_from_handle(parent);
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_toolbar_t *tb = vg_toolbar_create(parent_widget, VG_TOOLBAR_VERTICAL);
    if (tb) {
        if (app)
            rt_gui_activate_app(app);
        rt_gui_apply_default_font((vg_widget_t *)tb);
    }
    return tb;
}

/// @brief Release resources and destroy the toolbar.
/// @param toolbar Toolbar widget handle; invalid handles are ignored.
void rt_toolbar_destroy(void *toolbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (tb)
        rt_widget_destroy(tb);
}

/// @brief Append an icon-only toolbar button.
///
/// Loads the icon from `icon_path` and uses `tooltip` for hover text.
/// @param toolbar Toolbar widget handle.
/// @param icon_path Runtime path for an image icon; empty produces no image.
/// @param tooltip Runtime tooltip text copied into the item.
/// @return Wrapped toolbar-item handle, or NULL on failure.
void *rt_toolbar_add_button(void *toolbar, rt_string icon_path, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    char *cicon = rt_string_to_cstr_no_nul(icon_path);
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if ((icon_path && !cicon) || !ctooltip) {
        free(cicon);
        free(ctooltip);
        return NULL;
    }

    vg_icon_t icon = rt_gui_icon_from_path_cstr(cicon);
    free(cicon);
    cicon = NULL;

    vg_toolbar_item_t *item = vg_toolbar_add_button(tb, NULL, NULL, icon, NULL, NULL);
    if (item) {
        vg_toolbar_item_set_tooltip(item, ctooltip);
    }

    free(ctooltip);
    return rt_gui_wrap_toolbar_item(item);
}

/// @brief Append a toolbar button that shows both an icon and a text label.
///
/// Forces `show_label = true` on the resulting item — toolbars
/// default to icon-only display, so this is required for the
/// label to actually render.
/// @param toolbar Toolbar widget handle.
/// @param icon_path Runtime path for an image icon.
/// @param text Runtime label text copied into the item.
/// @param tooltip Runtime tooltip text copied into the item.
/// @return Wrapped toolbar-item handle, or NULL on failure.
void *rt_toolbar_add_button_with_text(void *toolbar,
                                      rt_string icon_path,
                                      rt_string text,
                                      rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    char *cicon = rt_string_to_cstr_no_nul(icon_path);
    char *ctext = rt_string_to_gui_cstr(text);
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if ((icon_path && !cicon) || !ctext || !ctooltip) {
        free(cicon);
        free(ctext);
        free(ctooltip);
        return NULL;
    }

    vg_icon_t icon = rt_gui_icon_from_path_cstr(cicon);
    free(cicon);
    cicon = NULL;

    vg_toolbar_item_t *item = vg_toolbar_add_button(tb, NULL, ctext, icon, NULL, NULL);
    if (item) {
        // Force label visible — tb->show_labels defaults to false, so items
        // created via AddButtonWithText would otherwise never show their label.
        item->show_label = true;
        vg_toolbar_item_set_tooltip(item, ctooltip);
    }

    free(ctext);
    free(ctooltip);
    return rt_gui_wrap_toolbar_item(item);
}

/// @brief Append an icon-only toolbar button using a built-in semantic icon name.
/// @param toolbar Toolbar widget handle.
/// @param icon_name Stable semantic icon name.
/// @param tooltip Runtime tooltip text copied into the item.
/// @return Wrapped toolbar-item handle, or NULL on failure.
void *rt_toolbar_add_named_button(void *toolbar, rt_string icon_name, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if (!ctooltip)
        return NULL;

    vg_toolbar_item_t *item =
        vg_toolbar_add_button(tb, NULL, NULL, rt_toolbar_icon_from_name(icon_name), NULL, NULL);
    if (item)
        vg_toolbar_item_set_tooltip(item, ctooltip);

    free(ctooltip);
    return rt_gui_wrap_toolbar_item(item);
}

/// @brief Append a toolbar button with text plus a built-in semantic icon.
/// @param toolbar Toolbar widget handle.
/// @param icon_name Stable semantic icon name.
/// @param text Runtime label text copied into the item.
/// @param tooltip Runtime tooltip text copied into the item.
/// @return Wrapped toolbar-item handle, or NULL on failure.
void *rt_toolbar_add_named_button_with_text(void *toolbar,
                                            rt_string icon_name,
                                            rt_string text,
                                            rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if (!ctext || !ctooltip) {
        free(ctext);
        free(ctooltip);
        return NULL;
    }

    vg_toolbar_item_t *item =
        vg_toolbar_add_button(tb, NULL, ctext, rt_toolbar_icon_from_name(icon_name), NULL, NULL);
    if (item) {
        item->show_label = true;
        vg_toolbar_item_set_tooltip(item, ctooltip);
    }

    free(ctext);
    free(ctooltip);
    return rt_gui_wrap_toolbar_item(item);
}

/// @brief Append a sticky toggle button (radio/checkbox-style press state).
/// @param toolbar Toolbar widget handle.
/// @param icon_path Runtime path for an image icon.
/// @param tooltip Runtime tooltip text copied into the item.
/// @return Wrapped toggle-item handle, or NULL on failure.
void *rt_toolbar_add_toggle(void *toolbar, rt_string icon_path, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    char *cicon = rt_string_to_cstr_no_nul(icon_path);
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if ((icon_path && !cicon) || !ctooltip) {
        free(cicon);
        free(ctooltip);
        return NULL;
    }

    vg_icon_t icon = rt_gui_icon_from_path_cstr(cicon);
    free(cicon);
    cicon = NULL;

    vg_toolbar_item_t *item = vg_toolbar_add_toggle(tb, NULL, NULL, icon, false, NULL, NULL);
    if (item) {
        vg_toolbar_item_set_tooltip(item, ctooltip);
    }

    free(ctooltip);
    return rt_gui_wrap_toolbar_item(item);
}

/// @brief Append a sticky toggle button using a built-in semantic icon name.
/// @param toolbar Toolbar widget handle.
/// @param icon_name Stable semantic icon name.
/// @param tooltip Runtime tooltip text copied into the item.
/// @return Wrapped toggle-item handle, or NULL on failure.
void *rt_toolbar_add_named_toggle(void *toolbar, rt_string icon_name, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if (!ctooltip)
        return NULL;

    vg_toolbar_item_t *item = vg_toolbar_add_toggle(
        tb, NULL, NULL, rt_toolbar_icon_from_name(icon_name), false, NULL, NULL);
    if (item)
        vg_toolbar_item_set_tooltip(item, ctooltip);

    free(ctooltip);
    return rt_gui_wrap_toolbar_item(item);
}

/// @brief Append a vertical (or horizontal, for vertical toolbars) separator line.
/// @param toolbar Toolbar widget handle.
/// @return Wrapped separator-item handle, or NULL on failure.
void *rt_toolbar_add_separator(void *toolbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    return tb ? rt_gui_wrap_toolbar_item(vg_toolbar_add_separator(tb)) : NULL;
}

/// @brief Append a flexible spacer (consumes free space, useful for right-aligning items).
/// @param toolbar Toolbar widget handle.
/// @return Wrapped spacer-item handle, or NULL on failure.
void *rt_toolbar_add_spacer(void *toolbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    return tb ? rt_gui_wrap_toolbar_item(vg_toolbar_add_spacer(tb)) : NULL;
}

/// @brief Append a dropdown button — clicking opens an attached menu of choices.
/// @param toolbar Toolbar widget handle.
/// @param tooltip Runtime tooltip text copied into the item.
/// @return Wrapped dropdown-item handle, or NULL on failure.
void *rt_toolbar_add_dropdown(void *toolbar, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if (!ctooltip)
        return NULL;

    vg_icon_t icon = {0};
    icon.type = VG_ICON_NONE;

    vg_toolbar_item_t *item = vg_toolbar_add_dropdown(tb, NULL, NULL, icon, NULL);
    if (item) {
        vg_toolbar_item_set_tooltip(item, ctooltip);
    }

    free(ctooltip);
    return rt_gui_wrap_toolbar_item(item);
}

/// @brief Remove an item from the toolbar.
/// @param toolbar Owning Toolbar widget handle.
/// @param item Wrapped item handle; foreign or stale items are ignored.
void rt_toolbar_remove_item(void *toolbar, void *item) {
    RT_ASSERT_MAIN_THREAD();
    if (!toolbar || !item)
        return;
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return;
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti || ti->owner != tb)
        return;
    rt_gui_app_t *app = rt_gui_app_from_widget(&tb->base);
    if (app && app->last_toolbar_clicked == ti)
        app->last_toolbar_clicked = NULL;
    vg_toolbar_remove_item_ptr(tb, ti);
    rt_gui_collect_retired_subhandles(&tb->base);
}

/// @brief Get a size property of the toolbar (button width or height).
/// @param toolbar Toolbar widget handle.
/// @param size Icon-size enumeration value, clamped to the supported range.
void rt_toolbar_set_icon_size(void *toolbar, int64_t size) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return;
    vg_toolbar_set_icon_size(tb, (vg_toolbar_icon_size_t)rt_gui_clamp_i64_to_i32(size, 0, 2));
}

/// @brief Get a size property of the toolbar (button width or height).
/// @param toolbar Toolbar widget handle.
/// @return Current icon-size enumeration, or medium for an invalid handle.
int64_t rt_toolbar_get_icon_size(void *toolbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    return tb ? tb->icon_size : RT_TOOLBAR_ICON_MEDIUM;
}

/// @brief Set the style of the toolbar.
/// @param toolbar Toolbar widget handle.
/// @param style Icon-only, text-only, or icon-and-text style value.
void rt_toolbar_set_style(void *toolbar, int64_t style) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return;
    if (style < RT_TOOLBAR_STYLE_ICON_ONLY || style > RT_TOOLBAR_STYLE_ICON_TEXT)
        return;
    vg_toolbar_set_show_labels(tb, style != RT_TOOLBAR_STYLE_ICON_ONLY);
}

/// @brief Get the number of items in the toolbar.
/// @param toolbar Toolbar widget handle.
/// @return Number of retained items, or 0 for an invalid handle.
int64_t rt_toolbar_get_item_count(void *toolbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    return tb ? (int64_t)tb->item_count : 0;
}

/// @brief Return the `index`-th item in the toolbar (NULL on out-of-range).
/// @param toolbar Toolbar widget handle.
/// @param index Zero-based item index.
/// @return Wrapped borrowed item handle, or NULL when out of range.
void *rt_toolbar_get_item(void *toolbar, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return NULL;
    if (index < 0 || index >= (int64_t)tb->item_count)
        return NULL;
    return rt_gui_wrap_toolbar_item(tb->items[index]);
}

/// @brief Show or hide the toolbar.
/// @param toolbar Toolbar widget handle.
/// @param visible Non-zero to show the toolbar; zero to hide it.
void rt_toolbar_set_visible(void *toolbar, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    if (!tb)
        return;
    vg_widget_set_visible(&tb->base, visible != 0);
}

/// @brief Check whether the toolbar is currently visible.
/// @param toolbar Toolbar widget handle.
/// @return 1 when visible, otherwise 0.
int64_t rt_toolbar_is_visible(void *toolbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_t *tb = rt_toolbar_checked(toolbar);
    return tb && tb->base.visible ? 1 : 0;
}

//=============================================================================
// ToolbarItem Widget (Phase 3)
//=============================================================================

/// @brief Set the icon of the toolbaritem.
/// @param item ToolbarItem handle.
/// @param icon_path Runtime path for the replacement image icon.
void rt_toolbaritem_set_icon(void *item, rt_string icon_path) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    char *cicon = rt_string_to_cstr_no_nul(icon_path);
    vg_icon_t icon = rt_gui_icon_from_path_cstr(cicon);
    free(cicon);
    vg_toolbar_item_set_icon(ti, icon);
}

/// @brief Set the icon pixels of the toolbaritem.
/// @param item ToolbarItem handle.
/// @param pixels Runtime Pixels handle used as the replacement icon.
void rt_toolbaritem_set_icon_pixels(void *item, void *pixels) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    vg_toolbar_item_set_icon(ti, rt_gui_icon_from_pixels(pixels));
}

/// @brief Replace a toolbar item icon with a built-in semantic icon.
/// @param item ToolbarItem handle.
/// @param icon_name Stable semantic icon name.
void rt_toolbaritem_set_named_icon(void *item, rt_string icon_name) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    vg_toolbar_item_set_icon(ti, rt_toolbar_icon_from_name(icon_name));
}

/// @brief Set the text of the toolbaritem.
/// @param item ToolbarItem handle.
/// @param text Runtime label text copied into the item.
void rt_toolbaritem_set_text(void *item, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    vg_toolbar_item_set_text(ti, ctext);
    free(ctext);
}

/// @brief Set the tooltip of the toolbaritem.
/// @param item ToolbarItem handle.
/// @param tooltip Runtime tooltip text copied into the item.
void rt_toolbaritem_set_tooltip(void *item, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    if (!ctooltip)
        return;
    vg_toolbar_item_set_tooltip(ti, ctooltip);
    free(ctooltip);
}

/// @brief Enable or disable a toolbar item.
/// @param item ToolbarItem handle.
/// @param enabled Non-zero to enable interaction; zero to disable it.
void rt_toolbaritem_set_enabled(void *item, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    vg_toolbar_item_set_enabled(ti, enabled != 0);
}

/// @brief Show or hide a toolbar item without removing it (ADR 0220).
/// @param item ToolbarItem handle.
/// @param visible Non-zero to show the item; zero hides it and its spacing.
void rt_toolbaritem_set_visible(void *item, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    vg_toolbar_item_set_visible(ti, visible != 0);
}

/// @brief Check whether a toolbar item is currently shown.
/// @param item ToolbarItem handle.
/// @return 1 when visible, otherwise 0.
int64_t rt_toolbaritem_is_visible(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return 0;
    return vg_toolbar_item_is_visible(ti) ? 1 : 0;
}

/// @brief Shared core resolving a toolbar item's on-screen rectangle (ADR 0220).
/// @param item Candidate ToolbarItem handle.
/// @param out_x Receives the on-screen left edge in logical coordinates.
/// @param out_y Receives the on-screen top edge in logical coordinates.
/// @param out_w Receives the item width.
/// @param out_h Receives the item height.
static void rt_toolbaritem_screen_rect(
    void *item, float *out_x, float *out_y, float *out_w, float *out_h) {
    *out_x = 0.0f;
    *out_y = 0.0f;
    *out_w = 0.0f;
    *out_h = 0.0f;
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    if (!vg_toolbar_item_screen_rect(ti, out_x, out_y, out_w, out_h))
        return;
    vg_widget_t *owner = &ti->owner->base;
    *out_x = (float)rt_gui_physical_to_logical(owner, *out_x);
    *out_y = (float)rt_gui_physical_to_logical(owner, *out_y);
    *out_w = (float)rt_gui_nonnegative_finite_or(rt_gui_physical_to_logical(owner, *out_w), 0.0);
    *out_h = (float)rt_gui_nonnegative_finite_or(rt_gui_physical_to_logical(owner, *out_h), 0.0);
}

/// @brief On-screen left edge of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Logical X, or zero when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_x(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_toolbaritem_screen_rect(item, &x, &y, &w, &h);
    return (double)x;
}

/// @brief On-screen top edge of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Logical Y, or zero when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_y(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_toolbaritem_screen_rect(item, &x, &y, &w, &h);
    return (double)y;
}

/// @brief On-screen width of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Item width, or zero when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_width(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_toolbaritem_screen_rect(item, &x, &y, &w, &h);
    return (double)w;
}

/// @brief On-screen height of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Item height, or zero when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_height(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_toolbaritem_screen_rect(item, &x, &y, &w, &h);
    return (double)h;
}

/// @brief Check whether a toolbar item is currently enabled.
/// @param item ToolbarItem handle.
/// @return 1 when enabled, otherwise 0.
int64_t rt_toolbaritem_is_enabled(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return 0;
    return ti->enabled ? 1 : 0;
}

/// @brief Set the toggled of the toolbaritem.
/// @param item Toggle ToolbarItem handle.
/// @param toggled Non-zero to select the item; zero to clear it.
void rt_toolbaritem_set_toggled(void *item, int64_t toggled) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return;
    vg_toolbar_item_set_checked(ti, toggled != 0);
}

/// @brief Check whether a toolbar toggle button is currently in the toggled state.
/// @param item Toggle ToolbarItem handle.
/// @return 1 when toggled, otherwise 0.
int64_t rt_toolbaritem_is_toggled(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return 0;
    return ti->checked ? 1 : 0;
}

/// @brief Record which toolbar item was clicked (for frame-based polling).
/// @param item Borrowed live toolbar item reported by the toolkit.
void rt_gui_set_clicked_toolbar_item(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = (vg_toolbar_item_t *)item;
    if (!vg_toolbar_item_is_live(ti) || !ti->owner)
        return;
    rt_gui_app_t *app = rt_gui_app_from_widget(&ti->owner->base);
    if (app)
        app->last_toolbar_clicked = ti;
}

/// @brief Check if a toolbar button was clicked this frame (edge-triggered).
/// @param item ToolbarItem handle.
/// @return 1 once when an unreported click exists for this item, otherwise 0.
int64_t rt_toolbaritem_was_clicked(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_toolbar_item_t *ti = rt_toolbaritem_checked(item);
    if (!ti)
        return 0;
    if (ti->was_clicked) {
        ti->was_clicked = false;
        rt_gui_app_t *app = ti->owner ? rt_gui_app_from_widget(&ti->owner->base) : NULL;
        if (app && app->last_toolbar_clicked == ti)
            app->last_toolbar_clicked = NULL;
        return 1;
    }
    if (!ti->owner)
        return 0;
    rt_gui_app_t *app = rt_gui_app_from_widget(&ti->owner->base);
    if (app && app->last_toolbar_clicked == ti) {
        app->last_toolbar_clicked = NULL;
        return 1;
    }
    return 0;
}


#else /* !ZANNA_ENABLE_GRAPHICS */


/// @brief Stub: graphics disabled — returns NULL; no status bar widget is created.
/// @param parent Ignored parent handle.
/// @return Always NULL.
void *rt_statusbar_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Release resources and destroy the statusbar.
/// @param bar Ignored StatusBar handle.
void rt_statusbar_destroy(void *bar) {
    (void)bar;
}

/// @brief Set the left text of the statusbar.
/// @param bar Ignored StatusBar handle.
/// @param text Ignored runtime string.
void rt_statusbar_set_left_text(void *bar, rt_string text) {
    (void)bar;
    (void)text;
}

/// @brief Set the center text of the statusbar.
/// @param bar Ignored StatusBar handle.
/// @param text Ignored runtime string.
void rt_statusbar_set_center_text(void *bar, rt_string text) {
    (void)bar;
    (void)text;
}

/// @brief Set the right text of the statusbar.
/// @param bar Ignored StatusBar handle.
/// @param text Ignored runtime string.
void rt_statusbar_set_right_text(void *bar, rt_string text) {
    (void)bar;
    (void)text;
}

/// @brief Get the left text of the statusbar.
/// @param bar Ignored StatusBar handle.
/// @return Empty runtime string.
rt_string rt_statusbar_get_left_text(void *bar) {
    (void)bar;
    return rt_str_empty();
}

/// @brief Get the center text of the statusbar.
/// @param bar Ignored StatusBar handle.
/// @return Empty runtime string.
rt_string rt_statusbar_get_center_text(void *bar) {
    (void)bar;
    return rt_str_empty();
}

/// @brief Get the right text of the statusbar.
/// @param bar Ignored StatusBar handle.
/// @return Empty runtime string.
rt_string rt_statusbar_get_right_text(void *bar) {
    (void)bar;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — returns NULL; no status bar text item is created.
/// @param bar Ignored StatusBar handle.
/// @param text Ignored runtime string.
/// @param zone Ignored zone.
/// @return Always NULL.
void *rt_statusbar_add_text(void *bar, rt_string text, int64_t zone) {
    (void)bar;
    (void)text;
    (void)zone;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no status bar button item is created.
/// @param bar Ignored StatusBar handle.
/// @param text Ignored runtime string.
/// @param zone Ignored zone.
/// @return Always NULL.
void *rt_statusbar_add_button(void *bar, rt_string text, int64_t zone) {
    (void)bar;
    (void)text;
    (void)zone;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no status bar progress item is created.
/// @param bar Ignored StatusBar handle.
/// @param zone Ignored zone.
/// @return Always NULL.
void *rt_statusbar_add_progress(void *bar, int64_t zone) {
    (void)bar;
    (void)zone;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no status bar separator item is created.
/// @param bar Ignored StatusBar handle.
/// @param zone Ignored zone.
/// @return Always NULL.
void *rt_statusbar_add_separator(void *bar, int64_t zone) {
    (void)bar;
    (void)zone;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no status bar spacer item is created.
/// @param bar Ignored StatusBar handle.
/// @param zone Ignored zone.
/// @return Always NULL.
void *rt_statusbar_add_spacer(void *bar, int64_t zone) {
    (void)bar;
    (void)zone;
    return NULL;
}

/// @brief Remove an item from the status bar.
/// @param bar Ignored StatusBar handle.
/// @param item Ignored item handle.
void rt_statusbar_remove_item(void *bar, void *item) {
    (void)bar;
    (void)item;
}

/// @brief Remove all items from all status bar zones.
/// @param bar Ignored StatusBar handle.
void rt_statusbar_clear(void *bar) {
    (void)bar;
}

/// @brief Show or hide the status bar.
/// @param bar Ignored StatusBar handle.
/// @param visible Ignored visibility flag.
void rt_statusbar_set_visible(void *bar, int64_t visible) {
    (void)bar;
    (void)visible;
}

/// @brief Check whether the status bar is currently visible.
/// @param bar Ignored StatusBar handle.
/// @return Always 0.
int64_t rt_statusbar_is_visible(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Set the text of the statusbaritem.
/// @param item Ignored StatusBarItem handle.
/// @param text Ignored runtime string.
void rt_statusbaritem_set_text(void *item, rt_string text) {
    (void)item;
    (void)text;
}

/// @brief Set the text color of the statusbaritem.
/// @param item Ignored StatusBarItem handle.
/// @param color Ignored color.
void rt_statusbaritem_set_text_color(void *item, int64_t color) {
    (void)item;
    (void)color;
}

/// @brief Graphics-disabled statusbaritem vector-icon setter stub.
/// @param item Ignored StatusBarItem handle.
/// @param name Ignored icon name.
void rt_statusbaritem_set_icon_name(void *item, rt_string name) {
    (void)item;
    (void)name;
}

/// @brief Get the text of the statusbaritem.
/// @param item Ignored StatusBarItem handle.
/// @return Empty runtime string.
rt_string rt_statusbaritem_get_text(void *item) {
    (void)item;
    return rt_str_empty();
}

/// @brief Set the tooltip of the statusbaritem.
/// @param item Ignored StatusBarItem handle.
/// @param tooltip Ignored runtime string.
void rt_statusbaritem_set_tooltip(void *item, rt_string tooltip) {
    (void)item;
    (void)tooltip;
}

/// @brief Set the progress of the statusbaritem.
/// @param item Ignored StatusBarItem handle.
/// @param value Ignored progress value.
void rt_statusbaritem_set_progress(void *item, double value) {
    (void)item;
    (void)value;
}

/// @brief Get the progress of the statusbaritem.
/// @param item Ignored StatusBarItem handle.
/// @return Always 0.
double rt_statusbaritem_get_progress(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Show or hide a status bar item.
/// @param item Ignored StatusBarItem handle.
/// @param visible Ignored visibility flag.
void rt_statusbaritem_set_visible(void *item, int64_t visible) {
    (void)item;
    (void)visible;
}

/// @brief Record which status bar item was clicked (for frame-based polling).
/// @param item Ignored StatusBarItem handle.
void rt_gui_set_clicked_statusbar_item(void *item) {
    (void)item;
}

/// @brief Check if a status bar item was clicked this frame (edge-triggered).
/// @param item Ignored StatusBarItem handle.
/// @return Always 0.
int64_t rt_statusbaritem_was_clicked(void *item) {
    (void)item;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no horizontal toolbar widget is created.
/// @param parent Ignored parent handle.
/// @return Always NULL.
void *rt_toolbar_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no vertical toolbar widget is created.
/// @param parent Ignored parent handle.
/// @return Always NULL.
void *rt_toolbar_new_vertical(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Release resources and destroy the toolbar.
/// @param toolbar Ignored Toolbar handle.
void rt_toolbar_destroy(void *toolbar) {
    (void)toolbar;
}

/// @brief Stub: graphics disabled — returns NULL; no toolbar button is created.
/// @param toolbar Ignored Toolbar handle.
/// @param icon_path Ignored icon path.
/// @param tooltip Ignored tooltip.
/// @return Always NULL.
void *rt_toolbar_add_button(void *toolbar, rt_string icon_path, rt_string tooltip) {
    (void)toolbar;
    (void)icon_path;
    (void)tooltip;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no toolbar button with text label is created.
/// @param toolbar Ignored Toolbar handle.
/// @param icon_path Ignored icon path.
/// @param text Ignored label text.
/// @param tooltip Ignored tooltip.
/// @return Always NULL.
void *rt_toolbar_add_button_with_text(void *toolbar,
                                      rt_string icon_path,
                                      rt_string text,
                                      rt_string tooltip) {
    (void)toolbar;
    (void)icon_path;
    (void)text;
    (void)tooltip;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no named toolbar button is created.
/// @param toolbar Ignored Toolbar handle.
/// @param icon_name Ignored icon name.
/// @param tooltip Ignored tooltip.
/// @return Always NULL.
void *rt_toolbar_add_named_button(void *toolbar, rt_string icon_name, rt_string tooltip) {
    (void)toolbar;
    (void)icon_name;
    (void)tooltip;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no named toolbar button with text is created.
/// @param toolbar Ignored Toolbar handle.
/// @param icon_name Ignored icon name.
/// @param text Ignored label text.
/// @param tooltip Ignored tooltip.
/// @return Always NULL.
void *rt_toolbar_add_named_button_with_text(void *toolbar,
                                            rt_string icon_name,
                                            rt_string text,
                                            rt_string tooltip) {
    (void)toolbar;
    (void)icon_name;
    (void)text;
    (void)tooltip;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no toolbar toggle button is created.
/// @param toolbar Ignored Toolbar handle.
/// @param icon_path Ignored icon path.
/// @param tooltip Ignored tooltip.
/// @return Always NULL.
void *rt_toolbar_add_toggle(void *toolbar, rt_string icon_path, rt_string tooltip) {
    (void)toolbar;
    (void)icon_path;
    (void)tooltip;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no named toolbar toggle button is created.
/// @param toolbar Ignored Toolbar handle.
/// @param icon_name Ignored icon name.
/// @param tooltip Ignored tooltip.
/// @return Always NULL.
void *rt_toolbar_add_named_toggle(void *toolbar, rt_string icon_name, rt_string tooltip) {
    (void)toolbar;
    (void)icon_name;
    (void)tooltip;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no toolbar separator item is created.
/// @param toolbar Ignored Toolbar handle.
/// @return Always NULL.
void *rt_toolbar_add_separator(void *toolbar) {
    (void)toolbar;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no toolbar spacer item is created.
/// @param toolbar Ignored Toolbar handle.
/// @return Always NULL.
void *rt_toolbar_add_spacer(void *toolbar) {
    (void)toolbar;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no toolbar dropdown item is created.
/// @param toolbar Ignored Toolbar handle.
/// @param tooltip Ignored tooltip.
/// @return Always NULL.
void *rt_toolbar_add_dropdown(void *toolbar, rt_string tooltip) {
    (void)toolbar;
    (void)tooltip;
    return NULL;
}

/// @brief Remove an item from the toolbar.
/// @param toolbar Ignored Toolbar handle.
/// @param item Ignored item handle.
void rt_toolbar_remove_item(void *toolbar, void *item) {
    (void)toolbar;
    (void)item;
}

/// @brief Get a size property of the toolbar (button width or height).
/// @param toolbar Ignored Toolbar handle.
/// @param size Ignored icon-size value.
void rt_toolbar_set_icon_size(void *toolbar, int64_t size) {
    (void)toolbar;
    (void)size;
}

/// @brief Get a size property of the toolbar (button width or height).
/// @param toolbar Ignored Toolbar handle.
/// @return Always 0.
int64_t rt_toolbar_get_icon_size(void *toolbar) {
    (void)toolbar;
    return 0;
}

/// @brief Set the style of the toolbar.
/// @param toolbar Ignored Toolbar handle.
/// @param style Ignored style value.
void rt_toolbar_set_style(void *toolbar, int64_t style) {
    (void)toolbar;
    (void)style;
}

/// @brief Get the number of items in the toolbar.
/// @param toolbar Ignored Toolbar handle.
/// @return Always 0.
int64_t rt_toolbar_get_item_count(void *toolbar) {
    (void)toolbar;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no toolbar exists to retrieve items from.
/// @param toolbar Ignored Toolbar handle.
/// @param index Ignored item index.
/// @return Always NULL.
void *rt_toolbar_get_item(void *toolbar, int64_t index) {
    (void)toolbar;
    (void)index;
    return NULL;
}

/// @brief Show or hide the toolbar.
/// @param toolbar Ignored Toolbar handle.
/// @param visible Ignored visibility flag.
void rt_toolbar_set_visible(void *toolbar, int64_t visible) {
    (void)toolbar;
    (void)visible;
}

/// @brief Check whether the toolbar is currently visible.
/// @param toolbar Ignored Toolbar handle.
/// @return Always 0.
int64_t rt_toolbar_is_visible(void *toolbar) {
    (void)toolbar;
    return 0;
}

/// @brief Set the icon of the toolbaritem.
/// @param item Ignored ToolbarItem handle.
/// @param icon_path Ignored icon path.
void rt_toolbaritem_set_icon(void *item, rt_string icon_path) {
    (void)item;
    (void)icon_path;
}

/// @brief Set the icon pixels of the toolbaritem.
/// @param item Ignored ToolbarItem handle.
/// @param pixels Ignored Pixels handle.
void rt_toolbaritem_set_icon_pixels(void *item, void *pixels) {
    (void)item;
    (void)pixels;
}

/// @brief Set the named icon of the toolbaritem.
/// @param item Ignored ToolbarItem handle.
/// @param icon_name Ignored icon name.
void rt_toolbaritem_set_named_icon(void *item, rt_string icon_name) {
    (void)item;
    (void)icon_name;
}

/// @brief Set the text of the toolbaritem.
/// @param item Ignored ToolbarItem handle.
/// @param text Ignored runtime string.
void rt_toolbaritem_set_text(void *item, rt_string text) {
    (void)item;
    (void)text;
}

/// @brief Set the tooltip of the toolbaritem.
/// @param item Ignored ToolbarItem handle.
/// @param tooltip Ignored runtime string.
void rt_toolbaritem_set_tooltip(void *item, rt_string tooltip) {
    (void)item;
    (void)tooltip;
}

/// @brief Enable or disable a toolbar item.
/// @param item Ignored ToolbarItem handle.
/// @param enabled Ignored enabled flag.
void rt_toolbaritem_set_enabled(void *item, int64_t enabled) {
    (void)item;
    (void)enabled;
}

/// @brief Stub: ignore toolbar item visibility changes when graphics is disabled.
/// @param item Ignored ToolbarItem handle.
/// @param visible Ignored visibility flag.
void rt_toolbaritem_set_visible(void *item, int64_t visible) {
    (void)item;
    (void)visible;
}

/// @brief Stub: report toolbar items hidden when graphics is disabled.
/// @param item Ignored ToolbarItem handle.
/// @return Always zero.
int64_t rt_toolbaritem_is_visible(void *item) {
    (void)item;
    return 0;
}

/// @brief Stub: report zero toolbar item screen X when graphics is disabled.
/// @param item Ignored ToolbarItem handle.
/// @return Always zero.
double rt_toolbaritem_get_screen_x(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Stub: report zero toolbar item screen Y when graphics is disabled.
/// @param item Ignored ToolbarItem handle.
/// @return Always zero.
double rt_toolbaritem_get_screen_y(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Stub: report zero toolbar item screen width when graphics is disabled.
/// @param item Ignored ToolbarItem handle.
/// @return Always zero.
double rt_toolbaritem_get_screen_width(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Stub: report zero toolbar item screen height when graphics is disabled.
/// @param item Ignored ToolbarItem handle.
/// @return Always zero.
double rt_toolbaritem_get_screen_height(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Check whether a toolbar item is currently enabled.
/// @param item Ignored ToolbarItem handle.
/// @return Always 0.
int64_t rt_toolbaritem_is_enabled(void *item) {
    (void)item;
    return 0;
}

/// @brief Set the toggled of the toolbaritem.
/// @param item Ignored ToolbarItem handle.
/// @param toggled Ignored toggled flag.
void rt_toolbaritem_set_toggled(void *item, int64_t toggled) {
    (void)item;
    (void)toggled;
}

/// @brief Check whether a toolbar toggle button is currently in the toggled state.
/// @param item Ignored ToolbarItem handle.
/// @return Always 0.
int64_t rt_toolbaritem_is_toggled(void *item) {
    (void)item;
    return 0;
}

/// @brief Record which toolbar item was clicked (for frame-based polling).
/// @param item Ignored ToolbarItem handle.
void rt_gui_set_clicked_toolbar_item(void *item) {
    (void)item;
}

/// @brief Check if a toolbar button was clicked this frame (edge-triggered).
/// @param item Ignored ToolbarItem handle.
/// @return Always 0.
int64_t rt_toolbaritem_was_clicked(void *item) {
    (void)item;
    return 0;
}


#endif /* ZANNA_ENABLE_GRAPHICS */
