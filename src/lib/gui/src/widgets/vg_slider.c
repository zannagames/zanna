//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_slider.c
// Purpose: Slider widget implementation — horizontal or vertical range selector
//          with clamping, optional step snapping, and an on_change callback.
// Key invariants:
//   - value is always clamped to [min_value, max_value] after every set.
//   - step == 0 means continuous (no snapping); step > 0 snaps to nearest multiple.
//   - on_change is fired only when the value actually changes.
// Ownership/Lifetime:
//   - No heap-allocated fields beyond the widget itself.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements horizontal and vertical sliders with clamping, step
///        snapping, pointer capture, keyboard control, and change callbacks.
/// @details Geometry conversion consistently reserves the thumb radius at both
///          track ends. Value mutations normalize non-finite input and notify
///          only on an actual stored-value transition.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <math.h>
#include <stdlib.h>

//=============================================================================
// Forward declarations
//=============================================================================

static void slider_measure(vg_widget_t *widget, float available_width, float available_height);
static void slider_arrange(vg_widget_t *widget, float x, float y, float w, float h);
static void slider_paint(vg_widget_t *widget, void *canvas);
static bool slider_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool slider_can_focus(vg_widget_t *widget);

//=============================================================================
// VTable
//=============================================================================

static vg_widget_vtable_t g_slider_vtable = {
    .destroy = NULL,
    .measure = slider_measure,
    .arrange = slider_arrange,
    .paint = slider_paint,
    .handle_event = slider_handle_event,
    .can_focus = slider_can_focus,
    .on_focus = NULL,
};

//=============================================================================
// Vtable Implementations
//=============================================================================

/// @brief Returns the slider's current value as a normalised fraction in [0, 1] within [min, max].
/// @param slider Slider whose stored value and range are inspected.
/// @return Clamped normalized fraction, or zero for a degenerate range.
static float slider_normalized_value(const vg_slider_t *slider) {
    float range = slider->max_value - slider->min_value;
    float norm = (range > 0.0f) ? (slider->value - slider->min_value) / range : 0.0f;
    if (norm < 0.0f)
        norm = 0.0f;
    if (norm > 1.0f)
        norm = 1.0f;
    return norm;
}

/// @brief Converts widget-local coordinates @p x/@p y to a normalised fraction [0, 1] along the
/// track axis.
/// @param slider Arranged slider supplying orientation and thumb size.
/// @param x Pointer X coordinate relative to the widget.
/// @param y Pointer Y coordinate relative to the widget.
/// @return Clamped normalized position; vertical sliders increase upward.
static float slider_normalized_from_point(const vg_slider_t *slider, float x, float y) {
    float thumb_r = slider->thumb_size > 0.0f ? slider->thumb_size * 0.5f : 8.0f;
    float norm = 0.0f;
    if (slider->orientation == VG_SLIDER_HORIZONTAL) {
        float track_len = slider->base.width - thumb_r * 2.0f;
        norm = track_len > 0.0f ? (x - thumb_r) / track_len : 0.0f;
    } else {
        float track_len = slider->base.height - thumb_r * 2.0f;
        norm = track_len > 0.0f ? ((slider->base.height - thumb_r) - y) / track_len : 0.0f;
    }
    if (norm < 0.0f)
        norm = 0.0f;
    if (norm > 1.0f)
        norm = 1.0f;
    return norm;
}

/// @brief VTable measure: sets a 100 px preferred length along the track axis and thumb_size along
/// the cross axis.
/// @param widget Slider widget base to measure.
/// @param available_width Offered width, unused before constraints.
/// @param available_height Offered height, unused before constraints.
static void slider_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_slider_t *slider = (vg_slider_t *)widget;
    (void)available_width;
    (void)available_height;
    if (slider->orientation == VG_SLIDER_HORIZONTAL) {
        widget->measured_width = 100.0f;
        widget->measured_height = slider->thumb_size > 0 ? slider->thumb_size : 24.0f;
    } else {
        widget->measured_width = slider->thumb_size > 0 ? slider->thumb_size : 24.0f;
        widget->measured_height = 100.0f;
    }
    vg_widget_apply_constraints(widget);
}

/// @brief VTable arrange: stores the assigned position and dimensions; the slider has no children
/// to arrange.
/// @param widget Slider widget base to arrange.
/// @param x Assigned left coordinate.
/// @param y Assigned top coordinate.
/// @param w Assigned width.
/// @param h Assigned height.
static void slider_arrange(vg_widget_t *widget, float x, float y, float w, float h) {
    widget->x = x;
    widget->y = y;
    widget->width = w;
    widget->height = h;
}

