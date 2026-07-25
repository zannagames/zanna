//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_colorpicker.c
/// @brief Implements the composite RGBA color-picker widget.
///
/// @details The picker combines red, green, blue, and optional alpha sliders
/// with a preview swatch and optional standard-color palette. The packed
/// AARRGGBB value and individual channel fields are kept synchronized in both
/// directions. Keyboard interaction selects a channel vertically and adjusts
/// it horizontally.
///
/// Programmatic slider synchronization sets `syncing_children` to suppress
/// reentrant change notifications. Child widgets are owned by the widget
/// hierarchy and require no separate destruction by this implementation.
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

static void colorpicker_destroy(vg_widget_t *widget);
static void colorpicker_measure(vg_widget_t *widget, float available_width, float available_height);
static void colorpicker_arrange(vg_widget_t *widget, float x, float y, float width, float height);
static void colorpicker_paint(vg_widget_t *widget, void *canvas);
static bool colorpicker_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool colorpicker_can_focus(vg_widget_t *widget);

static void colorpicker_update_color_from_components(vg_colorpicker_t *picker);
static void colorpicker_update_components_from_color(vg_colorpicker_t *picker);
static void colorpicker_update_preview(vg_colorpicker_t *picker);
static void colorpicker_emit_change(vg_colorpicker_t *picker, uint32_t old_color);
static void colorpicker_update_accessible_value(vg_colorpicker_t *picker);

//=============================================================================
// ColorPicker VTable
//=============================================================================

static vg_widget_vtable_t g_colorpicker_vtable = {.destroy = colorpicker_destroy,
                                                  .measure = colorpicker_measure,
                                                  .arrange = colorpicker_arrange,
                                                  .paint = colorpicker_paint,
                                                  .handle_event = colorpicker_handle_event,
                                                  .can_focus = colorpicker_can_focus,
                                                  .on_focus = NULL};

//=============================================================================
// Internal Callbacks
//=============================================================================

/// @brief Applies a user-originated red-slider value to the picker.
///
/// @param slider Slider that emitted the change; otherwise unused.
/// @param value New red-channel value, rounded to the nearest integer.
/// @param user_data Picker registered as the slider callback context.
static void on_slider_r_change(vg_widget_t *slider, float value, void *user_data) {
    (void)slider;
    vg_colorpicker_t *picker = (vg_colorpicker_t *)user_data;
    if (!picker)
        return;

    uint32_t old_color = picker->color;
    picker->r = (uint8_t)(value + 0.5f);
    colorpicker_update_color_from_components(picker);
    if (!picker->syncing_children)
        colorpicker_emit_change(picker, old_color);
}

/// @brief Applies a user-originated green-slider value to the picker.
///
/// @param slider Slider that emitted the change; otherwise unused.
/// @param value New green-channel value, rounded to the nearest integer.
/// @param user_data Picker registered as the slider callback context.
static void on_slider_g_change(vg_widget_t *slider, float value, void *user_data) {
    (void)slider;
    vg_colorpicker_t *picker = (vg_colorpicker_t *)user_data;
    if (!picker)
        return;

    uint32_t old_color = picker->color;
    picker->g = (uint8_t)(value + 0.5f);
    colorpicker_update_color_from_components(picker);
    if (!picker->syncing_children)
        colorpicker_emit_change(picker, old_color);
}

/// @brief Applies a user-originated blue-slider value to the picker.
///
/// @param slider Slider that emitted the change; otherwise unused.
/// @param value New blue-channel value, rounded to the nearest integer.
/// @param user_data Picker registered as the slider callback context.
static void on_slider_b_change(vg_widget_t *slider, float value, void *user_data) {
    (void)slider;
    vg_colorpicker_t *picker = (vg_colorpicker_t *)user_data;
    if (!picker)
        return;

    uint32_t old_color = picker->color;
    picker->b = (uint8_t)(value + 0.5f);
    colorpicker_update_color_from_components(picker);
    if (!picker->syncing_children)
        colorpicker_emit_change(picker, old_color);
}

