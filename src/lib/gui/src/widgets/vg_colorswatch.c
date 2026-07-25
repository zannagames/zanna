//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_colorswatch.c
/// @brief Implements a selectable single-color swatch widget.
///
/// @details A swatch displays one packed AARRGGBB color and visualizes
/// translucency by compositing that color over a checkerboard. It supports
/// pointer and keyboard activation, an optional border, focus indication, and
/// an accessible color description. The explicit `selected` field is mirrored
/// into `VG_STATE_SELECTED` on the base widget.
///
/// The swatch owns no heap allocations beyond its widget object. Theme border
/// colors are resolved at paint time, with cached widget colors serving as
/// fallbacks.
///
/// @see vg_widgets.h
/// @see vg_theme.h
/// @see vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void colorswatch_destroy(vg_widget_t *widget);
static void colorswatch_measure(vg_widget_t *widget, float available_width, float available_height);
static void colorswatch_paint(vg_widget_t *widget, void *canvas);
static bool colorswatch_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool colorswatch_can_focus(vg_widget_t *widget);
static void colorswatch_update_accessible_value(vg_colorswatch_t *swatch);

//=============================================================================
// ColorSwatch VTable
//=============================================================================

static vg_widget_vtable_t g_colorswatch_vtable = {.destroy = colorswatch_destroy,
                                                  .measure = colorswatch_measure,
                                                  .arrange = NULL,
                                                  .paint = colorswatch_paint,
                                                  .handle_event = colorswatch_handle_event,
                                                  .can_focus = colorswatch_can_focus,
                                                  .on_focus = NULL};

//=============================================================================
// Helper: Draw checkerboard pattern for transparency
//=============================================================================

/// @brief Paints a color alpha-composited over an alternating checkerboard.
///
/// @details Each checker cell is blended in software so translucent colors
/// remain visible even though the rectangle primitive itself is opaque.
/// Partial cells along the right and bottom edges are clipped to the requested
/// dimensions.
///
/// @param canvas Target canvas.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param check_size Checker cell size in pixels; non-positive values use eight.
/// @param rgb Foreground packed color whose RGB channels are blended.
/// @param alpha Foreground opacity in the range `[0, 255]`.
static void draw_checkerboard(
    void *canvas, float x, float y, float w, float h, int check_size, uint32_t rgb, uint8_t alpha) {
    vgfx_window_t win = (vgfx_window_t)canvas;
    if (check_size <= 0)
        check_size = 8;

    for (int cy = 0; cy * check_size < (int)h; cy++) {
        for (int cx = 0; cx * check_size < (int)w; cx++) {
            uint32_t checker = ((cx + cy) % 2 == 0) ? 0xFFAAAAAA : 0xFF888888;
            uint32_t color =
                vg_color_blend(checker, 0xFF000000u | (rgb & 0x00FFFFFFu), (float)alpha / 255.0f);
            int rx = (int)x + cx * check_size;
            int ry = (int)y + cy * check_size;
            int rw = check_size;
            if (rx + rw > (int)(x + w))
                rw = (int)(x + w) - rx;
            int rh = check_size;
            if (ry + rh > (int)(y + h))
                rh = (int)(y + h) - ry;
            if (rw > 0 && rh > 0)
                vgfx_fill_rect(win, rx, ry, rw, rh, color);
        }
    }
}

//=============================================================================
// ColorSwatch Implementation
//=============================================================================

/// @brief Creates a color swatch widget displaying the given color.
///
/// @details The default size is 24 by 24 logical pixels with a one-pixel border
/// and two-pixel corner radius. Theme colors initialize its normal and selected
/// border styles.
///
/// @param parent Widget to attach to as a child; may be null.
/// @param color Initial packed AARRGGBB color.
/// @return Newly allocated swatch, or null on allocation failure.
vg_colorswatch_t *vg_colorswatch_create(vg_widget_t *parent, uint32_t color) {
    vg_colorswatch_t *swatch = calloc(1, sizeof(vg_colorswatch_t));
    if (!swatch)
        return NULL;

    // Initialize base widget
    vg_widget_init(&swatch->base, VG_WIDGET_COLORSWATCH, &g_colorswatch_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize swatch-specific fields
    swatch->color = color;
    swatch->selected = false;
    swatch->show_border = true;

    // Default appearance
    swatch->size = 24.0f;
    swatch->border_color = theme->colors.border_primary;
    swatch->selected_border = theme->colors.accent_primary;
    swatch->border_width = 1.0f;
    swatch->corner_radius = 2.0f;

    // Callbacks
    swatch->on_select = NULL;
    swatch->on_select_data = NULL;

    // Set constraints
    swatch->base.constraints.min_width = swatch->size;
    swatch->base.constraints.min_height = swatch->size;
    swatch->base.constraints.preferred_width = swatch->size;
    swatch->base.constraints.preferred_height = swatch->size;
    colorswatch_update_accessible_value(swatch);

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &swatch->base);
    }

    return swatch;
}

