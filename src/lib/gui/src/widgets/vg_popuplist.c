//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_popuplist.c
// Purpose: Caret-anchored, filterable, keyboard-navigable popup list (exposed as
//          Zanna.GUI.PopupList). Renders in the overlay pass at an absolute anchor;
//          the host drives keyboard and visibility and supplies pre-ranked items.
// Key invariants:
//   - filtered[] holds the indices of items matching the (case-insensitive) filter;
//     selected is an index into filtered[] in [0, filtered_count).
//   - Each item string is heap-owned; freed on clear/destroy.
//   - The widget contributes no normal-flow size (measured 0); it paints only in the
//     overlay pass at (anchor_x, anchor_y) when visible.
// Links: lib/gui/include/vg_ide_widgets_ui.h, lib/gui/include/vg_font.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements the caret-anchored, filterable popup-list overlay.
/// @details The widget owns all candidate strings and a parallel array of
///          matching item indices. It contributes no normal-flow layout size;
///          explicit anchor coordinates and visual bounds describe its overlay.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_font.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward declarations
//=============================================================================

static void popuplist_measure(vg_widget_t *widget, float avail_w, float avail_h);
static void popuplist_arrange(vg_widget_t *widget, float x, float y, float w, float h);
static void popuplist_paint_overlay(vg_widget_t *widget, void *canvas);
static void popuplist_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height);
static void popuplist_destroy(vg_widget_t *widget);

static vg_widget_vtable_t g_popuplist_vtable = {
    .destroy = popuplist_destroy,
    .measure = popuplist_measure,
    .arrange = popuplist_arrange,
    .paint = NULL, // the popup only draws in the overlay pass
    .paint_overlay = popuplist_paint_overlay,
    .handle_event = NULL,
    .can_focus = NULL,
    .on_focus = NULL,
    .get_visual_bounds = popuplist_get_visual_bounds,
};

//=============================================================================
// Internal helpers
//=============================================================================

/// @brief Allocate a private copy of a popup-list string.
/// @param text Null-terminated source string.
/// @return Newly allocated copy, or `NULL` for null input/allocation failure.
static char *popuplist_dup(const char *text) {
    if (!text)
        return NULL;
    size_t n = strlen(text);
    char *copy = (char *)malloc(n + 1);
    if (copy)
        memcpy(copy, text, n + 1);
    return copy;
}

/// @brief Case-insensitive substring test; an empty/NULL needle always matches.
/// @param hay Candidate string to search.
/// @param needle Filter string.
/// @return `true` when @p needle is empty or occurs under bytewise ASCII-style
///         case folding.
static bool popuplist_contains_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0])
        return true;
    if (!hay)
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return true;
    }
    return false;
}

/// @brief Rebuild filtered[] from items + filter and clamp the selection.
/// @param list Popup list whose match-index array is regenerated.
static void popuplist_recompute(vg_popuplist_t *list) {
    list->filtered_count = 0;
    if (list->filtered) {
        for (int i = 0; i < list->item_count; i++) {
            if (popuplist_contains_ci(list->items[i], list->filter))
                list->filtered[list->filtered_count++] = i;
        }
    }
    if (list->filtered_count == 0)
        list->selected = 0;
    else if (list->selected >= list->filtered_count)
        list->selected = list->filtered_count - 1;
    else if (list->selected < 0)
        list->selected = 0;
}

/// @brief Grow items[] and filtered[] to hold at least @p min_cap entries.
/// @details Existing arrays remain valid when either allocation fails, though a
///          successful first resize may provide reusable excess capacity.
/// @param list Popup list owning both arrays.
/// @param min_cap Minimum required entry capacity.
/// @return `true` when both arrays can hold @p min_cap entries.
static bool popuplist_ensure_capacity(vg_popuplist_t *list, int min_cap) {
    if (min_cap <= list->item_capacity)
        return true;
    int new_cap = list->item_capacity > 0 ? list->item_capacity * 2 : 16;
    if (new_cap < min_cap)
        new_cap = min_cap;
    char **ni = (char **)realloc(list->items, (size_t)new_cap * sizeof(char *));
    if (!ni)
        return false;
    list->items = ni;
    int *nf = (int *)realloc(list->filtered, (size_t)new_cap * sizeof(int));
    if (!nf)
        return false;
    list->filtered = nf;
    list->item_capacity = new_cap;
    return true;
}