/// @brief Applies a user-originated alpha-slider value to the picker.
///
/// @param slider Slider that emitted the change; otherwise unused.
/// @param value New alpha-channel value, rounded to the nearest integer.
/// @param user_data Picker registered as the slider callback context.
static void on_slider_a_change(vg_widget_t *slider, float value, void *user_data) {
    (void)slider;
    vg_colorpicker_t *picker = (vg_colorpicker_t *)user_data;
    if (!picker)
        return;

    uint32_t old_color = picker->color;
    picker->a = (uint8_t)(value + 0.5f);
    colorpicker_update_color_from_components(picker);
    if (!picker->syncing_children)
        colorpicker_emit_change(picker, old_color);
}

/// @brief Applies a quick-palette selection to the picker.
///
/// @param palette Palette widget that emitted the selection; otherwise unused.
/// @param color Selected packed AARRGGBB color.
/// @param index Selected palette index; otherwise unused.
/// @param user_data Picker registered as the palette callback context.
static void on_palette_select(vg_widget_t *palette, uint32_t color, int index, void *user_data) {
    (void)palette;
    (void)index;
    vg_colorpicker_t *picker = (vg_colorpicker_t *)user_data;
    if (!picker)
        return;

    vg_colorpicker_set_color(picker, color);
}

//=============================================================================
// ColorPicker Implementation
//=============================================================================

/// @brief Creates a color picker with channel sliders, preview, and quick palette.
///
/// @details The picker initially represents opaque black. Its alpha slider is
/// hidden and its standard 16-color palette is visible by default. All child
/// widgets are created and parented automatically; individual child-allocation
/// failures leave the corresponding optional control absent.
///
/// @param parent Widget to attach to as a child; may be null.
/// @return Newly allocated picker, or null if the picker allocation fails.
vg_colorpicker_t *vg_colorpicker_create(vg_widget_t *parent) {
    vg_colorpicker_t *picker = calloc(1, sizeof(vg_colorpicker_t));
    if (!picker)
        return NULL;

    // Initialize base widget
    vg_widget_init(&picker->base, VG_WIDGET_COLORPICKER, &g_colorpicker_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize color to black (fully opaque)
    picker->color = 0xFF000000;
    picker->r = 0;
    picker->g = 0;
    picker->b = 0;
    picker->a = 255;

    // Display options
    picker->show_alpha = false;
    picker->show_palette = true;
    picker->show_labels = true;
    picker->show_values = true;
    picker->font = NULL;
    picker->font_size = theme->typography.size_small;

    // Callbacks
    picker->on_change = NULL;
    picker->on_change_data = NULL;
    picker->syncing_children = false;
    picker->active_channel = 0;

    // Create child widgets

    // Color preview swatch
    picker->preview = vg_colorswatch_create(&picker->base, picker->color);
    if (picker->preview) {
        vg_colorswatch_set_size(picker->preview, 48.0f);
    }

    // RGB sliders
    picker->slider_r = vg_slider_create(&picker->base, VG_SLIDER_HORIZONTAL);
    if (picker->slider_r) {
        vg_slider_set_range(picker->slider_r, 0, 255);
        vg_slider_set_value(picker->slider_r, 0);
        vg_slider_set_on_change(picker->slider_r, on_slider_r_change, picker);
    }

    picker->slider_g = vg_slider_create(&picker->base, VG_SLIDER_HORIZONTAL);
    if (picker->slider_g) {
        vg_slider_set_range(picker->slider_g, 0, 255);
        vg_slider_set_value(picker->slider_g, 0);
        vg_slider_set_on_change(picker->slider_g, on_slider_g_change, picker);
    }

    picker->slider_b = vg_slider_create(&picker->base, VG_SLIDER_HORIZONTAL);
    if (picker->slider_b) {
        vg_slider_set_range(picker->slider_b, 0, 255);
        vg_slider_set_value(picker->slider_b, 0);
        vg_slider_set_on_change(picker->slider_b, on_slider_b_change, picker);
    }

    picker->slider_a = vg_slider_create(&picker->base, VG_SLIDER_HORIZONTAL);
    if (picker->slider_a) {
        vg_slider_set_range(picker->slider_a, 0, 255);
        vg_slider_set_value(picker->slider_a, 255);
        vg_slider_set_on_change(picker->slider_a, on_slider_a_change, picker);
        // Hide by default
        if (!picker->show_alpha) {
            vg_widget_set_visible(&picker->slider_a->base, false);
        }
    }

    // Quick palette
    picker->palette = vg_colorpalette_create(&picker->base);
    if (picker->palette) {
        vg_colorpalette_load_standard_16(picker->palette);
        vg_colorpalette_set_on_select(picker->palette, on_palette_select, picker);
        if (!picker->show_palette) {
            vg_widget_set_visible(&picker->palette->base, false);
        }
    }

    // Set minimum size
    picker->base.constraints.min_width = 200.0f;
    picker->base.constraints.min_height = 150.0f;
    colorpicker_update_accessible_value(picker);

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &picker->base);
    }

    return picker;
}

