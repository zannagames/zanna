//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_button.c
// Purpose: Button widget implementation — rendering, event handling, and the
//          public API for text, style, icon, font, and click-callback control.
// Key invariants:
//   - button->text is always a valid heap-allocated string (never NULL).
//   - Style changes immediately recompute bg/fg/border colours from the theme.
//   - The vtable (g_button_vtable) is statically allocated and shared.
// Ownership/Lifetime:
//   - vg_button_create copies the text string; the caller may free the original.
//   - button_destroy frees text and icon_text before the base widget is freed.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_icon_vector.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements themed push-button measurement, painting, activation, and configuration.
/// @details Buttons own copied label and text-icon strings, borrow their font and callback payload,
/// and render style presets with animated hover, press, and focus effects. Vector icons take
/// precedence over text icons and participate in the same centered content layout.

//=============================================================================
// Forward Declarations
//=============================================================================

static void button_destroy(vg_widget_t *widget);
static void button_measure(vg_widget_t *widget, float available_width, float available_height);
static void button_paint(vg_widget_t *widget, void *canvas);
static bool button_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool button_can_focus(vg_widget_t *widget);

//=============================================================================
// Button VTable
//=============================================================================

static vg_widget_vtable_t g_button_vtable = {.destroy = button_destroy,
                                             .measure = button_measure,
                                             .arrange = NULL,
                                             .paint = button_paint,
                                             .handle_event = button_handle_event,
                                             .can_focus = button_can_focus,
                                             .on_focus = NULL};

//=============================================================================
// Helpers
//=============================================================================

/// @brief Return the effective corner radius (button override or theme default).
/// @param button Button whose explicit radius is preferred when positive.
/// @param theme Current theme supplying the fallback radius.
/// @return Corner radius rounded down to pixels, with a minimum of two.
static int button_corner_radius(const vg_button_t *button, const vg_theme_t *theme) {
    float radius = button && button->border_radius > 0.0f
                       ? button->border_radius
                       : (theme ? theme->button.border_radius : 4.0f);
    if (radius < 2.0f)
        radius = 2.0f;
    return (int)radius;
}

//=============================================================================
// Button Implementation
//=============================================================================

/// @brief Create a button widget with the given label text.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @param text   Label text; copied internally. An empty string is used if NULL.
/// @return Newly allocated vg_button_t, or NULL on allocation failure.
vg_button_t *vg_button_create(vg_widget_t *parent, const char *text) {
    vg_button_t *button = calloc(1, sizeof(vg_button_t));
    if (!button)
        return NULL;

    // Initialize base widget
    vg_widget_init(&button->base, VG_WIDGET_BUTTON, &g_button_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize button-specific fields
    button->text = text ? vg_strdup(text) : vg_strdup("");
    if (!button->text) {
        vg_widget_destroy(&button->base);
        return NULL;
    }
    button->font = theme->typography.font_regular;
    button->font_size = theme->typography.size_normal;
    button->style = VG_BUTTON_STYLE_DEFAULT;
    button->on_click = NULL;
    button->icon_vector_id = -1;
    button->user_data = NULL;

    // Default appearance from theme
    button->bg_color = theme->colors.bg_tertiary;
    button->fg_color = theme->colors.fg_primary;
    button->border_color = theme->colors.border_primary;
    button->border_radius = theme->button.border_radius;

    // Set minimum size
    button->base.constraints.min_height = theme->button.height;
    button->base.constraints.min_width = 60.0f;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &button->base);
    }

    return button;
}

/// @brief Release strings owned by a button during base-widget destruction.
/// @param widget Button base widget being destroyed.
static void button_destroy(vg_widget_t *widget) {
    vg_button_t *button = (vg_button_t *)widget;
    if (button->text) {
        free(button->text);
        button->text = NULL;
    }
    free(button->icon_text);
    button->icon_text = NULL;
}

