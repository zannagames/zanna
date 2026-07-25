//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_breadcrumb.c
// Purpose: Breadcrumb navigation widget — renders a horizontal trail of clickable
//          path segments separated by a configurable separator string, with
//          optional per-item drop-down menus.
// Key invariants:
//   - items[] is a heap-allocated array of vg_breadcrumb_item_t; each item owns
//     its label, tooltip, and dropdown_items array (freed in free_breadcrumb_item).
//   - When max_items > 0, the oldest item (index 0) is evicted via memmove on
//     overflow to keep the visible trail bounded.
//   - dropdown_open/dropdown_index track the currently expanded drop-down; input
//     capture is held on the widget while the drop-down is visible.
//   - Separator string is heap-owned; replaced atomically by vg_breadcrumb_set_separator.
// Ownership/Lifetime:
//   - bc->items and bc->separator are heap-allocated and freed in breadcrumb_destroy.
//   - Item user_data is only freed if owns_user_data is true.
//   - The widget does not own the linked vg_font_t.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements the breadcrumb navigation widget and its optional item menus.
/// @details The widget owns copied segment labels, separator text, and drop-down labels, lays them
/// out from left to right, and captures pointer input while a drop-down is open. User payload
/// pointers and the configured font remain borrowed unless an item explicitly marks its payload
/// as owned.

//=============================================================================
// Forward Declarations
//=============================================================================

static void breadcrumb_destroy(vg_widget_t *widget);
static void breadcrumb_measure(vg_widget_t *widget, float available_width, float available_height);
static void breadcrumb_paint(vg_widget_t *widget, void *canvas);
static bool breadcrumb_handle_event(vg_widget_t *widget, vg_event_t *event);
static void breadcrumb_set_font_widget(vg_widget_t *widget, void *font, float size);
static void breadcrumb_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
static void breadcrumb_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
static float breadcrumb_outer_padding(const vg_breadcrumb_t *bc);
static bool breadcrumb_item_rect(
    const vg_breadcrumb_t *bc, int index, float *out_x, float *out_y, float *out_w, float *out_h);
static bool breadcrumb_dropdown_rect(
    const vg_breadcrumb_t *bc, float *out_x, float *out_y, float *out_w, float *out_h);
static int breadcrumb_find_dropdown_item_at(const vg_breadcrumb_t *bc, float px, float py);

//=============================================================================
// Breadcrumb VTable
//=============================================================================

static vg_widget_vtable_t g_breadcrumb_vtable = {.destroy = breadcrumb_destroy,
                                                 .measure = breadcrumb_measure,
                                                 .arrange = NULL,
                                                 .paint = breadcrumb_paint,
                                                 .handle_event = breadcrumb_handle_event,
                                                 .can_focus = NULL,
                                                 .on_focus = NULL,
                                                 .set_font = breadcrumb_set_font_widget};

//=============================================================================
// Breadcrumb Item Management
//=============================================================================

/// @brief Release every allocation owned by one breadcrumb item.
/// @param item Item whose label, tooltip, optionally owned payload, and drop-down labels are freed;
/// NULL is ignored.
static void free_breadcrumb_item(vg_breadcrumb_item_t *item) {
    if (!item)
        return;

    free(item->label);
    free(item->tooltip);
    if (item->owns_user_data)
        free(item->user_data);

    for (size_t i = 0; i < item->dropdown_count; i++) {
        free(item->dropdown_items[i].label);
    }
    free(item->dropdown_items);
}

/// @brief Fill a rounded rectangle through the shared drawing helper.
/// @param win Target graphics window.
/// @param x Left edge in pixels.
/// @param y Top edge in pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param radius Corner radius in pixels.
/// @param color Packed fill color.
static void breadcrumb_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_fill(win, (float)x, (float)y, (float)w, (float)h, (float)radius, color);
}

/// @brief Stroke a one-pixel rounded-rectangle border through the shared drawing helper.
/// @param win Target graphics window.
/// @param x Left edge in pixels.
/// @param y Top edge in pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param radius Corner radius in pixels.
/// @param color Packed stroke color.
static void breadcrumb_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_stroke(
        win, (float)x, (float)y, (float)w, (float)h, (float)radius, 1.0f, color);
}