/// @brief Completes picker-specific destruction.
///
/// @details The picker has no separately owned allocations; the base widget
/// destroys the child swatch, sliders, and palette through the widget hierarchy.
///
/// @param widget Picker base widget being destroyed.
static void colorpicker_destroy(vg_widget_t *widget) {
    (void)widget;
    // Child widgets are destroyed automatically through widget hierarchy
}

/// @brief Measures the picker from its preview, channel rows, and optional palette.
///
/// @details The intrinsic width is fixed at 200 logical pixels. Height includes
/// the preview row, three or four slider rows, and two palette rows when
/// enabled, after which the widget constraints are applied.
///
/// @param widget Picker base widget whose measured dimensions are updated.
/// @param available_width Width offered by the parent; currently unused.
/// @param available_height Height offered by the parent; currently unused.
static void colorpicker_measure(vg_widget_t *widget,
                                float available_width,
                                float available_height) {
    vg_colorpicker_t *picker = (vg_colorpicker_t *)widget;
    (void)available_width;
    (void)available_height;

    // Calculate minimum height needed
    float height = 0;
    float width = 200.0f;

    // Preview swatch row
    height += 48.0f + 8.0f; // swatch + gap

    // RGB sliders (3 rows, or 4 if alpha shown)
    int slider_count = picker->show_alpha ? 4 : 3;
    height += slider_count * (24.0f + 4.0f); // slider height + gap

    // Palette if shown
    if (picker->show_palette && picker->palette) {
        height += 8.0f;             // gap
        height += 2 * 20.0f + 2.0f; // 2 rows of swatches
    }

    widget->measured_width = width;
    widget->measured_height = height;

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

/// @brief Arranges the picker preview, channel sliders, and optional palette.
///
/// @param widget Picker base widget and child owner.
/// @param x Allocated left coordinate.
/// @param y Allocated top coordinate.
/// @param width Allocated width used to size sliders and the palette.
/// @param height Allocated height stored on the base widget.
static void colorpicker_arrange(vg_widget_t *widget, float x, float y, float width, float height) {
    vg_colorpicker_t *picker = (vg_colorpicker_t *)widget;

    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;

    float current_y = y + 4.0f;
    float padding = 4.0f;
    float slider_height = 24.0f;
    float label_width = picker->show_labels ? 20.0f : 0;
    float value_width = picker->show_values ? 40.0f : 0;

    // Arrange preview swatch (right-aligned)
    if (picker->preview) {
        float swatch_size = 48.0f;
        vg_widget_arrange(&picker->preview->base,
                          x + width - swatch_size - padding,
                          current_y,
                          swatch_size,
                          swatch_size);
    }

    // Calculate slider width
    float slider_width =
        width - padding * 2 - label_width - value_width - 56.0f; // Leave space for preview

    // Arrange R slider
    if (picker->slider_r) {
        vg_widget_arrange(&picker->slider_r->base,
                          x + padding + label_width,
                          current_y,
                          slider_width,
                          slider_height);
        current_y += slider_height + 4.0f;
    }

    // Arrange G slider
    if (picker->slider_g) {
        vg_widget_arrange(&picker->slider_g->base,
                          x + padding + label_width,
                          current_y,
                          slider_width,
                          slider_height);
        current_y += slider_height + 4.0f;
    }

    // Arrange B slider
    if (picker->slider_b) {
        vg_widget_arrange(&picker->slider_b->base,
                          x + padding + label_width,
                          current_y,
                          slider_width,
                          slider_height);
        current_y += slider_height + 4.0f;
    }

    // Arrange A slider if visible
    if (picker->slider_a && picker->show_alpha) {
        vg_widget_arrange(&picker->slider_a->base,
                          x + padding + label_width,
                          current_y,
                          slider_width,
                          slider_height);
        current_y += slider_height + 4.0f;
    }

    // Arrange palette if visible
    if (picker->palette && picker->show_palette) {
        current_y += 8.0f; // Extra gap
        float palette_width = width - padding * 2;
        float palette_height = 2 * 20.0f + 2.0f; // 2 rows
        vg_widget_arrange(
            &picker->palette->base, x + padding, current_y, palette_width, palette_height);
    }
}

/// @brief Paints channel labels, numeric values, and the picker focus outline.
///
/// @details Child controls paint themselves. This pass aligns optional labels
/// and decimal readouts with each visible slider, emphasizes the active
/// keyboard channel, and outlines the complete picker while focused.
///
/// @param widget Picker base widget to render.
/// @param canvas Destination drawing context.
static void colorpicker_paint(vg_widget_t *widget, void *canvas) {
    vg_colorpicker_t *picker = (vg_colorpicker_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    if (picker->font && (picker->show_labels || picker->show_values)) {
        static const char *channel_names[] = {"R", "G", "B", "A"};
        vg_slider_t *sliders[] = {
            picker->slider_r, picker->slider_g, picker->slider_b, picker->slider_a};
        unsigned values[] = {picker->r, picker->g, picker->b, picker->a};
        int channel_count = picker->show_alpha ? 4 : 3;
        vg_font_metrics_t metrics;
        vg_font_get_metrics(picker->font, picker->font_size, &metrics);
        for (int channel = 0; channel < channel_count; channel++) {
            vg_slider_t *slider = sliders[channel];
            if (!slider)
                continue;
            float baseline = slider->base.y +
                             (slider->base.height - (float)metrics.line_height) * 0.5f +
                             metrics.ascent;
            uint32_t text_color = channel == picker->active_channel ? theme->colors.accent_primary
                                                                    : theme->colors.fg_primary;
            if (picker->show_labels) {
                vg_font_draw_text(canvas,
                                  picker->font,
                                  picker->font_size,
                                  widget->x + 4.0f,
                                  baseline,
                                  channel_names[channel],
                                  text_color);
            }
            if (picker->show_values) {
                char value[4];
                (void)snprintf(value, sizeof(value), "%u", values[channel]);
                vg_font_draw_text(canvas,
                                  picker->font,
                                  picker->font_size,
                                  slider->base.x + slider->base.width + 6.0f,
                                  baseline,
                                  value,
                                  text_color);
            }
        }
    }
    if ((widget->state & VG_STATE_FOCUSED) && widget->width > 0 && widget->height > 0) {
        vgfx_rect((vgfx_window_t)canvas,
                  (int32_t)widget->x,
                  (int32_t)widget->y,
                  (int32_t)widget->width,
                  (int32_t)widget->height,
                  theme->colors.border_focus);
    }
}

/// @brief Returns one channel component selected by keyboard navigation.
///
/// @param picker Picker to inspect.
/// @param channel Channel index: zero for red through three for alpha.
/// @return Component value in the range `[0, 255]`, or zero for a null picker
///         or invalid channel.
static uint8_t colorpicker_channel_value(const vg_colorpicker_t *picker, int channel) {
    if (!picker)
        return 0;
    switch (channel) {
        case 0:
            return picker->r;
        case 1:
            return picker->g;
        case 2:
            return picker->b;
        case 3:
            return picker->a;
        default:
            return 0;
    }
}

/// @brief Sets one channel through the normal synchronized public APIs.
///
/// @details RGB updates preserve the other color channels, while alpha updates
/// use the dedicated alpha setter. Invalid channels leave the picker unchanged.
///
/// @param picker Picker to update.
/// @param channel Channel index: zero for red through three for alpha.
/// @param value New component value in the range `[0, 255]`.
static void colorpicker_set_channel_value(vg_colorpicker_t *picker, int channel, uint8_t value) {
    if (!picker)
        return;
    if (channel == 3) {
        vg_colorpicker_set_alpha(picker, value);
        return;
    }
    uint8_t r = picker->r;
    uint8_t g = picker->g;
    uint8_t b = picker->b;
    if (channel == 0)
        r = value;
    else if (channel == 1)
        g = value;
    else if (channel == 2)
        b = value;
    else
        return;
    vg_colorpicker_set_rgb(picker, r, g, b);
}

/// @brief Navigates and edits RGB or RGBA channels from the keyboard.
///
/// @details Up and Down wrap the active-channel index. Left and Right adjust
/// the selected value by one, or ten while Shift is held; Home and End choose
/// the channel minimum and maximum. Resulting values are clamped to one byte.
///
/// @param widget Picker base widget receiving the event.
/// @param event Event to inspect and mark handled when consumed.
/// @return `true` when a supported enabled key event is consumed; otherwise
///         `false`.
static bool colorpicker_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_colorpicker_t *picker = (vg_colorpicker_t *)widget;
    if (!widget->enabled || !event || event->type != VG_EVENT_KEY_DOWN)
        return false;
    int channel_count = picker->show_alpha ? 4 : 3;
    if (event->key.key == VG_KEY_UP || event->key.key == VG_KEY_DOWN) {
        int delta = event->key.key == VG_KEY_UP ? -1 : 1;
        int next = picker->active_channel + delta;
        if (next < 0)
            next = channel_count - 1;
        if (next >= channel_count)
            next = 0;
        if (next != picker->active_channel) {
            picker->active_channel = next;
            widget->needs_paint = true;
            vg_widget_note_revision(widget);
        }
        event->handled = true;
        return true;
    }
    uint8_t old_value = colorpicker_channel_value(picker, picker->active_channel);
    int value = old_value;
    int step = (event->modifiers & VG_MOD_SHIFT) != 0 ? 10 : 1;
    if (event->key.key == VG_KEY_LEFT)
        value -= step;
    else if (event->key.key == VG_KEY_RIGHT)
        value += step;
    else if (event->key.key == VG_KEY_HOME)
        value = 0;
    else if (event->key.key == VG_KEY_END)
        value = 255;
    else
        return false;
    if (value < 0)
        value = 0;
    if (value > 255)
        value = 255;
    colorpicker_set_channel_value(picker, picker->active_channel, (uint8_t)value);
    event->handled = true;
    return true;
}

/// @brief Reports whether the picker can receive keyboard focus.
///
/// @param widget Picker base widget to inspect.
/// @return `true` when the widget is enabled and visible; otherwise `false`.
static bool colorpicker_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

//=============================================================================
// Internal Helpers
//=============================================================================

/// @brief Packs the component fields into the picker's AARRGGBB color.
///
/// @param picker Picker whose `a`, `r`, `g`, and `b` fields are authoritative.
static void colorpicker_update_color_from_components(vg_colorpicker_t *picker) {
    picker->color = ((uint32_t)picker->a << 24) | ((uint32_t)picker->r << 16) |
                    ((uint32_t)picker->g << 8) | ((uint32_t)picker->b);
}

/// @brief Unpacks the picker's AARRGGBB color into its component fields.
///
/// @param picker Picker whose packed `color` field is authoritative.
static void colorpicker_update_components_from_color(vg_colorpicker_t *picker) {
    picker->a = (picker->color >> 24) & 0xFF;
    picker->r = (picker->color >> 16) & 0xFF;
    picker->g = (picker->color >> 8) & 0xFF;
    picker->b = picker->color & 0xFF;
}

/// @brief Pushes the current packed color into the preview swatch.
///
/// @param picker Picker whose preview should be refreshed; may be null.
static void colorpicker_update_preview(vg_colorpicker_t *picker) {
    if (picker && picker->preview)
        vg_colorswatch_set_color(picker->preview, picker->color);
}

/// @brief Refresh the picker's semantic color value for accessibility bridges.
///
/// @details The exposed text combines a six-digit RGB value with the decimal
/// alpha component.
///
/// @param picker Picker whose RGB and alpha components should be described.
static void colorpicker_update_accessible_value(vg_colorpicker_t *picker) {
    if (!picker)
        return;
    char value[48];
    (void)snprintf(
        value, sizeof(value), "#%06X alpha %u", picker->color & 0x00FFFFFFu, (unsigned)picker->a);
    vg_widget_set_accessible_value(&picker->base, value);
}

/// @brief Synchronizes slider positions with the current component fields.
///
/// @details Child callbacks are suppressed for the duration of synchronization
/// so programmatic slider updates cannot recursively emit picker changes.
///
/// @param picker Picker whose sliders should be updated.
/// @param sync_alpha Whether to update the alpha slider in addition to RGB.
static void colorpicker_sync_sliders(vg_colorpicker_t *picker, bool sync_alpha) {
    if (!picker)
        return;
    picker->syncing_children = true;
    if (picker->slider_r)
        vg_slider_set_value(picker->slider_r, picker->r);
    if (picker->slider_g)
        vg_slider_set_value(picker->slider_g, picker->g);
    if (picker->slider_b)
        vg_slider_set_value(picker->slider_b, picker->b);
    if (sync_alpha && picker->slider_a)
        vg_slider_set_value(picker->slider_a, picker->a);
    picker->syncing_children = false;
}

/// @brief Finalizes a color update and emits change notifications when needed.
///
/// @details The preview and repaint state are always refreshed. Accessibility,
/// widget observers, and the application callback are updated only when the
/// packed color differs from @p old_color.
///
/// @param picker Picker whose change is being finalized.
/// @param old_color Packed AARRGGBB value from before the update.
static void colorpicker_emit_change(vg_colorpicker_t *picker, uint32_t old_color) {
    if (!picker)
        return;
    colorpicker_update_preview(picker);
    picker->base.needs_paint = true;
    if (old_color != picker->color) {
        colorpicker_update_accessible_value(picker);
        vg_widget_note_change(&picker->base);
        if (picker->on_change)
            picker->on_change(&picker->base, picker->color, picker->on_change_data);
    }
}

//=============================================================================
// ColorPicker API
//=============================================================================

/// @brief Sets the picker's color from a packed AARRGGBB value.
///
/// @details The component fields, every channel slider, and the preview are
/// synchronized. Observers are notified only when the packed value changes.
///
/// @param picker The color picker to update.
/// @param color New AARRGGBB color value.
void vg_colorpicker_set_color(vg_colorpicker_t *picker, uint32_t color) {
    if (!picker)
        return;

    uint32_t old_color = picker->color;
    picker->color = color;
    colorpicker_update_components_from_color(picker);

    colorpicker_sync_sliders(picker, true);
    colorpicker_emit_change(picker, old_color);
}

/// @brief Returns the picker's current color as a packed AARRGGBB value.
///
/// @param picker The color picker to query.
/// @return Current AARRGGBB color, or zero when @p picker is null.
uint32_t vg_colorpicker_get_color(vg_colorpicker_t *picker) {
    if (!picker)
        return 0;
    return picker->color;
}

/// @brief Sets the RGB components without changing alpha.
///
/// @details The packed color, RGB sliders, and preview are synchronized.
/// Observers are notified only when the resulting packed color changes.
///
/// @param picker The color picker to update.
/// @param r New red component in the range `[0, 255]`.
/// @param g New green component in the range `[0, 255]`.
/// @param b New blue component in the range `[0, 255]`.
void vg_colorpicker_set_rgb(vg_colorpicker_t *picker, uint8_t r, uint8_t g, uint8_t b) {
    if (!picker)
        return;

    uint32_t old_color = picker->color;
    picker->r = r;
    picker->g = g;
    picker->b = b;
    colorpicker_update_color_from_components(picker);

    colorpicker_sync_sliders(picker, false);
    colorpicker_emit_change(picker, old_color);
}

/// @brief Retrieves the current red, green, and blue components.
///
/// @param picker The color picker to query.
/// @param[out] r Receives red when non-null.
/// @param[out] g Receives green when non-null.
/// @param[out] b Receives blue when non-null.
void vg_colorpicker_get_rgb(vg_colorpicker_t *picker, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!picker)
        return;
    if (r)
        *r = picker->r;
    if (g)
        *g = picker->g;
    if (b)
        *b = picker->b;
}

