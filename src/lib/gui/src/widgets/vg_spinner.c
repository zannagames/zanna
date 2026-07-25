//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_spinner.c
// Purpose: Spinner (numeric input) widget — displays a numeric value with up/down
//          arrow buttons for increment/decrement and an optional inline text-edit
//          mode for direct keyboard entry.
// Key invariants:
//   - value is always clamped to [min_value, max_value] after every change.
//   - indeterminate retains value as an editing seed but presents "Mixed"
//     until a concrete value is assigned or the user begins editing.
//   - editing==true means the user is typing directly; text_buffer is a live
//     edit buffer that may not yet represent a valid number.
//   - spinner_commit_edit parses text_buffer via strtod and calls set_value;
//     spinner_cancel_edit restores text_buffer from the committed value.
//   - step must be a positive finite number; non-positive or non-finite values
//     are silently normalised to 1.
// Ownership/Lifetime:
//   - spinner->text_buffer is heap-allocated and freed in spinner_destroy.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h,
//        docs/adr/0167-spinner-mixed-value-state.md
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements numeric spinner display, direct text editing, mixed-value
///        presentation, and step-based pointer/keyboard adjustment.
/// @details The committed finite value is always range-clamped. The owned edit
///          buffer may temporarily contain incomplete numeric text, while
///          commit and cancellation explicitly reconcile it with stored state.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VG_SPINNER_MAX_DECIMALS 128

static void spinner_destroy(vg_widget_t *widget);
static void spinner_measure(vg_widget_t *widget, float available_width, float available_height);
static void spinner_paint(vg_widget_t *widget, void *canvas);
static bool spinner_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool spinner_can_focus(vg_widget_t *widget);
static void update_text_buffer(vg_spinner_t *spinner);
static bool spinner_point_in_text_area(const vg_spinner_t *spinner, float x, float y);
static void spinner_begin_edit(vg_spinner_t *spinner);
static void spinner_cancel_edit(vg_spinner_t *spinner);
static bool spinner_commit_edit(vg_spinner_t *spinner);
static void spinner_insert_char(vg_spinner_t *spinner, char ch);

static vg_widget_vtable_t g_spinner_vtable = {.destroy = spinner_destroy,
                                              .measure = spinner_measure,
                                              .arrange = NULL,
                                              .paint = spinner_paint,
                                              .handle_event = spinner_handle_event,
                                              .can_focus = spinner_can_focus,
                                              .on_focus = NULL};

/// @brief Add delta to the spinner's current value and clamp the result.
/// @param spinner Spinner to adjust.
/// @param delta Signed amount added through the normal value-setting path.
static void spinner_adjust_value(vg_spinner_t *spinner, double delta) {
    if (!spinner)
        return;
    vg_spinner_set_value(spinner, spinner->value + delta);
}

/// @brief Return true if local coordinates (x,y) fall within the up-arrow button.
/// @param spinner Arranged spinner providing button geometry.
/// @param x Widget-local horizontal coordinate.
/// @param y Widget-local vertical coordinate.
/// @return `true` when the point lies in the upper button half.
static bool spinner_point_in_up_button(const vg_spinner_t *spinner, float x, float y) {
    return x >= spinner->base.width - spinner->button_width && x < spinner->base.width &&
           y >= 0.0f && y < spinner->base.height * 0.5f;
}

/// @brief Return true if local coordinates (x,y) fall within the down-arrow button.
/// @param spinner Arranged spinner providing button geometry.
/// @param x Widget-local horizontal coordinate.
/// @param y Widget-local vertical coordinate.
/// @return `true` when the point lies in the lower button half.
static bool spinner_point_in_down_button(const vg_spinner_t *spinner, float x, float y) {
    return x >= spinner->base.width - spinner->button_width && x < spinner->base.width &&
           y >= spinner->base.height * 0.5f && y < spinner->base.height;
}

