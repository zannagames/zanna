//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_checkbox.c
// Purpose: Checkbox widget implementation — tri-state (checked/unchecked/
//          indeterminate) with label text, click-toggle, and change callbacks.
// Key invariants:
//   - checkbox->text is always a valid heap-allocated string (never NULL).
//   - Setting indeterminate clears checked and fires on_change if it was set.
//   - VG_STATE_CHECKED in base.state mirrors checkbox->checked at all times.
// Ownership/Lifetime:
//   - vg_checkbox_create copies the text string; checkbox_destroy frees it.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements the themed tri-state checkbox widget and its public state API.
/// @details A checkbox owns its copied label, borrows its font and callback payload, mirrors the
/// binary checked value into the base-widget state flags, and renders an independent mixed-state
/// indicator. Pointer clicks and Space/Enter keyboard activation share the same toggle path.

//=============================================================================
// Forward Declarations
//=============================================================================

static void checkbox_destroy(vg_widget_t *widget);
static void checkbox_measure(vg_widget_t *widget, float available_width, float available_height);
static void checkbox_paint(vg_widget_t *widget, void *canvas);
static bool checkbox_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool checkbox_can_focus(vg_widget_t *widget);

//=============================================================================
// Checkbox VTable
//=============================================================================

static vg_widget_vtable_t g_checkbox_vtable = {.destroy = checkbox_destroy,
                                               .measure = checkbox_measure,
                                               .arrange = NULL,
                                               .paint = checkbox_paint,
                                               .handle_event = checkbox_handle_event,
                                               .can_focus = checkbox_can_focus,
                                               .on_focus = NULL};

//=============================================================================
// Checkbox Implementation
//=============================================================================

/// @brief Create a checkbox widget.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @param text   Label text displayed to the right of the box (copied).
/// @return Newly allocated vg_checkbox_t, or NULL on allocation failure.
vg_checkbox_t *vg_checkbox_create(vg_widget_t *parent, const char *text) {
    vg_checkbox_t *checkbox = calloc(1, sizeof(vg_checkbox_t));
    if (!checkbox)
        return NULL;

    // Initialize base widget
    vg_widget_init(&checkbox->base, VG_WIDGET_CHECKBOX, &g_checkbox_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();
    float scale = theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;

    // Initialize checkbox-specific fields
    checkbox->text = text ? vg_strdup(text) : vg_strdup("");
    if (!checkbox->text) {
        vg_widget_destroy(&checkbox->base);
        return NULL;
    }
    checkbox->font = theme->typography.font_regular;
    checkbox->font_size = theme->typography.size_normal;
    checkbox->checked = false;
    checkbox->indeterminate = false;

    // Appearance
    checkbox->box_size = 18.0f * scale;
    checkbox->gap = 10.0f * scale;
    checkbox->check_color = theme->colors.fg_primary;
    checkbox->box_color = theme->colors.bg_tertiary;
    checkbox->text_color = theme->colors.fg_primary;

    // Callback
    checkbox->on_change = NULL;
    checkbox->on_change_data = NULL;

    // Set minimum size
    checkbox->base.constraints.min_height =
        theme->input.height > checkbox->box_size ? theme->input.height : checkbox->box_size;
    checkbox->base.constraints.min_width = checkbox->box_size;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &checkbox->base);
    }

    return checkbox;
}

/// @brief Release the copied checkbox label during base-widget destruction.
/// @param widget Checkbox base widget being destroyed.
static void checkbox_destroy(vg_widget_t *widget) {
    vg_checkbox_t *checkbox = (vg_checkbox_t *)widget;
    if (checkbox->text) {
        free(checkbox->text);
        checkbox->text = NULL;
    }
}

/// @brief Measure the checkbox square, label gap, and optional text.
/// @param widget Checkbox base widget whose measured dimensions are updated.
/// @param available_width Parent-provided width constraint; sizing is content-driven.
/// @param available_height Parent-provided height constraint; minimum constraints are applied.
static void checkbox_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_checkbox_t *checkbox = (vg_checkbox_t *)widget;
    (void)available_width;
    (void)available_height;

    float width = checkbox->box_size;
    float height = checkbox->box_size;

    // Add text width if we have text and font
    if (checkbox->text && checkbox->text[0] && checkbox->font) {
        vg_text_metrics_t metrics;
        vg_font_measure_text(checkbox->font, checkbox->font_size, checkbox->text, &metrics);
        width += checkbox->gap + metrics.width;
        if (metrics.height > height) {
            height = metrics.height;
        }
    }

    widget->measured_width = width;
    widget->measured_height = height;

    vg_widget_apply_constraints(widget);
}

