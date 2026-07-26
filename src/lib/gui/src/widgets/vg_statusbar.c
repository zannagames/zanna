//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_statusbar.c
// Purpose: Horizontal status bar with three independent item zones (LEFT,
//          CENTER, RIGHT), supporting text, buttons, progress bars, separators,
//          and spacers.
// Key invariants:
//   - Items in each zone are stored in an independently-allocated pointer array
//     that doubles from INITIAL_ITEM_CAPACITY (8) when exhausted.
//   - hovered_item is NULLed in remove/clear before the item is freed so there
//     is never a dangling pointer in the hover state.
//   - Progress values are clamped to [0.0, 1.0] in vg_statusbar_item_set_progress.
//   - All items are owned by the status bar and freed in statusbar_destroy.
// Ownership/Lifetime:
//   - vg_statusbar_item_t instances are heap-allocated by create_item and owned
//     by the status bar. Callers receive handles but must not free them directly.
//   - item->text and item->tooltip are strdup'd and freed by free_item.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements three-zone status-bar layout, heterogeneous item
///        ownership, painting, hit testing, and deferred retirement.
/// @details Left and center zones lay out forward while the right zone lays out
///          in reverse so high-priority edge items survive constrained widths.
///          Shared layout metrics keep painting and button hit testing aligned.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_icon_vector.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Constants
//=============================================================================

#define INITIAL_ITEM_CAPACITY 8
#define STATUSBAR_DEFAULT_HEIGHT 24
#define STATUSBAR_ITEM_PADDING 8
#define STATUSBAR_SEPARATOR_WIDTH 1
#define VG_STATUSBAR_ITEM_MAGIC UINT64_C(0x5653475354415449)
#define VG_STATUSBAR_ITEM_RETIRED_MAGIC UINT64_C(0x5653475354524554)

//=============================================================================
// Forward Declarations
//=============================================================================

static void statusbar_destroy(vg_widget_t *widget);
static void statusbar_measure(vg_widget_t *widget, float available_width, float available_height);
static void statusbar_arrange(vg_widget_t *widget, float x, float y, float width, float height);
static void statusbar_paint(vg_widget_t *widget, void *canvas);
static bool statusbar_handle_event(vg_widget_t *widget, vg_event_t *event);

typedef struct statusbar_zone_metrics {
    float natural_width;
    float fixed_width;
    size_t visible_count;
    size_t spacer_count;
} statusbar_zone_metrics_t;

typedef struct statusbar_layout {
    float left_start;
    float left_width;
    float center_start;
    float center_width;
    float right_start;
    float right_width;
} statusbar_layout_t;

//=============================================================================
// StatusBar VTable
//=============================================================================

static vg_widget_vtable_t g_statusbar_vtable = {.destroy = statusbar_destroy,
                                                .measure = statusbar_measure,
                                                .arrange = statusbar_arrange,
                                                .paint = statusbar_paint,
                                                .handle_event = statusbar_handle_event,
                                                .can_focus = NULL,
                                                .on_focus = NULL};

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Grow *items to fit at least needed entries, doubling from INITIAL_ITEM_CAPACITY.
/// @param items Address of the owned item-pointer array.
/// @param capacity Address of its current element capacity.
/// @param needed Minimum capacity required.
/// @return `true` when the array can hold @p needed entries; `false` with the
///         previous allocation valid on overflow or allocation failure.
static bool ensure_item_capacity(vg_statusbar_item_t ***items, size_t *capacity, size_t needed) {
    if (needed <= *capacity)
        return true;

    size_t new_capacity = *capacity == 0 ? INITIAL_ITEM_CAPACITY : *capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2)
            return false;
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(vg_statusbar_item_t *))
        return false;

    vg_statusbar_item_t **new_items = realloc(*items, new_capacity * sizeof(vg_statusbar_item_t *));
    if (!new_items)
        return false;

    *items = new_items;
    *capacity = new_capacity;
    return true;
}

/// @brief Free an item's text, tooltip, and the item struct itself.
/// @param item Owned live or retired item to destroy; `NULL` is ignored.
static void free_item(vg_statusbar_item_t *item) {
    if (!item)
        return;
    if (item->text)
        free(item->text);
    if (item->tooltip)
        free(item->tooltip);
    free(item);
}

/// @brief Validate that a status-bar item remains attached to an owner.
/// @param item Item handle to inspect.
/// @return `true` when live magic and an owning status bar are present.
bool vg_statusbar_item_is_live(const vg_statusbar_item_t *item) {
    return item && item->magic == VG_STATUSBAR_ITEM_MAGIC && item->owner != NULL;
}

/// @brief Tear down @p item's owned strings and park it on @p sb's retired
///        list for deferred freeing (so removal during event dispatch can't
///        free memory still referenced up the call stack).
/// @param sb Status bar receiving the retired record.
/// @param item Detached item whose callbacks and ownership are revoked.
static void retire_item(vg_statusbar_t *sb, vg_statusbar_item_t *item) {
    if (!sb || !item)
        return;
    free(item->text);
    item->text = NULL;
    free(item->tooltip);
    item->tooltip = NULL;
    item->owner = NULL;
    item->on_click = NULL;
    item->user_data = NULL;
    item->magic = VG_STATUSBAR_ITEM_RETIRED_MAGIC;
    item->retired_next = sb->retired_items;
    sb->retired_items = item;
}

/// @brief Drain and free every status-bar item on @p sb's retired list.
/// @param sb Status bar whose retirement chain is destroyed.
static void free_retired_items(vg_statusbar_t *sb) {
    if (!sb)
        return;
    vg_statusbar_item_t *item = sb->retired_items;
    while (item) {
        vg_statusbar_item_t *next = item->retired_next;
        item->retired_next = NULL;
        free_item(item);
        item = next;
    }
    sb->retired_items = NULL;
}