/// @brief Return the outer horizontal padding used between the widget edge and the first item.
/// @param bc Breadcrumb whose separator padding supplies the spacing basis; NULL uses a fallback.
/// @return Horizontal outer padding in pixels.
static float breadcrumb_outer_padding(const vg_breadcrumb_t *bc) {
    if (!bc)
        return 4.0f;
    return (float)bc->separator_padding + 2.0f;
}

/// @brief Compute the rendered rectangle of one breadcrumb segment.
/// @details Widths preceding @p index include both item padding and measured separator runs.
/// @param bc Breadcrumb with a configured font and laid-out widget bounds.
/// @param index Zero-based segment index.
/// @param[out] out_x Optional destination for the left edge.
/// @param[out] out_y Optional destination for the top edge.
/// @param[out] out_w Optional destination for the width.
/// @param[out] out_h Optional destination for the height.
/// @return `true` when the requested segment is valid and measurable, otherwise `false`.
static bool breadcrumb_item_rect(
    const vg_breadcrumb_t *bc, int index, float *out_x, float *out_y, float *out_w, float *out_h) {
    float x = 0.0f;
    float item_y = 0.0f;
    float item_h = 0.0f;

    if (!bc || !bc->font || index < 0 || (size_t)index >= bc->item_count)
        return false;

    x = bc->base.x + breadcrumb_outer_padding(bc);
    item_y = bc->base.y + 4.0f;
    item_h = bc->base.height - 8.0f;
    if (item_h < 12.0f)
        item_h = bc->base.height;

    for (size_t i = 0; i < bc->item_count; i++) {
        vg_text_metrics_t metrics = {0};
        float item_width = 0.0f;
        vg_font_measure_text(bc->font, bc->font_size, bc->items[i].label, &metrics);
        item_width = metrics.width + bc->item_padding * 2;
        if ((int)i == index) {
            if (out_x)
                *out_x = x;
            if (out_y)
                *out_y = item_y;
            if (out_w)
                *out_w = item_width;
            if (out_h)
                *out_h = item_h;
            return true;
        }

        x += item_width;
        if (i < bc->item_count - 1 && bc->separator) {
            vg_font_measure_text(bc->font, bc->font_size, bc->separator, &metrics);
            x += metrics.width + bc->separator_padding * 2;
        }
    }

    return false;
}

/// @brief Compute the rectangle of the currently open drop-down panel.
/// @details The panel begins below its owning segment, is at least 160 pixels wide, and assigns a
/// row height based on the segment height with a 24-pixel minimum.
/// @param bc Breadcrumb whose open menu should be measured.
/// @param[out] out_x Optional destination for the left edge.
/// @param[out] out_y Optional destination for the top edge.
/// @param[out] out_w Optional destination for the width.
/// @param[out] out_h Optional destination for the height.
/// @return `true` when a valid open menu with at least one item exists.
static bool breadcrumb_dropdown_rect(
    const vg_breadcrumb_t *bc, float *out_x, float *out_y, float *out_w, float *out_h) {
    vg_breadcrumb_item_t *item = NULL;
    float crumb_x = 0.0f;
    float crumb_y = 0.0f;
    float crumb_w = 0.0f;
    float crumb_h = 0.0f;
    float dropdown_w = 0.0f;
    float dropdown_h = 0.0f;

    if (!bc || !bc->dropdown_open || bc->dropdown_index < 0 ||
        (size_t)bc->dropdown_index >= bc->item_count) {
        return false;
    }

    item = &bc->items[bc->dropdown_index];
    if (item->dropdown_count == 0 ||
        !breadcrumb_item_rect(bc, bc->dropdown_index, &crumb_x, &crumb_y, &crumb_w, &crumb_h)) {
        return false;
    }

    dropdown_w = crumb_w > 160.0f ? crumb_w : 160.0f;
    dropdown_h = (float)item->dropdown_count * (crumb_h > 24.0f ? crumb_h : 24.0f);

    if (out_x)
        *out_x = crumb_x;
    if (out_y)
        *out_y = crumb_y + crumb_h + 4.0f;
    if (out_w)
        *out_w = dropdown_w;
    if (out_h)
        *out_h = dropdown_h;
    return true;
}

