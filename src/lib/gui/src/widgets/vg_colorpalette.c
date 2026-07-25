//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_colorpalette.c
/// @brief Implements the selectable grid-based color-palette widget.
///
/// @details The palette displays packed AARRGGBB colors as uniformly sized
/// swatches, supports pointer and keyboard selection, publishes an accessible
/// textual value, and invokes application callbacks when users activate an
/// entry. Grid dimensions are derived from the configured column count, swatch
/// size, and gap. Hit testing deliberately excludes the gaps between swatches.
///
/// The widget owns its heap-allocated color array. Replacing or clearing the
/// palette releases that storage, and destruction releases any remaining
/// array. A selected index of `-1` represents no selection.
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
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void colorpalette_destroy(vg_widget_t *widget);
static void colorpalette_measure(vg_widget_t *widget,
                                 float available_width,
                                 float available_height);
static void colorpalette_paint(vg_widget_t *widget, void *canvas);
static bool colorpalette_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool colorpalette_can_focus(vg_widget_t *widget);
static void colorpalette_update_accessible_value(vg_colorpalette_t *palette);

//=============================================================================
// ColorPalette VTable
//=============================================================================

static vg_widget_vtable_t g_colorpalette_vtable = {.destroy = colorpalette_destroy,
                                                   .measure = colorpalette_measure,
                                                   .arrange = NULL,
                                                   .paint = colorpalette_paint,
                                                   .handle_event = colorpalette_handle_event,
                                                   .can_focus = colorpalette_can_focus,
                                                   .on_focus = NULL};

//=============================================================================
// Standard 16-color palette (classic Windows/DOS colors)
//=============================================================================

static const uint32_t g_standard_16_colors[] = {
    0xFF000000, // Black
    0xFF800000, // Dark Red
    0xFF008000, // Dark Green
    0xFF808000, // Dark Yellow (Olive)
    0xFF000080, // Dark Blue
    0xFF800080, // Dark Magenta
    0xFF008080, // Dark Cyan
    0xFFC0C0C0, // Light Gray
    0xFF808080, // Dark Gray
    0xFFFF0000, // Red
    0xFF00FF00, // Green
    0xFFFFFF00, // Yellow
    0xFF0000FF, // Blue
    0xFFFF00FF, // Magenta
    0xFF00FFFF, // Cyan
    0xFFFFFFFF  // White
};

//=============================================================================
// ColorPalette Implementation
//=============================================================================

/// @brief Create an empty color palette widget.
///
/// @details Default layout is 8 columns, 20×20 pixel swatches with 2-pixel
///          gaps.  Appearance colours are taken from the current theme.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_colorpalette_t, or NULL on allocation failure.
vg_colorpalette_t *vg_colorpalette_create(vg_widget_t *parent) {
    vg_colorpalette_t *palette = calloc(1, sizeof(vg_colorpalette_t));
    if (!palette)
        return NULL;

    // Initialize base widget
    vg_widget_init(&palette->base, VG_WIDGET_COLORPALETTE, &g_colorpalette_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize palette-specific fields
    palette->colors = NULL;
    palette->color_count = 0;
    palette->columns = 8;
    palette->selected_index = -1;

    // Default appearance
    palette->swatch_size = 20.0f;
    palette->gap = 2.0f;
    palette->bg_color = theme->colors.bg_secondary;
    palette->border_color = theme->colors.border_primary;
    palette->selected_border = theme->colors.accent_primary;

    // Callbacks
    palette->on_select = NULL;
    palette->on_select_data = NULL;

    // Set minimum size
    palette->base.constraints.min_width = palette->swatch_size;
    palette->base.constraints.min_height = palette->swatch_size;
    colorpalette_update_accessible_value(palette);

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &palette->base);
    }

    return palette;
}

/// @brief Releases the palette's owned color storage during widget destruction.
///
/// @param widget Palette base widget being destroyed.
static void colorpalette_destroy(vg_widget_t *widget) {
    vg_colorpalette_t *palette = (vg_colorpalette_t *)widget;
    if (palette->colors) {
        free(palette->colors);
        palette->colors = NULL;
    }
    palette->color_count = 0;
}