/// @brief Completes swatch-specific destruction.
///
/// @details No action is required because the swatch has no separately owned
/// allocations.
///
/// @param widget Swatch base widget being destroyed.
static void colorswatch_destroy(vg_widget_t *widget) {
    (void)widget;
    // No owned resources to free
}

/// @brief Measures the square swatch and applies its widget constraints.
///
/// @param widget Swatch base widget whose measured dimensions are updated.
/// @param available_width Width offered by the parent; currently unused.
/// @param available_height Height offered by the parent; currently unused.
static void colorswatch_measure(vg_widget_t *widget,
                                float available_width,
                                float available_height) {
    vg_colorswatch_t *swatch = (vg_colorswatch_t *)widget;
    (void)available_width;
    (void)available_height;

    widget->measured_width = swatch->size;
    widget->measured_height = swatch->size;

    // Apply constraints
    if (widget->constraints.min_width > widget->measured_width) {
        widget->measured_width = widget->constraints.min_width;
    }
    if (widget->constraints.min_height > widget->measured_height) {
        widget->measured_height = widget->constraints.min_height;
    }
    if (widget->constraints.max_width > 0 &&
        widget->measured_width > widget->constraints.max_width) {
        widget->measured_width = widget->constraints.max_width;
    }
    if (widget->constraints.max_height > 0 &&
        widget->measured_height > widget->constraints.max_height) {
        widget->measured_height = widget->constraints.max_height;
    }
}

/// @brief Paints the swatch color, transparency pattern, border, and focus ring.
///
/// @details Opaque colors fill the tile directly. Translucent colors are
/// software-composited over a checkerboard. Hovered and selected swatches use
/// the current theme's focus border, while other bordered swatches use the
/// normal theme border.
///
/// @param widget Swatch base widget to render.
/// @param canvas Destination drawing context.
static void colorswatch_paint(vg_widget_t *widget, void *canvas) {
    vg_colorswatch_t *swatch = (vg_colorswatch_t *)widget;

    vgfx_window_t win = (vgfx_window_t)canvas;
    int32_t x = (int32_t)widget->x;
    int32_t y = (int32_t)widget->y;
    int32_t w = (int32_t)widget->width;
    int32_t h = (int32_t)widget->height;
    if (w <= 0 || h <= 0)
        return;
    vg_theme_t *theme = vg_theme_get_current();
    uint32_t border_color = theme ? theme->colors.border_primary : swatch->border_color;
    uint32_t selected_border = theme ? theme->colors.border_focus : swatch->selected_border;

    // Get alpha component (color is stored as AARRGGBB)
    uint8_t alpha = (swatch->color >> 24) & 0xFF;

    // If color has transparency, draw checkerboard first
    if (alpha < 255) {
        draw_checkerboard(
            canvas, widget->x, widget->y, widget->width, widget->height, 4, swatch->color, alpha);
    } else {
        vgfx_fill_rect(win, x, y, w, h, 0xFF000000u | (swatch->color & 0x00FFFFFFu));
    }

    // Draw border
    if (swatch->show_border) {
        uint32_t border = swatch->selected ? selected_border : border_color;
        if (widget->state & VG_STATE_HOVERED)
            border = selected_border;
        vgfx_rect(win, x, y, w, h, border);
    }
    if (widget->state & VG_STATE_FOCUSED)
        vgfx_rect(win, x - 2, y - 2, w + 4, h + 4, selected_border);
}

/// @brief Commit one swatch activation from pointer or keyboard input.
///
/// @details Selection state is committed before callbacks so observers see the
/// new state. The swatch-specific callback runs before the generic widget click
/// callback.
///
/// @param swatch Swatch to activate.
static void colorswatch_activate(vg_colorswatch_t *swatch) {
    if (!swatch)
        return;
    vg_colorswatch_set_selected(swatch, true);
    if (swatch->on_select)
        swatch->on_select(&swatch->base, swatch->color, swatch->on_select_data);
    if (swatch->base.on_click)
        swatch->base.on_click(&swatch->base, swatch->base.callback_data);
}

/// @brief Handles pointer and keyboard activation of the swatch.
///
/// @param widget Swatch base widget receiving the event.
/// @param event Event to inspect and mark handled when consumed.
/// @return `true` when an enabled swatch consumes a click, Space, or Enter;
///         otherwise `false`.
static bool colorswatch_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_colorswatch_t *swatch = (vg_colorswatch_t *)widget;

    if (widget->state & VG_STATE_DISABLED) {
        return false;
    }

    if (event->type == VG_EVENT_CLICK) {
        colorswatch_activate(swatch);
        event->handled = true;
        return true;
    }
    if (event->type == VG_EVENT_KEY_DOWN &&
        (event->key.key == VG_KEY_SPACE || event->key.key == VG_KEY_ENTER)) {
        colorswatch_activate(swatch);
        event->handled = true;
        return true;
    }

    return false;
}