/// @brief VTable measure: sizes the button from icon, text, and padding dimensions then applies
/// layout constraints.
/// @param widget Button base widget whose measured dimensions are updated.
/// @param available_width Parent-provided width constraint; sizing is content-driven.
/// @param available_height Parent-provided height constraint; the theme supplies button height.
static void button_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_button_t *button = (vg_button_t *)widget;
    (void)available_width;
    (void)available_height;

    vg_theme_t *theme = vg_theme_get_current();
    float padding = theme->button.padding_h;

    // Start with minimum size
    float width = widget->constraints.min_width;
    float height = theme->button.height;

    // If we have a font, measure text and/or icon
    if (button->font) {
        float content_w = 0;

        if (button->text && button->text[0]) {
            vg_text_metrics_t metrics;
            vg_font_measure_text(button->font, button->font_size, button->text, &metrics);
            content_w = metrics.width;
        }

        if (button->icon_vector_id >= 0) {
            content_w += button->font_size + 2.0f; // vector icon box
            if (button->text && button->text[0])
                content_w += 5.0f; // gap between icon and label
        } else if (button->icon_text && button->icon_text[0]) {
            vg_text_metrics_t icon_metrics;
            vg_font_measure_text(button->font, button->font_size, button->icon_text, &icon_metrics);
            content_w += icon_metrics.width;
            if (button->text && button->text[0])
                content_w += 4.0f; // gap between icon and label
        }

        if (content_w > 0) {
            width = content_w + padding * 2;
            if (width < widget->constraints.min_width)
                width = widget->constraints.min_width;
        }
    }

    widget->measured_width = width;
    widget->measured_height = height;

    vg_widget_apply_constraints(widget);
}

