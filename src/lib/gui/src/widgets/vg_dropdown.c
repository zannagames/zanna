//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_dropdown.c
/// @brief Implements a scrolling pop-up list selector.
///
/// @details The widget paints a compact trigger and a floating overlay panel
/// that can flip above the trigger to remain inside the root viewport. It
/// supports pointer hit testing, wheel scrolling, keyboard navigation,
/// single-character typeahead, placeholder text, and change callbacks.
///
/// The dropdown owns its dynamically grown item array, every copied item
/// string, and the copied placeholder. A selected index of `-1` means no
/// selection. The panel is closed before operations that invalidate item
/// addresses or indices.
///
/// @see vg_widgets.h
/// @see vg_theme.h
/// @see vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void dropdown_destroy(vg_widget_t *widget);
static void dropdown_measure(vg_widget_t *widget, float avail_w, float avail_h);
static void dropdown_paint(vg_widget_t *widget, void *canvas);
static void dropdown_paint_overlay(vg_widget_t *widget, void *canvas);
static void dropdown_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height);
static bool dropdown_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool dropdown_can_focus(vg_widget_t *widget);

static void dropdown_clamp_scroll(vg_dropdown_t *dd, float panel_h);
static void dropdown_scroll_to_index(vg_dropdown_t *dd, int index);
static void dropdown_open(vg_widget_t *widget, vg_dropdown_t *dd, int preferred_hover);
static void dropdown_close(vg_widget_t *widget, vg_dropdown_t *dd);
static float dropdown_item_height(vg_dropdown_t *dd);
static float dropdown_measure_text_width(vg_dropdown_t *dd, const char *text);
static float dropdown_preferred_width(vg_dropdown_t *dd);
static float dropdown_panel_width(vg_dropdown_t *dd, float trigger_width);
static int dropdown_find_typeahead_index(vg_dropdown_t *dd, uint32_t codepoint, int start_index);
static void dropdown_emit_change(vg_dropdown_t *dd, int old_index);

static float dropdown_scrollbar_thumb_size(float track_size,
                                           float content_size,
                                           float viewport_size);
static float dropdown_scrollbar_thumb_offset(float scroll_pos,
                                             float scroll_range,
                                             float track_size,
                                             float thumb_size);
static float dropdown_text_baseline(vg_dropdown_t *dd, float row_y, float row_h);

//=============================================================================
// Dropdown VTable
//=============================================================================

static vg_widget_vtable_t g_dropdown_vtable = {.destroy = dropdown_destroy,
                                               .measure = dropdown_measure,
                                               .arrange = NULL,
                                               .paint = dropdown_paint,
                                               .paint_overlay = dropdown_paint_overlay,
                                               .handle_event = dropdown_handle_event,
                                               .can_focus = dropdown_can_focus,
                                               .on_focus = NULL,
                                               .get_visual_bounds = dropdown_get_visual_bounds};

/// @brief Releases input capture and all dropdown-owned strings and storage.
///
/// @param widget Dropdown base widget being destroyed.
static void dropdown_destroy(vg_widget_t *widget) {
    vg_dropdown_t *dd = (vg_dropdown_t *)widget;
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    for (int i = 0; i < dd->item_count; i++)
        free(dd->items[i]);
    free(dd->items);
    free(dd->placeholder);
    dd->items = NULL;
    dd->item_count = 0;
    dd->item_capacity = 0;
}

/// @brief Measures the trigger from its widest label and one item-row height.
///
/// @param widget Dropdown base widget whose measured dimensions are updated.
/// @param avail_w Available width used to cap the preferred width.
/// @param avail_h Available height; currently unused.
static void dropdown_measure(vg_widget_t *widget, float avail_w, float avail_h) {
    (void)avail_h;

    float width = dropdown_preferred_width((vg_dropdown_t *)widget);
    if (avail_w > 0.0f && width > avail_w)
        width = avail_w;
    if (width < 120.0f)
        width = 120.0f;
    widget->measured_width = width;
    widget->measured_height = dropdown_item_height((vg_dropdown_t *)widget);
    vg_widget_apply_constraints(widget);
}

/// @brief Returns the height of one trigger or list row.
///
/// @param dd Dropdown supplying optional font metrics.
/// @return Larger of the theme input height and padded font line height.
static float dropdown_item_height(vg_dropdown_t *dd) {
    vg_theme_t *theme = vg_theme_get_current();
    float scale = theme && theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    float height = theme ? theme->input.height : 28.0f * scale;

    if (dd && dd->font) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(dd->font, dd->font_size, &metrics);
        float metrics_height = (float)metrics.line_height + 10.0f * scale;
        if (metrics_height > height)
            height = metrics_height;
    }

    return height;
}

/// @brief Returns the baseline that vertically centers text within a row.
///
/// @param dd Dropdown supplying font metrics.
/// @param row_y Top coordinate of the row.
/// @param row_h Row height.
/// @return Baseline coordinate, or @p row_y when no font is configured.
static float dropdown_text_baseline(vg_dropdown_t *dd, float row_y, float row_h) {
    if (!dd || !dd->font)
        return row_y;

    vg_font_metrics_t metrics = {0};
    vg_font_get_metrics(dd->font, dd->font_size, &metrics);
    return row_y + (row_h + (float)metrics.ascent + (float)metrics.descent) / 2.0f;
}