//=============================================================================
// VTable implementations
//=============================================================================

/// @brief Measure the absolute overlay as zero-sized normal-flow content.
/// @param widget Popup-list widget base to measure.
/// @param avail_w Offered width, unused.
/// @param avail_h Offered height, unused.
static void popuplist_measure(vg_widget_t *widget, float avail_w, float avail_h) {
    (void)avail_w;
    (void)avail_h;
    // Floats at an absolute anchor; contribute no normal-flow size.
    widget->measured_width = 0.0f;
    widget->measured_height = 0.0f;
    vg_widget_apply_constraints(widget);
}

/// @brief Store the layout rectangle assigned by the parent.
/// @param widget Popup-list widget base to arrange.
/// @param x Assigned left coordinate.
/// @param y Assigned top coordinate.
/// @param w Assigned width.
/// @param h Assigned height.
static void popuplist_arrange(vg_widget_t *widget, float x, float y, float w, float h) {
    widget->x = x;
    widget->y = y;
    widget->width = w;
    widget->height = h;
}

/// @brief Report the absolute rectangle occupied by the visible suggestion list.
/// @details Uses the same row limit and line-height fallback as overlay painting.
///          The list deliberately has zero normal-flow size, so this callback is
///          the authoritative retained-damage extent for its popup pixels.
/// @param widget Popup-list widget being queried.
/// @param x Receives popup anchor X, or zero when nothing can be painted.
/// @param y Receives popup anchor Y, or zero when nothing can be painted.
/// @param width Receives popup width, or zero when nothing can be painted.
/// @param height Receives visible rows times line height, or zero when empty.
static void popuplist_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height) {
    if (x)
        *x = 0.0f;
    if (y)
        *y = 0.0f;
    if (width)
        *width = 0.0f;
    if (height)
        *height = 0.0f;
    vg_popuplist_t *list = (vg_popuplist_t *)widget;
    if (!list || !widget->visible || list->filtered_count <= 0 || !list->font)
        return;
    int rows = list->filtered_count;
    if (list->max_rows > 0 && rows > list->max_rows)
        rows = list->max_rows;
    float line_height = list->line_height > 0.0f ? list->line_height : 18.0f;
    if (x)
        *x = list->anchor_x;
    if (y)
        *y = list->anchor_y;
    if (width)
        *width = list->width;
    if (height)
        *height = (float)rows * line_height;
}

/// @brief Paint the visible filtered rows at the absolute popup anchor.
/// @details Limits rows to `max_rows`, scrolls the logical window to retain the
///          selection, and draws the selected row with theme-derived colors.
/// @param widget Popup-list widget base owning overlay state.
/// @param canvas Backend canvas for rectangle and text drawing.
static void popuplist_paint_overlay(vg_widget_t *widget, void *canvas) {
    vg_popuplist_t *list = (vg_popuplist_t *)widget;
    if (!widget->visible || list->filtered_count == 0 || !list->font)
        return;
    vgfx_window_t win = (vgfx_window_t)canvas;

    float lh = list->line_height > 0.0f ? list->line_height : 18.0f;
    int rows = list->filtered_count;
    if (list->max_rows > 0 && rows > list->max_rows)
        rows = list->max_rows;

    int32_t x = (int32_t)list->anchor_x;
    int32_t y = (int32_t)list->anchor_y;
    int32_t w = (int32_t)list->width;
    int32_t h = (int32_t)((float)rows * lh);

    vgfx_fill_rect(win, x, y, w, h, list->bg_color);
    vgfx_rect(win, x, y, w, h, list->border_color);

    // Scroll so the selected row stays visible.
    int first = 0;
    if (list->selected >= rows)
        first = list->selected - rows + 1;

    vg_font_metrics_t fm = {0};
    vg_font_get_metrics(list->font, list->font_size, &fm);
    float baseline = (lh - (float)(fm.ascent - fm.descent)) * 0.5f + (float)fm.ascent;

    for (int r = 0; r < rows; r++) {
        int fi = first + r;
        if (fi >= list->filtered_count)
            break;
        int32_t ry = y + (int32_t)((float)r * lh);
        bool sel = (fi == list->selected);
        if (sel)
            vgfx_fill_rect(win, x, ry, w, (int32_t)lh, list->sel_bg_color);
        const char *text = list->items[list->filtered[fi]];
        if (text)
            vg_font_draw_text(canvas,
                              list->font,
                              list->font_size,
                              (float)x + 6.0f,
                              (float)ry + baseline,
                              text,
                              sel ? list->sel_fg_color : list->fg_color);
    }
}