/// @brief Allocate and zero-initialise a status bar item of the given type with optional text.
/// @param type Item behavior and rendering type.
/// @param text Optional label copied by the item.
/// @return Detached live item ready for zone insertion, or `NULL` on failure.
static vg_statusbar_item_t *create_item(vg_statusbar_item_type_t type, const char *text) {
    vg_statusbar_item_t *item = calloc(1, sizeof(vg_statusbar_item_t));
    if (!item)
        return NULL;

    item->type = type;
    item->magic = VG_STATUSBAR_ITEM_MAGIC;
    item->owner = NULL;
    item->text = text ? vg_strdup(text) : NULL;
    if (text && !item->text) {
        free(item);
        return NULL;
    }
    item->tooltip = NULL;
    item->min_width = 0;
    item->max_width = 0;
    item->visible = true;
    item->icon_vector_id = -1;
    item->progress = 0.0f;
    item->user_data = NULL;
    item->on_click = NULL;

    return item;
}

/// @brief Return the pixel width of item, honouring min_width, max_width, and text measurement.
/// @param sb Status bar supplying font metrics.
/// @param item Visible item to measure.
/// @return Natural or constrained item width; flexible spacers return zero.
static float measure_item_width(vg_statusbar_t *sb, vg_statusbar_item_t *item) {
    if (!item->visible)
        return 0;

    switch (item->type) {
        case VG_STATUSBAR_ITEM_SEPARATOR:
            return STATUSBAR_SEPARATOR_WIDTH + STATUSBAR_ITEM_PADDING;

        case VG_STATUSBAR_ITEM_SPACER:
            return 0; // Spacers expand to fill

        case VG_STATUSBAR_ITEM_PROGRESS:
            return item->min_width > 0 ? item->min_width : 60;

        case VG_STATUSBAR_ITEM_TEXT:
        case VG_STATUSBAR_ITEM_BUTTON:
            if (sb->font && item->text) {
                vg_text_metrics_t metrics;
                vg_font_measure_text(sb->font, sb->font_size, item->text, &metrics);
                float width = metrics.width + STATUSBAR_ITEM_PADDING * 2;
                if (item->icon_vector_id >= 0)
                    width += sb->font_size + 4.0f; // leading vector glyph + gap
                if (item->min_width > 0 && width < item->min_width)
                    width = item->min_width;
                if (item->max_width > 0 && width > item->max_width)
                    width = item->max_width;
                return width;
            }
            return item->min_width > 0 ? item->min_width : 40;

        default:
            return 40;
    }
}

/// @brief Back @p len up to a UTF-8 codepoint boundary before copying a prefix.
/// @param text UTF-8 byte string containing @p len as a valid index.
/// @param len Candidate prefix byte count.
/// @return Greatest safe prefix no longer than @p len.
static size_t statusbar_utf8_prev_boundary(const char *text, size_t len) {
    while (len > 0 && (((unsigned char)text[len] & 0xC0u) == 0x80u))
        len--;
    return len;
}

/// @brief Return a heap copy of @p text fitted to @p max_width with "..." if needed.
/// @param sb Status bar supplying font metrics.
/// @param text Null-terminated source text.
/// @param max_width Maximum rendered width.
/// @return Newly allocated full, ellipsized, or empty string; `NULL` on failure.
static char *statusbar_fit_text(vg_statusbar_t *sb, const char *text, float max_width) {
    if (!text || !sb->font || max_width <= 0.0f)
        return vg_strdup("");

    vg_text_metrics_t metrics;
    vg_font_measure_text(sb->font, sb->font_size, text, &metrics);
    if (metrics.width <= max_width)
        return vg_strdup(text);

    vg_text_metrics_t ellipsis_metrics;
    vg_font_measure_text(sb->font, sb->font_size, "...", &ellipsis_metrics);
    if (ellipsis_metrics.width > max_width)
        return vg_strdup("");

    size_t len = strlen(text);
    char *buf = (char *)malloc(len + 4); /* original text + "...\0" */
    if (!buf)
        return NULL;

    while (len > 0) {
        memcpy(buf, text, len);
        memcpy(buf + len, "...", 4);
        vg_font_measure_text(sb->font, sb->font_size, buf, &metrics);
        if (metrics.width <= max_width)
            return buf;
        len--;
        len = statusbar_utf8_prev_boundary(text, len);
    }

    memcpy(buf, "...", 4);
    return buf;
}

/// @brief Measure visible items in a zone, separating fixed items from flexible spacers.
/// @param sb Status bar supplying item metrics.
/// @param items Zone item array.
/// @param count Number of array entries.
/// @return Aggregate natural width and visibility/spacer counts.
static statusbar_zone_metrics_t statusbar_measure_zone(vg_statusbar_t *sb,
                                                       vg_statusbar_item_t **items,
                                                       size_t count) {
    statusbar_zone_metrics_t metrics = {0};
    for (size_t i = 0; i < count; i++) {
        vg_statusbar_item_t *item = items[i];
        if (!item || !item->visible)
            continue;
        metrics.visible_count++;
        if (item->type == VG_STATUSBAR_ITEM_SPACER) {
            metrics.spacer_count++;
        } else {
            metrics.fixed_width += measure_item_width(sb, item);
        }
    }
    metrics.natural_width = metrics.fixed_width;
    return metrics;
}