/// @brief Return true if local coordinates (x,y) fall within the text display area.
/// @param spinner Arranged spinner providing button geometry.
/// @param x Widget-local horizontal coordinate.
/// @param y Widget-local vertical coordinate.
/// @return `true` when the point lies left of the button gutter.
static bool spinner_point_in_text_area(const vg_spinner_t *spinner, float x, float y) {
    return x >= 0.0f && x < spinner->base.width - spinner->button_width && y >= 0.0f &&
           y < spinner->base.height;
}

/// @brief Create a spinner widget with default range [0, 100] and step 1.
///
/// @details Default appearance values are taken from the current theme.  The
///          widget's minimum width is 80 logical pixels and minimum height
///          follows theme->input.height.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_spinner_t, or NULL on allocation failure.
vg_spinner_t *vg_spinner_create(vg_widget_t *parent) {
    vg_spinner_t *spinner = calloc(1, sizeof(vg_spinner_t));
    if (!spinner)
        return NULL;

    vg_widget_init(&spinner->base, VG_WIDGET_SPINNER, &g_spinner_vtable);

    spinner->min_value = 0;
    spinner->max_value = 100;
    spinner->value = 0;
    spinner->step = 1;
    spinner->decimal_places = 0;

    spinner->text_buffer = NULL;
    update_text_buffer(spinner);

    vg_theme_t *theme = vg_theme_get_current();
    float scale = theme && theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    spinner->font = theme->typography.font_regular;
    spinner->font_size = theme->typography.size_normal;
    spinner->button_width = 26.0f * scale;
    spinner->bg_color = vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.35f);
    spinner->text_color = theme->colors.fg_primary;
    spinner->border_color = theme->colors.border_primary;
    spinner->button_color =
        vg_color_blend(theme->colors.bg_tertiary, theme->colors.bg_secondary, 0.45f);

    spinner->base.constraints.min_width = 80.0f;
    spinner->base.constraints.min_height = theme->input.height;

    if (parent) {
        vg_widget_add_child(parent, &spinner->base);
    }

    return spinner;
}

/// @brief VTable destroy: releases input capture if held; no heap fields beyond the widget struct.
/// @param widget Spinner widget base being destroyed.
static void spinner_destroy(vg_widget_t *widget) {
    vg_spinner_t *spinner = (vg_spinner_t *)widget;
    free(spinner->text_buffer);
    spinner->text_buffer = NULL;
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
}

/// @brief VTable measure: sizes the spinner from text extent plus up/down button gutter and input
/// padding.
/// @param widget Spinner widget base to measure.
/// @param available_width Offered width, unused before constraints.
/// @param available_height Offered height, unused before constraints.
static void spinner_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_spinner_t *spinner = (vg_spinner_t *)widget;
    (void)available_width;
    (void)available_height;

    float width = widget->constraints.min_width > 0.0f ? widget->constraints.min_width : 80.0f;
    float height = widget->constraints.min_height > 0.0f ? widget->constraints.min_height : 28.0f;

    const char *display_text =
        spinner->indeterminate && !spinner->editing ? "Mixed" : spinner->text_buffer;
    if (spinner->font && display_text) {
        vg_text_metrics_t metrics = {0};
        vg_font_measure_text(spinner->font, spinner->font_size, display_text, &metrics);
        width = metrics.width + spinner->button_width + 18.0f;
        if (width < widget->constraints.min_width)
            width = widget->constraints.min_width;
        height = spinner->font_size + 12.0f;
        if (height < widget->constraints.min_height)
            height = widget->constraints.min_height;
    }

    widget->measured_width = width;
    widget->measured_height = height;
    vg_widget_apply_constraints(widget);
}