/// @brief Release item strings, index storage, and the copied filter.
/// @param widget Popup-list widget base being destroyed.
static void popuplist_destroy(vg_widget_t *widget) {
    vg_popuplist_t *list = (vg_popuplist_t *)widget;
    for (int i = 0; i < list->item_count; i++)
        free(list->items[i]);
    free(list->items);
    free(list->filtered);
    free(list->filter);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Create a hidden popup list with theme-derived styling.
/// @param root Optional root widget that receives the popup as a child.
/// @return Newly allocated popup list, or `NULL` on allocation failure.
vg_popuplist_t *vg_popuplist_create(vg_widget_t *root) {
    vg_popuplist_t *list = (vg_popuplist_t *)calloc(1, sizeof(vg_popuplist_t));
    if (!list)
        return NULL;

    vg_widget_init(&list->base, VG_WIDGET_POPUPLIST, &g_popuplist_vtable);
    list->base.visible = false;
    list->width = 240.0f;
    list->max_rows = 10;

    vg_theme_t *theme = vg_theme_get_current();
    list->font = theme->typography.font_regular;
    list->font_size = theme->typography.size_normal;
    if (list->font && list->font_size > 0.0f) {
        vg_font_metrics_t m = {0};
        vg_font_get_metrics(list->font, list->font_size, &m);
        list->line_height = m.line_height > 0 ? (float)m.line_height : list->font_size * 1.4f;
    } else {
        list->line_height = 18.0f;
    }
    list->bg_color = theme->colors.bg_secondary;
    list->fg_color = theme->colors.fg_primary;
    list->sel_bg_color = theme->colors.accent_primary;
    list->sel_fg_color = theme->colors.bg_primary;
    list->border_color = theme->colors.border_primary;

    if (root)
        vg_widget_add_child(root, &list->base);

    return list;
}

/// @brief Destroy a popup list and all copied candidate strings.
/// @param list Popup list to destroy; `NULL` is ignored.
void vg_popuplist_destroy(vg_popuplist_t *list) {
    if (!list)
        return;
    vg_widget_destroy(&list->base);
}

/// @brief Append a copied candidate and recompute filter matches.
/// @param list Popup list to modify.
/// @param text Null-terminated display text copied by the list.
void vg_popuplist_add_item(vg_popuplist_t *list, const char *text) {
    if (!list || !text)
        return;
    if (!popuplist_ensure_capacity(list, list->item_count + 1))
        return;
    char *copy = popuplist_dup(text);
    if (!copy)
        return;
    list->items[list->item_count++] = copy;
    popuplist_recompute(list);
    list->base.needs_paint = true;
}

/// @brief Remove every candidate while retaining reusable array capacity.
/// @param list Popup list to clear; `NULL` is ignored.
void vg_popuplist_clear(vg_popuplist_t *list) {
    if (!list)
        return;
    for (int i = 0; i < list->item_count; i++)
        free(list->items[i]);
    list->item_count = 0;
    list->filtered_count = 0;
    list->selected = 0;
    list->base.needs_paint = true;
}

/// @brief Replace the case-insensitive substring filter.
/// @details The filter is copied, selection returns to the first matching row,
///          and the filtered-index array is rebuilt.
/// @param list Popup list to update.
/// @param filter Null-terminated filter text, or `NULL` to match all candidates.
void vg_popuplist_set_filter(vg_popuplist_t *list, const char *filter) {
    if (!list)
        return;
    free(list->filter);
    list->filter = popuplist_dup(filter);
    list->selected = 0;
    popuplist_recompute(list);
    list->base.needs_paint = true;
}

/// @brief Query the number of candidates matching the active filter.
/// @param list Popup list to inspect.
/// @return Filtered row count, or zero for `NULL`.
int vg_popuplist_visible_count(const vg_popuplist_t *list) {
    return list ? list->filtered_count : 0;
}

/// @brief Move selection one matching row upward without wrapping.
/// @param list Popup list to navigate.
void vg_popuplist_navigate_up(vg_popuplist_t *list) {
    if (!list || list->filtered_count == 0)
        return;
    if (list->selected > 0)
        list->selected--;
    list->base.needs_paint = true;
}

/// @brief Move selection one matching row downward without wrapping.
/// @param list Popup list to navigate.
void vg_popuplist_navigate_down(vg_popuplist_t *list) {
    if (!list || list->filtered_count == 0)
        return;
    if (list->selected < list->filtered_count - 1)
        list->selected++;
    list->base.needs_paint = true;
}

/// @brief Select a row by its index in the filtered view.
/// @param list Popup list to update.
/// @param index Requested filtered index, clamped to available matches.
void vg_popuplist_set_selected_index(vg_popuplist_t *list, int index) {
    if (!list || list->filtered_count == 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= list->filtered_count)
        index = list->filtered_count - 1;
    list->selected = index;
    list->base.needs_paint = true;
}

/// @brief Query the selected index within the filtered view.
/// @param list Popup list to inspect.
/// @return Selected filtered index, or -1 when no match exists.
int vg_popuplist_selected_index(const vg_popuplist_t *list) {
    if (!list || list->filtered_count == 0)
        return -1;
    return list->selected;
}

/// @brief Return the candidate text represented by the selected filtered row.
/// @param list Popup list to inspect.
/// @return Borrowed list-owned string, or `NULL` when selection is invalid.
const char *vg_popuplist_selected_text(const vg_popuplist_t *list) {
    if (!list || list->filtered_count == 0 || list->selected < 0 ||
        list->selected >= list->filtered_count)
        return NULL;
    return list->items[list->filtered[list->selected]];
}

/// @brief Latch an acceptance event when a matching row is selected.
/// @param list Popup list whose selected row is accepted.
void vg_popuplist_accept_selected(vg_popuplist_t *list) {
    if (list && list->filtered_count > 0)
        list->accepted = true;
}

/// @brief Consume the popup's edge-triggered acceptance latch.
/// @param list Popup list to query.
/// @return Previous acceptance state; the latch is cleared before return.
bool vg_popuplist_was_accepted(vg_popuplist_t *list) {
    if (!list)
        return false;
    bool accepted = list->accepted;
    list->accepted = false;
    return accepted;
}

/// @brief Set the overlay's absolute top-left anchor.
/// @param list Popup list to position.
/// @param x Absolute horizontal coordinate.
/// @param y Absolute vertical coordinate.
void vg_popuplist_anchor_at(vg_popuplist_t *list, float x, float y) {
    if (!list)
        return;
    list->anchor_x = x;
    list->anchor_y = y;
    list->base.needs_paint = true;
}

/// @brief Set a positive popup width.
/// @param list Popup list to resize.
/// @param width Requested width in pixels; non-positive values are ignored.
void vg_popuplist_set_width(vg_popuplist_t *list, float width) {
    if (list && width > 0.0f) {
        list->width = width;
        list->base.needs_paint = true;
    }
}

/// @brief Limit the number of rows painted at once.
/// @param list Popup list to configure.
/// @param max_rows Positive visible-row limit.
void vg_popuplist_set_max_rows(vg_popuplist_t *list, int max_rows) {
    if (list && max_rows > 0) {
        list->max_rows = max_rows;
        list->base.needs_paint = true;
    }
}

/// @brief Set the borrowed font and recompute row height.
/// @param list Popup list to configure.
/// @param font Font that must outlive its use by the list.
/// @param size Positive font size, or non-positive to retain the current size.
void vg_popuplist_set_font(vg_popuplist_t *list, vg_font_t *font, float size) {
    if (!list)
        return;
    list->font = font;
    list->font_size = size > 0.0f ? size : list->font_size;
    if (font && list->font_size > 0.0f) {
        vg_font_metrics_t m = {0};
        vg_font_get_metrics(font, list->font_size, &m);
        if (m.line_height > 0)
            list->line_height = (float)m.line_height;
    }
    list->base.needs_paint = true;
}

/// @brief Show or hide popup overlay painting.
/// @param list Popup list to update.
/// @param visible `true` to expose the filtered rows.
void vg_popuplist_set_visible(vg_popuplist_t *list, bool visible) {
    if (!list)
        return;
    list->base.visible = visible;
    list->base.needs_paint = true;
}

/// @brief Query the popup widget's visibility flag.
/// @param list Popup list to inspect.
/// @return Current visibility, or `false` for `NULL`.
bool vg_popuplist_is_visible(const vg_popuplist_t *list) {
    return list ? list->base.visible : false;
}