/// @brief Width assigned to each flexible spacer in a zone.
/// @param metrics Aggregate fixed width and spacer count.
/// @param zone_width Width allocated to the complete zone.
/// @return Equal flexible width per spacer, or zero when no excess remains.
static float statusbar_spacer_width(statusbar_zone_metrics_t metrics, float zone_width) {
    if (metrics.spacer_count == 0 || zone_width <= metrics.fixed_width)
        return 0.0f;
    return (zone_width - metrics.fixed_width) / (float)metrics.spacer_count;
}

/// @brief Compute non-overlapping local or screen-space rectangles for all zones.
/// @param sb Status bar whose zones are measured.
/// @param origin_x Horizontal origin in the caller's coordinate space.
/// @param width Total available status-bar width.
/// @param layout Receives left, centered, and right-aligned zone rectangles.
static void statusbar_compute_layout(vg_statusbar_t *sb,
                                     float origin_x,
                                     float width,
                                     statusbar_layout_t *layout) {
    if (!layout)
        return;
    memset(layout, 0, sizeof(*layout));

    float inner_start = origin_x + sb->item_padding;
    float inner_end = origin_x + width - sb->item_padding;
    if (inner_end < inner_start)
        inner_end = inner_start;
    float inner_width = inner_end - inner_start;
    float gap = sb->item_padding;
    if (inner_width < gap * 2.0f)
        gap = 0.0f;

    statusbar_zone_metrics_t center =
        statusbar_measure_zone(sb, sb->center_items, sb->center_count);
    statusbar_zone_metrics_t right = statusbar_measure_zone(sb, sb->right_items, sb->right_count);

    float center_width = center.natural_width;
    if (center_width > inner_width)
        center_width = inner_width;
    if (center.visible_count == 0)
        center_width = 0.0f;

    float center_start = origin_x + width / 2.0f - center_width / 2.0f;
    if (center_start < inner_start)
        center_start = inner_start;
    if (center_start + center_width > inner_end)
        center_start = inner_end - center_width;
    if (center_start < inner_start)
        center_start = inner_start;

    float center_end = center_start + center_width;
    bool has_center = center_width > 0.0f && center.visible_count > 0;

    float right_available_start = has_center ? center_end + gap : inner_start;
    if (right_available_start > inner_end)
        right_available_start = inner_end;
    float right_available_width = inner_end - right_available_start;
    if (right_available_width < 0.0f)
        right_available_width = 0.0f;

    float right_width = right.natural_width;
    if (right_width > right_available_width)
        right_width = right_available_width;
    if (right.visible_count == 0)
        right_width = 0.0f;
    float right_start = inner_end - right_width;

    float left_end = has_center ? center_start - gap : right_start - gap;
    if (left_end < inner_start)
        left_end = inner_start;
    if (left_end > inner_end)
        left_end = inner_end;

    layout->left_start = inner_start;
    layout->left_width = left_end - inner_start;
    layout->center_start = center_start;
    layout->center_width = center_width;
    layout->right_start = right_start;
    layout->right_width = right_width;
}

//=============================================================================
// StatusBar Implementation
//=============================================================================

/// @brief Create a status bar widget with empty left, center, and right item zones.
///
/// @details Seeds colours and font size from the current theme. Default height is
///          STATUSBAR_DEFAULT_HEIGHT. Attaches to parent if non-NULL.
///
/// @param parent Optional parent widget; may be NULL.
/// @return       Heap-allocated status bar, or NULL on allocation failure.
vg_statusbar_t *vg_statusbar_create(vg_widget_t *parent) {
    vg_statusbar_t *sb = calloc(1, sizeof(vg_statusbar_t));
    if (!sb)
        return NULL;

    // Initialize base widget
    vg_widget_init(&sb->base, VG_WIDGET_STATUSBAR, &g_statusbar_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize arrays
    sb->left_items = NULL;
    sb->left_count = 0;
    sb->left_capacity = 0;

    sb->center_items = NULL;
    sb->center_count = 0;
    sb->center_capacity = 0;

    sb->right_items = NULL;
    sb->right_count = 0;
    sb->right_capacity = 0;

    // Styling — scale pixel constants by ui_scale for HiDPI displays.
    float s = theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    sb->height = (int)(STATUSBAR_DEFAULT_HEIGHT * s);
    sb->item_padding = (int)(STATUSBAR_ITEM_PADDING * s);
    sb->separator_width = STATUSBAR_SEPARATOR_WIDTH;

    // Font
    sb->font = NULL;
    sb->font_size = theme->typography.size_small;

    // Colors from theme
    sb->bg_color = theme->colors.bg_secondary;
    sb->text_color = theme->colors.fg_secondary;
    sb->hover_color = theme->colors.bg_hover;
    sb->border_color = theme->colors.border_secondary;

    // State
    sb->hovered_item = NULL;

    // Set minimum size
    sb->base.constraints.min_height = (float)sb->height;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &sb->base);
    }

    return sb;
}

/// @brief vtable destroy — frees all items in all three zones and their backing arrays.
/// @param widget StatusBar base widget being destroyed.
static void statusbar_destroy(vg_widget_t *widget) {
    vg_statusbar_t *sb = (vg_statusbar_t *)widget;

    // Free left items
    for (size_t i = 0; i < sb->left_count; i++) {
        free_item(sb->left_items[i]);
    }
    free(sb->left_items);

    // Free center items
    for (size_t i = 0; i < sb->center_count; i++) {
        free_item(sb->center_items[i]);
    }
    free(sb->center_items);

    // Free right items
    for (size_t i = 0; i < sb->right_count; i++) {
        free_item(sb->right_items[i]);
    }
    free(sb->right_items);
    free_retired_items(sb);
}