/// @brief VTable paint: draws the input field background, current value text, up/down arrow buttons
/// with hover/press tints, border, and focus ring.
/// @param widget Arranged spinner widget base to paint.
/// @param canvas Backend canvas used for field, glyph, text, and caret drawing.
static void spinner_paint(vg_widget_t *widget, void *canvas) {
    vg_spinner_t *spinner = (vg_spinner_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    const int32_t x = (int32_t)widget->x;
    const int32_t y = (int32_t)widget->y;
    const int32_t w = (int32_t)widget->width;
    const int32_t h = (int32_t)widget->height;
    const int32_t button_x = (int32_t)(widget->x + widget->width - spinner->button_width);
    const int32_t button_w = (int32_t)spinner->button_width;
    const int32_t half_h = h / 2;
    const int32_t up_mid_x = button_x + button_w / 2;
    const int32_t up_mid_y = y + half_h / 2;
    const int32_t down_mid_y = y + half_h + (h - half_h) / 2;

    uint32_t bg_color = spinner->bg_color;
    uint32_t button_color = spinner->button_color;
    uint32_t text_color = spinner->text_color;
    uint32_t border = spinner->border_color;

    if (widget->state & VG_STATE_DISABLED) {
        bg_color = theme->colors.bg_disabled;
        button_color = vg_color_blend(theme->colors.bg_disabled, theme->colors.bg_secondary, 0.35f);
        text_color = theme->colors.fg_disabled;
        border = theme->colors.border_secondary;
    } else {
        if (widget->state & VG_STATE_HOVERED)
            bg_color = vg_color_blend(bg_color, theme->colors.bg_hover, 0.22f);
        if (widget->state & VG_STATE_FOCUSED)
            border = theme->colors.border_focus;
        else if (widget->state & VG_STATE_HOVERED)
            border = theme->colors.border_secondary;
    }

    vgfx_fill_rect((vgfx_window_t)canvas, x, y, w, h, bg_color);
    if (w > 2) {
        vgfx_fill_rect(
            (vgfx_window_t)canvas, x + 1, y + 1, w - 2, 1, vg_color_lighten(bg_color, 0.08f));
    }
    vgfx_fill_rect((vgfx_window_t)canvas,
                   button_x,
                   y,
                   button_w,
                   half_h,
                   spinner->up_pressed   ? theme->colors.bg_active
                   : spinner->up_hovered ? theme->colors.bg_hover
                                         : button_color);
    vgfx_fill_rect((vgfx_window_t)canvas,
                   button_x,
                   y + half_h,
                   button_w,
                   h - half_h,
                   spinner->down_pressed   ? theme->colors.bg_active
                   : spinner->down_hovered ? theme->colors.bg_hover
                                           : button_color);
    if (button_w > 2) {
        vgfx_fill_rect((vgfx_window_t)canvas,
                       button_x + 1,
                       y + 1,
                       button_w - 2,
                       1,
                       vg_color_lighten(button_color, 0.08f));
    }

    vgfx_rect((vgfx_window_t)canvas, x, y, w, h, border);
    vgfx_line((vgfx_window_t)canvas, button_x, y, button_x, y + h, spinner->border_color);
    vgfx_line((vgfx_window_t)canvas,
              button_x,
              y + half_h,
              button_x + button_w,
              y + half_h,
              spinner->border_color);

    const char *display_text =
        spinner->indeterminate && !spinner->editing ? "Mixed" : spinner->text_buffer;
    if (spinner->font && display_text) {
        vg_font_metrics_t metrics;
        vg_font_get_metrics(spinner->font, spinner->font_size, &metrics);

        vg_text_metrics_t text_metrics = {0};
        vg_font_measure_text(spinner->font, spinner->font_size, display_text, &text_metrics);

        const float text_area_w = widget->width - spinner->button_width - 12.0f;
        const float text_x = spinner->editing
                                 ? (widget->x + 8.0f)
                                 : (widget->x + 8.0f + (text_area_w - text_metrics.width) * 0.5f);
        const float text_y = widget->y +
                             (widget->height - (metrics.ascent - metrics.descent)) * 0.5f +
                             metrics.ascent;
        int32_t clip_x = x + 2;
        int32_t clip_y = y + 2;
        int32_t clip_w = button_x - x - 4;
        int32_t clip_h = h - 4;
        if (clip_w > 0 && clip_h > 0)
            vgfx_set_clip((vgfx_window_t)canvas, clip_x, clip_y, clip_w, clip_h);
        vg_font_draw_text(
            canvas, spinner->font, spinner->font_size, text_x, text_y, display_text, text_color);
        if ((widget->state & VG_STATE_FOCUSED) && spinner->editing) {
            size_t cursor_pos = spinner->cursor_pos;
            size_t text_len = strlen(spinner->text_buffer);
            if (cursor_pos > text_len)
                cursor_pos = text_len;
            char saved = spinner->text_buffer[cursor_pos];
            spinner->text_buffer[cursor_pos] = '\0';
            vg_text_metrics_t cursor_metrics = {0};
            vg_font_measure_text(
                spinner->font, spinner->font_size, spinner->text_buffer, &cursor_metrics);
            spinner->text_buffer[cursor_pos] = saved;
            int32_t cursor_x = (int32_t)(text_x + cursor_metrics.width);
            vgfx_line((vgfx_window_t)canvas, cursor_x, y + 5, cursor_x, y + h - 5, text_color);
        }
        if (clip_w > 0 && clip_h > 0)
            vgfx_clear_clip((vgfx_window_t)canvas);
    }

    const uint32_t glyph =
        (widget->state & VG_STATE_DISABLED) ? theme->colors.fg_disabled : theme->colors.fg_primary;
    // Anti-aliased up/down chevrons.
    vg_draw_line_aa(
        (vgfx_window_t)canvas, up_mid_x - 4, up_mid_y + 2, up_mid_x, up_mid_y - 2, 1.4f, glyph);
    vg_draw_line_aa(
        (vgfx_window_t)canvas, up_mid_x, up_mid_y - 2, up_mid_x + 4, up_mid_y + 2, 1.4f, glyph);
    vg_draw_line_aa(
        (vgfx_window_t)canvas, up_mid_x - 4, down_mid_y - 2, up_mid_x, down_mid_y + 2, 1.4f, glyph);
    vg_draw_line_aa(
        (vgfx_window_t)canvas, up_mid_x, down_mid_y + 2, up_mid_x + 4, down_mid_y - 2, 1.4f, glyph);
}

/// @brief VTable handle_event: handles click on up/down arrow buttons, mouse-wheel
/// increment/decrement, arrow/Home/End keys, and inline text editing.
/// @param widget Spinner widget receiving input.
/// @param event Pointer, wheel, key, or character event to interpret.
/// @return `true` when the spinner consumes the event.
static bool spinner_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_spinner_t *spinner = (vg_spinner_t *)widget;
    if (widget->state & VG_STATE_DISABLED)
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            const bool old_up_hovered = spinner->up_hovered;
            const bool old_down_hovered = spinner->down_hovered;
            spinner->up_hovered =
                spinner_point_in_up_button(spinner, event->mouse.x, event->mouse.y);
            spinner->down_hovered =
                spinner_point_in_down_button(spinner, event->mouse.x, event->mouse.y);
            if (old_up_hovered != spinner->up_hovered ||
                old_down_hovered != spinner->down_hovered) {
                widget->needs_paint = true;
            }
            return spinner->up_pressed || spinner->down_pressed;
        }

        case VG_EVENT_MOUSE_LEAVE:
            if (!spinner->up_pressed && !spinner->down_pressed) {
                spinner->up_hovered = false;
                spinner->down_hovered = false;
                widget->needs_paint = true;
            }
            return false;

        case VG_EVENT_MOUSE_DOWN:
            if (spinner_point_in_up_button(spinner, event->mouse.x, event->mouse.y)) {
                if (spinner->editing)
                    spinner_commit_edit(spinner);
                spinner->up_pressed = true;
                spinner->down_pressed = false;
                vg_widget_set_input_capture(widget);
                spinner_adjust_value(spinner, spinner->step);
                widget->needs_paint = true;
                return true;
            }
            if (spinner_point_in_down_button(spinner, event->mouse.x, event->mouse.y)) {
                if (spinner->editing)
                    spinner_commit_edit(spinner);
                spinner->down_pressed = true;
                spinner->up_pressed = false;
                vg_widget_set_input_capture(widget);
                spinner_adjust_value(spinner, -spinner->step);
                widget->needs_paint = true;
                return true;
            }
            if (spinner_point_in_text_area(spinner, event->mouse.x, event->mouse.y)) {
                spinner_begin_edit(spinner);
                if (spinner->font && spinner->text_buffer) {
                    float local_x = event->mouse.x - 8.0f;
                    int hit = vg_font_hit_test(
                        spinner->font, spinner->font_size, spinner->text_buffer, local_x);
                    if (hit >= 0)
                        spinner->cursor_pos = (size_t)hit;
                    else
                        spinner->cursor_pos = strlen(spinner->text_buffer);
                }
                widget->needs_paint = true;
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_UP:
            if (spinner->up_pressed || spinner->down_pressed) {
                spinner->up_pressed = false;
                spinner->down_pressed = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                widget->needs_paint = true;
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_WHEEL:
            if (spinner->editing)
                spinner_commit_edit(spinner);
            if (event->wheel.delta_y > 0.0f) {
                spinner_adjust_value(spinner, spinner->step);
                return true;
            }
            if (event->wheel.delta_y < 0.0f) {
                spinner_adjust_value(spinner, -spinner->step);
                return true;
            }
            return false;

        case VG_EVENT_KEY_DOWN:
            if (spinner->editing) {
                size_t len = spinner->text_buffer ? strlen(spinner->text_buffer) : 0;
                switch (event->key.key) {
                    case VG_KEY_LEFT:
                        if (spinner->cursor_pos > 0)
                            spinner->cursor_pos--;
                        widget->needs_paint = true;
                        return true;
                    case VG_KEY_RIGHT:
                        if (spinner->cursor_pos < len)
                            spinner->cursor_pos++;
                        widget->needs_paint = true;
                        return true;
                    case VG_KEY_HOME:
                        spinner->cursor_pos = 0;
                        widget->needs_paint = true;
                        return true;
                    case VG_KEY_END:
                        spinner->cursor_pos = len;
                        widget->needs_paint = true;
                        return true;
                    case VG_KEY_BACKSPACE:
                        if (spinner->cursor_pos > 0 && spinner->text_buffer) {
                            memmove(spinner->text_buffer + spinner->cursor_pos - 1,
                                    spinner->text_buffer + spinner->cursor_pos,
                                    len - spinner->cursor_pos + 1);
                            spinner->cursor_pos--;
                            widget->needs_layout = true;
                            widget->needs_paint = true;
                        }
                        return true;
                    case VG_KEY_DELETE:
                        if (spinner->cursor_pos < len && spinner->text_buffer) {
                            memmove(spinner->text_buffer + spinner->cursor_pos,
                                    spinner->text_buffer + spinner->cursor_pos + 1,
                                    len - spinner->cursor_pos);
                            widget->needs_layout = true;
                            widget->needs_paint = true;
                        }
                        return true;
                    case VG_KEY_ENTER:
                        if (spinner_commit_edit(spinner))
                            vg_widget_note_submission(widget);
                        widget->needs_paint = true;
                        return true;
                    case VG_KEY_ESCAPE:
                        spinner_cancel_edit(spinner);
                        widget->needs_paint = true;
                        return true;
                    default:
                        break;
                }
            }
            if (event->key.key == VG_KEY_UP) {
                if (spinner->editing)
                    spinner_commit_edit(spinner);
                spinner_adjust_value(spinner, spinner->step);
                return true;
            }
            if (event->key.key == VG_KEY_DOWN) {
                if (spinner->editing)
                    spinner_commit_edit(spinner);
                spinner_adjust_value(spinner, -spinner->step);
                return true;
            }
            return false;

        case VG_EVENT_KEY_CHAR: {
            uint32_t cp = event->key.codepoint;
            if (cp < 0x20 || cp > 0x7E)
                return false;
            if (!spinner->editing) {
                spinner_begin_edit(spinner);
                if (spinner->text_buffer)
                    spinner->text_buffer[0] = '\0';
                spinner->cursor_pos = 0;
            }
            spinner_insert_char(spinner, (char)cp);
            widget->needs_layout = true;
            widget->needs_paint = true;
            return true;
        }

        default:
            return false;
    }
}