/// @brief Measures text with the dropdown's current typography.
///
/// @param dd Dropdown supplying font and size.
/// @param text Text to measure.
/// @return Rendered width, a character-count estimate when no font is present,
///         or zero for empty input.
static float dropdown_measure_text_width(vg_dropdown_t *dd, const char *text) {
    if (!dd || !text || !text[0])
        return 0.0f;
    if (!dd->font)
        return (float)strlen(text) * (dd->font_size > 0.0f ? dd->font_size * 0.6f : 8.0f);

    vg_text_metrics_t metrics = {0};
    vg_font_measure_text(dd->font, dd->font_size, text, &metrics);
    return metrics.width;
}

/// @brief Computes the natural trigger width for all labels and the arrow gutter.
///
/// @param dd Dropdown whose items and placeholder are measured.
/// @return Preferred width in logical pixels.
static float dropdown_preferred_width(vg_dropdown_t *dd) {
    vg_theme_t *theme = vg_theme_get_current();
    float text_width = dropdown_measure_text_width(dd, dd ? dd->placeholder : NULL);
    if (dd) {
        for (int i = 0; i < dd->item_count; i++) {
            float item_width = dropdown_measure_text_width(dd, dd->items[i]);
            if (item_width > text_width)
                text_width = item_width;
        }
    }
    float padding = theme ? theme->input.padding_h : 10.0f;
    float gutter = (dd ? dd->arrow_size : 10.0f) + 24.0f;
    return text_width + padding * 2.0f + gutter;
}

/// @brief Resolves the overlay width from content and trigger geometry.
///
/// @param dd Dropdown whose content is measured.
/// @param trigger_width Current arranged trigger width.
/// @return Larger of preferred content width and @p trigger_width.
static float dropdown_panel_width(vg_dropdown_t *dd, float trigger_width) {
    float panel_width = dropdown_preferred_width(dd);
    if (panel_width < trigger_width)
        panel_width = trigger_width;
    return panel_width;
}

/// @brief Finds the next item beginning with a typeahead code point.
///
/// @details ASCII letters are compared case-insensitively, the first item
/// character is decoded as UTF-8, and the scan wraps through the full list.
///
/// @param dd Dropdown whose item labels are searched.
/// @param codepoint Character typed by the user.
/// @param start_index First index to inspect.
/// @return Matching index, or `-1` when no item matches.
static int dropdown_find_typeahead_index(vg_dropdown_t *dd, uint32_t codepoint, int start_index) {
    if (!dd || dd->item_count <= 0)
        return -1;

    uint32_t needle = codepoint;
    if (needle >= 'A' && needle <= 'Z')
        needle = needle - 'A' + 'a';

    for (int offset = 0; offset < dd->item_count; offset++) {
        int index = (start_index + offset) % dd->item_count;
        const char *item = dd->items[index];
        if (!item || !item[0])
            continue;
        const char *cursor = item;
        uint32_t first = vg_utf8_decode(&cursor);
        if (first >= 'A' && first <= 'Z')
            first = first - 'A' + 'a';
        if (first == needle)
            return index;
    }
    return -1;
}

/// @brief Publishes a selection transition when the index changed.
///
/// @param dd Dropdown whose selection was updated.
/// @param old_index Selection index before the update.
static void dropdown_emit_change(vg_dropdown_t *dd, int old_index) {
    if (!dd)
        return;
    if (old_index == dd->selected_index)
        return;
    vg_widget_note_change(&dd->base);
    if (dd->on_change)
        dd->on_change(
            &dd->base, dd->selected_index, vg_dropdown_get_selected_text(dd), dd->on_change_data);
}

/// @brief Retrieves root-ancestor screen bounds for overlay placement.
///
/// @param dd Dropdown whose widget ancestry is traversed.
/// @param[out] out_x Optional root screen X receiver.
/// @param[out] out_y Optional root screen Y receiver.
/// @param[out] out_w Optional root width receiver.
/// @param[out] out_h Optional root height receiver.
static void dropdown_get_viewport_bounds(
    vg_dropdown_t *dd, float *out_x, float *out_y, float *out_w, float *out_h) {
    vg_widget_t *root = &dd->base;
    while (root->parent)
        root = root->parent;
    vg_widget_get_screen_bounds(root, out_x, out_y, out_w, out_h);
}

/// @brief Resolves overlay height and vertical placement within the viewport.
///
/// @details The panel is capped by `dropdown_height` and flips above the trigger
/// when the upper side offers more space than the lower side.
///
/// @param dd Dropdown whose overlay is positioned.
/// @param abs_widget_x Absolute trigger X coordinate; currently unused.
/// @param abs_widget_y Absolute trigger Y coordinate.
/// @param[out] out_panel_top Optional absolute panel-top receiver.
/// @param[out] out_panel_h Optional panel-height receiver.
static void dropdown_resolve_panel_rect(vg_dropdown_t *dd,
                                        float abs_widget_x,
                                        float abs_widget_y,
                                        float *out_panel_top,
                                        float *out_panel_h) {
    float ih = dropdown_item_height(dd);
    float panel_h = ih * dd->item_count;
    if (panel_h > dd->dropdown_height)
        panel_h = dd->dropdown_height;

    float below_top = abs_widget_y + dd->base.height;
    float panel_top = below_top;

    float viewport_y = 0.0f;
    float viewport_h = 0.0f;
    dropdown_get_viewport_bounds(dd, NULL, &viewport_y, NULL, &viewport_h);
    if (viewport_h > 0.0f) {
        float viewport_bottom = viewport_y + viewport_h;
        float space_below = viewport_bottom - below_top;
        float space_above = abs_widget_y - viewport_y;
        if (space_below < panel_h && space_above > space_below) {
            panel_top = abs_widget_y - panel_h;
            if (panel_top < viewport_y)
                panel_top = viewport_y;
        }
    }

    (void)abs_widget_x;
    if (out_panel_top)
        *out_panel_top = panel_top;
    if (out_panel_h)
        *out_panel_h = panel_h;
}