/// @brief vtable measure — fills available_width; height is the fixed sb->height field.
/// @param widget StatusBar base widget whose measured dimensions are updated.
/// @param available_width Parent-provided horizontal space.
/// @param available_height Parent-provided height; intentionally ignored.
static void statusbar_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_statusbar_t *sb = (vg_statusbar_t *)widget;
    (void)available_height;

    widget->measured_width = available_width > 0 ? available_width : 400;
    widget->measured_height = (float)sb->height;
}

/// @brief vtable arrange — stores the assigned bounds directly (no children to lay out).
/// @param widget StatusBar base widget to position.
/// @param x Assigned X origin.
/// @param y Assigned Y origin.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
static void statusbar_arrange(vg_widget_t *widget, float x, float y, float width, float height) {
    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;
}

/// @brief Draw a single status bar item (text, button, progress, separator, or spacer).
/// @param sb StatusBar supplying theme, font, and hover state.
/// @param win Backend window receiving primitive drawing commands.
/// @param item Visible item to draw.
/// @param x Left edge of the item's constrained slot.
/// @param item_width Width available to the item.
/// @param text_y Baseline Y coordinate for item text.
/// @param canvas Backend canvas forwarded to font drawing.
static void statusbar_draw_item(vg_statusbar_t *sb,
                                vgfx_window_t win,
                                vg_statusbar_item_t *item,
                                float x,
                                float item_width,
                                float text_y,
                                void *canvas) {
    float wy = sb->base.y, wh = (float)sb->height;

    // Draw hover background for buttons
    if (item->type == VG_STATUSBAR_ITEM_BUTTON && item == sb->hovered_item &&
        item_width > 2.0f) {
        vg_draw_round_rect_fill(win,
                                x + 1.0f,
                                wy + 2.0f,
                                item_width - 2.0f,
                                wh - 4.0f,
                                vg_theme_get_current()->radius.sm,
                                sb->hover_color);
    }

    switch (item->type) {
        case VG_STATUSBAR_ITEM_TEXT:
        case VG_STATUSBAR_ITEM_BUTTON:
            if (item->text) {
                uint32_t text_color = item->has_text_color ? item->text_color : sb->text_color;
                float content_x = x + sb->item_padding;
                float text_width = item_width - sb->item_padding * 2.0f;
                if (item->icon_vector_id >= 0) {
                    float icon_sz = sb->font_size;
                    float icon_y = wy + (wh - icon_sz) / 2.0f;
                    vg_icon_vector_draw(win,
                                        item->icon_vector_id,
                                        (int32_t)(content_x + 0.5f),
                                        (int32_t)(icon_y + 0.5f),
                                        (int32_t)(icon_sz + 0.5f),
                                        text_color);
                    content_x += icon_sz + 4.0f;
                    text_width -= icon_sz + 4.0f;
                }
                char *fitted = statusbar_fit_text(sb, item->text, text_width);
                if (fitted && fitted[0] != '\0') {
                    vg_font_draw_text(canvas,
                                      sb->font,
                                      sb->font_size,
                                      content_x,
                                      text_y,
                                      fitted,
                                      text_color);
                }
                free(fitted);
            }
            break;

        case VG_STATUSBAR_ITEM_SEPARATOR: {
            // Vertical separator centered within the item
            int32_t sx = (int32_t)(x + item_width / 2.0f);
            vgfx_fill_rect(win, sx, (int32_t)(wy + 3), 1, (int32_t)(wh - 6), sb->border_color);
            break;
        }

        case VG_STATUSBAR_ITEM_PROGRESS: {
            float pw = item_width;
            if (pw <= 0.0f)
                break;
            float bar_h = 4.0f;
            float bar_y = wy + (wh - bar_h) / 2.0f;
            // Track background
            vgfx_fill_rect(
                win, (int32_t)x, (int32_t)bar_y, (int32_t)pw, (int32_t)bar_h, 0x00404040);
            // Progress fill
            float fill_w = item->progress * pw;
            if (fill_w > 0.0f) {
                vgfx_fill_rect(
                    win, (int32_t)x, (int32_t)bar_y, (int32_t)fill_w, (int32_t)bar_h, 0x000078D4);
            }
            break;
        }

        default:
            break;
    }
}

/// @brief Draw a left-to-right zone, truncating later pixels to @p zone_width.
/// @param sb StatusBar supplying metrics and drawing state.
/// @param win Backend window receiving primitives.
/// @param items Zone item array in logical order.
/// @param count Number of entries in @p items.
/// @param zone_start Left edge of the constrained zone.
/// @param zone_width Available zone width.
/// @param text_y Baseline Y coordinate for text.
/// @param canvas Backend canvas forwarded to font drawing.
static void statusbar_draw_zone_forward(vg_statusbar_t *sb,
                                        vgfx_window_t win,
                                        vg_statusbar_item_t **items,
                                        size_t count,
                                        float zone_start,
                                        float zone_width,
                                        float text_y,
                                        void *canvas) {
    if (zone_width <= 0.0f)
        return;

    statusbar_zone_metrics_t metrics = statusbar_measure_zone(sb, items, count);
    float spacer_width = statusbar_spacer_width(metrics, zone_width);
    float x = zone_start;
    float remaining = zone_width;

    for (size_t i = 0; i < count && remaining > 0.0f; i++) {
        vg_statusbar_item_t *item = items[i];
        if (!item || !item->visible)
            continue;

        float item_width =
            item->type == VG_STATUSBAR_ITEM_SPACER ? spacer_width : measure_item_width(sb, item);
        if (item_width > remaining)
            item_width = remaining;
        if (item_width > 0.0f && item->type != VG_STATUSBAR_ITEM_SPACER)
            statusbar_draw_item(sb, win, item, x, item_width, text_y, canvas);
        x += item_width;
        remaining -= item_width;
    }
}