/// @brief VTable can_focus: returns true when the widget is both enabled and visible.
/// @param widget Candidate spinner widget.
/// @return `true` when keyboard focus is permitted.
static bool spinner_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief Reformat spinner->value into text_buffer using the current decimal_places setting.
/// @details Storage is resized only after the required formatted length is
///          known; allocation failure preserves the previous buffer.
/// @param spinner Spinner whose owned display buffer is rebuilt.
static void update_text_buffer(vg_spinner_t *spinner) {
    if (!spinner)
        return;
    int needed = 0;
    if (spinner->decimal_places <= 0) {
        needed = snprintf(NULL, 0, "%.0f", spinner->value);
    } else {
        needed = snprintf(NULL, 0, "%.*f", spinner->decimal_places, spinner->value);
    }
    if (needed < 0)
        return;
    char *buffer = realloc(spinner->text_buffer, (size_t)needed + 1u);
    if (!buffer)
        return;
    spinner->text_buffer = buffer;
    if (spinner->decimal_places <= 0)
        snprintf(spinner->text_buffer, (size_t)needed + 1u, "%.0f", spinner->value);
    else
        snprintf(spinner->text_buffer,
                 (size_t)needed + 1u,
                 "%.*f",
                 spinner->decimal_places,
                 spinner->value);
    spinner->cursor_pos = strlen(spinner->text_buffer);
}