/// @brief Report the open dropdown panel's absolute rectangle for damage tracking.
/// @details The trigger's arranged bounds are unioned by the widget core. This
///          callback contributes only the floating panel, using the same placement,
///          viewport-flip, width, and height calculations as paint and hit testing.
/// @param widget Dropdown widget being queried.
/// @param x Receives panel X, or zero when closed.
/// @param y Receives panel Y, or zero when closed.
/// @param width Receives panel width, or zero when closed.
/// @param height Receives panel height, or zero when closed.
static void dropdown_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height) {
    if (x)
        *x = 0.0f;
    if (y)
        *y = 0.0f;
    if (width)
        *width = 0.0f;
    if (height)
        *height = 0.0f;
    vg_dropdown_t *dd = (vg_dropdown_t *)widget;
    if (!dd || !dd->open || dd->item_count <= 0)
        return;

    float screen_x = 0.0f;
    float screen_y = 0.0f;
    vg_widget_get_screen_bounds(widget, &screen_x, &screen_y, NULL, NULL);
    float panel_y = 0.0f;
    float panel_h = 0.0f;
    dropdown_resolve_panel_rect(dd, screen_x, screen_y, &panel_y, &panel_h);
    if (x)
        *x = screen_x;
    if (y)
        *y = panel_y;
    if (width)
        *width = dropdown_panel_width(dd, widget->width);
    if (height)
        *height = panel_h;
}

/// @brief Hit-tests absolute coordinates against the open overlay panel.
///
/// @param dd Dropdown whose panel and scroll offset are queried.
/// @param screen_x Absolute pointer X coordinate.
/// @param screen_y Absolute pointer Y coordinate.
/// @param[out] index Optional receiver for the hit item, or `-1` when the point
///                   is in no item row.
/// @return `true` when the point lies inside the panel rectangle, even if no
///         item occupies that position; otherwise `false`.
static bool dropdown_panel_hit(vg_dropdown_t *dd, float screen_x, float screen_y, int *index) {
    float sx = 0.0f;
    float sy = 0.0f;
    vg_widget_get_screen_bounds(&dd->base, &sx, &sy, NULL, NULL);

    float ih = dropdown_item_height(dd);
    float panel_top = 0.0f;
    float panel_h = 0.0f;
    dropdown_resolve_panel_rect(dd, sx, sy, &panel_top, &panel_h);

    float panel_left = sx;
    float panel_right = panel_left + dropdown_panel_width(dd, dd->base.width);
    float panel_bottom = panel_top + panel_h;
    if (screen_x < panel_left || screen_x >= panel_right || screen_y < panel_top ||
        screen_y >= panel_bottom) {
        if (index)
            *index = -1;
        return false;
    }

    float rel_y = screen_y - panel_top + dd->scroll_y;
    int hit = rel_y >= 0.0f ? (int)(rel_y / ih) : -1;
    if (hit < 0 || hit >= dd->item_count)
        hit = -1;
    if (index)
        *index = hit;
    return true;
}

/// @brief Clamps the vertical scroll offset to the panel's content range.
///
/// @param dd Dropdown whose `scroll_y` is normalized.
/// @param panel_h Visible panel height.
static void dropdown_clamp_scroll(vg_dropdown_t *dd, float panel_h) {
    if (!dd)
        return;

    if (dd->scroll_y < 0.0f)
        dd->scroll_y = 0.0f;

    float max_scroll = dd->item_count * dropdown_item_height(dd) - panel_h;
    if (max_scroll < 0.0f)
        max_scroll = 0.0f;
    if (dd->scroll_y > max_scroll)
        dd->scroll_y = max_scroll;
}

/// @brief Computes a proportional scrollbar-thumb length.
///
/// @param track_size Scrollbar track length.
/// @param content_size Total scrollable content length.
/// @param viewport_size Visible content length.
/// @return Thumb length clamped between 18 pixels and the track length; invalid
///         geometry returns @p track_size.
static float dropdown_scrollbar_thumb_size(float track_size,
                                           float content_size,
                                           float viewport_size) {
    if (track_size <= 0.0f || content_size <= 0.0f || viewport_size <= 0.0f)
        return track_size;

    float thumb_size = track_size * (viewport_size / content_size);
    if (thumb_size < 18.0f)
        thumb_size = 18.0f;
    if (thumb_size > track_size)
        thumb_size = track_size;
    return thumb_size;
}