/// @brief Hit-test the open drop-down at a pointer coordinate.
/// @param bc Breadcrumb owning the drop-down.
/// @param px Pointer x coordinate in widget space.
/// @param py Pointer y coordinate in widget space.
/// @return Zero-based drop-down row index, or `-1` outside a valid open panel.
static int breadcrumb_find_dropdown_item_at(const vg_breadcrumb_t *bc, float px, float py) {
    vg_breadcrumb_item_t *item = NULL;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float row_h = 0.0f;

    if (!bc || !breadcrumb_dropdown_rect(bc, &x, &y, &w, &h))
        return -1;

    item = &bc->items[bc->dropdown_index];
    row_h = item->dropdown_count > 0 ? h / (float)item->dropdown_count : 0.0f;
    if (row_h <= 0.0f || px < x || px >= x + w || py < y || py >= y + h)
        return -1;

    return (int)((py - y) / row_h);
}

//=============================================================================
// Breadcrumb Implementation
//=============================================================================

/// @brief Create a breadcrumb navigation widget with default theme colours.
///
/// @details Default separator is ">", item_padding and separator_padding are
///          derived from the current theme.  Attach a font with
///          vg_breadcrumb_set_font before painting.
///
/// @return Newly allocated vg_breadcrumb_t, or NULL on allocation failure.
vg_breadcrumb_t *vg_breadcrumb_create(void) {
    vg_breadcrumb_t *bc = calloc(1, sizeof(vg_breadcrumb_t));
    if (!bc)
        return NULL;

    vg_widget_init(&bc->base, VG_WIDGET_CUSTOM, &g_breadcrumb_vtable);

    vg_theme_t *theme = vg_theme_get_current();

    // Defaults
    bc->separator = vg_strdup(">");
    bc->item_padding = theme ? (uint32_t)(theme->spacing.sm + 4.0f) : 8;
    bc->separator_padding = theme ? (uint32_t)(theme->spacing.sm + 1.0f) : 4;
    bc->bg_color = 0;
    bc->text_color = 0;
    bc->hover_bg = 0;
    bc->separator_color = 0;

    bc->font_size = theme ? theme->typography.size_normal : 13.0f;
    bc->hovered_index = -1;
    bc->dropdown_index = -1;
    bc->dropdown_hovered = -1;

    return bc;
}

/// @brief Release breadcrumb-owned data during base-widget destruction.
/// @param widget Breadcrumb base widget being destroyed.
static void breadcrumb_destroy(vg_widget_t *widget) {
    vg_breadcrumb_t *bc = (vg_breadcrumb_t *)widget;

    for (size_t i = 0; i < bc->item_count; i++) {
        free_breadcrumb_item(&bc->items[i]);
    }
    free(bc->items);
    free(bc->separator);
}

/// @brief Destroy the breadcrumb widget, freeing all items and the separator string.
///
/// @param bc The breadcrumb to destroy; may be NULL.
void vg_breadcrumb_destroy(vg_breadcrumb_t *bc) {
    if (!bc)
        return;
    vg_widget_destroy(&bc->base);
}