/// @brief VTable paint: renders the button background (rounded rect), border, hover/press tints,
/// optional icon, and centred label text.
/// @param widget Button base widget to render.
/// @param canvas Graphics window used for shapes, icons, clipping, and text.
static void button_paint(vg_widget_t *widget, void *canvas) {
    vg_button_t *button = (vg_button_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;
    int radius = button_corner_radius(button, theme);

    uint32_t bg_color = button->bg_color ? button->bg_color : theme->colors.bg_tertiary;
    uint32_t fg_color = button->fg_color ? button->fg_color : theme->colors.fg_primary;
    uint32_t border_color =
        button->border_color ? button->border_color : theme->colors.border_primary;

    if (button->style == VG_BUTTON_STYLE_PRIMARY) {
        bg_color = theme->colors.accent_primary;
        fg_color = 0x00FFFFFF;
        border_color =
            vg_color_blend(theme->colors.accent_primary, theme->colors.border_focus, 0.45f);
    } else if (button->style == VG_BUTTON_STYLE_DANGER) {
        bg_color = theme->colors.accent_danger;
        fg_color = 0x00FFFFFF;
        border_color = vg_color_darken(theme->colors.accent_danger, 0.18f);
    } else if (button->style == VG_BUTTON_STYLE_SECONDARY) {
        bg_color = vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.35f);
        fg_color = theme->colors.fg_primary;
        border_color = theme->colors.border_secondary;
    } else if (button->style == VG_BUTTON_STYLE_TEXT) {
        bg_color = theme->colors.bg_primary;
        fg_color = theme->colors.fg_link;
        border_color = bg_color;
    }

    if (widget->state & VG_STATE_DISABLED) {
        bg_color = theme->colors.bg_disabled;
        fg_color = theme->colors.fg_disabled;
        border_color = theme->colors.border_secondary;
    } else {
        // Hover/press tints scale with the eased animation amounts so the
        // button fades between states instead of snapping.
        float hover_amt =
            (button->style == VG_BUTTON_STYLE_TEXT ? 0.03f : 0.05f) * widget->anim_hover;
        float press_amt =
            (button->style == VG_BUTTON_STYLE_TEXT ? 0.04f : 0.10f) * widget->anim_press;
        if (hover_amt > 0.001f)
            bg_color = vg_color_lighten(bg_color, hover_amt);
        if (press_amt > 0.001f)
            bg_color = vg_color_darken(bg_color, press_amt);
    }

    // Refined Depth rendering: a soft accent glow when focused (or a resting
    // elevation lift otherwise), a subtle vertical gradient body, a 1px top
    // sheen, and an anti-aliased border — all via the shared vg_draw core.
    bool is_text_style = (button->style == VG_BUTTON_STYLE_TEXT);
    bool disabled = (widget->state & VG_STATE_DISABLED) != 0;
    bool pressed = (widget->state & VG_STATE_PRESSED) != 0;
    float fx = widget->x, fy = widget->y, fw = widget->width, fh = widget->height;
    float frad = (float)radius;

    // Focus glow fades in/out with the eased focus amount; otherwise a resting
    // elevation lift (skipped while pressed for a tactile "pushed in" feel).
    if (widget->anim_focus > 0.01f && !disabled && !is_text_style) {
        uint8_t glow_a = (uint8_t)((float)theme->focus.glow_alpha * widget->anim_focus);
        vg_draw_round_rect_shadow(win,
                                  fx,
                                  fy,
                                  fw,
                                  fh,
                                  frad,
                                  theme->focus.glow_width * 2.5f,
                                  0,
                                  0,
                                  glow_a,
                                  theme->focus.glow_color);
    } else if (!pressed && !disabled && !is_text_style) {
        vg_elevation_t el = theme->elevation.level1;
        vg_draw_round_rect_shadow(win,
                                  fx,
                                  fy,
                                  fw,
                                  fh,
                                  frad,
                                  el.blur,
                                  el.dx,
                                  el.dy,
                                  el.alpha,
                                  theme->elevation.shadow_rgb);
    }

    if (!is_text_style && theme->gradient.enabled) {
        uint32_t top = vg_color_lighten(bg_color, theme->gradient.strength * 0.5f);
        uint32_t bot = vg_color_darken(bg_color, theme->gradient.strength * 0.5f);
        vg_draw_round_rect_gradient_v(win, fx, fy, fw, fh, frad, top, bot);
    } else {
        vg_draw_round_rect_fill(win, fx, fy, fw, fh, frad, bg_color);
    }

    if (!is_text_style && !disabled)
        vg_draw_inner_highlight_top(
            win, fx, fy + 1.0f, fw, frad, vg_color_lighten(bg_color, 0.10f));

    if (widget->state & VG_STATE_FOCUSED) {
        border_color = theme->colors.border_focus;
    }
    if (!is_text_style || (widget->state & (VG_STATE_HOVERED | VG_STATE_FOCUSED))) {
        float bw = theme->button.border_width > 0.0f ? theme->button.border_width : 1.0f;
        vg_draw_round_rect_stroke(win, fx, fy, fw, fh, frad, bw, border_color);
    }

    if (button->font) {
        vg_font_metrics_t font_metrics;
        vg_font_get_metrics(button->font, button->font_size, &font_metrics);
        float baseline_y = widget->y +
                           (widget->height - (font_metrics.ascent - font_metrics.descent)) / 2.0f +
                           font_metrics.ascent;
        int32_t clip_x = (int32_t)widget->x + 6;
        int32_t clip_y = (int32_t)widget->y + 2;
        int32_t clip_w = (int32_t)widget->width - 12;
        int32_t clip_h = (int32_t)widget->height - 4;
        if (clip_w < 0)
            clip_w = 0;
        if (clip_h < 0)
            clip_h = 0;
        vgfx_set_clip(win, clip_x, clip_y, clip_w, clip_h);

        bool has_text = button->text && button->text[0];
        bool has_icon = button->icon_text && button->icon_text[0];
        bool has_vector = button->icon_vector_id >= 0;

        if (has_vector) {
            // Vector icon (takes precedence over icon text): icon box left of
            // the centred icon+text pair, vertically centred in the button.
            float icon_sz = button->font_size + 2.0f;
            vg_text_metrics_t text_m = {0};
            if (has_text)
                vg_font_measure_text(button->font, button->font_size, button->text, &text_m);
            float gap = has_text ? 5.0f : 0.0f;
            float total_w = icon_sz + gap + text_m.width;
            float start_x = widget->x + (widget->width - total_w) / 2.0f;
            float icon_y = widget->y + (widget->height - icon_sz) / 2.0f;
            vg_icon_vector_draw(win,
                                button->icon_vector_id,
                                (int32_t)(start_x + 0.5f),
                                (int32_t)(icon_y + 0.5f),
                                (int32_t)(icon_sz + 0.5f),
                                fg_color);
            if (has_text) {
                vg_font_draw_text(canvas,
                                  button->font,
                                  button->font_size,
                                  start_x + icon_sz + gap,
                                  baseline_y,
                                  button->text,
                                  fg_color);
            }
        } else if (!has_icon) {
            // Text-only (original behaviour)
            if (has_text) {
                vg_text_metrics_t metrics;
                vg_font_measure_text(button->font, button->font_size, button->text, &metrics);
                float text_x = widget->x + (widget->width - metrics.width) / 2.0f;
                vg_font_draw_text(canvas,
                                  button->font,
                                  button->font_size,
                                  text_x,
                                  baseline_y,
                                  button->text,
                                  fg_color);
            }
        } else {
            // Measure both pieces
            vg_text_metrics_t icon_m = {0};
            vg_text_metrics_t text_m = {0};
            vg_font_measure_text(button->font, button->font_size, button->icon_text, &icon_m);
            if (has_text)
                vg_font_measure_text(button->font, button->font_size, button->text, &text_m);

            float gap = has_text ? 4.0f : 0.0f;
            float total_w = icon_m.width + gap + text_m.width;
            float start_x = widget->x + (widget->width - total_w) / 2.0f;

            if (button->icon_pos == 1) {
                // icon on the right
                float text_x = start_x;
                if (has_text)
                    vg_font_draw_text(canvas,
                                      button->font,
                                      button->font_size,
                                      text_x,
                                      baseline_y,
                                      button->text,
                                      fg_color);
                float icon_x = start_x + text_m.width + gap;
                vg_font_draw_text(canvas,
                                  button->font,
                                  button->font_size,
                                  icon_x,
                                  baseline_y,
                                  button->icon_text,
                                  fg_color);
            } else {
                // icon on the left (default)
                vg_font_draw_text(canvas,
                                  button->font,
                                  button->font_size,
                                  start_x,
                                  baseline_y,
                                  button->icon_text,
                                  fg_color);
                if (has_text) {
                    float text_x = start_x + icon_m.width + gap;
                    vg_font_draw_text(canvas,
                                      button->font,
                                      button->font_size,
                                      text_x,
                                      baseline_y,
                                      button->text,
                                      fg_color);
                }
            }
        }
        vgfx_clear_clip(win);
    }
}