/// @brief Computes scrollbar-thumb offset from the current scroll position.
///
/// @param scroll_pos Current content scroll position.
/// @param scroll_range Maximum content scroll position.
/// @param track_size Scrollbar track length.
/// @param thumb_size Scrollbar thumb length.
/// @return Offset within the track, or zero when no travel is available.
static float dropdown_scrollbar_thumb_offset(float scroll_pos,
                                             float scroll_range,
                                             float track_size,
                                             float thumb_size) {
    float thumb_travel = track_size - thumb_size;
    if (scroll_range <= 0.0f || thumb_travel <= 0.0f)
        return 0.0f;
    return (scroll_pos / scroll_range) * thumb_travel;
}

/// @brief Adjusts scrolling so an item is fully visible.
///
/// @param dd Dropdown whose overlay is scrolled.
/// @param index Zero-based item index to reveal.
static void dropdown_scroll_to_index(vg_dropdown_t *dd, int index) {
    if (!dd || index < 0 || index >= dd->item_count)
        return;

    float sx = 0.0f;
    float sy = 0.0f;
    float panel_top = 0.0f;
    float panel_h = 0.0f;
    vg_widget_get_screen_bounds(&dd->base, &sx, &sy, NULL, NULL);
    dropdown_resolve_panel_rect(dd, sx, sy, &panel_top, &panel_h);
    dropdown_clamp_scroll(dd, panel_h);

    float ih = dropdown_item_height(dd);
    float item_top = index * ih;
    float item_bottom = item_top + ih;
    if (item_top < dd->scroll_y)
        dd->scroll_y = item_top;
    else if (item_bottom > dd->scroll_y + panel_h)
        dd->scroll_y = item_bottom - panel_h;
    dropdown_clamp_scroll(dd, panel_h);
}

/// @brief Opens the panel, initializes hover, and acquires input capture.
///
/// @param widget Dropdown base widget that receives input capture.
/// @param dd Dropdown state to open.
/// @param preferred_hover Preferred valid item index, or an invalid value to use
///                        the current selection or first item.
static void dropdown_open(vg_widget_t *widget, vg_dropdown_t *dd, int preferred_hover) {
    if (!dd || dd->open)
        return;

    dd->open = true;
    if (preferred_hover >= 0 && preferred_hover < dd->item_count)
        dd->hovered_index = preferred_hover;
    else if (dd->selected_index >= 0 && dd->selected_index < dd->item_count)
        dd->hovered_index = dd->selected_index;
    else if (dd->item_count > 0)
        dd->hovered_index = 0;
    else
        dd->hovered_index = -1;
    if (dd->hovered_index >= 0)
        dropdown_scroll_to_index(dd, dd->hovered_index);
    vg_widget_set_input_capture(widget);
    widget->needs_paint = true;
}

/// @brief Closes the panel and releases dropdown input capture.
///
/// @param widget Dropdown base widget that may own capture.
/// @param dd Dropdown state to close.
static void dropdown_close(vg_widget_t *widget, vg_dropdown_t *dd) {
    if (!dd || !dd->open)
        return;
    dd->open = false;
    dd->hovered_index = -1;
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    widget->needs_paint = true;
}