/// @brief VTable paint: draws the box border, fill/check-mark when checked, focus ring, and label
/// text.
/// @param widget Checkbox base widget to render.
/// @param canvas Graphics window used for shapes and text.
static void checkbox_paint(vg_widget_t *widget, void *canvas) {
    vg_checkbox_t *checkbox = (vg_checkbox_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    // Determine colors based on state
    uint32_t box_color = checkbox->box_color;
    uint32_t check_color = checkbox->check_color;
    uint32_t text_color = checkbox->text_color;

    if (widget->state & VG_STATE_DISABLED) {
        box_color = theme->colors.bg_disabled;
        check_color = theme->colors.fg_disabled;
        text_color = theme->colors.fg_disabled;
    } else if (widget->state & VG_STATE_HOVERED) {
        box_color = theme->colors.bg_hover;
    }

    // Draw checkbox box
    float box_x = widget->x;
    float box_y = widget->y + (widget->height - checkbox->box_size) / 2.0f;

    vgfx_window_t win = (vgfx_window_t)canvas;
    int32_t bx = (int32_t)box_x;
    int32_t by = (int32_t)box_y;
    int32_t bs = (int32_t)checkbox->box_size;

    // Draw box background and border (anti-aliased rounded square)
    float cb_rad = theme->radius.sm;
    vg_draw_round_rect_fill(win, (float)bx, (float)by, (float)bs, (float)bs, cb_rad, box_color);
    vg_draw_round_rect_stroke(win,
                              (float)bx,
                              (float)by,
                              (float)bs,
                              (float)bs,
                              cb_rad,
                              1.0f,
                              theme->colors.border_primary);

    // Draw check mark or indeterminate mark
    if (checkbox->checked || checkbox->indeterminate) {
        if (checkbox->indeterminate) {
            // Dash for indeterminate
            vgfx_fill_rect(win, bx + 3, by + bs / 2 - 1, bs - 6, 2, check_color);
        } else {
            // Two-segment tick mark (✓): short leg then long leg, anti-aliased.
            vg_draw_line_aa(win,
                            bx + 2.0f,
                            by + bs / 2.0f,
                            bx + bs / 2.0f - 1.0f,
                            by + bs - 3.0f,
                            1.8f,
                            check_color);
            vg_draw_line_aa(win,
                            bx + bs / 2.0f - 1.0f,
                            by + bs - 3.0f,
                            bx + bs - 2.0f,
                            by + 2.0f,
                            1.8f,
                            check_color);
        }
    }

    // Draw focus ring
    if (widget->state & VG_STATE_FOCUSED) {
        vg_draw_round_rect_stroke(win,
                                  (float)(bx - 2),
                                  (float)(by - 2),
                                  (float)(bs + 4),
                                  (float)(bs + 4),
                                  cb_rad + 2.0f,
                                  1.5f,
                                  theme->colors.border_focus);
    }

    // Draw label text
    if (checkbox->text && checkbox->text[0] && checkbox->font) {
        float text_x = widget->x + checkbox->box_size + checkbox->gap;

        vg_font_metrics_t font_metrics;
        vg_font_get_metrics(checkbox->font, checkbox->font_size, &font_metrics);
        float text_y =
            widget->y +
            (widget->height + (float)font_metrics.ascent + (float)font_metrics.descent) / 2.0f;

        vg_font_draw_text(canvas,
                          checkbox->font,
                          checkbox->font_size,
                          text_x,
                          text_y,
                          checkbox->text,
                          text_color);
    }
}

/// @brief Handle pointer or keyboard checkbox activation.
/// @param widget Checkbox base widget receiving the event.
/// @param event Click or key event to process.
/// @return `true` when an enabled checkbox consumes an activation event.
static bool checkbox_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_checkbox_t *checkbox = (vg_checkbox_t *)widget;

    if (widget->state & VG_STATE_DISABLED) {
        return false;
    }

    if (event->type == VG_EVENT_CLICK) {
        vg_checkbox_toggle(checkbox);
        return true;
    }

    if (event->type == VG_EVENT_KEY_DOWN) {
        if (event->key.key == VG_KEY_SPACE || event->key.key == VG_KEY_ENTER) {
            vg_checkbox_toggle(checkbox);
            return true;
        }
    }

    return false;
}

/// @brief Return whether a checkbox may participate in keyboard focus traversal.
/// @param widget Checkbox base widget to inspect.
/// @return `true` when the widget is enabled and visible.
static bool checkbox_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

//=============================================================================
// Checkbox API
//=============================================================================