/// @brief VTable handle_event: handles hover, press/release state transitions, click firing, and
/// Space/Enter keyboard activation.
/// @param widget Button base widget receiving the event.
/// @param event Click or key event to process.
/// @return `true` when activation is handled, otherwise `false`.
static bool button_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_button_t *button = (vg_button_t *)widget;

    if (widget->state & VG_STATE_DISABLED) {
        return false;
    }

    if (event->type == VG_EVENT_CLICK) {
        if (button->on_click) {
            button->on_click(widget, button->user_data);
        }
        // Also call widget's generic on_click if set
        if (widget->on_click) {
            widget->on_click(widget, widget->callback_data);
        }
        return true;
    }

    if (event->type == VG_EVENT_KEY_DOWN) {
        if (event->key.key == VG_KEY_SPACE || event->key.key == VG_KEY_ENTER) {
            if (event->key.repeat) {
                event->handled = true;
                return true;
            }
            vg_widget_note_click(widget, event->timestamp);
            if (button->on_click)
                button->on_click(widget, button->user_data);
            if (widget->on_click)
                widget->on_click(widget, widget->callback_data);
            event->handled = true;
            return true;
        }
    }

    return false;
}

/// @brief Return whether a button may participate in keyboard focus traversal.
/// @param widget Button base widget to inspect.
/// @return `true` when the widget is enabled and visible.
static bool button_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

//=============================================================================
// Button API
//=============================================================================

/// @brief Replace the button's label text.
///
/// @param button The button to update.
/// @param text   New label text (copied); NULL is treated as an empty string.
void vg_button_set_text(vg_button_t *button, const char *text) {
    if (!button)
        return;

    const char *new_text = text ? text : "";
    if (button->text && strcmp(button->text, new_text) == 0)
        return;

    char *copy = vg_strdup(new_text);
    if (!copy)
        return;

    free(button->text);
    button->text = copy;
    button->base.needs_layout = true;
    button->base.needs_paint = true;
}

/// @brief Return the button's current label text.
///
/// @param button The button to query.
/// @return Pointer to the internal label string, or NULL if button is NULL.
const char *vg_button_get_text(vg_button_t *button) {
    return button ? button->text : NULL;
}

/// @brief Set the click callback invoked when the button is activated.
///
/// @param button    The button to configure.
/// @param callback  Function called on click; may be NULL to clear the callback.
/// @param user_data Opaque pointer passed to @p callback (not dereferenced here).
void vg_button_set_on_click(vg_button_t *button, vg_button_callback_t callback, void *user_data) {
    if (!button)
        return;

    button->on_click = callback;
    button->user_data = user_data;
}