/// @brief Measures the swatch grid and applies the widget's size constraints.
///
/// @details Non-empty palettes use the configured columns, swatch size, and
/// inter-swatch gap. An empty palette reports the dimensions of one swatch.
///
/// @param widget Palette base widget whose measured dimensions are updated.
/// @param available_width Width offered by the parent; currently unused because
///                        the grid uses explicit swatch geometry.
/// @param available_height Height offered by the parent; currently unused.
static void colorpalette_measure(vg_widget_t *widget,
                                 float available_width,
                                 float available_height) {
    vg_colorpalette_t *palette = (vg_colorpalette_t *)widget;
    (void)available_width;
    (void)available_height;

    if (palette->color_count == 0 || palette->columns <= 0) {
        widget->measured_width = palette->swatch_size;
        widget->measured_height = palette->swatch_size;
        return;
    }

    // Calculate grid dimensions
    int rows = (palette->color_count + palette->columns - 1) / palette->columns;

    float total_width =
        palette->columns * palette->swatch_size + (palette->columns - 1) * palette->gap;
    float total_height = rows * palette->swatch_size + (rows - 1) * palette->gap;

    widget->measured_width = total_width;
    widget->measured_height = total_height;

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

/// @brief Paints the palette background, swatch grid, selection, and focus ring.
///
/// @details Current theme colors take precedence over the palette's cached
/// fallback colors. Empty palettes still receive a border, and the selected
/// swatch receives a double border in the focus color.
///
/// @param widget Palette base widget to render.
/// @param canvas Destination drawing context.
static void colorpalette_paint(vg_widget_t *widget, void *canvas) {
    vg_colorpalette_t *palette = (vg_colorpalette_t *)widget;
    vgfx_window_t win = (vgfx_window_t)canvas;
    vg_theme_t *theme = vg_theme_get_current();
    uint32_t background = theme ? theme->colors.bg_secondary : palette->bg_color;
    uint32_t border = theme ? theme->colors.border_primary : palette->border_color;
    uint32_t selected = theme ? theme->colors.border_focus : palette->selected_border;

    int32_t bx = (int32_t)widget->x;
    int32_t by = (int32_t)widget->y;
    int32_t bw = (int32_t)widget->width;
    int32_t bh = (int32_t)widget->height;
    if (bw > 0 && bh > 0)
        vgfx_fill_rect(win, bx, by, bw, bh, background);

    if (palette->color_count == 0 || !palette->colors) {
        if (bw > 0 && bh > 0)
            vgfx_rect(win, bx, by, bw, bh, border);
        if ((widget->state & VG_STATE_FOCUSED) && bw > 0 && bh > 0)
            vgfx_rect(win, bx - 2, by - 2, bw + 4, bh + 4, selected);
        return;
    }

    // Draw each color swatch in the grid
    for (int i = 0; i < palette->color_count; i++) {
        int col = i % palette->columns;
        int row = i / palette->columns;

        float swatch_x = widget->x + col * (palette->swatch_size + palette->gap);
        float swatch_y = widget->y + row * (palette->swatch_size + palette->gap);

        int32_t sx = (int32_t)swatch_x;
        int32_t sy = (int32_t)swatch_y;
        int32_t ss = (int32_t)palette->swatch_size;
        if (ss <= 0)
            continue;

        vgfx_fill_rect(win, sx, sy, ss, ss, palette->colors[i]);

        if (i == palette->selected_index) {
            vgfx_rect(win, sx - 1, sy - 1, ss + 2, ss + 2, selected);
            vgfx_rect(win, sx, sy, ss, ss, selected);
        } else {
            vgfx_rect(win, sx, sy, ss, ss, border);
        }
    }

    if (bw > 0 && bh > 0)
        vgfx_rect(win, bx, by, bw, bh, border);
    if ((widget->state & VG_STATE_FOCUSED) && bw > 0 && bh > 0)
        vgfx_rect(win, bx - 2, by - 2, bw + 4, bh + 4, selected);
}

/// @brief Finds the swatch under a pair of widget-local coordinates.
///
/// @details Coordinates landing in an inter-swatch gap, outside the configured
/// columns, or beyond the final partial row do not select an entry.
///
/// @param palette Palette whose swatch grid is queried.
/// @param x Horizontal coordinate relative to the widget's content origin.
/// @param y Vertical coordinate relative to the widget's content origin.
/// @return Zero-based swatch index, or `-1` when no swatch contains the point.
static int colorpalette_hit_test_swatch(vg_colorpalette_t *palette, float x, float y) {
    if (palette->color_count == 0 || !palette->colors) {
        return -1;
    }

    float local_x = x;
    float local_y = y;

    if (local_x < 0 || local_y < 0) {
        return -1;
    }

    // Calculate which cell
    float cell_size = palette->swatch_size + palette->gap;
    int col = (int)(local_x / cell_size);
    int row = (int)(local_y / cell_size);

    // Check if within bounds
    if (col >= palette->columns) {
        return -1;
    }

    // Check if within the actual swatch (not in the gap)
    float cell_x = local_x - col * cell_size;
    float cell_y = local_y - row * cell_size;
    if (cell_x >= palette->swatch_size || cell_y >= palette->swatch_size) {
        return -1; // In the gap
    }

    int index = row * palette->columns + col;
    if (index >= palette->color_count) {
        return -1;
    }

    return index;
}

/// @brief Notifies application callbacks of the selected palette color.
///
/// @details Selection is already committed before this helper runs. Keyboard
/// and pointer activation share the same ordering: the color-specific callback
/// runs first, followed by the widget's generic click callback.
///
/// @param palette Palette containing the selected entry.
static void colorpalette_activate_selected(vg_colorpalette_t *palette) {
    if (!palette || palette->selected_index < 0 || palette->selected_index >= palette->color_count)
        return;
    int index = palette->selected_index;
    if (palette->on_select)
        palette->on_select(&palette->base, palette->colors[index], index, palette->on_select_data);
    if (palette->base.on_click)
        palette->base.on_click(&palette->base, palette->base.callback_data);
}

/// @brief Handles pointer activation and keyboard navigation for the palette.
///
/// @details Clicks select and activate the hit swatch. Arrow, Home, and End
/// keys clamp selection movement within the array, while Space and Enter
/// activate the current entry. Disabled palettes reject all events.
///
/// @param widget Palette base widget receiving the event.
/// @param event Event to inspect and mark handled when consumed.
/// @return `true` when the event was consumed; otherwise `false`.
static bool colorpalette_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_colorpalette_t *palette = (vg_colorpalette_t *)widget;

    if (widget->state & VG_STATE_DISABLED) {
        return false;
    }

    if (event->type == VG_EVENT_MOUSE_DOWN) {
        int index = colorpalette_hit_test_swatch(palette, event->mouse.x, event->mouse.y);
        if (index >= 0) {
            event->handled = true;
            return true;
        }
    }

    if (event->type == VG_EVENT_CLICK) {
        // Hit test to find which swatch was clicked
        int index = colorpalette_hit_test_swatch(palette, event->mouse.x, event->mouse.y);
        if (index >= 0) {
            vg_colorpalette_set_selected(palette, index);
            colorpalette_activate_selected(palette);
            event->handled = true;
            return true;
        }
    }

    if (event->type == VG_EVENT_KEY_DOWN && palette->color_count > 0) {
        int current = palette->selected_index >= 0 ? palette->selected_index : 0;
        int next = current;
        switch (event->key.key) {
            case VG_KEY_LEFT:
                next = current - 1;
                break;
            case VG_KEY_RIGHT:
                next = current + 1;
                break;
            case VG_KEY_UP:
                next = current - palette->columns;
                break;
            case VG_KEY_DOWN:
                next = current + palette->columns;
                break;
            case VG_KEY_HOME:
                next = 0;
                break;
            case VG_KEY_END:
                next = palette->color_count - 1;
                break;
            case VG_KEY_SPACE:
            case VG_KEY_ENTER:
                if (palette->selected_index < 0)
                    vg_colorpalette_set_selected(palette, 0);
                colorpalette_activate_selected(palette);
                event->handled = true;
                return true;
            default:
                return false;
        }
        if (next < 0)
            next = 0;
        if (next >= palette->color_count)
            next = palette->color_count - 1;
        int old = palette->selected_index;
        vg_colorpalette_set_selected(palette, next);
        if (old != palette->selected_index)
            colorpalette_activate_selected(palette);
        event->handled = true;
        return true;
    }

    return false;
}