/// @brief VTable measure: sums item widths including separators, claims available width, and sets a
/// fixed height from the theme input height.
/// @param widget Breadcrumb base widget whose measured dimensions are updated.
/// @param available_width Parent-provided width constraint; currently informational.
/// @param available_height Parent-provided height constraint; currently informational.
static void breadcrumb_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_breadcrumb_t *bc = (vg_breadcrumb_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    (void)available_width;
    (void)available_height;

    float width = breadcrumb_outer_padding(bc) * 2.0f;
    float height = 0;

    if (!bc->font) {
        widget->measured_width = 0;
        widget->measured_height = theme ? theme->input.height : 28.0f;
        return;
    }

    for (size_t i = 0; i < bc->item_count; i++) {
        vg_breadcrumb_item_t *item = &bc->items[i];

        vg_text_metrics_t metrics;
        vg_font_measure_text(bc->font, bc->font_size, item->label, &metrics);

        width += metrics.width + bc->item_padding * 2;
        if (metrics.height > height)
            height = metrics.height;

        // Add separator width
        if (i < bc->item_count - 1 && bc->separator) {
            vg_font_measure_text(bc->font, bc->font_size, bc->separator, &metrics);
            width += metrics.width + bc->separator_padding * 2;
        }
    }

    if (theme && height + theme->spacing.sm * 2.0f < theme->input.height) {
        height = theme->input.height - theme->spacing.sm * 2.0f;
    }
    widget->measured_width = width;
    widget->measured_height = height + (theme ? theme->spacing.sm * 2.0f : 8.0f);
}