/// @brief Enter inline edit mode, placing the cursor at end of text.
/// @param spinner Spinner to begin editing; mixed presentation is first resolved.
static void spinner_begin_edit(vg_spinner_t *spinner) {
    if (!spinner)
        return;
    if (spinner->indeterminate)
        vg_spinner_set_indeterminate(spinner, false);
    spinner->editing = true;
    if (!spinner->text_buffer)
        return;
    spinner->cursor_pos = strlen(spinner->text_buffer);
}

/// @brief Abandon inline edit and restore text_buffer from the committed value.
/// @param spinner Spinner whose in-progress text is discarded.
static void spinner_cancel_edit(vg_spinner_t *spinner) {
    if (!spinner)
        return;
    spinner->editing = false;
    update_text_buffer(spinner);
}

/// @brief Parse text_buffer and commit it as the new value; cancels on invalid input.
/// @param spinner Spinner with an active owned edit buffer.
/// @return `true` when the complete buffer parsed as a finite number and was
///         committed.
static bool spinner_commit_edit(vg_spinner_t *spinner) {
    if (!spinner || !spinner->editing || !spinner->text_buffer)
        return false;

    if (spinner->text_buffer[0] == '\0' || strcmp(spinner->text_buffer, "-") == 0 ||
        strcmp(spinner->text_buffer, ".") == 0 || strcmp(spinner->text_buffer, "-.") == 0) {
        spinner_cancel_edit(spinner);
        return false;
    }

    char *end = NULL;
    double value = strtod(spinner->text_buffer, &end);
    spinner->editing = false;
    if (!end || end == spinner->text_buffer || *end != '\0' || !isfinite(value)) {
        update_text_buffer(spinner);
        return false;
    }
    vg_spinner_set_value(spinner, value);
    return true;
}