/// @brief Draw a right-to-left zone so rightmost status items remain visible first.
/// @param sb StatusBar supplying metrics and drawing state.
/// @param win Backend window receiving primitives.
/// @param items Zone item array in logical order.
/// @param count Number of entries in @p items.
/// @param zone_start Left edge of the constrained zone.
/// @param zone_width Available zone width.
/// @param text_y Baseline Y coordinate for text.
/// @param canvas Backend canvas forwarded to font drawing.
static void statusbar_draw_zone_reverse(vg_statusbar_t *sb,
                                        vgfx_window_t win,
                                        vg_statusbar_item_t **items,
                                        size_t count,
                                        float zone_start,
                                        float zone_width,
                                        float text_y,
                                        void *canvas) {
    if (zone_width <= 0.0f)
        return;

    statusbar_zone_metrics_t metrics = statusbar_measure_zone(sb, items, count);
    float spacer_width = statusbar_spacer_width(metrics, zone_width);
    float x = zone_start + zone_width;
    float remaining = zone_width;

    for (size_t i = count; i > 0 && remaining > 0.0f; i--) {
        vg_statusbar_item_t *item = items[i - 1];
        if (!item || !item->visible)
            continue;

        float item_width =
            item->type == VG_STATUSBAR_ITEM_SPACER ? spacer_width : measure_item_width(sb, item);
        if (item_width > remaining)
            item_width = remaining;
        x -= item_width;
        if (item_width > 0.0f && item->type != VG_STATUSBAR_ITEM_SPACER)
            statusbar_draw_item(sb, win, item, x, item_width, text_y, canvas);
        remaining -= item_width;
    }
}

/// @brief vtable paint — fills background, then paints constrained, non-overlapping zones.
/// @param widget Arranged StatusBar base widget to render.
/// @param canvas Backend canvas used for font and primitive drawing.
static void statusbar_paint(vg_widget_t *widget, void *canvas) {
    vg_statusbar_t *sb = (vg_statusbar_t *)widget;

    if (!sb->font)
        return;

    vgfx_window_t win = (vgfx_window_t)canvas;
    float wx = widget->x, wy = widget->y, ww = widget->width, wh = widget->height;

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(sb->font, sb->font_size, &font_metrics);

    // Draw background
    vgfx_fill_rect(win, (int32_t)wx, (int32_t)wy, (int32_t)ww, (int32_t)wh, sb->bg_color);

    // Draw top border (1px separator line)
    vgfx_fill_rect(win, (int32_t)wx, (int32_t)wy, (int32_t)ww, 1, sb->border_color);

    float text_y = wy + (wh - font_metrics.line_height) / 2.0f + font_metrics.ascent;

    statusbar_layout_t layout;
    statusbar_compute_layout(sb, wx, ww, &layout);

    statusbar_draw_zone_forward(sb,
                                win,
                                sb->left_items,
                                sb->left_count,
                                layout.left_start,
                                layout.left_width,
                                text_y,
                                canvas);
    statusbar_draw_zone_reverse(sb,
                                win,
                                sb->right_items,
                                sb->right_count,
                                layout.right_start,
                                layout.right_width,
                                text_y,
                                canvas);
    statusbar_draw_zone_forward(sb,
                                win,
                                sb->center_items,
                                sb->center_count,
                                layout.center_start,
                                layout.center_width,
                                text_y,
                                canvas);
}

/// @brief Hit-test a left-to-right zone using the same constrained widths as paint.
/// @param sb StatusBar supplying item metrics.
/// @param items Zone item array in logical order.
/// @param count Number of entries in @p items.
/// @param zone_start Left edge of the constrained zone.
/// @param zone_width Available zone width.
/// @param mouse_x Local horizontal pointer coordinate.
/// @return Visible button item under the coordinate, or NULL.
static vg_statusbar_item_t *statusbar_find_in_zone_forward(vg_statusbar_t *sb,
                                                          vg_statusbar_item_t **items,
                                                          size_t count,
                                                          float zone_start,
                                                          float zone_width,
                                                          float mouse_x) {
    if (zone_width <= 0.0f || mouse_x < zone_start || mouse_x >= zone_start + zone_width)
        return NULL;

    statusbar_zone_metrics_t metrics = statusbar_measure_zone(sb, items, count);
    float spacer_width = statusbar_spacer_width(metrics, zone_width);
    float x = zone_start;
    float remaining = zone_width;

    for (size_t i = 0; i < count && remaining > 0.0f; i++) {
        vg_statusbar_item_t *item = items[i];
        if (!item || !item->visible)
            continue;

        float item_width =
            item->type == VG_STATUSBAR_ITEM_SPACER ? spacer_width : measure_item_width(sb, item);
        if (item_width > remaining)
            item_width = remaining;
        if (item_width > 0.0f && item->type == VG_STATUSBAR_ITEM_BUTTON &&
            mouse_x >= x && mouse_x < x + item_width)
            return item;

        x += item_width;
        remaining -= item_width;
    }
    return NULL;
}