/// @brief Reports whether the palette may participate in keyboard focus.
///
/// @param widget Palette base widget to inspect.
/// @return `true` when the widget is enabled and visible; otherwise `false`.
static bool colorpalette_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief Refreshes the palette's semantic selection value.
///
/// @details Empty or unselected palettes expose an empty value. A selected
/// palette exposes its one-based position, total count, and six-digit RGB value
/// for accessibility and headless or native bridges.
///
/// @param palette Palette whose semantic value should be refreshed.
static void colorpalette_update_accessible_value(vg_colorpalette_t *palette) {
    if (!palette)
        return;
    if (palette->selected_index < 0 || palette->selected_index >= palette->color_count) {
        vg_widget_set_accessible_value(&palette->base, "");
        return;
    }
    char value[64];
    (void)snprintf(value,
                   sizeof(value),
                   "%d of %d, #%06X",
                   palette->selected_index + 1,
                   palette->color_count,
                   palette->colors[palette->selected_index] & 0x00FFFFFFu);
    vg_widget_set_accessible_value(&palette->base, value);
}

//=============================================================================
// ColorPalette API
//=============================================================================

/// @brief Replace the palette's colour list with a copy of the supplied array.
///
/// @details The previous colour array is freed.  The selection index is reset
/// to `-1`. Passing null or a non-positive count clears the palette. The input
/// array is copied, and ownership remains with the caller.
///
/// @param palette The color palette to update.
/// @param colors Array of @p count AARRGGBB color values; may be null to clear.
/// @param count Number of colors in @p colors.
void vg_colorpalette_set_colors(vg_colorpalette_t *palette, const uint32_t *colors, int count) {
    if (!palette)
        return;

    int normalized_count = colors && count > 0 ? count : 0;
    bool colors_equal = normalized_count == palette->color_count;
    if (colors_equal && normalized_count > 0) {
        colors_equal =
            palette->colors &&
            memcmp(palette->colors, colors, (size_t)normalized_count * sizeof(uint32_t)) == 0;
    }
    if (colors_equal && palette->selected_index == -1)
        return;

    uint32_t *new_colors = NULL;
    if (normalized_count > 0) {
        if ((size_t)normalized_count > SIZE_MAX / sizeof(uint32_t))
            return;
        new_colors = malloc((size_t)normalized_count * sizeof(uint32_t));
        if (!new_colors)
            return;
        memcpy(new_colors, colors, (size_t)normalized_count * sizeof(uint32_t));
    }

    free(palette->colors);
    palette->colors = new_colors;
    palette->color_count = new_colors ? normalized_count : 0;
    palette->selected_index = -1;

    if (!colors || count <= 0) {
        palette->color_count = 0;
    }

    palette->base.needs_layout = true;
    palette->base.needs_paint = true;
    colorpalette_update_accessible_value(palette);
    vg_widget_note_change(&palette->base);
}