/// @brief Insert a character at the cursor position if it is numerically valid.
/// @param spinner Spinner whose owned edit buffer is extended.
/// @param ch ASCII digit, decimal point, or leading minus candidate.
static void spinner_insert_char(vg_spinner_t *spinner, char ch) {
    if (!spinner || !spinner->text_buffer)
        return;

    size_t len = strlen(spinner->text_buffer);
    if (spinner->cursor_pos > len)
        spinner->cursor_pos = len;

    bool allow = false;
    if (ch >= '0' && ch <= '9') {
        allow = true;
    } else if (ch == '.' && spinner->decimal_places > 0 &&
               strchr(spinner->text_buffer, '.') == NULL) {
        allow = true;
    } else if (ch == '-' && spinner->min_value < 0.0 && spinner->cursor_pos == 0 &&
               strchr(spinner->text_buffer, '-') == NULL) {
        allow = true;
    }
    if (!allow)
        return;

    char *buffer = realloc(spinner->text_buffer, len + 2u);
    if (!buffer)
        return;
    spinner->text_buffer = buffer;

    memmove(spinner->text_buffer + spinner->cursor_pos + 1,
            spinner->text_buffer + spinner->cursor_pos,
            len - spinner->cursor_pos + 1);
    spinner->text_buffer[spinner->cursor_pos++] = ch;
}