/// @brief Hit-test a right-to-left zone so invisible overflow does not steal clicks.
/// @param sb StatusBar supplying item metrics.
/// @param items Zone item array in logical order.
/// @param count Number of entries in @p items.
/// @param zone_start Left edge of the constrained zone.
/// @param zone_width Available zone width.
/// @param mouse_x Local horizontal pointer coordinate.
/// @return Visible button item under the coordinate, or NULL.
static vg_statusbar_item_t *statusbar_find_in_zone_reverse(vg_statusbar_t *sb,
                                                          vg_statusbar_item_t **items,
                                                          size_t count,
                                                          float zone_start,
                                                          float zone_width,
                                                          float mouse_x) {
    if (zone_width <= 0.0f || mouse_x < zone_start || mouse_x >= zone_start + zone_width)
        return NULL;

    statusbar_zone_metrics_t metrics = statusbar_measure_zone(sb, items, count);
    float spacer_width = statusbar_spacer_width(metrics, zone_width);
    float x = zone_start + zone_width;
    float remaining = zone_width;

    for (size_t i = count; i > 0 && remaining > 0.0f; i--) {
        vg_statusbar_item_t *item = items[i - 1];
        if (!item || !item->visible)
            continue;

        float item_width =
            item->type == VG_STATUSBAR_ITEM_SPACER ? spacer_width : measure_item_width(sb, item);
        if (item_width > remaining)
            item_width = remaining;
        x -= item_width;
        if (item_width > 0.0f && item->type == VG_STATUSBAR_ITEM_BUTTON &&
            mouse_x >= x && mouse_x < x + item_width)
            return item;

        remaining -= item_width;
    }
    return NULL;
}

/// @brief Return the item whose local rect contains (mouse_x, mouse_y), or NULL.
/// @param sb StatusBar whose three zones are hit-tested.
/// @param mouse_x Local horizontal pointer coordinate.
/// @param mouse_y Local vertical pointer coordinate.
/// @return Visible button item under the point, or NULL.
static vg_statusbar_item_t *find_item_at(vg_statusbar_t *sb, float mouse_x, float mouse_y) {
    if (mouse_y < 0.0f || mouse_y >= sb->base.height)
        return NULL;

    statusbar_layout_t layout;
    statusbar_compute_layout(sb, 0.0f, sb->base.width, &layout);

    vg_statusbar_item_t *item = statusbar_find_in_zone_forward(
        sb, sb->left_items, sb->left_count, layout.left_start, layout.left_width, mouse_x);
    if (item)
        return item;

    item = statusbar_find_in_zone_reverse(
        sb, sb->right_items, sb->right_count, layout.right_start, layout.right_width, mouse_x);
    if (item)
        return item;

    return statusbar_find_in_zone_forward(sb,
                                          sb->center_items,
                                          sb->center_count,
                                          layout.center_start,
                                          layout.center_width,
                                          mouse_x);
}

/// @brief vtable handle_event — tracks hover and fires on_click for button items.
/// @param widget StatusBar base widget receiving the event.
/// @param event Mutable GUI event to interpret.
/// @return True when a clickable item consumes the event.
static bool statusbar_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_statusbar_t *sb = (vg_statusbar_t *)widget;

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            vg_statusbar_item_t *item = find_item_at(sb, event->mouse.x, event->mouse.y);
            if (item != sb->hovered_item) {
                sb->hovered_item = item;
                widget->needs_paint = true;
            }
            return false; // Don't consume
        }

        case VG_EVENT_MOUSE_LEAVE:
            if (sb->hovered_item) {
                sb->hovered_item = NULL;
                widget->needs_paint = true;
            }
            return false;

        case VG_EVENT_CLICK: {
            vg_statusbar_item_t *item = find_item_at(sb, event->mouse.x, event->mouse.y);
            if (item && item->on_click) {
                item->on_click(item, item->user_data);
                return true;
            }
            return false;
        }

        default:
            return false;
    }
}

//=============================================================================
// StatusBar API
//=============================================================================

/// @brief Append item to the given zone's array, growing capacity if needed; sets item->owner.
/// @param sb Status bar that takes ownership.
/// @param zone Destination left, center, or right zone.
/// @param item Detached item to append.
/// @return Attached item handle, or `NULL` after freeing it on failure.
static vg_statusbar_item_t *add_item_to_zone(vg_statusbar_t *sb,
                                             vg_statusbar_zone_t zone,
                                             vg_statusbar_item_t *item) {
    vg_statusbar_item_t ***items;
    size_t *count;
    size_t *capacity;

    switch (zone) {
        case VG_STATUSBAR_ZONE_LEFT:
            items = &sb->left_items;
            count = &sb->left_count;
            capacity = &sb->left_capacity;
            break;
        case VG_STATUSBAR_ZONE_CENTER:
            items = &sb->center_items;
            count = &sb->center_count;
            capacity = &sb->center_capacity;
            break;
        case VG_STATUSBAR_ZONE_RIGHT:
            items = &sb->right_items;
            count = &sb->right_count;
            capacity = &sb->right_capacity;
            break;
        default:
            return NULL;
    }

    if (*count == SIZE_MAX || !ensure_item_capacity(items, capacity, *count + 1)) {
        free_item(item);
        return NULL;
    }

    (*items)[*count] = item;
    (*count)++;
    item->owner = sb;

    sb->base.needs_layout = true;
    sb->base.needs_paint = true;
    return item;
}

/// @brief Append a static text label to the given zone.
///
/// @param sb   The status bar to append to; may be NULL (returns NULL).
/// @param zone Target zone (VG_STATUSBAR_ZONE_LEFT/CENTER/RIGHT).
/// @param text Display text; copied internally, may be NULL.
/// @return     New item handle, or NULL on allocation failure.
vg_statusbar_item_t *vg_statusbar_add_text(vg_statusbar_t *sb,
                                           vg_statusbar_zone_t zone,
                                           const char *text) {
    if (!sb)
        return NULL;

    vg_statusbar_item_t *item = create_item(VG_STATUSBAR_ITEM_TEXT, text);
    if (!item)
        return NULL;

    return add_item_to_zone(sb, zone, item);
}