/// @brief Apply a visual style preset, updating the button's bg/fg/border colours
///        from the current theme.
///
/// @param button The button to update.
/// @param style  One of VG_BUTTON_STYLE_PRIMARY, SECONDARY, DANGER, TEXT, or DEFAULT.
void vg_button_set_style(vg_button_t *button, vg_button_style_t style) {
    if (!button)
        return;

    if (style < VG_BUTTON_STYLE_DEFAULT || style > VG_BUTTON_STYLE_ICON)
        style = VG_BUTTON_STYLE_DEFAULT;
    button->style = style;
    vg_theme_t *theme = vg_theme_get_current();

    // Update colors based on style
    switch (style) {
        case VG_BUTTON_STYLE_PRIMARY:
            button->bg_color = theme->colors.accent_primary;
            button->fg_color = 0x00FFFFFF; // White
            button->border_color =
                vg_color_blend(theme->colors.accent_primary, theme->colors.border_focus, 0.45f);
            break;
        case VG_BUTTON_STYLE_SECONDARY:
            button->bg_color =
                vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.35f);
            button->fg_color = theme->colors.fg_primary;
            button->border_color = theme->colors.border_secondary;
            break;
        case VG_BUTTON_STYLE_DANGER:
            button->bg_color = theme->colors.accent_danger;
            button->fg_color = 0x00FFFFFF;
            button->border_color = vg_color_darken(theme->colors.accent_danger, 0.18f);
            break;
        case VG_BUTTON_STYLE_TEXT:
            button->bg_color = theme->colors.bg_primary;
            button->fg_color = theme->colors.fg_link;
            button->border_color = theme->colors.bg_primary;
            break;
        default:
            button->bg_color = theme->colors.bg_tertiary;
            button->fg_color = theme->colors.fg_primary;
            button->border_color = theme->colors.border_primary;
            break;
    }

    button->base.needs_paint = true;
}

/// @brief Override the button's label font and size.
///
/// @param button The button to configure.
/// @param font   Font to use; NULL keeps the existing font.
/// @param size   Font size in pixels; <= 0 falls back to the theme normal size.
void vg_button_set_font(vg_button_t *button, vg_font_t *font, float size) {
    if (!button)
        return;

    float font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    if (button->font == font && button->font_size == font_size)
        return;
    button->font = font;
    button->font_size = font_size;
    button->base.needs_layout = true;
    button->base.needs_paint = true;
}

/// @brief Set an icon string (typically a single Unicode symbol) drawn beside the label.
///
/// @param button The button to configure.
/// @param icon   Icon string (copied internally); NULL removes the icon.
void vg_button_set_icon(vg_button_t *button, const char *icon) {
    if (!button)
        return;

    if ((!button->icon_text && (!icon || icon[0] == '\0')) ||
        (button->icon_text && icon && strcmp(button->icon_text, icon) == 0))
        return;

    char *copy = icon ? vg_strdup(icon) : NULL;
    if (icon && !copy)
        return;

    free(button->icon_text);
    button->icon_text = copy;
    button->base.needs_layout = true;
}

/// @brief Set (or clear) the button's scalable vector icon.
/// @details Vector icons take precedence over icon text when both are set.
/// @param button The button to configure.
/// @param vector_id Icon id from vg_icon_vector_find; negative clears it.
void vg_button_set_vector_icon(vg_button_t *button, int32_t vector_id) {
    if (!button)
        return;
    if (vector_id < 0)
        vector_id = -1;
    if (button->icon_vector_id == vector_id)
        return;
    button->icon_vector_id = vector_id;
    button->base.needs_layout = true;
    button->base.needs_paint = true;
    vg_widget_note_revision(&button->base);
    button->base.needs_paint = true;
}

/// @brief Set whether the icon appears to the left (0) or right (1) of the label.
///
/// @param button The button to configure.
/// @param pos    Icon position: 0 = left, 1 = right.
void vg_button_set_icon_position(vg_button_t *button, int pos) {
    if (!button)
        return;

    int icon_pos = pos == 1 ? 1 : 0;
    if (button->icon_pos == icon_pos)
        return;
    button->icon_pos = icon_pos;
    button->base.needs_paint = true;
}