/// @brief Appends a single color to the end of the palette.
///
/// @details The owned color array is grown as needed. Allocation failure or a
/// count that cannot be represented leaves the palette unchanged.
///
/// @param palette The color palette to update.
/// @param color AARRGGBB color value to append.
void vg_colorpalette_add_color(vg_colorpalette_t *palette, uint32_t color) {
    if (!palette)
        return;

    if (palette->color_count == INT_MAX)
        return;
    int new_count = palette->color_count + 1;
    if ((size_t)new_count > SIZE_MAX / sizeof(uint32_t))
        return;
    uint32_t *new_colors = realloc(palette->colors, (size_t)new_count * sizeof(uint32_t));
    if (!new_colors)
        return;

    palette->colors = new_colors;
    palette->colors[palette->color_count] = color;
    palette->color_count = new_count;

    palette->base.needs_layout = true;
    palette->base.needs_paint = true;
    colorpalette_update_accessible_value(palette);
    vg_widget_note_change(&palette->base);
}

/// @brief Remove one palette color by zero-based index.
///
/// @details Storage is compacted in place. Selection follows a surviving
/// logical entry and clears if the selected entry itself is removed.
///
/// @param palette Palette to update.
/// @param index Zero-based entry index.
/// @return `true` when an entry was removed; otherwise `false`.
bool vg_colorpalette_remove_color(vg_colorpalette_t *palette, int index) {
    if (!palette || index < 0 || index >= palette->color_count)
        return false;
    int old_selected = palette->selected_index;
    if (index + 1 < palette->color_count) {
        memmove(palette->colors + index,
                palette->colors + index + 1,
                (size_t)(palette->color_count - index - 1) * sizeof(uint32_t));
    }
    palette->color_count--;
    if (palette->color_count == 0) {
        free(palette->colors);
        palette->colors = NULL;
    } else {
        uint32_t *shrunk =
            realloc(palette->colors, (size_t)palette->color_count * sizeof(uint32_t));
        if (shrunk)
            palette->colors = shrunk;
    }
    if (old_selected == index)
        palette->selected_index = -1;
    else if (old_selected > index)
        palette->selected_index = old_selected - 1;
    palette->base.needs_layout = true;
    palette->base.needs_paint = true;
    colorpalette_update_accessible_value(palette);
    vg_widget_note_change(&palette->base);
    return true;
}

/// @brief Removes all colors from the palette and resets its selection.
///
/// @details The owned color array is released, the widget is marked for layout
/// and paint, and observers are notified when the state actually changes.
///
/// @param palette The color palette to clear.
void vg_colorpalette_clear(vg_colorpalette_t *palette) {
    if (!palette)
        return;
    if (palette->color_count == 0 && palette->selected_index == -1)
        return;

    if (palette->colors) {
        free(palette->colors);
        palette->colors = NULL;
    }
    palette->color_count = 0;
    palette->selected_index = -1;

    palette->base.needs_layout = true;
    palette->base.needs_paint = true;
    colorpalette_update_accessible_value(palette);
    vg_widget_note_change(&palette->base);
}