/// @brief VTable paint: clips to the widget, draws each breadcrumb item with hover highlight,
/// separator arrows, overflow dropdown button, and open panel overlay.
/// @param widget Breadcrumb base widget to render.
/// @param canvas Graphics window used for primitives and text.
static void breadcrumb_paint(vg_widget_t *widget, void *canvas) {
    vg_breadcrumb_t *bc = (vg_breadcrumb_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    if (!bc->font || widget->width <= 0.0f || widget->height <= 0.0f)
        return;

    float outer_padding = breadcrumb_outer_padding(bc);
    float x = widget->x + outer_padding;
    float item_y = widget->y + 4.0f;
    float item_h = widget->height - 8.0f;
    if (item_h < 12.0f)
        item_h = widget->height;

    vg_font_metrics_t font_metrics = {0};
    vg_font_get_metrics(bc->font, bc->font_size, &font_metrics);
    int radius = theme ? (int)theme->button.border_radius : 6;
    if (radius < 4)
        radius = 4;

    uint32_t strip_bg =
        theme ? vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.45f)
              : 0x252526u;
    uint32_t strip_border = theme ? theme->colors.border_secondary : 0x3C3C3Cu;
    uint32_t strip_highlight =
        theme ? vg_color_lighten(strip_bg, 0.06f) : vg_color_lighten(strip_bg, 0.06f);
    uint32_t accent = theme ? theme->colors.accent_primary : strip_border;
    uint32_t text_color =
        bc->text_color ? bc->text_color : (theme ? theme->colors.fg_secondary : 0xD0D0D0u);
    uint32_t current_text = theme ? theme->colors.fg_primary : 0xFFFFFFu;
    uint32_t hover_bg =
        bc->hover_bg
            ? bc->hover_bg
            : (theme ? vg_color_blend(theme->colors.bg_hover, theme->colors.bg_selected, 0.25f)
                     : 0x2F3440u);
    uint32_t active_bg =
        theme ? vg_color_blend(theme->colors.bg_selected, theme->colors.accent_primary, 0.18f)
              : 0x334C73u;
    uint32_t separator_color =
        bc->separator_color ? bc->separator_color : (theme ? theme->colors.fg_tertiary : 0x8A8A8Au);

    breadcrumb_fill_round_rect(win,
                               (int32_t)widget->x,
                               (int32_t)widget->y,
                               (int32_t)widget->width,
                               (int32_t)widget->height,
                               radius,
                               bc->bg_color ? bc->bg_color : strip_bg);
    breadcrumb_stroke_round_rect(win,
                                 (int32_t)widget->x,
                                 (int32_t)widget->y,
                                 (int32_t)widget->width,
                                 (int32_t)widget->height,
                                 radius,
                                 strip_border);
    if (widget->width > radius * 2.0f) {
        vgfx_fill_rect(win,
                       (int32_t)(widget->x + radius),
                       (int32_t)(widget->y + 1.0f),
                       (int32_t)(widget->width - radius * 2.0f),
                       1,
                       strip_highlight);
        vgfx_fill_rect(win,
                       (int32_t)(widget->x + radius),
                       (int32_t)(widget->y + 1.0f),
                       (int32_t)(widget->width - radius * 2.0f),
                       2,
                       accent);
    }

    vgfx_set_clip(win,
                  (int32_t)widget->x,
                  (int32_t)widget->y,
                  (int32_t)widget->width,
                  (int32_t)widget->height);

    for (size_t i = 0; i < bc->item_count; i++) {
        vg_breadcrumb_item_t *item = &bc->items[i];

        vg_text_metrics_t metrics;
        vg_font_measure_text(bc->font, bc->font_size, item->label, &metrics);
        float item_width = metrics.width + bc->item_padding * 2;
        float baseline_y = item_y + (item_h - metrics.height) * 0.5f + font_metrics.ascent;
        bool is_current = i == bc->item_count - 1;
        bool is_hovered = (int)i == bc->hovered_index;

        if (is_current || is_hovered) {
            uint32_t pill_bg = is_current ? active_bg : hover_bg;
            if (is_current && is_hovered)
                pill_bg = vg_color_lighten(pill_bg, 0.08f);
            breadcrumb_fill_round_rect(win,
                                       (int32_t)x,
                                       (int32_t)item_y,
                                       (int32_t)item_width,
                                       (int32_t)item_h,
                                       radius - 2,
                                       pill_bg);
        }

        uint32_t crumb_text = is_current ? current_text : text_color;
        vg_font_draw_text(canvas,
                          bc->font,
                          bc->font_size,
                          x + bc->item_padding,
                          baseline_y,
                          item->label,
                          crumb_text);

        x += item_width;

        if (i < bc->item_count - 1 && bc->separator) {
            x += bc->separator_padding;
            float sep_y = item_y + (item_h - metrics.height) * 0.5f + font_metrics.ascent;
            vg_font_draw_text(
                canvas, bc->font, bc->font_size, x, sep_y, bc->separator, separator_color);
            vg_font_measure_text(bc->font, bc->font_size, bc->separator, &metrics);
            x += metrics.width + bc->separator_padding;
        }

        if (x > widget->x + widget->width)
            break;
    }
    vgfx_clear_clip(win);

    // Draw dropdown if open
    if (bc->dropdown_open && bc->dropdown_index >= 0 && bc->dropdown_index < (int)bc->item_count) {
        vg_breadcrumb_item_t *item = &bc->items[bc->dropdown_index];
        float dd_x = 0.0f;
        float dd_y = 0.0f;
        float dd_w = 0.0f;
        float dd_h = 0.0f;
        if (breadcrumb_dropdown_rect(bc, &dd_x, &dd_y, &dd_w, &dd_h)) {
            float row_h = item->dropdown_count > 0 ? dd_h / (float)item->dropdown_count : 0.0f;
            uint32_t dd_bg =
                theme ? vg_color_blend(theme->colors.bg_primary, theme->colors.bg_secondary, 0.22f)
                      : 0x1F2633u;
            uint32_t dd_border = theme ? theme->colors.border_primary : 0x3C4658u;
            uint32_t dd_hover = theme ? theme->colors.bg_hover : 0x2F3440u;

            breadcrumb_fill_round_rect(
                win, (int32_t)dd_x, (int32_t)dd_y, (int32_t)dd_w, (int32_t)dd_h, radius, dd_bg);
            breadcrumb_stroke_round_rect(
                win, (int32_t)dd_x, (int32_t)dd_y, (int32_t)dd_w, (int32_t)dd_h, radius, dd_border);
            vgfx_set_clip(win, (int32_t)dd_x, (int32_t)dd_y, (int32_t)dd_w, (int32_t)dd_h);

            for (size_t i = 0; i < item->dropdown_count; i++) {
                float row_y = dd_y + row_h * (float)i;
                vg_font_metrics_t dd_metrics = {0};
                vg_font_get_metrics(bc->font, bc->font_size, &dd_metrics);
                if ((int)i == bc->dropdown_hovered) {
                    vgfx_fill_rect(win,
                                   (int32_t)dd_x,
                                   (int32_t)row_y,
                                   (int32_t)dd_w,
                                   (int32_t)row_h,
                                   dd_hover);
                }
                vg_font_draw_text(canvas,
                                  bc->font,
                                  bc->font_size,
                                  dd_x + (float)bc->item_padding,
                                  row_y + (row_h - bc->font_size) * 0.5f + dd_metrics.ascent,
                                  item->dropdown_items[i].label,
                                  theme ? theme->colors.fg_primary : 0xFFFFFFu);
            }

            vgfx_clear_clip(win);
        }
    }
}

