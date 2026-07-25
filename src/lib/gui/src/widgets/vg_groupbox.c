//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_groupbox.c
/// @brief Implements a titled, vertically stacked card container.
///
/// @details The group box reserves a title area, measures its visible children
/// intrinsically, arranges them as a vertical stack, and paints an elevated
/// rounded card with an accent divider. Its title is always an owned,
/// null-terminated heap string. Children are owned through the ordinary widget
/// hierarchy.
///
/// @see vg_ide_widgets_ui.h
/// @see vg_draw.c
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widget.h"
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Helpers
//=============================================================================

/// @brief Returns the height reserved for the title and divider.
///
/// @param gb Group box supplying padding and typography.
/// @return Title-area height in logical pixels.
static float groupbox_title_height(const vg_groupbox_t *gb) {
    float fs = gb->font_size > 0.0f ? gb->font_size : 14.0f;
    return gb->padding + fs + 8.0f;
}

//=============================================================================
// VTable
//=============================================================================

/// @brief Releases the group box's owned title during widget destruction.
///
/// @param widget Group-box base widget being destroyed.
static void groupbox_destroy(vg_widget_t *widget) {
    vg_groupbox_t *gb = (vg_groupbox_t *)widget;
    free(gb->title);
    gb->title = NULL;
}

/// @brief Measures the intrinsic card size from its visible children.
///
/// @details Child widths are constrained by the padded available width, but the
/// result remains intrinsic so parent layout can stretch the card consistently.
/// Heights include the title area, child spacing, and bottom padding.
///
/// @param widget Group-box base widget whose measured dimensions are updated.
/// @param available_width Width offered by the parent.
/// @param available_height Height offered to child measurement.
static void groupbox_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_groupbox_t *gb = (vg_groupbox_t *)widget;
    float pad = gb->padding;
    float inner_w = available_width - pad * 2.0f;
    if (inner_w < 0.0f)
        inner_w = 0.0f;

    float content_h = 0.0f;
    float max_cw = 0.0f;
    int n = 0;
    for (vg_widget_t *c = widget->first_child; c; c = c->next_sibling) {
        if (!c->visible)
            continue;
        vg_widget_measure(c, inner_w, available_height);
        if (c->measured_width > max_cw)
            max_cw = c->measured_width;
        content_h += c->measured_height;
        n++;
    }
    if (n > 1)
        content_h += gb->spacing * (float)(n - 1);

    // Report the INTRINSIC width only. Do NOT inflate to available_width here:
    // the parent VBox stretches us to its width, and inflating made our measured
    // width exceed the viewport, so the scroll view arranged children at a
    // different width than they were measured at (word-wrap then wrapped
    // differently → overlapping text and wrong card heights).
    float w = max_cw + pad * 2.0f;
    float h = groupbox_title_height(gb) + content_h + pad;

    widget->measured_width = w;
    widget->measured_height = h;
    vg_widget_apply_constraints(widget);
}

/// @brief Arranges visible children vertically below the title area.
///
/// @param widget Group-box base widget to arrange.
/// @param x Allocated left coordinate.
/// @param y Allocated top coordinate.
/// @param width Allocated card width.
/// @param height Allocated card height.
static void groupbox_arrange(vg_widget_t *widget, float x, float y, float width, float height) {
    vg_groupbox_t *gb = (vg_groupbox_t *)widget;
    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;

    float pad = gb->padding;
    float content_w = width - pad * 2.0f;
    if (content_w < 0.0f)
        content_w = 0.0f;
    // Children are stored parent-relative like every flow container: the
    // painters and vg_widget_get_screen_bounds sum ancestor origins, so
    // including this card's own (x, y) here would double-offset the content.
    float cy = groupbox_title_height(gb);
    for (vg_widget_t *c = widget->first_child; c; c = c->next_sibling) {
        if (!c->visible)
            continue;
        float ch = c->measured_height;
        vg_widget_arrange(c, pad, cy, content_w, ch);
        cy += ch + gb->spacing;
    }
}