/// @brief Set the spinner's numeric value; clamps to [min_value, max_value].
///
/// @details Non-finite values are clamped to min_value.  Fires on_change if
///          the value differs from the previous one.
///
/// @param spinner The spinner widget to update.
/// @param value   The new numeric value; clamped to the configured range.
void vg_spinner_set_value(vg_spinner_t *spinner, double value) {
    if (!spinner)
        return;

    if (!isfinite(value))
        value = spinner->min_value;

    if (value < spinner->min_value)
        value = spinner->min_value;
    if (value > spinner->max_value)
        value = spinner->max_value;

    double old = spinner->value;
    bool old_indeterminate = spinner->indeterminate;
    spinner->value = value;
    spinner->indeterminate = false;
    update_text_buffer(spinner);
    spinner->editing = false;
    spinner->base.needs_paint = true;

    if (old != value || old_indeterminate) {
        vg_widget_note_change(&spinner->base);
        if (old != value && spinner->on_change)
            spinner->on_change(&spinner->base, value, spinner->on_change_data);
    }
}

/// @brief Return the spinner's current committed numeric value.
///
/// @param spinner The spinner widget to query.
/// @return Current value, or 0 if spinner is NULL.
double vg_spinner_get_value(vg_spinner_t *spinner) {
    return spinner ? spinner->value : 0;
}

/// @brief Enter or leave mixed-value presentation without discarding the numeric seed.
///
/// @details The mixed state is a value-state transition and therefore advances
///          both the common change edge and monotonic revision. Entering it
///          cancels an incomplete text edit. Leaving it reveals the retained
///          numeric value. Assigning a concrete value through
///          vg_spinner_set_value also leaves this state.
///
/// @param spinner Spinner widget to update.
/// @param indeterminate true to display "Mixed"; false to display the retained
///        numeric value.
void vg_spinner_set_indeterminate(vg_spinner_t *spinner, bool indeterminate) {
    if (!spinner || spinner->indeterminate == indeterminate)
        return;
    spinner->indeterminate = indeterminate;
    if (indeterminate) {
        spinner->editing = false;
        update_text_buffer(spinner);
    }
    spinner->base.needs_layout = true;
    spinner->base.needs_paint = true;
    vg_widget_note_change(&spinner->base);
}

/// @brief Return whether a spinner currently presents a mixed group value.
/// @param spinner Spinner to inspect.
/// @return Current indeterminate state, or `false` for `NULL`.
bool vg_spinner_is_indeterminate(const vg_spinner_t *spinner) {
    return spinner ? spinner->indeterminate : false;
}