/// @brief Append a clickable button item to the given zone.
///
/// @param sb        The status bar to append to; may be NULL (returns NULL).
/// @param zone      Target zone.
/// @param text      Button label; copied internally.
/// @param on_click  Callback invoked on click with (item, user_data); may be NULL.
/// @param user_data Opaque pointer forwarded to on_click.
/// @return          New item handle, or NULL on allocation failure.
vg_statusbar_item_t *vg_statusbar_add_button(vg_statusbar_t *sb,
                                             vg_statusbar_zone_t zone,
                                             const char *text,
                                             void (*on_click)(vg_statusbar_item_t *, void *),
                                             void *user_data) {
    if (!sb)
        return NULL;

    vg_statusbar_item_t *item = create_item(VG_STATUSBAR_ITEM_BUTTON, text);
    if (!item)
        return NULL;

    item->on_click = on_click;
    item->user_data = user_data;

    return add_item_to_zone(sb, zone, item);
}

/// @brief Append a progress bar item with default min_width 60 px to the given zone.
///
/// @param sb   The status bar to append to; may be NULL (returns NULL).
/// @param zone Target zone.
/// @return     New item handle, or NULL on allocation failure.
vg_statusbar_item_t *vg_statusbar_add_progress(vg_statusbar_t *sb, vg_statusbar_zone_t zone) {
    if (!sb)
        return NULL;

    vg_statusbar_item_t *item = create_item(VG_STATUSBAR_ITEM_PROGRESS, NULL);
    if (!item)
        return NULL;

    item->min_width = 60;
    return add_item_to_zone(sb, zone, item);
}

/// @brief Append a thin vertical separator line to the given zone.
///
/// @param sb   The status bar to append to; may be NULL (returns NULL).
/// @param zone Target zone.
/// @return     New item handle, or NULL on allocation failure.
vg_statusbar_item_t *vg_statusbar_add_separator(vg_statusbar_t *sb, vg_statusbar_zone_t zone) {
    if (!sb)
        return NULL;

    vg_statusbar_item_t *item = create_item(VG_STATUSBAR_ITEM_SEPARATOR, NULL);
    if (!item)
        return NULL;

    return add_item_to_zone(sb, zone, item);
}

/// @brief Append a flexible spacer that expands to fill available zone width.
///
/// @param sb   The status bar to append to; may be NULL (returns NULL).
/// @param zone Target zone.
/// @return     New item handle, or NULL on allocation failure.
vg_statusbar_item_t *vg_statusbar_add_spacer(vg_statusbar_t *sb, vg_statusbar_zone_t zone) {
    if (!sb)
        return NULL;

    vg_statusbar_item_t *item = create_item(VG_STATUSBAR_ITEM_SPACER, NULL);
    if (!item)
        return NULL;

    return add_item_to_zone(sb, zone, item);
}

/// @brief Remove and free an item from whichever zone it belongs to.
///
/// @details Searches all three zones. Uses memmove to close the gap. Clears
///          hovered_item before freeing to prevent a dangling hover reference.
///
/// @param sb   The status bar; may be NULL (no-op).
/// @param item The item to remove; may be NULL (no-op).
void vg_statusbar_remove_item(vg_statusbar_t *sb, vg_statusbar_item_t *item) {
    if (!sb || !vg_statusbar_item_is_live(item) || item->owner != sb)
        return;

    // Search in all zones
    vg_statusbar_item_t ***zones[] = {&sb->left_items, &sb->center_items, &sb->right_items};
    size_t *counts[] = {&sb->left_count, &sb->center_count, &sb->right_count};

    for (int z = 0; z < 3; z++) {
        vg_statusbar_item_t **items = *zones[z];
        size_t count = *counts[z];

        for (size_t i = 0; i < count; i++) {
            if (items[i] == item) {
                if (sb->hovered_item == item)
                    sb->hovered_item = NULL;
                retire_item(sb, item);
                memmove(&items[i], &items[i + 1], (count - i - 1) * sizeof(vg_statusbar_item_t *));
                (*counts[z])--;
                sb->base.needs_layout = true;
                sb->base.needs_paint = true;
                return;
            }
        }
    }
}

/// @brief Remove and free all items from a single zone, leaving the other zones intact.
///
/// @param sb   The status bar; may be NULL (no-op).
/// @param zone The zone to clear (LEFT, CENTER, or RIGHT).
void vg_statusbar_clear_zone(vg_statusbar_t *sb, vg_statusbar_zone_t zone) {
    if (!sb)
        return;

    vg_statusbar_item_t ***items;
    size_t *count;

    switch (zone) {
        case VG_STATUSBAR_ZONE_LEFT:
            items = &sb->left_items;
            count = &sb->left_count;
            break;
        case VG_STATUSBAR_ZONE_CENTER:
            items = &sb->center_items;
            count = &sb->center_count;
            break;
        case VG_STATUSBAR_ZONE_RIGHT:
            items = &sb->right_items;
            count = &sb->right_count;
            break;
        default:
            return;
    }

    for (size_t i = 0; i < *count; i++) {
        if (sb->hovered_item == (*items)[i])
            sb->hovered_item = NULL;
        retire_item(sb, (*items)[i]);
    }
    *count = 0;
    sb->base.needs_layout = true;
    sb->base.needs_paint = true;
}

/// @brief Unlink and free one exact retained StatusBar item record.
/// @details The candidate is dereferenced only after an address match in the owner's retirement
///          chain, making foreign and already reclaimed pointers harmless.
/// @copydetails vg_statusbar_reclaim_retired_item
bool vg_statusbar_reclaim_retired_item(vg_statusbar_t *sb, vg_statusbar_item_t *item) {
    if (!sb || !item)
        return false;
    vg_statusbar_item_t **link = &sb->retired_items;
    while (*link) {
        vg_statusbar_item_t *candidate = *link;
        if (candidate == item) {
            *link = candidate->retired_next;
            candidate->retired_next = NULL;
            free_item(candidate);
            return true;
        }
        link = &candidate->retired_next;
    }
    return false;
}