/// @brief VTable can_focus: returns true when the widget is both enabled and visible.
/// @param widget Candidate slider widget.
/// @return `true` when keyboard focus is permitted.
static bool slider_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief VTable paint: draws the track background, filled portion, thumb circle with border and
/// hover/drag tint, and focus rect.
/// @param widget Arranged slider widget base to paint.
/// @param canvas Backend canvas used for track, thumb, and focus primitives.
static void slider_paint(vg_widget_t *widget, void *canvas) {
    vg_slider_t *slider = (vg_slider_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;
    float x = widget->x, y = widget->y, w = widget->width, h = widget->height;
    float norm = slider_normalized_value(slider);
    uint32_t track_color =
        slider->track_color
            ? slider->track_color
            : vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.45f);
    uint32_t fill_color = slider->fill_color ? slider->fill_color : theme->colors.accent_primary;
    uint32_t thumb_color = slider->thumb_color ? slider->thumb_color : theme->colors.bg_primary;
    if (slider->dragging) {
        thumb_color = vg_color_lighten(fill_color, 0.30f);
    } else if (slider->thumb_hovered) {
        thumb_color = slider->thumb_hover_color ? slider->thumb_hover_color
                                                : vg_color_lighten(thumb_color, 0.12f);
    }

    if (slider->orientation == VG_SLIDER_HORIZONTAL) {
        float track_th = slider->track_thickness > 0 ? slider->track_thickness : 4.0f;
        float thumb_rf = slider->thumb_size * 0.5f;
        float track_xf = x + thumb_rf;
        float track_wf = w - thumb_rf * 2.0f;
        if (track_wf < 1.0f)
            track_wf = 1.0f;
        int32_t track_y = (int32_t)(y + (h - track_th) / 2.0f);
        int32_t track_h = (int32_t)track_th;

        float track_rad = track_th * 0.5f;
        vg_draw_round_rect_fill(
            win, track_xf, (float)track_y, track_wf, (float)track_h, track_rad, track_color);

        float fill_wf = norm * track_wf;
        if (fill_wf > 0.5f)
            vg_draw_round_rect_fill(
                win, track_xf, (float)track_y, fill_wf, (float)track_h, track_rad, fill_color);

        float thumb_cx = track_xf + norm * track_wf;
        float thumb_cy = y + h / 2.0f;
        vg_draw_disc_fill(win, thumb_cx, thumb_cy, thumb_rf + 1.0f, theme->colors.border_primary);
        vg_draw_disc_fill(win, thumb_cx, thumb_cy, thumb_rf, thumb_color);
        vg_draw_circle_stroke(
            win, thumb_cx, thumb_cy, thumb_rf, 1.0f, vg_color_darken(thumb_color, 0.18f));
    } else {
        float track_th = slider->track_thickness > 0 ? slider->track_thickness : 4.0f;
        float thumb_rf = slider->thumb_size * 0.5f;
        float track_yf = y + thumb_rf;
        float track_hf = h - thumb_rf * 2.0f;
        if (track_hf < 1.0f)
            track_hf = 1.0f;
        int32_t track_x = (int32_t)(x + (w - track_th) / 2.0f);
        int32_t track_w = (int32_t)track_th;

        float track_rad = track_th * 0.5f;
        vg_draw_round_rect_fill(
            win, (float)track_x, track_yf, (float)track_w, track_hf, track_rad, track_color);

        float fill_hf = norm * track_hf;
        float fill_yf = track_yf + track_hf - fill_hf;
        if (fill_hf > 0.5f)
            vg_draw_round_rect_fill(
                win, (float)track_x, fill_yf, (float)track_w, fill_hf, track_rad, fill_color);

        float thumb_cx = x + w / 2.0f;
        float thumb_cy = track_yf + track_hf - norm * track_hf;
        vg_draw_disc_fill(win, thumb_cx, thumb_cy, thumb_rf + 1.0f, theme->colors.border_primary);
        vg_draw_disc_fill(win, thumb_cx, thumb_cy, thumb_rf, thumb_color);
        vg_draw_circle_stroke(
            win, thumb_cx, thumb_cy, thumb_rf, 1.0f, vg_color_darken(thumb_color, 0.18f));
    }

    if (widget->state & VG_STATE_FOCUSED) {
        vg_draw_round_rect_stroke(
            win, x, y, w, h, theme->radius.sm, 1.5f, theme->colors.border_focus);
    }
}