/// @brief Sets the alpha component without changing RGB.
///
/// @details The packed color, alpha slider, and preview are synchronized.
/// Observers are notified only when the resulting packed color changes.
///
/// @param picker The color picker to update.
/// @param alpha New alpha component; zero is transparent and 255 is opaque.
void vg_colorpicker_set_alpha(vg_colorpicker_t *picker, uint8_t alpha) {
    if (!picker)
        return;

    uint32_t old_color = picker->color;
    picker->a = alpha;
    colorpicker_update_color_from_components(picker);

    colorpicker_sync_sliders(picker, true);
    colorpicker_emit_change(picker, old_color);
}

/// @brief Returns the current alpha component.
///
/// @param picker The color picker to query.
/// @return Alpha in the range `[0, 255]`, or 255 when @p picker is null.
uint8_t vg_colorpicker_get_alpha(vg_colorpicker_t *picker) {
    if (!picker)
        return 255;
    return picker->a;
}

/// @brief Show or hide the alpha slider row.
///
/// @details Hiding alpha while it is the active keyboard channel returns the
/// active channel to red. The stored alpha value itself is preserved.
///
/// @param picker The color picker to configure.
/// @param show `true` to display alpha editing; `false` to hide it.
void vg_colorpicker_show_alpha(vg_colorpicker_t *picker, bool show) {
    if (!picker || picker->show_alpha == show)
        return;

    picker->show_alpha = show;
    if (picker->slider_a) {
        vg_widget_set_visible(&picker->slider_a->base, show);
    }
    if (!show && picker->active_channel == 3)
        picker->active_channel = 0;
    picker->base.needs_layout = true;
    picker->base.needs_paint = true;
    vg_widget_note_revision(&picker->base);
}