/// @brief Set the minimum and maximum bounds for the spinner value.
///
/// @details Non-finite bounds are silently ignored.  If min_val > max_val the
///          arguments are swapped.  The current value is re-clamped to the new
///          range.
///
/// @param spinner  The spinner widget to configure.
/// @param min_val  Lower bound (inclusive).
/// @param max_val  Upper bound (inclusive).
void vg_spinner_set_range(vg_spinner_t *spinner, double min_val, double max_val) {
    if (!spinner)
        return;
    if (!isfinite(min_val) || !isfinite(max_val))
        return;
    if (min_val > max_val) {
        double tmp = min_val;
        min_val = max_val;
        max_val = tmp;
    }
    double old_min = spinner->min_value;
    double old_max = spinner->max_value;
    uint64_t old_change_revision = spinner->base.change_revision;
    bool was_indeterminate = spinner->indeterminate;
    spinner->min_value = min_val;
    spinner->max_value = max_val;
    spinner->indeterminate = false;
    vg_spinner_set_value(spinner, spinner->value);
    spinner->indeterminate = was_indeterminate;
    if ((old_min != min_val || old_max != max_val) &&
        old_change_revision == spinner->base.change_revision) {
        vg_widget_note_revision(&spinner->base);
    }
}

/// @brief Set the amount added or subtracted by each arrow-button click or key press.
///
/// @param spinner The spinner widget to configure.
/// @param step    Positive increment amount; non-positive or non-finite values
///                are normalised to 1.
void vg_spinner_set_step(vg_spinner_t *spinner, double step) {
    if (!spinner)
        return;
    double normalized = isfinite(step) && step > 0 ? step : 1;
    if (spinner->step == normalized)
        return;
    spinner->step = normalized;
    spinner->base.needs_paint = true;
    vg_widget_note_revision(&spinner->base);
}

/// @brief Set the number of decimal places shown and accepted during text edit.
///
/// @details 0 formats the value as an integer.  The text buffer is immediately
///          reformatted and the widget re-laid-out.  Clamped to
///          [0, VG_SPINNER_MAX_DECIMALS].
///
/// @param spinner  The spinner widget to configure.
/// @param decimals Number of decimal places; negative values are clamped to 0.
void vg_spinner_set_decimals(vg_spinner_t *spinner, int decimals) {
    if (!spinner)
        return;
    if (decimals < 0)
        decimals = 0;
    if (decimals > VG_SPINNER_MAX_DECIMALS)
        decimals = VG_SPINNER_MAX_DECIMALS;
    if (spinner->decimal_places == decimals)
        return;
    spinner->decimal_places = decimals;
    update_text_buffer(spinner);
    spinner->base.needs_layout = true;
    spinner->base.needs_paint = true;
    vg_widget_note_revision(&spinner->base);
}

/// @brief Set the font and size used to render the spinner's numeric text.
///
/// @param spinner The spinner widget to configure.
/// @param font    Font to use for the numeric label; NULL leaves the font unchanged.
/// @param size    Font size in logical pixels; non-positive or non-finite values
///                default to the theme's normal text size.
void vg_spinner_set_font(vg_spinner_t *spinner, vg_font_t *font, float size) {
    if (!spinner)
        return;
    if (!font)
        return;
    spinner->font = font;
    spinner->font_size =
        (isfinite(size) && size > 0.0f) ? size : vg_theme_get_current()->typography.size_normal;
    spinner->base.needs_layout = true;
    spinner->base.needs_paint = true;
}

/// @brief Register a callback invoked whenever the committed value changes.
///
/// @param spinner   The spinner widget to configure.
/// @param callback  Function called with (widget, new_value, user_data) on each
///                  committed value change.  May be NULL to unregister.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_spinner_set_on_change(vg_spinner_t *spinner,
                              vg_spinner_callback_t callback,
                              void *user_data) {
    if (!spinner)
        return;
    spinner->on_change = callback;
    spinner->on_change_data = user_data;
}