/// @brief Set the checkbox's checked state; clears indeterminate and fires on_change.
///
/// @param checkbox The checkbox to update.
/// @param checked  true to check; false to uncheck.
void vg_checkbox_set_checked(vg_checkbox_t *checkbox, bool checked) {
    if (!checkbox)
        return;

    bool old_checked = checkbox->checked;
    bool old_indeterminate = checkbox->indeterminate;
    checkbox->checked = checked;
    checkbox->indeterminate = false;

    if (checked) {
        checkbox->base.state |= VG_STATE_CHECKED;
    } else {
        checkbox->base.state &= ~VG_STATE_CHECKED;
    }

    if (old_checked != checked || old_indeterminate) {
        checkbox->base.needs_paint = true;
        vg_widget_note_change(&checkbox->base);

        if (old_checked != checked && checkbox->on_change) {
            checkbox->on_change(&checkbox->base, checked, checkbox->on_change_data);
        }
    }
}

/// @brief Return true if the checkbox is currently checked.
///
/// @param checkbox The checkbox to query.
/// @return The checked state, or false if checkbox is NULL.
bool vg_checkbox_is_checked(vg_checkbox_t *checkbox) {
    return checkbox ? checkbox->checked : false;
}

/// @brief Toggle the checkbox between checked and unchecked.
///
/// @param checkbox The checkbox to toggle.
void vg_checkbox_toggle(vg_checkbox_t *checkbox) {
    if (!checkbox)
        return;

    vg_checkbox_set_checked(checkbox, !checkbox->checked);
}

/// @brief Set the checkbox's indeterminate (mixed) state.
///
/// @param checkbox       The checkbox to update.
/// @param indeterminate  true to enter indeterminate state (clears checked);
///                       false to leave it.
void vg_checkbox_set_indeterminate(vg_checkbox_t *checkbox, bool indeterminate) {
    if (!checkbox)
        return;

    bool old_checked = checkbox->checked;
    bool old_indeterminate = checkbox->indeterminate;
    checkbox->indeterminate = indeterminate;
    if (indeterminate) {
        checkbox->checked = false;
        checkbox->base.state &= ~VG_STATE_CHECKED;
    }
    if (old_checked == checkbox->checked && old_indeterminate == checkbox->indeterminate)
        return;

    checkbox->base.needs_paint = true;
    vg_widget_note_change(&checkbox->base);

    if (old_checked != checkbox->checked && checkbox->on_change)
        checkbox->on_change(&checkbox->base, checkbox->checked, checkbox->on_change_data);
}

/// @brief Return true if the checkbox is in the indeterminate state.
///
/// @param checkbox The checkbox to query.
/// @return The indeterminate state, or false if checkbox is NULL.
bool vg_checkbox_is_indeterminate(vg_checkbox_t *checkbox) {
    return checkbox ? checkbox->indeterminate : false;
}

/// @brief Replace the checkbox label text.
///
/// @param checkbox The checkbox to update.
/// @param text     New label (copied); NULL is treated as an empty string.
void vg_checkbox_set_text(vg_checkbox_t *checkbox, const char *text) {
    if (!checkbox)
        return;

    const char *new_text = text ? text : "";
    if (checkbox->text && strcmp(checkbox->text, new_text) == 0)
        return;

    char *copy = vg_strdup(new_text);
    if (!copy)
        return;

    free(checkbox->text);
    checkbox->text = copy;
    checkbox->base.needs_layout = true;
    checkbox->base.needs_paint = true;
    vg_widget_note_revision(&checkbox->base);
}

/// @brief Set the change callback invoked when the checked state changes.
///
/// @param checkbox  The checkbox to configure.
/// @param callback  Function called with (widget, new_checked, user_data).
/// @param user_data Opaque pointer passed to @p callback.
void vg_checkbox_set_on_change(vg_checkbox_t *checkbox,
                               vg_checkbox_callback_t callback,
                               void *user_data) {
    if (!checkbox)
        return;

    checkbox->on_change = callback;
    checkbox->on_change_data = user_data;
}

/// @brief Override the checkbox label font and size.
///
/// @param checkbox The checkbox to configure.
/// @param font     Font to use; NULL keeps the existing font.
/// @param size     Font size in pixels; <= 0 falls back to the theme normal size.
void vg_checkbox_set_font(vg_checkbox_t *checkbox, vg_font_t *font, float size) {
    if (!checkbox)
        return;

    float font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    if (checkbox->font == font && checkbox->font_size == font_size)
        return;
    checkbox->font = font;
    checkbox->font_size = font_size;
    checkbox->base.needs_layout = true;
    checkbox->base.needs_paint = true;
}