/// @brief Hit-test breadcrumb segments at a pointer coordinate.
/// @param bc Breadcrumb whose measured labels define the hit regions.
/// @param px Pointer x coordinate in widget space.
/// @param py Pointer y coordinate in widget space.
/// @return Zero-based segment index, or `-1` outside every segment.
static int find_item_at(vg_breadcrumb_t *bc, float px, float py) {
    if (!bc->font)
        return -1;
    if (py < bc->base.y || py > bc->base.y + bc->base.height)
        return -1;

    float x = bc->base.x + breadcrumb_outer_padding(bc);

    for (size_t i = 0; i < bc->item_count; i++) {
        vg_breadcrumb_item_t *item = &bc->items[i];

        vg_text_metrics_t metrics;
        vg_font_measure_text(bc->font, bc->font_size, item->label, &metrics);

        float item_width = metrics.width + bc->item_padding * 2;

        if (px >= x && px < x + item_width) {
            return (int)i;
        }

        x += item_width;

        // Skip separator
        if (i < bc->item_count - 1 && bc->separator) {
            vg_font_measure_text(bc->font, bc->font_size, bc->separator, &metrics);
            x += metrics.width + bc->separator_padding * 2;
        }
    }

    return -1;
}

/// @brief VTable handle_event: handles click on items and overflow dropdown, hover tracking, and
/// mouse-leave cleanup.
/// @param widget Breadcrumb base widget receiving the event.
/// @param event Pointer event to process.
/// @return `true` when the breadcrumb consumes the event, otherwise `false`.
static bool breadcrumb_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_breadcrumb_t *bc = (vg_breadcrumb_t *)widget;

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            if (bc->dropdown_open) {
                int dd_idx = breadcrumb_find_dropdown_item_at(bc, event->mouse.x, event->mouse.y);
                if (dd_idx != bc->dropdown_hovered) {
                    bc->dropdown_hovered = dd_idx;
                    bc->base.needs_paint = true;
                }
                if (dd_idx >= 0)
                    return true;
            }
            int idx = find_item_at(bc, event->mouse.x, event->mouse.y);
            if (idx != bc->hovered_index) {
                bc->hovered_index = idx;
                bc->base.needs_paint = true;
            }
            return true;
        }

        case VG_EVENT_MOUSE_LEAVE:
            bc->hovered_index = -1;
            bc->base.needs_paint = true;
            return true;

        case VG_EVENT_CLICK: {
            if (bc->dropdown_open) {
                int dd_idx = breadcrumb_find_dropdown_item_at(bc, event->mouse.x, event->mouse.y);
                if (dd_idx >= 0 && bc->dropdown_index >= 0 &&
                    (size_t)bc->dropdown_index < bc->item_count) {
                    if (bc->on_dropdown_select) {
                        bc->on_dropdown_select(bc, bc->dropdown_index, dd_idx, bc->user_data);
                    }
                    bc->dropdown_open = false;
                    bc->dropdown_hovered = -1;
                    if (vg_widget_get_input_capture() == widget)
                        vg_widget_release_input_capture();
                    bc->base.needs_paint = true;
                    return true;
                }
            }

            int idx = find_item_at(bc, event->mouse.x, event->mouse.y);
            if (idx >= 0) {
                vg_breadcrumb_item_t *item = &bc->items[idx];

                // Check if item has dropdown
                if (item->dropdown_count > 0) {
                    bc->dropdown_open = !(bc->dropdown_open && bc->dropdown_index == idx);
                    bc->dropdown_index = idx;
                    bc->dropdown_hovered = -1;
                    if (bc->dropdown_open)
                        vg_widget_set_input_capture(widget);
                    else if (vg_widget_get_input_capture() == widget)
                        vg_widget_release_input_capture();
                    bc->base.needs_paint = true;
                } else {
                    // Regular click
                    bc->dropdown_open = false;
                    if (vg_widget_get_input_capture() == widget)
                        vg_widget_release_input_capture();
                    if (bc->on_click) {
                        bc->on_click(bc, idx, bc->user_data);
                    }
                }
                return true;
            }

            if (bc->dropdown_open) {
                bc->dropdown_open = false;
                bc->dropdown_hovered = -1;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                bc->base.needs_paint = true;
                return true;
            }
            break;
        }

        default:
            break;
    }

    return false;
}