/// @brief Return whether alpha-channel editing is enabled.
///
/// @param picker Picker to inspect.
/// @return `true` when the alpha slider is enabled and visible; otherwise
///         `false`.
bool vg_colorpicker_is_alpha_enabled(const vg_colorpicker_t *picker) {
    return picker ? picker->show_alpha : false;
}

/// @brief Shows or hides the quick-access 16-color palette.
///
/// @param picker The color picker to configure.
/// @param show `true` to display the palette; `false` to hide it.
void vg_colorpicker_show_palette(vg_colorpicker_t *picker, bool show) {
    if (!picker || picker->show_palette == show)
        return;

    picker->show_palette = show;
    if (picker->palette) {
        vg_widget_set_visible(&picker->palette->base, show);
    }
    picker->base.needs_layout = true;
    picker->base.needs_paint = true;
    vg_widget_note_revision(&picker->base);
}

/// @brief Registers a callback for committed picker color changes.
///
/// @param picker The color picker to configure.
/// @param callback Function called with the widget, packed color, and
///                 @p user_data; may be null to unregister.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_colorpicker_set_on_change(vg_colorpicker_t *picker,
                                  vg_colorpicker_callback_t callback,
                                  void *user_data) {
    if (!picker)
        return;

    picker->on_change = callback;
    picker->on_change_data = user_data;
}

/// @brief Set the font and size used to render channel labels and value readouts.
///
/// @param picker The color picker to configure.
/// @param font Borrowed font used while the caller keeps it alive; may be null
///             to suppress text rendering.
/// @param size Finite positive font size in logical pixels; invalid values use
///             a 12-pixel fallback.
void vg_colorpicker_set_font(vg_colorpicker_t *picker, vg_font_t *font, float size) {
    if (!picker)
        return;

    float normalized = isfinite(size) && size > 0 ? size : 12.0f;
    if (picker->font == font && picker->font_size == normalized)
        return;
    picker->font = font;
    picker->font_size = normalized;
    picker->base.needs_paint = true;
    vg_widget_note_revision(&picker->base);
}