/// @brief Returns the number of stored palette colors.
///
/// @param palette Palette to inspect.
/// @return Non-negative color count, or zero when @p palette is null.
int vg_colorpalette_get_color_count(const vg_colorpalette_t *palette) {
    return palette ? palette->color_count : 0;
}

/// @brief Reads one palette color into an output parameter.
///
/// @param palette Palette to inspect.
/// @param index Zero-based color index.
/// @param out_color Receives AARRGGBB on success.
/// @return `true` for a valid index and output pointer; otherwise `false`.
bool vg_colorpalette_get_color_at(const vg_colorpalette_t *palette,
                                  int index,
                                  uint32_t *out_color) {
    if (!palette || !out_color || index < 0 || index >= palette->color_count)
        return false;
    *out_color = palette->colors[index];
    return true;
}

/// @brief Set the number of columns in the swatch grid.
///
/// @param palette The color palette to configure.
/// @param columns Positive number of columns; other values are ignored.
void vg_colorpalette_set_columns(vg_colorpalette_t *palette, int columns) {
    if (!palette || columns <= 0)
        return;
    if (palette->columns == columns)
        return;

    palette->columns = columns;
    palette->base.needs_layout = true;
    palette->base.needs_paint = true;
    vg_widget_note_revision(&palette->base);
}

/// @brief Programmatically select a swatch by zero-based index.
///
/// @details Out-of-range values normalize to no selection. Changing the
/// selection refreshes the accessible value and notifies widget observers, but
/// does not invoke the user-activation callback.
///
/// @param palette The color palette to update.
/// @param index Zero-based swatch index, or `-1` to clear the selection.
void vg_colorpalette_set_selected(vg_colorpalette_t *palette, int index) {
    if (!palette)
        return;

    if (index < -1 || index >= palette->color_count) {
        index = -1;
    }
    if (palette->selected_index == index)
        return;

    palette->selected_index = index;
    palette->base.needs_paint = true;
    colorpalette_update_accessible_value(palette);
    vg_widget_note_change(&palette->base);
}

/// @brief Returns the index of the currently selected swatch.
///
/// @param palette The color palette to query.
/// @return Zero-based selected index, or `-1` if none is selected or
///         @p palette is null.
int vg_colorpalette_get_selected(vg_colorpalette_t *palette) {
    if (!palette)
        return -1;
    return palette->selected_index;
}

/// @brief Returns the AARRGGBB color of the selected swatch.
///
/// @param palette The color palette to query.
/// @return Selected color, or zero when the selection is invalid or
///         @p palette is null.
uint32_t vg_colorpalette_get_selected_color(vg_colorpalette_t *palette) {
    if (!palette || palette->selected_index < 0 ||
        palette->selected_index >= palette->color_count) {
        return 0;
    }
    return palette->colors[palette->selected_index];
}

/// @brief Registers the callback invoked when a swatch is user-activated.
///
/// @param palette The color palette to configure.
/// @param callback Function called with the widget, color, index, and
///                 @p user_data on activation; may be null to unregister.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_colorpalette_set_on_select(vg_colorpalette_t *palette,
                                   vg_colorpalette_callback_t callback,
                                   void *user_data) {
    if (!palette)
        return;

    palette->on_select = callback;
    palette->on_select_data = user_data;
}

/// @brief Sets the uniform width and height of each swatch.
///
/// @param palette The color palette to configure.
/// @param size Finite positive swatch size in logical pixels; other values are
///             ignored.
void vg_colorpalette_set_swatch_size(vg_colorpalette_t *palette, float size) {
    if (!palette || !isfinite(size) || size <= 0 || palette->swatch_size == size)
        return;

    palette->swatch_size = size;
    palette->base.needs_layout = true;
    palette->base.needs_paint = true;
    vg_widget_note_revision(&palette->base);
}

/// @brief Populates the palette with the classic 16 Windows/DOS colors.
///
/// @details Existing colors and selection are replaced, and the grid is
/// configured as two rows of eight entries.
///
/// @param palette The color palette to populate.
void vg_colorpalette_load_standard_16(vg_colorpalette_t *palette) {
    if (!palette)
        return;

    vg_colorpalette_set_colors(palette, g_standard_16_colors, 16);
    palette->columns = 8; // 2 rows of 8
}