/// @brief Forward a generic widget font assignment to the typed breadcrumb API.
/// @param widget Breadcrumb base widget to configure.
/// @param font Borrowed @ref vg_font_t handle.
/// @param size Text size in pixels.
static void breadcrumb_set_font_widget(vg_widget_t *widget, void *font, float size) {
    if (!widget || !font)
        return;
    vg_breadcrumb_set_font((vg_breadcrumb_t *)widget, (vg_font_t *)font, size);
}

/// @brief Append a new item to the end of the breadcrumb trail.
///
/// @details If max_items is set and the trail is already at capacity, the oldest
///          item (index 0) is evicted before the new one is appended.
///
/// @param bc    The breadcrumb to update.
/// @param label Display text for the new segment; copied internally.
/// @param data  Arbitrary user pointer associated with the item (not freed by the widget).
void vg_breadcrumb_push(vg_breadcrumb_t *bc, const char *label, void *data) {
    if (!bc || !label)
        return;

    // Expand capacity if needed
    if (bc->item_count >= bc->item_capacity) {
        size_t new_cap = bc->item_capacity * 2;
        if (new_cap < 8)
            new_cap = 8;
        if (new_cap <= bc->item_capacity || new_cap > SIZE_MAX / sizeof(vg_breadcrumb_item_t))
            return;
        vg_breadcrumb_item_t *new_items =
            realloc(bc->items, new_cap * sizeof(vg_breadcrumb_item_t));
        if (!new_items)
            return;
        bc->items = new_items;
        bc->item_capacity = new_cap;
    }

    char *label_copy = vg_strdup(label);
    if (!label_copy)
        return;

    /* Enforce max_items: remove oldest (index 0) when limit exceeded */
    if (bc->max_items > 0 && (int)bc->item_count >= bc->max_items) {
        free_breadcrumb_item(&bc->items[0]);
        memmove(&bc->items[0], &bc->items[1], (bc->item_count - 1) * sizeof(vg_breadcrumb_item_t));
        bc->item_count--;
    }

    vg_breadcrumb_item_t *item = &bc->items[bc->item_count++];
    memset(item, 0, sizeof(vg_breadcrumb_item_t));
    item->label = label_copy;
    item->user_data = data;
    item->owns_user_data = false;

    bc->base.needs_layout = true;
    bc->base.needs_paint = true;
}

/// @brief Remove the last item from the breadcrumb trail and free its resources.
///
/// @param bc The breadcrumb to update; may be NULL or empty.
void vg_breadcrumb_pop(vg_breadcrumb_t *bc) {
    if (!bc || bc->item_count == 0)
        return;

    bc->item_count--;
    free_breadcrumb_item(&bc->items[bc->item_count]);

    bc->base.needs_layout = true;
    bc->base.needs_paint = true;
}

/// @brief Remove all items from the breadcrumb trail and close any open drop-down.
///
/// @param bc The breadcrumb to clear; may be NULL.
void vg_breadcrumb_clear(vg_breadcrumb_t *bc) {
    if (!bc)
        return;

    for (size_t i = 0; i < bc->item_count; i++) {
        free_breadcrumb_item(&bc->items[i]);
    }
    bc->item_count = 0;

    if (vg_widget_get_input_capture() == &bc->base)
        vg_widget_release_input_capture();
    bc->dropdown_open = false;
    bc->dropdown_index = -1;
    bc->dropdown_hovered = -1;
    bc->hovered_index = -1;

    bc->base.needs_layout = true;
    bc->base.needs_paint = true;
}