/// @brief Replace the text of a status bar item, triggering a layout invalidation.
///
/// @param item The item to update; may be NULL.
/// @param text New display text; copied internally, may be NULL to clear.
void vg_statusbar_item_set_text(vg_statusbar_item_t *item, const char *text) {
    if (!vg_statusbar_item_is_live(item))
        return;
    if ((!item->text && (!text || text[0] == '\0')) ||
        (item->text && text && strcmp(item->text, text) == 0))
        return;

    char *copy = text ? vg_strdup(text) : NULL;
    if (text && !copy)
        return;
    free(item->text);
    item->text = copy;
    if (item->owner)
        vg_widget_invalidate_layout(&item->owner->base);
}

/// @brief Set a per-item text color override.
///
/// @param item  The item to update; may be NULL.
/// @param color Text color as 0xRRGGBB.
void vg_statusbar_item_set_text_color(vg_statusbar_item_t *item, uint32_t color) {
    if (!vg_statusbar_item_is_live(item))
        return;
    if (item->has_text_color && item->text_color == color)
        return;
    item->text_color = color;
    item->has_text_color = true;
    if (item->owner)
        item->owner->base.needs_paint = true;
}

/// @brief Set or clear the leading vector icon for a text or button item.
/// @details Negative identifiers normalize to -1. A changed icon affects the
///          item's measured width and schedules layout and painting.
/// @param item Live status-bar item to configure.
/// @param vector_id Registered vector icon identifier, or a negative value for none.
void vg_statusbar_item_set_icon_vector(vg_statusbar_item_t *item, int32_t vector_id) {
    if (!vg_statusbar_item_is_live(item))
        return;
    if (vector_id < 0)
        vector_id = -1;
    if (item->icon_vector_id == vector_id)
        return;
    item->icon_vector_id = vector_id;
    if (item->owner) {
        item->owner->base.needs_layout = true;
        item->owner->base.needs_paint = true;
    }
}

/// @brief Set the hover tooltip string for a status bar item.
///
/// @param item    The item to update; may be NULL.
/// @param tooltip Tooltip text; copied internally, may be NULL to clear.
void vg_statusbar_item_set_tooltip(vg_statusbar_item_t *item, const char *tooltip) {
    if (!vg_statusbar_item_is_live(item))
        return;
    if ((!item->tooltip && (!tooltip || tooltip[0] == '\0')) ||
        (item->tooltip && tooltip && strcmp(item->tooltip, tooltip) == 0))
        return;

    char *copy = tooltip ? vg_strdup(tooltip) : NULL;
    if (tooltip && !copy)
        return;
    free(item->tooltip);
    item->tooltip = copy;
    if (item->owner)
        item->owner->base.needs_paint = true;
}

/// @brief Set the fill level of a progress bar item, clamped to [0.0, 1.0].
///
/// @param item     The progress bar item to update; may be NULL.
/// @param progress Fill fraction in [0.0, 1.0]; values outside the range are clamped.
void vg_statusbar_item_set_progress(vg_statusbar_item_t *item, float progress) {
    if (!vg_statusbar_item_is_live(item))
        return;

    if (progress < 0.0f)
        progress = 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;
    if (item->progress == progress)
        return;
    item->progress = progress;
    if (item->owner)
        item->owner->base.needs_paint = true;
}

/// @brief Show or hide a status bar item, triggering a layout pass on its owner.
///
/// @param item    The item to update; may be NULL.
/// @param visible false to hide the item (its slot collapses to zero width).
void vg_statusbar_item_set_visible(vg_statusbar_item_t *item, bool visible) {
    if (!vg_statusbar_item_is_live(item))
        return;
    if (item->visible == visible)
        return;
    item->visible = visible;
    if (item->owner)
        vg_widget_invalidate_layout(&item->owner->base);
}

/// @brief Set the font and size used for all text and button labels.
///
/// @param sb   The status bar to configure; may be NULL.
/// @param font Font to use; NULL retains the current font.
/// @param size Font size in points; if <= 0 the theme's small size is used.
void vg_statusbar_set_font(vg_statusbar_t *sb, vg_font_t *font, float size) {
    if (!sb)
        return;

    float font_size = size > 0 ? size : vg_theme_get_current()->typography.size_small;
    if (sb->font == font && sb->font_size == font_size)
        return;
    sb->font = font;
    sb->font_size = font_size;
    sb->base.needs_layout = true;
    sb->base.needs_paint = true;
}

/// @brief Update the last text item in the right zone to show "Ln N, Col N".
///
/// @details Formats line and col into a "Ln %d, Col %d" string and calls
///          vg_statusbar_item_set_text on the last right-zone text item if one exists.
///
/// @param sb   The status bar to update; may be NULL.
/// @param line 1-based line number to display.
/// @param col  1-based column number to display.
void vg_statusbar_set_cursor_position(vg_statusbar_t *sb, int line, int col) {
    if (!sb)
        return;

    // Find or create the cursor position item
    // For now, just update if there's a text item in the right zone
    char buf[64];
    snprintf(buf, sizeof(buf), "Ln %d, Col %d", line, col);

    // This is a simplified version - a full implementation would
    // track specific items by ID
    if (sb->right_count > 0) {
        vg_statusbar_item_t *item = sb->right_items[sb->right_count - 1];
        if (item->type == VG_STATUSBAR_ITEM_TEXT) {
            vg_statusbar_item_set_text(item, buf);
            sb->base.needs_paint = true;
        }
    }
}