/// @brief Paints the elevated card, title, border, and accent divider.
///
/// @param widget Group-box base widget to render.
/// @param canvas Destination drawing context.
static void groupbox_paint(vg_widget_t *widget, void *canvas) {
    vg_groupbox_t *gb = (vg_groupbox_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    float x = widget->x, y = widget->y, w = widget->width, h = widget->height;
    uint32_t bg = gb->bg_color ? gb->bg_color : theme->colors.bg_secondary;
    uint32_t border = gb->border_color ? gb->border_color : theme->colors.border_primary;
    uint32_t tcol = gb->title_color ? gb->title_color : theme->colors.fg_primary;
    float rad = gb->corner_radius > 0.0f ? gb->corner_radius : theme->radius.lg;

    // Elevated rounded card.
    vg_elevation_t el = theme->elevation.level1;
    vg_draw_round_rect_shadow(
        win, x, y, w, h, rad, el.blur, el.dx, el.dy, el.alpha, theme->elevation.shadow_rgb);
    vg_draw_round_rect_fill(win, x, y, w, h, rad, bg);
    vg_draw_inner_highlight_top(win, x, y + 1.0f, w, rad, vg_color_lighten(bg, 0.06f));
    vg_draw_round_rect_stroke(win, x, y, w, h, rad, 1.0f, border);

    // Title text + an accent underline separating it from the content.
    if (gb->title && gb->title[0] && gb->font) {
        vg_font_metrics_t fm;
        vg_font_get_metrics(gb->font, gb->font_size, &fm);
        float tx = x + gb->padding;
        float ty = y + gb->padding + fm.ascent;
        vg_font_draw_text(canvas, gb->font, gb->font_size, tx, ty, gb->title, tcol);
    }
    float underline_y = y + groupbox_title_height(gb) - 4.0f;
    vg_draw_round_rect_fill(win,
                            x + gb->padding,
                            underline_y,
                            w - gb->padding * 2.0f,
                            2.0f,
                            1.0f,
                            theme->colors.accent_primary);
}

/// @brief Applies typography from the generic widget font hook.
///
/// @param widget Group-box base widget to configure.
/// @param font Borrowed font pointer; may be null.
/// @param size Positive font size, or a non-positive value to retain the current
///             size.
static void groupbox_set_font(vg_widget_t *widget, void *font, float size) {
    vg_groupbox_t *gb = (vg_groupbox_t *)widget;
    gb->font = (vg_font_t *)font;
    if (size > 0.0f)
        gb->font_size = size;
}

static vg_widget_vtable_t g_groupbox_vtable = {.destroy = groupbox_destroy,
                                               .measure = groupbox_measure,
                                               .arrange = groupbox_arrange,
                                               .paint = groupbox_paint,
                                               .handle_event = NULL,
                                               .can_focus = NULL,
                                               .on_focus = NULL,
                                               .set_font = groupbox_set_font};

//=============================================================================
// Public API
//=============================================================================

/// @brief Creates a group box with theme-derived card styling.
///
/// @param parent Widget to receive the group box as a child; may be null.
/// @param title Title to copy; null selects an empty title.
/// @return Newly allocated group box, or null on allocation failure.
vg_groupbox_t *vg_groupbox_create(vg_widget_t *parent, const char *title) {
    vg_groupbox_t *gb = calloc(1, sizeof(vg_groupbox_t));
    if (!gb)
        return NULL;

    vg_widget_init(&gb->base, VG_WIDGET_GROUPBOX, &g_groupbox_vtable);

    gb->title = vg_strdup(title ? title : "");
    if (!gb->title) {
        vg_widget_destroy(&gb->base);
        return NULL;
    }

    vg_theme_t *theme = vg_theme_get_current();
    gb->bg_color = 0;
    gb->border_color = 0;
    gb->title_color = 0;
    gb->corner_radius = theme->radius.lg;
    gb->padding = theme->spacing.md + 4.0f;
    gb->spacing = theme->spacing.md;
    gb->font =
        theme->typography.font_bold ? theme->typography.font_bold : theme->typography.font_regular;
    gb->font_size = theme->typography.size_normal;

    if (parent)
        vg_widget_add_child(parent, &gb->base);
    return gb;
}

/// @brief Destroys a group box and its child-widget subtree.
///
/// @param gb Group box to destroy; may be null.
void vg_groupbox_destroy(vg_groupbox_t *gb) {
    if (gb)
        vg_widget_destroy(&gb->base);
}

/// @brief Replaces the group-box title with an owned copy.
///
/// @details The new string is allocated before the previous title is released,
/// preserving state on allocation failure.
///
/// @param gb Group box to update.
/// @param title Title to copy; null selects an empty title.
void vg_groupbox_set_title(vg_groupbox_t *gb, const char *title) {
    if (!gb)
        return;
    char *copy = vg_strdup(title ? title : "");
    if (!copy)
        return;
    free(gb->title);
    gb->title = copy;
}

/// @brief Adds a child to the group box's owned vertical stack.
///
/// @param gb Group box that adopts the child.
/// @param child Widget whose ownership transfers to the group box.
void vg_groupbox_add_child(vg_groupbox_t *gb, vg_widget_t *child) {
    if (gb && child)
        vg_widget_add_child(&gb->base, child);
}