/// @brief Append a drop-down option to a breadcrumb item.
///
/// @details Items with at least one drop-down entry show a pop-over menu when
///          clicked instead of firing the on_click callback.
///
/// @param item  The breadcrumb item to extend; must not be NULL.
/// @param label Display text for the drop-down option; copied internally.
/// @param data  Arbitrary user pointer for the drop-down entry (not freed).
void vg_breadcrumb_item_add_dropdown(vg_breadcrumb_item_t *item, const char *label, void *data) {
    if (!item || !label)
        return;

    // Expand capacity if needed
    if (item->dropdown_count >= item->dropdown_capacity) {
        size_t new_cap = item->dropdown_capacity * 2;
        if (new_cap < 4)
            new_cap = 4;
        if (new_cap <= item->dropdown_capacity ||
            new_cap > SIZE_MAX / sizeof(vg_breadcrumb_dropdown_t))
            return;
        vg_breadcrumb_dropdown_t *new_items =
            realloc(item->dropdown_items, new_cap * sizeof(vg_breadcrumb_dropdown_t));
        if (!new_items)
            return;
        item->dropdown_items = new_items;
        item->dropdown_capacity = new_cap;
    }

    char *label_copy = vg_strdup(label);
    if (!label_copy)
        return;

    vg_breadcrumb_dropdown_t *dd = &item->dropdown_items[item->dropdown_count++];
    dd->label = label_copy;
    dd->data = data;
}

/// @brief Set the separator string drawn between breadcrumb segments.
///
/// @param bc  The breadcrumb to configure; may be NULL.
/// @param sep New separator text (e.g. "/" or ">"); NULL removes the separator.
///            The string is copied internally.
void vg_breadcrumb_set_separator(vg_breadcrumb_t *bc, const char *sep) {
    if (!bc)
        return;

    char *new_separator = sep ? vg_strdup(sep) : NULL;
    if (sep && !new_separator)
        return;

    free(bc->separator);
    bc->separator = new_separator;
    bc->base.needs_layout = true;
    bc->base.needs_paint = true;
}

/// @brief Register a callback invoked when a segment without a drop-down is clicked.
///
/// @param bc        The breadcrumb to configure; may be NULL.
/// @param callback  Called with (bc, item_index, user_data); NULL to unregister.
/// @param user_data Opaque pointer forwarded to callback.
void vg_breadcrumb_set_on_click(vg_breadcrumb_t *bc,
                                void (*callback)(vg_breadcrumb_t *, int, void *),
                                void *user_data) {
    if (!bc)
        return;
    bc->on_click = callback;
    bc->user_data = user_data;
}

/// @brief Set the font used to measure and render breadcrumb text.
///
/// @param bc   The breadcrumb to configure; may be NULL.
/// @param font The font to use; must outlive the widget.
/// @param size Point size for text rendering.
void vg_breadcrumb_set_font(vg_breadcrumb_t *bc, vg_font_t *font, float size) {
    if (!bc)
        return;

    bc->font = font;
    bc->font_size = size;
    bc->base.needs_layout = true;
    bc->base.needs_paint = true;
}

/// @brief Set the maximum number of items retained in the breadcrumb trail.
///
/// @details If the current item count exceeds max, the oldest items are evicted
///          immediately.  A value ≤ 0 disables the limit.
///
/// @param bc  The breadcrumb to configure; may be NULL.
/// @param max Maximum item count, or ≤ 0 for unlimited.
void vg_breadcrumb_set_max_items(vg_breadcrumb_t *bc, int max) {
    if (!bc)
        return;
    bc->max_items = max;
    /* Trim existing items if already over the limit */
    if (max > 0) {
        while ((int)bc->item_count > max && bc->item_count > 0) {
            free_breadcrumb_item(&bc->items[0]);
            memmove(
                &bc->items[0], &bc->items[1], (bc->item_count - 1) * sizeof(vg_breadcrumb_item_t));
            bc->item_count--;
        }
    }
    bc->base.needs_layout = true;
    bc->base.needs_paint = true;
}