/// @brief Paints the dropdown trigger and its current label.
///
/// @details State-dependent colors cover disabled, hovered, focused, and open
/// states. Selected text or placeholder is clipped before the arrow gutter,
/// which receives a separate background and open-direction chevron.
///
/// @param widget Dropdown base widget to render.
/// @param canvas Destination drawing context.
static void dropdown_paint(vg_widget_t *widget, void *canvas) {
    vg_dropdown_t *dd = (vg_dropdown_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    int32_t x = (int32_t)widget->x;
    int32_t y = (int32_t)widget->y;
    int32_t w = (int32_t)widget->width;
    int32_t h = (int32_t)widget->height;
    int32_t gutter_w = (int32_t)(dd->arrow_size + 14.0f);

    uint32_t bg = dd->bg_color;
    uint32_t border = dd->border_color;
    uint32_t text = dd->text_color;

    if (!widget->enabled) {
        bg = theme->colors.bg_disabled;
        border = theme->colors.border_secondary;
        text = theme->colors.fg_disabled;
    } else {
        if (widget->state & VG_STATE_HOVERED)
            bg = vg_color_blend(bg, theme->colors.bg_hover, 0.3f);
        if (dd->open)
            bg = vg_color_blend(bg, theme->colors.bg_secondary, 0.25f);
        if (widget->state & VG_STATE_FOCUSED)
            border = theme->colors.border_focus;
        else if (widget->state & VG_STATE_HOVERED || dd->open)
            border = theme->colors.border_secondary;
    }
    uint32_t gutter_bg = widget->enabled ? vg_color_blend(bg, theme->colors.bg_tertiary, 0.55f)
                                         : theme->colors.bg_disabled;

    // Header box
    vgfx_fill_rect(win, x, y, w, h, bg);
    if (w > gutter_w)
        vgfx_fill_rect(win, x + w - gutter_w, y + 1, gutter_w - 1, h - 2, gutter_bg);
    if (w > gutter_w)
        vgfx_fill_rect(win, x + w - gutter_w, y + 2, 1, h - 4, theme->colors.border_secondary);
    if (w > 2)
        vgfx_fill_rect(win, x + 1, y + 1, w - 2, 1, vg_color_lighten(bg, 0.05f));
    vgfx_rect(win, x, y, w, h, border);
    if (w > 2)
        vgfx_fill_rect(win,
                       x + 1,
                       y + h - 2,
                       w - 2,
                       2,
                       dd->open ? theme->colors.accent_primary : vg_color_darken(border, 0.15f));

    // Selected text or placeholder
    const char *label = (dd->selected_index >= 0 && dd->selected_index < dd->item_count)
                            ? dd->items[dd->selected_index]
                            : dd->placeholder;
    if (label && dd->font) {
        int32_t clip_x = x + (int32_t)theme->input.padding_h;
        int32_t clip_y = y + 2;
        int32_t clip_w = w - gutter_w - (int32_t)theme->input.padding_h - 6;
        int32_t clip_h = h - 4;
        if (clip_w > 0 && clip_h > 0)
            vgfx_set_clip(win, clip_x, clip_y, clip_w, clip_h);
        vg_font_draw_text(canvas,
                          dd->font,
                          dd->font_size,
                          widget->x + theme->input.padding_h,
                          dropdown_text_baseline(dd, widget->y, widget->height),
                          label,
                          label == dd->placeholder ? theme->colors.fg_secondary : text);
        if (clip_w > 0 && clip_h > 0)
            vgfx_clear_clip(win);
    }

    // Arrow glyph
    float ax = widget->x + widget->width - dd->arrow_size - 8.0f;
    float ay = widget->y + widget->height / 2.0f;
    float as2 = dd->arrow_size / 2.0f;
    vgfx_line(win,
              (int32_t)(ax),
              (int32_t)(dd->open ? ay + as2 / 2 : ay - as2 / 2),
              (int32_t)(ax + as2),
              (int32_t)(dd->open ? ay - as2 / 2 : ay + as2 / 2),
              text);
    vgfx_line(win,
              (int32_t)(ax + as2),
              (int32_t)(dd->open ? ay - as2 / 2 : ay + as2 / 2),
              (int32_t)(ax + dd->arrow_size),
              (int32_t)(dd->open ? ay + as2 / 2 : ay - as2 / 2),
              text);
}

/// @brief Paints the floating item-list overlay above ordinary widgets.
///
/// @details The panel uses resolved screen placement, clips visible rows,
/// distinguishes hovered and selected items, and adds a proportional scrollbar
/// when content exceeds the configured panel height.
///
/// @param widget Dropdown base widget whose overlay is rendered.
/// @param canvas Destination drawing context.
static void dropdown_paint_overlay(vg_widget_t *widget, void *canvas) {
    vg_dropdown_t *dd = (vg_dropdown_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    if (!dd->open || dd->item_count <= 0)
        return;

    float sx = 0.0f;
    float sy = 0.0f;
    float sw = 0.0f;
    float sh = 0.0f;
    vg_widget_get_screen_bounds(&dd->base, &sx, &sy, &sw, &sh);

    float ih = dropdown_item_height(dd);
    float panel_top_abs = 0.0f;
    float panel_h = 0.0f;
    dropdown_resolve_panel_rect(dd, sx, sy, &panel_top_abs, &panel_h);

    int32_t px = (int32_t)sx;
    int32_t py = (int32_t)panel_top_abs;
    int32_t pw = (int32_t)dropdown_panel_width(dd, sw);
    int32_t ph = (int32_t)panel_h;
    int32_t scroll_track_w = 6;
    int32_t content_right_pad = 8;
    bool show_scrollbar = dd->item_count * ih > panel_h;

    dropdown_clamp_scroll(dd, panel_h);

    // Real soft drop shadow beneath the floating list (Refined Depth elevation).
    vg_elevation_t el = theme->elevation.level2;
    vg_draw_round_rect_shadow(win,
                              (float)px,
                              (float)py,
                              (float)pw,
                              (float)ph,
                              0.0f,
                              el.blur,
                              el.dx,
                              el.dy,
                              el.alpha,
                              theme->elevation.shadow_rgb);
    vgfx_fill_rect(win, px, py, pw, ph, dd->dropdown_bg);
    vgfx_rect(win, px, py, pw, ph, dd->border_color);
    if (pw > 2)
        vgfx_fill_rect(win, px + 1, py + 1, pw - 2, 1, theme->colors.accent_primary);

    int visible_count = (int)(panel_h / ih);
    int start_item = (int)(dd->scroll_y / ih);
    float scroll_offset = dd->scroll_y - (float)start_item * ih;
    if (start_item < 0)
        start_item = 0;

    int32_t row_clip_w = pw - 2 - content_right_pad - (show_scrollbar ? scroll_track_w + 6 : 0);
    int32_t row_clip_h = ph - 2;
    if (row_clip_w > 0 && row_clip_h > 0)
        vgfx_set_clip(win, px + 1, py + 1, row_clip_w, row_clip_h);

    for (int i = start_item; i < dd->item_count && i < start_item + visible_count + 1; i++) {
        float iy = (float)py - scroll_offset + (float)(i - start_item) * ih;
        int32_t iy32 = (int32_t)iy;
        uint32_t row_bg = (i == dd->hovered_index)
                              ? dd->hover_bg
                              : ((i == dd->selected_index)
                                     ? dd->selected_bg
                                     : (((i & 1) == 0) ? dd->dropdown_bg
                                                       : vg_color_blend(dd->dropdown_bg,
                                                                        theme->colors.bg_secondary,
                                                                        0.35f)));

        vgfx_fill_rect(win, px + 1, iy32, pw - 2, (int32_t)ih, row_bg);
        if (i == dd->selected_index)
            vgfx_fill_rect(win, px + 1, iy32, 3, (int32_t)ih, theme->colors.accent_primary);
        vgfx_fill_rect(
            win, px + 1, iy32 + (int32_t)ih - 1, pw - 2, 1, theme->colors.border_secondary);

        if (dd->items[i] && dd->font) {
            vg_font_draw_text(canvas,
                              dd->font,
                              dd->font_size,
                              (float)px + theme->input.padding_h,
                              dropdown_text_baseline(dd, iy, ih),
                              dd->items[i],
                              dd->text_color);
        }
    }

    if (row_clip_w > 0 && row_clip_h > 0)
        vgfx_clear_clip(win);

    if (show_scrollbar) {
        float track_x = (float)(px + pw - scroll_track_w - 3);
        float track_y = (float)(py + 3);
        float track_h = panel_h - 6.0f;
        float thumb_h = dropdown_scrollbar_thumb_size(track_h, dd->item_count * ih, panel_h);
        float scroll_range = dd->item_count * ih - panel_h;
        float thumb_y =
            track_y + dropdown_scrollbar_thumb_offset(dd->scroll_y, scroll_range, track_h, thumb_h);
        vgfx_fill_rect(win,
                       (int32_t)track_x,
                       (int32_t)track_y,
                       scroll_track_w,
                       (int32_t)track_h,
                       theme->colors.bg_secondary);
        vgfx_fill_rect(win,
                       (int32_t)track_x + 1,
                       (int32_t)thumb_y,
                       scroll_track_w - 2,
                       (int32_t)thumb_h,
                       theme->colors.bg_hover);
    }
}

/// @brief Handles panel toggling, selection, navigation, scrolling, and typeahead.
///
/// @param widget Dropdown base widget receiving the event.
/// @param event Event to inspect and mark handled where appropriate.
/// @return `true` when an enabled dropdown consumes the event; otherwise
///         `false`.
static bool dropdown_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_dropdown_t *dd = (vg_dropdown_t *)widget;

    if (!widget->enabled)
        return false;

    switch (event->type) {
        case VG_EVENT_CLICK: {
            if (!dd->open) {
                dropdown_open(widget, dd, dd->selected_index);
            } else {
                int idx = -1;
                if (dropdown_panel_hit(dd, event->mouse.screen_x, event->mouse.screen_y, &idx) &&
                    idx >= 0) {
                    vg_dropdown_set_selected(dd, idx);
                }
                dropdown_close(widget, dd);
            }
            event->handled = true;
            return true;
        }

        case VG_EVENT_MOUSE_MOVE:
            if (dd->open) {
                int old_hover = dd->hovered_index;
                if (!dropdown_panel_hit(
                        dd, event->mouse.screen_x, event->mouse.screen_y, &dd->hovered_index)) {
                    dd->hovered_index = -1;
                }
                if (old_hover != dd->hovered_index)
                    widget->needs_paint = true;
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_WHEEL:
            if (!dd->open)
                return false;
            dd->scroll_y -= event->wheel.delta_y * dropdown_item_height(dd);
            {
                float sx = 0.0f;
                float sy = 0.0f;
                float panel_top = 0.0f;
                float panel_h = 0.0f;
                vg_widget_get_screen_bounds(&dd->base, &sx, &sy, NULL, NULL);
                dropdown_resolve_panel_rect(dd, sx, sy, &panel_top, &panel_h);
                dropdown_clamp_scroll(dd, panel_h);
            }
            widget->needs_paint = true;
            event->handled = true;
            return true;

        case VG_EVENT_KEY_DOWN:
            if (!dd->open) {
                if (event->key.key == VG_KEY_ENTER || event->key.key == VG_KEY_SPACE ||
                    event->key.key == VG_KEY_DOWN || event->key.key == VG_KEY_UP) {
                    int preferred = dd->selected_index;
                    if (event->key.key == VG_KEY_UP && dd->item_count > 0 && preferred < 0)
                        preferred = dd->item_count - 1;
                    dropdown_open(widget, dd, preferred);
                    event->handled = true;
                    return true;
                }
                return false;
            }
            if (event->key.key == VG_KEY_ESCAPE) {
                dropdown_close(widget, dd);
                event->handled = true;
                return true;
            }
            if (event->key.key == VG_KEY_DOWN) {
                if (dd->hovered_index < dd->item_count - 1)
                    dd->hovered_index++;
                dropdown_scroll_to_index(dd, dd->hovered_index);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_UP) {
                if (dd->hovered_index > 0)
                    dd->hovered_index--;
                dropdown_scroll_to_index(dd, dd->hovered_index);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_HOME) {
                if (dd->item_count > 0)
                    dd->hovered_index = 0;
                dropdown_scroll_to_index(dd, dd->hovered_index);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_END) {
                if (dd->item_count > 0)
                    dd->hovered_index = dd->item_count - 1;
                dropdown_scroll_to_index(dd, dd->hovered_index);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_PAGE_DOWN) {
                int page = (int)(dd->dropdown_height / dropdown_item_height(dd));
                if (page < 1)
                    page = 1;
                dd->hovered_index += page;
                if (dd->hovered_index >= dd->item_count)
                    dd->hovered_index = dd->item_count - 1;
                dropdown_scroll_to_index(dd, dd->hovered_index);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_PAGE_UP) {
                int page = (int)(dd->dropdown_height / dropdown_item_height(dd));
                if (page < 1)
                    page = 1;
                dd->hovered_index -= page;
                if (dd->hovered_index < 0)
                    dd->hovered_index = 0;
                dropdown_scroll_to_index(dd, dd->hovered_index);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_ENTER) {
                if (dd->hovered_index >= 0 && dd->hovered_index < dd->item_count)
                    vg_dropdown_set_selected(dd, dd->hovered_index);
                dropdown_close(widget, dd);
                event->handled = true;
                return true;
            }
            return false;

        case VG_EVENT_KEY_CHAR: {
            int start =
                dd->open && dd->hovered_index >= 0 ? dd->hovered_index + 1 : dd->selected_index + 1;
            if (start < 0)
                start = 0;
            int found = dropdown_find_typeahead_index(dd, event->key.codepoint, start);
            if (found < 0)
                return false;
            if (dd->open) {
                dd->hovered_index = found;
                dropdown_scroll_to_index(dd, dd->hovered_index);
            } else {
                vg_dropdown_set_selected(dd, found);
            }
            widget->needs_paint = true;
            return true;
        }

        default:
            return false;
    }
}

/// @brief Reports whether the dropdown may receive keyboard focus.
///
/// @param widget Dropdown base widget to inspect.
/// @return `true` when enabled and visible; otherwise `false`.
static bool dropdown_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief Creates an empty dropdown with theme-derived defaults.
///
/// @details The initial item array holds eight entries, no item is selected,
/// and the floating panel height and arrow size follow the current UI scale.
///
/// @param parent Widget to receive the dropdown as a child; may be null.
/// @return Newly allocated dropdown, or null on allocation failure.
vg_dropdown_t *vg_dropdown_create(vg_widget_t *parent) {
    vg_dropdown_t *dropdown = calloc(1, sizeof(vg_dropdown_t));
    if (!dropdown)
        return NULL;
    vg_theme_t *theme = vg_theme_get_current();

    vg_widget_init(&dropdown->base, VG_WIDGET_DROPDOWN, &g_dropdown_vtable);
    dropdown->selected_index = -1;
    dropdown->hovered_index = -1;
    dropdown->item_capacity = 8;
    dropdown->items = calloc(dropdown->item_capacity, sizeof(char *));
    if (!dropdown->items) {
        vg_widget_destroy(&dropdown->base);
        return NULL;
    }

    // Default appearance
    dropdown->font = theme->typography.font_regular;
    dropdown->font_size = theme->typography.size_normal;
    dropdown->dropdown_height = 220.0f * (theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f);
    dropdown->arrow_size = 11.0f * (theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f);
    dropdown->bg_color = theme->colors.bg_primary;
    dropdown->text_color = theme->colors.fg_primary;
    dropdown->border_color = theme->colors.border_primary;
    dropdown->dropdown_bg = theme->colors.bg_secondary;
    dropdown->hover_bg = theme->colors.bg_hover;
    dropdown->selected_bg = theme->colors.bg_selected;

    if (parent) {
        vg_widget_add_child(parent, &dropdown->base);
    }

    return dropdown;
}

/// @brief Appends a copied item label to the dropdown.
///
/// @details The owned pointer array grows geometrically. Overflow or allocation
/// failure leaves the dropdown unchanged.
///
/// @param dropdown The dropdown to modify.
/// @param text Display text to copy.
/// @return Zero-based index of the new item, or `-1` on failure.
int vg_dropdown_add_item(vg_dropdown_t *dropdown, const char *text) {
    if (!dropdown || !text)
        return -1;

    // Grow array if needed
    if (dropdown->item_count >= dropdown->item_capacity) {
        if (dropdown->item_capacity <= 0 || dropdown->item_capacity > INT_MAX / 2)
            return -1;
        int new_cap = dropdown->item_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(char *))
            return -1;
        char **new_items = realloc(dropdown->items, new_cap * sizeof(char *));
        if (!new_items)
            return -1;
        dropdown->items = new_items;
        dropdown->item_capacity = new_cap;
    }

    dropdown->items[dropdown->item_count] = vg_strdup(text);
    if (!dropdown->items[dropdown->item_count])
        return -1;
    dropdown->base.needs_layout = true;
    dropdown->base.needs_paint = true;
    int index = dropdown->item_count++;
    vg_widget_note_revision(&dropdown->base);
    return index;
}

/// @brief Removes an item and adjusts selection and hover indices.
///
/// @details An open panel closes first. Removing the selected item clears
/// selection; removing an earlier item shifts the selected index. A resulting
/// selection change is published through the normal callback path.
///
/// @param dropdown The dropdown to modify.
/// @param index Zero-based index of the item to remove.
void vg_dropdown_remove_item(vg_dropdown_t *dropdown, int index) {
    if (!dropdown || index < 0 || index >= dropdown->item_count)
        return;

    int old_selected = dropdown->selected_index;

    if (dropdown->open)
        dropdown_close(&dropdown->base, dropdown);

    free(dropdown->items[index]);

    // Shift remaining items
    for (int i = index; i < dropdown->item_count - 1; i++) {
        dropdown->items[i] = dropdown->items[i + 1];
    }
    dropdown->item_count--;
    dropdown->items[dropdown->item_count] = NULL;

    // Adjust selected index
    if (dropdown->selected_index == index) {
        dropdown->selected_index = -1;
    } else if (dropdown->selected_index > index) {
        dropdown->selected_index--;
    }
    if (dropdown->hovered_index == index) {
        dropdown->hovered_index = -1;
    } else if (dropdown->hovered_index > index) {
        dropdown->hovered_index--;
    }
    dropdown_emit_change(dropdown, old_selected);
    if (old_selected == dropdown->selected_index)
        vg_widget_note_revision(&dropdown->base);
    dropdown->base.needs_layout = true;
    dropdown->base.needs_paint = true;
}

/// @brief Removes all items and resets selection, hover, and scrolling.
///
/// @param dropdown The dropdown to clear.
void vg_dropdown_clear(vg_dropdown_t *dropdown) {
    if (!dropdown)
        return;

    int old_selected = dropdown->selected_index;
    int old_count = dropdown->item_count;

    if (dropdown->open)
        dropdown_close(&dropdown->base, dropdown);

    for (int i = 0; i < dropdown->item_count; i++) {
        free(dropdown->items[i]);
        dropdown->items[i] = NULL;
    }
    dropdown->item_count = 0;
    dropdown->selected_index = -1;
    dropdown->hovered_index = -1;
    dropdown->scroll_y = 0.0f;
    dropdown_emit_change(dropdown, old_selected);
    if (old_count > 0 && old_selected == dropdown->selected_index)
        vg_widget_note_revision(&dropdown->base);
    dropdown->base.needs_layout = true;
    dropdown->base.needs_paint = true;
}

/// @brief Sets or clears the selected item.
///
/// @details Valid indices range from `-1` through the final item. A changed
/// selection publishes widget change state and invokes the registered callback.
///
/// @param dropdown The dropdown to update.
/// @param index Zero-based item index, or `-1` to clear selection.
void vg_dropdown_set_selected(vg_dropdown_t *dropdown, int index) {
    if (!dropdown)
        return;

    int old_index = dropdown->selected_index;
    if (index < -1 || index >= dropdown->item_count)
        return;
    if (old_index == index)
        return;
    if (index == -1) {
        dropdown->selected_index = -1;
    } else {
        dropdown->selected_index = index;
    }

    dropdown_emit_change(dropdown, old_index);
    dropdown->base.needs_paint = true;
}

/// @brief Returns the current selection index.
///
/// @param dropdown The dropdown to query.
/// @return Zero-based selected index, or `-1` for no selection or a null widget.
int vg_dropdown_get_selected(vg_dropdown_t *dropdown) {
    return dropdown ? dropdown->selected_index : -1;
}

/// @brief Returns the current selected item's display text.
///
/// @param dropdown The dropdown to query.
/// @return Borrowed string owned by @p dropdown, or null when nothing is
///         selected.
const char *vg_dropdown_get_selected_text(vg_dropdown_t *dropdown) {
    if (!dropdown || dropdown->selected_index < 0 ||
        dropdown->selected_index >= dropdown->item_count) {
        return NULL;
    }
    return dropdown->items[dropdown->selected_index];
}

/// @brief Sets the placeholder shown when no item is selected.
///
/// @details The input is copied before the previous placeholder is released,
/// preserving state on allocation failure.
///
/// @param dropdown The dropdown to configure.
/// @param text Placeholder string to copy; null removes it.
void vg_dropdown_set_placeholder(vg_dropdown_t *dropdown, const char *text) {
    if (!dropdown)
        return;
    if ((!dropdown->placeholder && (!text || text[0] == '\0')) ||
        (dropdown->placeholder && text && strcmp(dropdown->placeholder, text) == 0))
        return;
    char *copy = text ? vg_strdup(text) : NULL;
    if (text && !copy)
        return;
    free(dropdown->placeholder);
    dropdown->placeholder = copy;
    dropdown->base.needs_layout = true;
    dropdown->base.needs_paint = true;
}

/// @brief Sets the borrowed font and size used by trigger and overlay text.
///
/// @details A non-positive size selects the theme's normal size. Typography
/// changes invalidate both layout and paint.
///
/// @param dropdown The dropdown to configure.
/// @param font Borrowed font to use; may be null to disable text rendering.
/// @param size Positive font size in pixels, or a non-positive value for the
///             theme default.
void vg_dropdown_set_font(vg_dropdown_t *dropdown, vg_font_t *font, float size) {
    if (!dropdown)
        return;
    float font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    if (dropdown->font == font && dropdown->font_size == font_size)
        return;
    dropdown->font = font;
    dropdown->font_size = font_size;
    dropdown->base.needs_layout = true;
    dropdown->base.needs_paint = true;
}

/// @brief Registers the callback invoked after selection changes.
///
/// @param dropdown The dropdown to configure.
/// @param callback Function called with the widget, new index, selected text,
///                 and @p user_data; may be null to unregister.
/// @param user_data Opaque pointer passed to @p callback.
void vg_dropdown_set_on_change(vg_dropdown_t *dropdown,
                               vg_dropdown_callback_t callback,
                               void *user_data) {
    if (!dropdown)
        return;
    dropdown->on_change = callback;
    dropdown->on_change_data = user_data;
}