/// @brief Reports whether the swatch may receive keyboard focus.
///
/// @param widget Swatch base widget to inspect.
/// @return `true` when the widget is enabled and visible; otherwise `false`.
static bool colorswatch_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief Refresh the swatch's headless/native accessible value.
///
/// @details Opaque colors use `#RRGGBB`; translucent colors append their
/// decimal alpha channel. Allocation failure inside semantic storage leaves the
/// prior value intact.
///
/// @param swatch Swatch whose current color is described.
static void colorswatch_update_accessible_value(vg_colorswatch_t *swatch) {
    if (!swatch)
        return;
    char value[40];
    uint8_t alpha = (uint8_t)(swatch->color >> 24);
    if (alpha == 255)
        (void)snprintf(value, sizeof(value), "#%06X", swatch->color & 0x00FFFFFFu);
    else
        (void)snprintf(
            value, sizeof(value), "#%06X alpha %u", swatch->color & 0x00FFFFFFu, (unsigned)alpha);
    vg_widget_set_accessible_value(&swatch->base, value);
}

//=============================================================================
// ColorSwatch API
//=============================================================================

/// @brief Sets the color displayed by the swatch.
///
/// @details A changed color refreshes the accessible value, schedules repaint,
/// and notifies widget observers.
///
/// @param swatch The color swatch to update.
/// @param color New packed AARRGGBB color.
void vg_colorswatch_set_color(vg_colorswatch_t *swatch, uint32_t color) {
    if (!swatch)
        return;
    if (swatch->color == color)
        return;

    swatch->color = color;
    swatch->base.needs_paint = true;
    colorswatch_update_accessible_value(swatch);
    vg_widget_note_change(&swatch->base);
}

/// @brief Returns the AARRGGBB color displayed by the swatch.
///
/// @param swatch The color swatch to query.
/// @return Current color value, or zero when @p swatch is null.
uint32_t vg_colorswatch_get_color(vg_colorswatch_t *swatch) {
    if (!swatch)
        return 0;
    return swatch->color;
}

/// @brief Sets the selection state and mirrors it to `VG_STATE_SELECTED`.
///
/// @details Selection changes schedule repaint and notify widget observers.
///
/// @param swatch The color swatch to update.
/// @param selected `true` to select the swatch; `false` to deselect it.
void vg_colorswatch_set_selected(vg_colorswatch_t *swatch, bool selected) {
    if (!swatch)
        return;
    if (swatch->selected == selected)
        return;

    swatch->selected = selected;
    if (selected) {
        swatch->base.state |= VG_STATE_SELECTED;
    } else {
        swatch->base.state &= ~VG_STATE_SELECTED;
    }
    swatch->base.needs_paint = true;
    vg_widget_note_change(&swatch->base);
}

/// @brief Reports whether the swatch is currently selected.
///
/// @param swatch The color swatch to query.
/// @return `true` when selected; `false` when unselected or @p swatch is null.
bool vg_colorswatch_is_selected(vg_colorswatch_t *swatch) {
    if (!swatch)
        return false;
    return swatch->selected;
}

/// @brief Registers a callback for user activation of the swatch.
///
/// @param swatch The color swatch to configure.
/// @param callback Function called with the widget, packed color, and
///                 @p user_data; may be null to unregister.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_colorswatch_set_on_select(vg_colorswatch_t *swatch,
                                  vg_colorswatch_callback_t callback,
                                  void *user_data) {
    if (!swatch)
        return;

    swatch->on_select = callback;
    swatch->on_select_data = user_data;
}

/// @brief Sets the uniform width and height of the swatch.
///
/// @details The minimum and preferred constraints are updated together, and a
/// changed size schedules both layout and repaint.
///
/// @param swatch The color swatch to resize.
/// @param size Finite positive size in logical pixels; other values are ignored.
void vg_colorswatch_set_size(vg_colorswatch_t *swatch, float size) {
    if (!swatch || !isfinite(size) || size <= 0 || swatch->size == size)
        return;

    swatch->size = size;
    swatch->base.constraints.min_width = size;
    swatch->base.constraints.min_height = size;
    swatch->base.constraints.preferred_width = size;
    swatch->base.constraints.preferred_height = size;
    swatch->base.needs_layout = true;
    swatch->base.needs_paint = true;
    vg_widget_note_revision(&swatch->base);
}