/// @brief VTable handle_event: handles thumb drag (mouse-down/move/up), track click-to-jump, leave
/// unhover, and arrow/Home/End keyboard control.
/// @param widget Slider widget receiving input.
/// @param event Pointer or keyboard event to interpret.
/// @return `true` when dragging, jumping, or keyboard adjustment consumes the event.
static bool slider_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_slider_t *slider = (vg_slider_t *)widget;
    float w = widget->width, h = widget->height;

    bool horizontal = (slider->orientation == VG_SLIDER_HORIZONTAL);
    float thumb_r = slider->thumb_size / 2.0f;

    switch (event->type) {
        case VG_EVENT_MOUSE_DOWN: {
            float norm = slider_normalized_value(slider);
            float track_w = horizontal ? (w - thumb_r * 2.0f) : w;
            float track_h = horizontal ? h : (h - thumb_r * 2.0f);
            float thumb_cx = horizontal ? (thumb_r + norm * track_w) : (w / 2.0f);
            float thumb_cy = horizontal ? (h / 2.0f) : (thumb_r + track_h - norm * track_h);
            float mx = event->mouse.x, my = event->mouse.y;
            float dx = mx - thumb_cx, dy = my - thumb_cy;
            if (dx * dx + dy * dy <= thumb_r * thumb_r) {
                slider->dragging = true;
                vg_widget_set_input_capture(widget);
                widget->needs_paint = true;
                event->handled = true;
                return true;
            }
            if (event->mouse.x >= 0.0f && event->mouse.x <= w && event->mouse.y >= 0.0f &&
                event->mouse.y <= h) {
                float click_norm =
                    slider_normalized_from_point(slider, event->mouse.x, event->mouse.y);
                float range = slider->max_value - slider->min_value;
                vg_slider_set_value(slider, slider->min_value + click_norm * range);
                slider->dragging = true;
                slider->thumb_hovered = true;
                vg_widget_set_input_capture(widget);
                widget->needs_paint = true;
                event->handled = true;
                return true;
            }
            break;
        }

        case VG_EVENT_MOUSE_MOVE: {
            float mx = event->mouse.x, my = event->mouse.y;
            if (slider->dragging) {
                float norm = slider_normalized_from_point(slider, mx, my);
                float range = slider->max_value - slider->min_value;
                float new_val = slider->min_value + norm * range;
                vg_slider_set_value(slider, new_val);
                slider->thumb_hovered = true;
                widget->needs_paint = true;
                event->handled = true;
                return true;
            }
            float norm = slider_normalized_value(slider);
            float track_w = horizontal ? (w - thumb_r * 2.0f) : w;
            float track_h = horizontal ? h : (h - thumb_r * 2.0f);
            float thumb_cx = horizontal ? (thumb_r + norm * track_w) : (w / 2.0f);
            float thumb_cy = horizontal ? (h / 2.0f) : (thumb_r + track_h - norm * track_h);
            float dx = mx - thumb_cx, dy = my - thumb_cy;
            bool hovered = (dx * dx + dy * dy <= thumb_r * thumb_r);
            if (hovered != slider->thumb_hovered) {
                slider->thumb_hovered = hovered;
                widget->needs_paint = true;
            }
            break;
        }

        case VG_EVENT_MOUSE_UP: {
            if (slider->dragging) {
                slider->dragging = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                widget->needs_paint = true;
                event->handled = true;
                return true;
            }
            break;
        }

        case VG_EVENT_MOUSE_LEAVE: {
            if (!slider->dragging && slider->thumb_hovered) {
                slider->thumb_hovered = false;
                widget->needs_paint = true;
            }
            break;
        }

        case VG_EVENT_KEY_DOWN: {
            /* Arrow keys adjust the slider value by one step (or 1% of range
             * when step == 0).  Home/End jump to the min/max extremes. */
            float step = (slider->step > 0.0f) ? slider->step
                                               : (slider->max_value - slider->min_value) * 0.01f;
            switch (event->key.key) {
                case VG_KEY_RIGHT:
                case VG_KEY_UP:
                    vg_slider_set_value(slider, slider->value + step);
                    event->handled = true;
                    return true;
                case VG_KEY_LEFT:
                case VG_KEY_DOWN:
                    vg_slider_set_value(slider, slider->value - step);
                    event->handled = true;
                    return true;
                case VG_KEY_HOME:
                    vg_slider_set_value(slider, slider->min_value);
                    event->handled = true;
                    return true;
                case VG_KEY_END:
                    vg_slider_set_value(slider, slider->max_value);
                    event->handled = true;
                    return true;
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
    return false;
}

/// @brief Create a slider widget.
///
/// @param parent      Widget to attach to as a child (may be NULL).
/// @param orientation VG_SLIDER_HORIZONTAL or VG_SLIDER_VERTICAL.
/// @return Newly allocated vg_slider_t, or NULL on allocation failure.
vg_slider_t *vg_slider_create(vg_widget_t *parent, vg_slider_orientation_t orientation) {
    vg_slider_t *slider = calloc(1, sizeof(vg_slider_t));
    if (!slider)
        return NULL;

    vg_widget_init(&slider->base, VG_WIDGET_SLIDER, &g_slider_vtable);
    slider->orientation = orientation;

    // Default values
    slider->min_value = 0;
    slider->max_value = 100;
    slider->value = 0;
    slider->step = 0; // continuous

    // Default appearance
    vg_theme_t *theme = vg_theme_get_current();
    float scale = theme && theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    slider->track_thickness = 4.0f * scale;
    slider->thumb_size = 16.0f * scale;
    slider->track_color =
        theme ? vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.45f)
              : 0x003C3C3C;
    slider->fill_color = theme ? theme->colors.accent_primary : 0x000078D4;
    slider->thumb_color = theme ? theme->colors.bg_primary : 0x00FFFFFF;
    slider->thumb_hover_color = theme ? vg_color_lighten(slider->thumb_color, 0.12f) : 0x00E0E0E0;
    slider->font_size = theme ? theme->typography.size_normal : 12.0f;

    if (parent) {
        vg_widget_add_child(parent, &slider->base);
    }

    return slider;
}

/// @brief Set the slider value; clamps to [min, max] and snaps to step if set.
///
/// @param slider The slider to update.
/// @param value  New value (clamped and step-snapped before storing).
void vg_slider_set_value(vg_slider_t *slider, float value) {
    if (!slider)
        return;

    if (!isfinite(value))
        value = slider->min_value;

    // Clamp to range
    if (value < slider->min_value)
        value = slider->min_value;
    if (value > slider->max_value)
        value = slider->max_value;

    // Snap to step if specified
    if (slider->step > 0) {
        float steps = (value - slider->min_value) / slider->step;
        value = slider->min_value + ((int)(steps + 0.5f)) * slider->step;
        if (value < slider->min_value)
            value = slider->min_value;
        if (value > slider->max_value)
            value = slider->max_value;
    }

    float old = slider->value;
    slider->value = value;

    slider->base.needs_paint = true;

    if (old != value) {
        vg_widget_note_change(&slider->base);
        if (slider->on_change)
            slider->on_change(&slider->base, value, slider->on_change_data);
    }
}

/// @brief Return the current slider value.
///
/// @param slider The slider to query.
/// @return Current value, or 0 if slider is NULL.
float vg_slider_get_value(vg_slider_t *slider) {
    return slider ? slider->value : 0;
}

/// @brief Set the slider's minimum and maximum values; re-clamps the current value.
///
/// @param slider  The slider to configure.
/// @param min_val Minimum value (swapped with max_val if larger).
/// @param max_val Maximum value.
void vg_slider_set_range(vg_slider_t *slider, float min_val, float max_val) {
    if (!slider)
        return;
    if (!isfinite(min_val) || !isfinite(max_val))
        return;
    if (min_val > max_val) {
        float tmp = min_val;
        min_val = max_val;
        max_val = tmp;
    }
    float old_min = slider->min_value;
    float old_max = slider->max_value;
    uint64_t old_change_revision = slider->base.change_revision;
    slider->min_value = min_val;
    slider->max_value = max_val;
    // Re-clamp current value
    vg_slider_set_value(slider, slider->value);
    if ((old_min != min_val || old_max != max_val) &&
        old_change_revision == slider->base.change_revision) {
        vg_widget_note_revision(&slider->base);
    }
    slider->base.needs_paint = true;
}

/// @brief Set the step increment for discrete snapping.
///
/// @param slider The slider to configure.
/// @param step   Positive step size; 0 or non-finite disables snapping (continuous).
void vg_slider_set_step(vg_slider_t *slider, float step) {
    if (!slider)
        return;
    float old_step = slider->step;
    uint64_t old_change_revision = slider->base.change_revision;
    slider->step = isfinite(step) && step > 0 ? step : 0;
    vg_slider_set_value(slider, slider->value);
    if (old_step != slider->step && old_change_revision == slider->base.change_revision)
        vg_widget_note_revision(&slider->base);
    slider->base.needs_paint = true;
}

/// @brief Set the change callback invoked when the slider value changes.
///
/// @param slider    The slider to configure.
/// @param callback  Function called with (widget, new_value, user_data).
/// @param user_data Opaque pointer passed to @p callback.
void vg_slider_set_on_change(vg_slider_t *slider, vg_slider_callback_t callback, void *user_data) {
    if (!slider)
        return;
    slider->on_change = callback;
    slider->on_change_data = user_data;
}
