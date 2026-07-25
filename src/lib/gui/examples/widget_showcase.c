//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: widget_showcase.c
// Purpose: Comprehensive widget showcase demo for ZannaGUI.
//
//===----------------------------------------------------------------------===//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vg_event.h"
#include "vg_font.h"
#include "vg_ide_widgets.h"
#include "vg_theme.h"
#include "vg_widget.h"
#include "vg_widgets.h"
#include "vgfx.h"

/// @file
/// @brief Presents a comprehensive interactive showcase of ZannaGUI widgets.
/// @details The example manually renders form inputs, selection controls, sliders, progress,
/// and action widgets while demonstrating hover, focus, dragging, text selection, callbacks,
/// and a small download-progress animation.

//=============================================================================
// Demo State
//=============================================================================

/// @brief Window resources, widget references, and interaction state for the showcase.
typedef struct {
    vgfx_window_t window;
    vg_font_t *font;
    bool running;

    // Widgets organized by category
    // --- Input Section ---
    vg_textinput_t *name_input;
    vg_textinput_t *email_input;
    vg_textinput_t *password_input;
    vg_spinner_t *age_spinner;

    // --- Selection Section ---
    vg_dropdown_t *country_dropdown;
    vg_listbox_t *languages_list;
    vg_radiogroup_t *gender_group;
    vg_radiobutton_t *radio_male;
    vg_radiobutton_t *radio_female;
    vg_radiobutton_t *radio_other;
    vg_checkbox_t *newsletter_check;
    vg_checkbox_t *terms_check;

    // --- Control Section ---
    vg_slider_t *volume_slider;
    vg_slider_t *brightness_slider;
    vg_progressbar_t *download_progress;
    vg_button_t *start_btn;
    vg_button_t *cancel_btn;
    vg_button_t *submit_btn;

    // --- Display Section ---
    vg_label_t *title_label;
    vg_label_t *status_label;
    vg_label_t *volume_label;
    vg_label_t *brightness_label;

    // Animation state
    float progress_value;
    bool downloading;

    // Text selection state
    vg_textinput_t *selecting_input; // Input being mouse-selected
    size_t selection_anchor;         // Start position of selection
} showcase_state_t;

static showcase_state_t g_state;

//=============================================================================
// Text Input Helpers
//=============================================================================

// Calculate cursor position from x-coordinate relative to text input
/// @brief Estimate a text cursor index from a horizontal widget-relative coordinate.
/// @details The showcase uses a fixed-width approximation consistent with its manual input
/// renderer and clamps the resulting index to the stored text length.
/// @param input Text input whose font size and content define the mapping.
/// @param rel_x Horizontal coordinate relative to the widget's left edge.
/// @return Clamped byte-oriented cursor index.
static size_t calc_cursor_from_x(vg_textinput_t *input, float rel_x) {
    if (!input || !input->text || input->text_len == 0)
        return 0;

    float padding = 8.0f;                       // Match the padding used in render_text_input
    float char_width = input->font_size * 0.6f; // Approximate character width

    // Calculate position relative to text start
    float text_x = rel_x - padding;
    if (text_x <= 0)
        return 0;

    // Calculate character index
    size_t pos = (size_t)(text_x / char_width);
    if (pos > input->text_len)
        pos = input->text_len;

    return pos;
}

//=============================================================================
// Callbacks
//=============================================================================

/// @brief Update the volume label from a slider value.
/// @param slider Slider that originated the callback.
/// @param value New percentage value.
/// @param data Callback context, unused by this demo.
static void on_volume_change(vg_widget_t *slider, float value, void *data) {
    (void)slider;
    (void)data;
    char buf[32];
    snprintf(buf, sizeof(buf), "Volume: %.0f%%", value);
    vg_label_set_text(g_state.volume_label, buf);
}

/// @brief Update the brightness label from a slider value.
/// @param slider Slider that originated the callback.
/// @param value New percentage value.
/// @param data Callback context, unused by this demo.
static void on_brightness_change(vg_widget_t *slider, float value, void *data) {
    (void)slider;
    (void)data;
    char buf[32];
    snprintf(buf, sizeof(buf), "Brightness: %.0f%%", value);
    vg_label_set_text(g_state.brightness_label, buf);
}

/// @brief Publish the newly selected country in the status label.
/// @param dropdown Dropdown that originated the callback.
/// @param index Zero-based selected item index.
/// @param text Selected item text, or NULL when no item is selected.
/// @param data Callback context, unused by this demo.
static void on_country_change(vg_widget_t *dropdown, int index, const char *text, void *data) {
    (void)dropdown;
    (void)index;
    (void)data;
    char buf[64];
    snprintf(buf, sizeof(buf), "Selected country: %s", text ? text : "None");
    vg_label_set_text(g_state.status_label, buf);
}

/// @brief Reset and begin the simulated download animation.
/// @param btn Button that originated the callback.
/// @param data Callback context, unused by this demo.
static void on_start_download(vg_widget_t *btn, void *data) {
    (void)btn;
    (void)data;
    g_state.downloading = true;
    g_state.progress_value = 0;
    vg_label_set_text(g_state.status_label, "Download started...");
}

/// @brief Stop and reset the simulated download animation.
/// @param btn Button that originated the callback.
/// @param data Callback context, unused by this demo.
static void on_cancel_download(vg_widget_t *btn, void *data) {
    (void)btn;
    (void)data;
    g_state.downloading = false;
    g_state.progress_value = 0;
    vg_progressbar_set_value(g_state.download_progress, 0);
    vg_label_set_text(g_state.status_label, "Download cancelled");
}

/// @brief Summarize the name and email fields in the status label.
/// @param btn Button that originated the callback.
/// @param data Callback context, unused by this demo.
static void on_submit(vg_widget_t *btn, void *data) {
    (void)btn;
    (void)data;
    const char *name = vg_textinput_get_text(g_state.name_input);
    const char *email = vg_textinput_get_text(g_state.email_input);
    char buf[128];
    snprintf(buf,
             sizeof(buf),
             "Submitted: %s <%s>",
             name && name[0] ? name : "(empty)",
             email && email[0] ? email : "(empty)");
    vg_label_set_text(g_state.status_label, buf);
}

/// @brief Publish the newsletter checkbox state in the status label.
/// @param cb Checkbox that originated the callback.
/// @param checked Current checkbox state.
/// @param data Callback context, unused by this demo.
static void on_newsletter_toggle(vg_widget_t *cb, bool checked, void *data) {
    (void)cb;
    (void)data;
    vg_label_set_text(g_state.status_label,
                      checked ? "Newsletter: Subscribed" : "Newsletter: Unsubscribed");
}

//=============================================================================
// Widget Rendering Helpers
//=============================================================================

/// @brief Fill an integer rectangle after discarding the packed color's alpha byte.
/// @param window Target graphics window.
/// @param x Left edge.
/// @param y Top edge.
/// @param w Width.
/// @param h Height.
/// @param color Packed ARGB color.
static void draw_rect(vgfx_window_t window, int x, int y, int w, int h, uint32_t color) {
    vgfx_fill_rect(window, x, y, w, h, color & 0x00FFFFFF);
}

/// @brief Stroke an integer rectangle after discarding the packed color's alpha byte.
/// @param window Target graphics window.
/// @param x Left edge.
/// @param y Top edge.
/// @param w Width.
/// @param h Height.
/// @param color Packed ARGB color.
static void draw_rect_outline(vgfx_window_t window, int x, int y, int w, int h, uint32_t color) {
    vgfx_rect(window, x, y, w, h, color & 0x00FFFFFF);
}

// Label rendering
/// @brief Render a visible label using its explicit or current-theme text color.
/// @param window Target graphics window.
/// @param label Label to render; null or hidden labels are ignored.
static void render_label(vgfx_window_t window, vg_label_t *label) {
    if (!label || !label->base.visible)
        return;
    float sx, sy;
    vg_widget_get_screen_bounds(&label->base, &sx, &sy, NULL, NULL);
    vg_theme_t *theme = vg_theme_get_current();
    uint32_t color = label->text_color ? label->text_color : theme->colors.fg_primary;
    if (label->font && label->text) {
        vg_font_draw_text(
            window, label->font, label->font_size, sx, sy + label->font_size, label->text, color);
    }
}

// Button rendering
/// @brief Render a button according to its style and hover/pressed state.
/// @param window Target graphics window.
/// @param button Button to render; null or hidden buttons are ignored.
static void render_button(vgfx_window_t window, vg_button_t *button) {
    if (!button || !button->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&button->base, &sx, &sy, &sw, &sh);
    vg_theme_t *theme = vg_theme_get_current();

    uint32_t bg = theme->colors.bg_secondary;
    uint32_t fg = theme->colors.fg_primary;
    if (button->style == VG_BUTTON_STYLE_PRIMARY) {
        bg = theme->colors.accent_primary;
        fg = 0xFFFFFFFF;
    } else if (button->style == VG_BUTTON_STYLE_DANGER) {
        bg = 0xFFCC3333;
        fg = 0xFFFFFFFF;
    }
    if (button->base.state & VG_STATE_HOVERED)
        bg = (bg & 0xFF000000) | ((bg & 0x00FEFEFE) >> 1) + 0x00404040;
    if (button->base.state & VG_STATE_PRESSED)
        bg = theme->colors.bg_active;

    draw_rect(window, (int)sx, (int)sy, (int)sw, (int)sh, bg);
    draw_rect_outline(window, (int)sx, (int)sy, (int)sw, (int)sh, theme->colors.border_primary);

    if (button->font && button->text) {
        vg_text_metrics_t m;
        vg_font_measure_text(button->font, button->font_size, button->text, &m);
        float tx = sx + (sw - m.width) / 2;
        float ty = sy + (sh + button->font_size) / 2 - 2;
        vg_font_draw_text(window, button->font, button->font_size, tx, ty, button->text, fg);
    }
}

// TextInput rendering
/// @brief Render a text input with placeholder, password masking, selection, and caret.
/// @param window Target graphics window.
/// @param input Text input to render; null or hidden inputs are ignored.
static void render_textinput(vgfx_window_t window, vg_textinput_t *input) {
    if (!input || !input->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&input->base, &sx, &sy, &sw, &sh);
    vg_theme_t *theme = vg_theme_get_current();

    uint32_t border = (input->base.state & VG_STATE_FOCUSED) ? theme->colors.border_focus
                                                             : theme->colors.border_primary;
    draw_rect(window, (int)sx, (int)sy, (int)sw, (int)sh, theme->colors.bg_primary);
    draw_rect_outline(window, (int)sx, (int)sy, (int)sw, (int)sh, border);

    const char *text = input->text;
    uint32_t color = theme->colors.fg_primary;
    bool is_placeholder = false;
    if ((!text || text[0] == '\0') && input->placeholder) {
        text = input->placeholder;
        color = theme->colors.fg_placeholder;
        is_placeholder = true;
    }
    // Show dots for password
    if (input->password_mode && input->text && input->text[0]) {
        static char dots[256];
        int len = (int)strlen(input->text);
        if (len > 255)
            len = 255;
        memset(dots, '*', len);
        dots[len] = '\0';
        text = dots;
        color = theme->colors.fg_primary;
    }

    float padding = 6.0f;
    float char_width = input->font_size * 0.6f;
    float ty = sy + (sh + input->font_size) / 2 - 2;

    // Draw selection highlight if focused and has selection
    if ((input->base.state & VG_STATE_FOCUSED) && !is_placeholder &&
        input->selection_start != input->selection_end) {
        size_t sel_start = input->selection_start < input->selection_end ? input->selection_start
                                                                         : input->selection_end;
        size_t sel_end = input->selection_start < input->selection_end ? input->selection_end
                                                                       : input->selection_start;
        float sel_x1 = sx + padding + sel_start * char_width;
        float sel_x2 = sx + padding + sel_end * char_width;
        float sel_y = ty - input->font_size + 2;
        draw_rect(window,
                  (int)sel_x1,
                  (int)sel_y,
                  (int)(sel_x2 - sel_x1),
                  (int)(input->font_size + 2),
                  0xFF4488CC);
    }

    // Draw text
    if (input->font && text) {
        vg_font_draw_text(window, input->font, input->font_size, sx + padding, ty, text, color);
    }

    // Draw cursor if focused
    if ((input->base.state & VG_STATE_FOCUSED) && !is_placeholder) {
        // Blink cursor using frame counter (~30fps, blink every 15 frames)
        static int frame_counter = 0;
        frame_counter++;
        bool show_cursor = (frame_counter / 15) % 2 == 0;
        if (show_cursor) {
            float cursor_x = sx + padding + input->cursor_pos * char_width;
            float cursor_y = ty - input->font_size + 2;
            vgfx_line(window,
                      (int)cursor_x,
                      (int)cursor_y,
                      (int)cursor_x,
                      (int)(cursor_y + input->font_size + 2),
                      theme->colors.fg_primary);
        }
    }
}

// Checkbox rendering
/// @brief Render a checkbox, hover border, checked mark, and label.
/// @param window Target graphics window.
/// @param cb Checkbox to render; null or hidden widgets are ignored.
static void render_checkbox(vgfx_window_t window, vg_checkbox_t *cb) {
    if (!cb || !cb->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&cb->base, &sx, &sy, &sw, &sh);
    vg_theme_t *theme = vg_theme_get_current();

    float box = cb->box_size > 0 ? cb->box_size : 16;
    float by = sy + (sh - box) / 2;
    draw_rect(window, (int)sx, (int)by, (int)box, (int)box, theme->colors.bg_primary);
    draw_rect_outline(window,
                      (int)sx,
                      (int)by,
                      (int)box,
                      (int)box,
                      (cb->base.state & VG_STATE_HOVERED) ? theme->colors.border_focus
                                                          : theme->colors.border_primary);

    if (cb->checked) {
        int cx = (int)(sx + box / 2), cy = (int)(by + box / 2);
        uint32_t chk = theme->colors.accent_primary & 0x00FFFFFF;
        vgfx_line(window, cx - 4, cy - 4, cx + 4, cy + 4, chk);
        vgfx_line(window, cx + 4, cy - 4, cx - 4, cy + 4, chk);
    }
    if (cb->font && cb->text) {
        float tx = sx + box + (cb->gap > 0 ? cb->gap : 8);
        float ty = sy + (sh + cb->font_size) / 2 - 2;
        vg_font_draw_text(
            window, cb->font, cb->font_size, tx, ty, cb->text, theme->colors.fg_primary);
    }
}

// RadioButton rendering
/// @brief Render a radio button circle, selection dot, and label.
/// @param window Target graphics window.
/// @param rb Radio button to render; null or hidden widgets are ignored.
static void render_radio(vgfx_window_t window, vg_radiobutton_t *rb) {
    if (!rb || !rb->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&rb->base, &sx, &sy, &sw, &sh);
    vg_theme_t *theme = vg_theme_get_current();

    float r = rb->circle_size > 0 ? rb->circle_size : 16;
    int cx = (int)(sx + r / 2);
    int cy = (int)(sy + sh / 2);
    int radius = (int)(r / 2);

    vgfx_circle(window,
                cx,
                cy,
                radius,
                (rb->base.state & VG_STATE_HOVERED) ? theme->colors.border_focus & 0x00FFFFFF
                                                    : theme->colors.border_primary & 0x00FFFFFF);
    if (rb->selected) {
        vgfx_fill_circle(window, cx, cy, radius - 4, theme->colors.accent_primary & 0x00FFFFFF);
    }
    if (rb->font && rb->text) {
        float tx = sx + r + (rb->gap > 0 ? rb->gap : 8);
        float ty = sy + (sh + rb->font_size) / 2 - 2;
        vg_font_draw_text(
            window, rb->font, rb->font_size, tx, ty, rb->text, theme->colors.fg_primary);
    }
}

// Slider rendering
/// @brief Render a horizontal slider track, fill, and hover-sensitive thumb.
/// @param window Target graphics window.
/// @param sl Slider to render; null or hidden sliders are ignored.
static void render_slider(vgfx_window_t window, vg_slider_t *sl) {
    if (!sl || !sl->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&sl->base, &sx, &sy, &sw, &sh);
    vg_theme_t *theme = vg_theme_get_current();

    float track_y = sy + sh / 2 - sl->track_thickness / 2;
    float pct = (sl->value - sl->min_value) / (sl->max_value - sl->min_value);
    float fill_w = sw * pct;
    float thumb_x = sx + fill_w;

    // Track
    draw_rect(window, (int)sx, (int)track_y, (int)sw, (int)sl->track_thickness, sl->track_color);
    // Fill
    draw_rect(window, (int)sx, (int)track_y, (int)fill_w, (int)sl->track_thickness, sl->fill_color);
    // Thumb
    int thumb_r = (int)(sl->thumb_size / 2);
    vgfx_fill_circle(window,
                     (int)thumb_x,
                     (int)(sy + sh / 2),
                     thumb_r,
                     (sl->thumb_hovered ? sl->thumb_hover_color : sl->thumb_color) & 0x00FFFFFF);
}

// ProgressBar rendering
/// @brief Render a progress track, fractional fill, border, and optional percentage.
/// @param window Target graphics window.
/// @param pb Progress bar to render; null or hidden bars are ignored.
static void render_progressbar(vgfx_window_t window, vg_progressbar_t *pb) {
    if (!pb || !pb->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&pb->base, &sx, &sy, &sw, &sh);

    draw_rect(window, (int)sx, (int)sy, (int)sw, (int)sh, pb->track_color);
    float fill_w = sw * pb->value;
    draw_rect(window, (int)sx, (int)sy, (int)fill_w, (int)sh, pb->fill_color);
    draw_rect_outline(window, (int)sx, (int)sy, (int)sw, (int)sh, 0xFF5A5A5A);

    if (pb->show_percentage && pb->font) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f%%", pb->value * 100);
        vg_text_metrics_t m;
        vg_font_measure_text(pb->font, pb->font_size, buf, &m);
        float tx = sx + (sw - m.width) / 2;
        float ty = sy + (sh + pb->font_size) / 2 - 2;
        vg_font_draw_text(window, pb->font, pb->font_size, tx, ty, buf, 0xFFFFFFFF);
    }
}

// Dropdown rendering
/// @brief Render a dropdown field and its open, hover-highlighted item list.
/// @param window Target graphics window.
/// @param dd Dropdown to render; null or hidden widgets are ignored.
static void render_dropdown(vgfx_window_t window, vg_dropdown_t *dd) {
    if (!dd || !dd->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&dd->base, &sx, &sy, &sw, &sh);
    vg_theme_t *theme = vg_theme_get_current();

    draw_rect(window, (int)sx, (int)sy, (int)sw, (int)sh, dd->bg_color);
    draw_rect_outline(window, (int)sx, (int)sy, (int)sw, (int)sh, dd->border_color);

    // Draw arrow
    int ax = (int)(sx + sw - 20);
    int ay = (int)(sy + sh / 2);
    vgfx_line(window, ax, ay - 3, ax + 6, ay + 3, theme->colors.fg_primary & 0x00FFFFFF);
    vgfx_line(window, ax + 6, ay + 3, ax + 12, ay - 3, theme->colors.fg_primary & 0x00FFFFFF);

    // Draw text
    const char *text = vg_dropdown_get_selected_text(dd);
    if (!text)
        text = dd->placeholder ? dd->placeholder : "Select...";
    if (dd->font) {
        float ty = sy + (sh + dd->font_size) / 2 - 2;
        vg_font_draw_text(window, dd->font, dd->font_size, sx + 8, ty, text, dd->text_color);
    }

    // Draw open dropdown list
    if (dd->open) {
        float list_y = sy + sh;
        float item_h = 28;
        float list_h = dd->item_count * item_h;
        if (list_h > dd->dropdown_height)
            list_h = dd->dropdown_height;

        draw_rect(window, (int)sx, (int)list_y, (int)sw, (int)list_h, dd->dropdown_bg);
        draw_rect_outline(window, (int)sx, (int)list_y, (int)sw, (int)list_h, dd->border_color);

        for (int i = 0; i < dd->item_count && i * item_h < list_h; i++) {
            float iy = list_y + i * item_h;
            if (i == dd->hovered_index) {
                draw_rect(window, (int)sx + 1, (int)iy, (int)sw - 2, (int)item_h, dd->hover_bg);
            }
            if (dd->font && dd->items[i]) {
                float ty = iy + (item_h + dd->font_size) / 2 - 2;
                vg_font_draw_text(
                    window, dd->font, dd->font_size, sx + 8, ty, dd->items[i], dd->text_color);
            }
        }
    }
}

// ListBox rendering
/// @brief Render the visible listbox items with selected and hovered backgrounds.
/// @param window Target graphics window.
/// @param lb Listbox to render; null or hidden widgets are ignored.
static void render_listbox(vgfx_window_t window, vg_listbox_t *lb) {
    if (!lb || !lb->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&lb->base, &sx, &sy, &sw, &sh);

    draw_rect(window, (int)sx, (int)sy, (int)sw, (int)sh, lb->bg_color);
    draw_rect_outline(window, (int)sx, (int)sy, (int)sw, (int)sh, lb->border_color);

    float iy = sy + 2;
    for (vg_listbox_item_t *item = lb->first_item; item && iy < sy + sh - 2; item = item->next) {
        if (item == lb->selected) {
            draw_rect(
                window, (int)sx + 1, (int)iy, (int)sw - 2, (int)lb->item_height, lb->selected_bg);
        } else if (item == lb->hovered) {
            draw_rect(
                window, (int)sx + 1, (int)iy, (int)sw - 2, (int)lb->item_height, lb->hover_bg);
        }
        if (lb->font && item->text) {
            float ty = iy + (lb->item_height + lb->font_size) / 2 - 2;
            vg_font_draw_text(
                window, lb->font, lb->font_size, sx + 8, ty, item->text, lb->text_color);
        }
        iy += lb->item_height;
    }
}

// Spinner rendering
/// @brief Render a numeric spinner's value field, increment buttons, and arrows.
/// @param window Target graphics window.
/// @param sp Spinner to render; null or hidden widgets are ignored.
static void render_spinner(vgfx_window_t window, vg_spinner_t *sp) {
    if (!sp || !sp->base.visible)
        return;
    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&sp->base, &sx, &sy, &sw, &sh);
    vg_theme_t *theme = vg_theme_get_current();

    float bw = sp->button_width;
    float tw = sw - bw;

    // Text area
    draw_rect(window, (int)sx, (int)sy, (int)tw, (int)sh, sp->bg_color);
    draw_rect_outline(window, (int)sx, (int)sy, (int)tw, (int)sh, sp->border_color);

    // Buttons
    float bx = sx + tw;
    draw_rect(window,
              (int)bx,
              (int)sy,
              (int)bw,
              (int)(sh / 2),
              sp->up_hovered ? theme->colors.bg_hover : sp->button_color);
    draw_rect(window,
              (int)bx,
              (int)(sy + sh / 2),
              (int)bw,
              (int)(sh / 2),
              sp->down_hovered ? theme->colors.bg_hover : sp->button_color);
    draw_rect_outline(window, (int)bx, (int)sy, (int)bw, (int)sh, sp->border_color);

    // Arrows
    int acx = (int)(bx + bw / 2);
    vgfx_line(window, acx - 4, (int)(sy + sh / 4 + 2), acx, (int)(sy + sh / 4 - 2), 0xCCCCCC);
    vgfx_line(window, acx, (int)(sy + sh / 4 - 2), acx + 4, (int)(sy + sh / 4 + 2), 0xCCCCCC);
    vgfx_line(
        window, acx - 4, (int)(sy + 3 * sh / 4 - 2), acx, (int)(sy + 3 * sh / 4 + 2), 0xCCCCCC);
    vgfx_line(
        window, acx, (int)(sy + 3 * sh / 4 + 2), acx + 4, (int)(sy + 3 * sh / 4 - 2), 0xCCCCCC);

    // Value text
    if (sp->font && sp->text_buffer) {
        vg_text_metrics_t m;
        vg_font_measure_text(sp->font, sp->font_size, sp->text_buffer, &m);
        float tx = sx + (tw - m.width) / 2;
        float ty = sy + (sh + sp->font_size) / 2 - 2;
        vg_font_draw_text(window, sp->font, sp->font_size, tx, ty, sp->text_buffer, sp->text_color);
    }
}

//=============================================================================
// Section Drawing
//=============================================================================

/// @brief Draw a bordered showcase section with title and separator.
/// @param window Target graphics window.
/// @param font Optional font for the section title.
/// @param title Optional section title.
/// @param x Left edge.
/// @param y Top edge.
/// @param w Section width.
/// @param h Section height.
static void draw_section(
    vgfx_window_t window, vg_font_t *font, const char *title, int x, int y, int w, int h) {
    vg_theme_t *theme = vg_theme_get_current();
    draw_rect(window, x, y, w, h, 0xFF252526);
    draw_rect_outline(window, x, y, w, h, theme->colors.border_primary);
    if (font && title) {
        vg_font_draw_text(window, font, 14, x + 10, y + 18, title, theme->colors.accent_primary);
    }
    // Separator line under title
    vgfx_line(window, x + 5, y + 26, x + w - 5, y + 26, theme->colors.border_primary & 0x00FFFFFF);
}

//=============================================================================
// Main Render
//=============================================================================

/// @brief Draw one complete frame of the categorized widget showcase.
/// @param state Initialized showcase state containing all widget values.
static void render_showcase(showcase_state_t *state) {
    vgfx_window_t window = state->window;
    vg_theme_t *theme = vg_theme_get_current();

    vgfx_cls(window, theme->colors.bg_primary & 0x00FFFFFF);

    // Title
    if (state->font) {
        vg_font_draw_text(
            window, state->font, 28, 20, 40, "ZannaGUI Widget Showcase", theme->colors.fg_primary);
    }

    // === Input Section ===
    draw_section(window, state->font, "Text Input", 20, 60, 360, 180);
    render_label(window,
                 (vg_label_t *)&(vg_label_t){
                     .base = {.visible = true, .x = 30, .y = 90, .width = 100, .height = 20},
                     .font = state->font,
                     .font_size = 12,
                     .text = "Name:"});
    render_textinput(window, state->name_input);
    render_label(window,
                 (vg_label_t *)&(vg_label_t){
                     .base = {.visible = true, .x = 30, .y = 125, .width = 100, .height = 20},
                     .font = state->font,
                     .font_size = 12,
                     .text = "Email:"});
    render_textinput(window, state->email_input);
    render_label(window,
                 (vg_label_t *)&(vg_label_t){
                     .base = {.visible = true, .x = 30, .y = 160, .width = 100, .height = 20},
                     .font = state->font,
                     .font_size = 12,
                     .text = "Password:"});
    render_textinput(window, state->password_input);
    render_label(window,
                 (vg_label_t *)&(vg_label_t){
                     .base = {.visible = true, .x = 30, .y = 195, .width = 100, .height = 20},
                     .font = state->font,
                     .font_size = 12,
                     .text = "Age:"});
    render_spinner(window, state->age_spinner);

    // === Selection Section ===
    draw_section(window, state->font, "Selection Controls", 400, 60, 380, 180);
    render_label(window,
                 (vg_label_t *)&(vg_label_t){
                     .base = {.visible = true, .x = 410, .y = 90, .width = 100, .height = 20},
                     .font = state->font,
                     .font_size = 12,
                     .text = "Country:"});
    render_dropdown(window, state->country_dropdown);
    render_label(window,
                 (vg_label_t *)&(vg_label_t){
                     .base = {.visible = true, .x = 590, .y = 90, .width = 100, .height = 20},
                     .font = state->font,
                     .font_size = 12,
                     .text = "Languages:"});
    render_listbox(window, state->languages_list);
    render_label(window,
                 (vg_label_t *)&(vg_label_t){
                     .base = {.visible = true, .x = 410, .y = 145, .width = 100, .height = 20},
                     .font = state->font,
                     .font_size = 12,
                     .text = "Gender:"});
    render_radio(window, state->radio_male);
    render_radio(window, state->radio_female);
    render_radio(window, state->radio_other);

    // === Options Section ===
    draw_section(window, state->font, "Options", 20, 250, 360, 100);
    render_checkbox(window, state->newsletter_check);
    render_checkbox(window, state->terms_check);

    // === Sliders Section ===
    draw_section(window, state->font, "Sliders & Progress", 400, 250, 380, 100);
    render_label(window, state->volume_label);
    render_slider(window, state->volume_slider);
    render_label(window, state->brightness_label);
    render_slider(window, state->brightness_slider);

    // === Progress Section ===
    draw_section(window, state->font, "Progress Bar", 20, 360, 760, 80);
    render_progressbar(window, state->download_progress);
    render_button(window, state->start_btn);
    render_button(window, state->cancel_btn);

    // === Actions Section ===
    draw_section(window, state->font, "Actions", 20, 450, 760, 80);
    render_button(window, state->submit_btn);
    render_label(window, state->status_label);

    // Credits
    if (state->font) {
        vg_font_draw_text(window,
                          state->font,
                          11,
                          20,
                          555,
                          "ZannaGUI - A lightweight GUI library for Zanna",
                          0xFF666666);
    }
}

//=============================================================================
// Event Handling
//=============================================================================

/// @brief Drain native events and update every interactive showcase control.
/// @details The handler maintains hover and press state, dropdown/list selections, slider
/// dragging, text focus and selection, printable editing, and spinner keyboard adjustment.
/// Close or Escape stops the main loop.
/// @param state Mutable initialized showcase state.
static void handle_events(showcase_state_t *state) {
    vgfx_event_t pe;

    while (vgfx_poll_event(state->window, &pe)) {
        if (pe.type == VGFX_EVENT_CLOSE) {
            state->running = false;
            return;
        }
        if (pe.type == VGFX_EVENT_KEY_DOWN && pe.data.key.key == VGFX_KEY_ESCAPE) {
            state->running = false;
            return;
        }

        int32_t mx, my;
        vgfx_mouse_pos(state->window, &mx, &my);

// Simple hover detection helper
/// @brief Update one widget's hovered flag from the current mouse coordinates.
#define CHECK_HOVER(widget)                                                                        \
    do {                                                                                           \
        float bx, by, bw, bh;                                                                      \
        vg_widget_get_screen_bounds(&(widget)->base, &bx, &by, &bw, &bh);                          \
        if (mx >= bx && mx < bx + bw && my >= by && my < by + bh)                                  \
            (widget)->base.state |= VG_STATE_HOVERED;                                              \
        else                                                                                       \
            (widget)->base.state &= ~VG_STATE_HOVERED;                                             \
    } while (0)

        // Update hover states
        if (pe.type == VGFX_EVENT_MOUSE_MOVE) {
            CHECK_HOVER(state->start_btn);
            CHECK_HOVER(state->cancel_btn);
            CHECK_HOVER(state->submit_btn);
            CHECK_HOVER(state->newsletter_check);
            CHECK_HOVER(state->terms_check);
            CHECK_HOVER(state->radio_male);
            CHECK_HOVER(state->radio_female);
            CHECK_HOVER(state->radio_other);
            CHECK_HOVER(state->country_dropdown);

            // Slider thumb hover
            float sx, sy, sw, sh;
            vg_widget_get_screen_bounds(&state->volume_slider->base, &sx, &sy, &sw, &sh);
            float pct = (state->volume_slider->value - state->volume_slider->min_value) /
                        (state->volume_slider->max_value - state->volume_slider->min_value);
            float thumb_x = sx + sw * pct;
            state->volume_slider->thumb_hovered =
                (mx >= thumb_x - 10 && mx <= thumb_x + 10 && my >= sy && my <= sy + sh);

            vg_widget_get_screen_bounds(&state->brightness_slider->base, &sx, &sy, &sw, &sh);
            pct = (state->brightness_slider->value - state->brightness_slider->min_value) /
                  (state->brightness_slider->max_value - state->brightness_slider->min_value);
            thumb_x = sx + sw * pct;
            state->brightness_slider->thumb_hovered =
                (mx >= thumb_x - 10 && mx <= thumb_x + 10 && my >= sy && my <= sy + sh);

            // Dropdown hover
            if (state->country_dropdown->open) {
                float ddx, ddy, ddw, ddh;
                vg_widget_get_screen_bounds(&state->country_dropdown->base, &ddx, &ddy, &ddw, &ddh);
                float list_y = ddy + ddh;
                state->country_dropdown->hovered_index = -1;
                if (mx >= ddx && mx < ddx + ddw && my >= list_y) {
                    int idx = (int)((my - list_y) / 28);
                    if (idx >= 0 && idx < state->country_dropdown->item_count) {
                        state->country_dropdown->hovered_index = idx;
                    }
                }
            }

            // ListBox hover
            float lbx, lby, lbw, lbh;
            vg_widget_get_screen_bounds(&state->languages_list->base, &lbx, &lby, &lbw, &lbh);
            state->languages_list->hovered = NULL;
            if (mx >= lbx && mx < lbx + lbw && my >= lby && my < lby + lbh) {
                int idx = (int)((my - lby - 2) / state->languages_list->item_height);
                vg_listbox_item_t *item = state->languages_list->first_item;
                for (int i = 0; item && i < idx; i++)
                    item = item->next;
                state->languages_list->hovered = item;
            }
        }

        // Handle clicks
        if (pe.type == VGFX_EVENT_MOUSE_DOWN) {
            // Buttons
            if (state->start_btn->base.state & VG_STATE_HOVERED) {
                on_start_download(&state->start_btn->base, NULL);
            }
            if (state->cancel_btn->base.state & VG_STATE_HOVERED) {
                on_cancel_download(&state->cancel_btn->base, NULL);
            }
            if (state->submit_btn->base.state & VG_STATE_HOVERED) {
                on_submit(&state->submit_btn->base, NULL);
            }

            // Checkboxes
            if (state->newsletter_check->base.state & VG_STATE_HOVERED) {
                vg_checkbox_toggle(state->newsletter_check);
                on_newsletter_toggle(
                    &state->newsletter_check->base, state->newsletter_check->checked, NULL);
            }
            if (state->terms_check->base.state & VG_STATE_HOVERED) {
                vg_checkbox_toggle(state->terms_check);
            }

            // Radio buttons
            if (state->radio_male->base.state & VG_STATE_HOVERED) {
                vg_radiobutton_set_selected(state->radio_male, true);
            }
            if (state->radio_female->base.state & VG_STATE_HOVERED) {
                vg_radiobutton_set_selected(state->radio_female, true);
            }
            if (state->radio_other->base.state & VG_STATE_HOVERED) {
                vg_radiobutton_set_selected(state->radio_other, true);
            }

            // Dropdown toggle
            if (state->country_dropdown->base.state & VG_STATE_HOVERED) {
                state->country_dropdown->open = !state->country_dropdown->open;
            } else if (state->country_dropdown->open) {
                // Check if clicking on list item
                if (state->country_dropdown->hovered_index >= 0) {
                    vg_dropdown_set_selected(state->country_dropdown,
                                             state->country_dropdown->hovered_index);
                    on_country_change(&state->country_dropdown->base,
                                      state->country_dropdown->selected_index,
                                      vg_dropdown_get_selected_text(state->country_dropdown),
                                      NULL);
                }
                state->country_dropdown->open = false;
            }

            // ListBox selection
            if (state->languages_list->hovered) {
                vg_listbox_select(state->languages_list, state->languages_list->hovered);
            }

            // Slider dragging
            if (state->volume_slider->thumb_hovered) {
                state->volume_slider->dragging = true;
            }
            if (state->brightness_slider->thumb_hovered) {
                state->brightness_slider->dragging = true;
            }

            // Focus text inputs and position cursor on click
            float bx, by, bw, bh;
            state->name_input->base.state &= ~VG_STATE_FOCUSED;
            state->email_input->base.state &= ~VG_STATE_FOCUSED;
            state->password_input->base.state &= ~VG_STATE_FOCUSED;
            state->selecting_input = NULL;

            vg_textinput_t *inputs[] = {
                state->name_input, state->email_input, state->password_input};
            for (int i = 0; i < 3; i++) {
                vg_widget_get_screen_bounds(&inputs[i]->base, &bx, &by, &bw, &bh);
                if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                    inputs[i]->base.state |= VG_STATE_FOCUSED;
                    // Calculate and set cursor position
                    float rel_x = mx - bx;
                    size_t pos = calc_cursor_from_x(inputs[i], rel_x);
                    inputs[i]->cursor_pos = pos;
                    // Start selection tracking
                    inputs[i]->selection_start = pos;
                    inputs[i]->selection_end = pos;
                    state->selecting_input = inputs[i];
                    state->selection_anchor = pos;
                    break;
                }
            }
        }

        if (pe.type == VGFX_EVENT_MOUSE_UP) {
            state->volume_slider->dragging = false;
            state->brightness_slider->dragging = false;
            state->selecting_input = NULL;
        }

        // Slider dragging and text selection
        if (pe.type == VGFX_EVENT_MOUSE_MOVE) {
            if (state->volume_slider->dragging) {
                float sx, sy, sw, sh;
                vg_widget_get_screen_bounds(&state->volume_slider->base, &sx, &sy, &sw, &sh);
                float pct = (mx - sx) / sw;
                if (pct < 0)
                    pct = 0;
                if (pct > 1)
                    pct = 1;
                float val =
                    state->volume_slider->min_value +
                    pct * (state->volume_slider->max_value - state->volume_slider->min_value);
                vg_slider_set_value(state->volume_slider, val);
            }
            if (state->brightness_slider->dragging) {
                float sx, sy, sw, sh;
                vg_widget_get_screen_bounds(&state->brightness_slider->base, &sx, &sy, &sw, &sh);
                float pct = (mx - sx) / sw;
                if (pct < 0)
                    pct = 0;
                if (pct > 1)
                    pct = 1;
                float val = state->brightness_slider->min_value +
                            pct * (state->brightness_slider->max_value -
                                   state->brightness_slider->min_value);
                vg_slider_set_value(state->brightness_slider, val);
            }
            // Text selection drag
            if (state->selecting_input) {
                float bx, by, bw, bh;
                vg_widget_get_screen_bounds(&state->selecting_input->base, &bx, &by, &bw, &bh);
                float rel_x = mx - bx;
                size_t pos = calc_cursor_from_x(state->selecting_input, rel_x);
                state->selecting_input->cursor_pos = pos;
                // Update selection range
                if (pos < state->selection_anchor) {
                    state->selecting_input->selection_start = pos;
                    state->selecting_input->selection_end = state->selection_anchor;
                } else {
                    state->selecting_input->selection_start = state->selection_anchor;
                    state->selecting_input->selection_end = pos;
                }
            }
        }

        // Text input handling
        if (pe.type == VGFX_EVENT_KEY_DOWN) {
            vg_textinput_t *focused = NULL;
            if (state->name_input->base.state & VG_STATE_FOCUSED)
                focused = state->name_input;
            else if (state->email_input->base.state & VG_STATE_FOCUSED)
                focused = state->email_input;
            else if (state->password_input->base.state & VG_STATE_FOCUSED)
                focused = state->password_input;

            if (focused) {
                vgfx_key_t key = pe.data.key.key;
                // Handle backspace and delete keys
                if (key == VGFX_KEY_BACKSPACE) {
                    if (focused->cursor_pos > 0 && focused->text_len > 0) {
                        memmove(focused->text + focused->cursor_pos - 1,
                                focused->text + focused->cursor_pos,
                                focused->text_len - focused->cursor_pos + 1);
                        focused->cursor_pos--;
                        focused->text_len--;
                    }
                } else if (key == VGFX_KEY_DELETE) {
                    // Forward delete - delete character at cursor
                    if (focused->cursor_pos < focused->text_len) {
                        memmove(focused->text + focused->cursor_pos,
                                focused->text + focused->cursor_pos + 1,
                                focused->text_len - focused->cursor_pos);
                        focused->text_len--;
                    }
                } else if (key == VGFX_KEY_LEFT) {
                    if (focused->cursor_pos > 0)
                        focused->cursor_pos--;
                } else if (key == VGFX_KEY_RIGHT) {
                    if (focused->cursor_pos < focused->text_len)
                        focused->cursor_pos++;
                } else if (key == VGFX_KEY_HOME) {
                    focused->cursor_pos = 0;
                } else if (key == VGFX_KEY_END) {
                    focused->cursor_pos = focused->text_len;
                } else if (key >= 32 && key <= 126) {
                    char ch = (char)key;
                    // Simple lowercase conversion (vgfx reports uppercase)
                    if (key >= 'A' && key <= 'Z') {
                        ch = ch - 'A' + 'a';
                    }
                    vg_textinput_insert(focused, (char[]){ch, '\0'});
                }
            }

            // Spinner up/down with arrow keys
            float sbx, sby, sbw, sbh;
            vg_widget_get_screen_bounds(&state->age_spinner->base, &sbx, &sby, &sbw, &sbh);
            if (mx >= sbx && mx < sbx + sbw && my >= sby && my < sby + sbh) {
                if (pe.data.key.key == VGFX_KEY_UP) {
                    vg_spinner_set_value(state->age_spinner,
                                         state->age_spinner->value + state->age_spinner->step);
                } else if (pe.data.key.key == VGFX_KEY_DOWN) {
                    vg_spinner_set_value(state->age_spinner,
                                         state->age_spinner->value - state->age_spinner->step);
                }
            }
        }
    }
}

//=============================================================================
// Animation Update
//=============================================================================

/// @brief Advance the simulated download and synchronize the progress widget.
/// @param state Mutable showcase state containing animation progress and status text.
static void update_animation(showcase_state_t *state) {
    if (state->downloading) {
        state->progress_value += 0.005f;
        if (state->progress_value >= 1.0f) {
            state->progress_value = 1.0f;
            state->downloading = false;
            vg_label_set_text(state->status_label, "Download complete!");
        }
        vg_progressbar_set_value(state->download_progress, state->progress_value);
    }
}

//=============================================================================
// Initialization
//=============================================================================

/// @brief Create the window, load a fallback font, and configure every showcase widget.
/// @details The function assigns fixed demonstration geometry and initial values for inputs,
/// selection controls, sliders, progress, and action widgets.
/// @param state Zero-initialized destination state.
/// @return True when the graphics window and showcase state are ready; otherwise false.
static bool init_showcase(showcase_state_t *state) {
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 800;
    params.height = 580;
    params.title = "ZannaGUI Widget Showcase";
    params.resizable = 0;
    params.fps = 60;

    state->window = vgfx_create_window(&params);
    if (!state->window) {
        fprintf(stderr, "Failed to create window\n");
        return false;
    }

    // Load font
    const char *font_paths[] = {"/System/Library/Fonts/SFNSMono.ttf",
                                "/System/Library/Fonts/Menlo.ttc",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                "C:\\Windows\\Fonts\\arial.ttf",
                                NULL};
    for (int i = 0; font_paths[i]; i++) {
        state->font = vg_font_load_file(font_paths[i]);
        if (state->font)
            break;
    }

    vg_theme_set_current(vg_theme_dark());

    // Create widgets
    // Text inputs
    state->name_input = vg_textinput_create(NULL);
    state->name_input->base.x = 100;
    state->name_input->base.y = 90;
    state->name_input->base.width = 260;
    state->name_input->base.height = 28;
    vg_textinput_set_font(state->name_input, state->font, 13);
    vg_textinput_set_placeholder(state->name_input, "Enter your name");

    state->email_input = vg_textinput_create(NULL);
    state->email_input->base.x = 100;
    state->email_input->base.y = 125;
    state->email_input->base.width = 260;
    state->email_input->base.height = 28;
    vg_textinput_set_font(state->email_input, state->font, 13);
    vg_textinput_set_placeholder(state->email_input, "you@example.com");

    state->password_input = vg_textinput_create(NULL);
    state->password_input->base.x = 100;
    state->password_input->base.y = 160;
    state->password_input->base.width = 260;
    state->password_input->base.height = 28;
    state->password_input->password_mode = true;
    vg_textinput_set_font(state->password_input, state->font, 13);
    vg_textinput_set_placeholder(state->password_input, "Password");

    state->age_spinner = vg_spinner_create(NULL);
    state->age_spinner->base.x = 100;
    state->age_spinner->base.y = 195;
    state->age_spinner->base.width = 100;
    state->age_spinner->base.height = 28;
    vg_spinner_set_font(state->age_spinner, state->font, 13);
    vg_spinner_set_range(state->age_spinner, 0, 120);
    vg_spinner_set_value(state->age_spinner, 25);

    // Dropdown
    state->country_dropdown = vg_dropdown_create(NULL);
    state->country_dropdown->base.x = 410;
    state->country_dropdown->base.y = 105;
    state->country_dropdown->base.width = 160;
    state->country_dropdown->base.height = 28;
    vg_dropdown_set_font(state->country_dropdown, state->font, 13);
    vg_dropdown_set_placeholder(state->country_dropdown, "Select country");
    vg_dropdown_add_item(state->country_dropdown, "United States");
    vg_dropdown_add_item(state->country_dropdown, "Canada");
    vg_dropdown_add_item(state->country_dropdown, "United Kingdom");
    vg_dropdown_add_item(state->country_dropdown, "Germany");
    vg_dropdown_add_item(state->country_dropdown, "France");
    vg_dropdown_add_item(state->country_dropdown, "Japan");

    // ListBox
    state->languages_list = vg_listbox_create(NULL);
    state->languages_list->base.x = 590;
    state->languages_list->base.y = 105;
    state->languages_list->base.width = 170;
    state->languages_list->base.height = 120;
    vg_listbox_set_font(state->languages_list, state->font, 12);
    vg_listbox_add_item(state->languages_list, "English", NULL);
    vg_listbox_add_item(state->languages_list, "Spanish", NULL);
    vg_listbox_add_item(state->languages_list, "French", NULL);
    vg_listbox_add_item(state->languages_list, "German", NULL);
    vg_listbox_add_item(state->languages_list, "Japanese", NULL);

    // Radio buttons
    state->gender_group = vg_radiogroup_create();
    state->radio_male = vg_radiobutton_create(NULL, "Male", state->gender_group);
    state->radio_male->base.x = 410;
    state->radio_male->base.y = 160;
    state->radio_male->base.width = 80;
    state->radio_male->base.height = 24;
    state->radio_male->font = state->font;

    state->radio_female = vg_radiobutton_create(NULL, "Female", state->gender_group);
    state->radio_female->base.x = 490;
    state->radio_female->base.y = 160;
    state->radio_female->base.width = 80;
    state->radio_female->base.height = 24;
    state->radio_female->font = state->font;

    state->radio_other = vg_radiobutton_create(NULL, "Other", state->gender_group);
    state->radio_other->base.x = 570;
    state->radio_other->base.y = 160;
    state->radio_other->base.width = 80;
    state->radio_other->base.height = 24;
    state->radio_other->font = state->font;

    // Checkboxes
    state->newsletter_check = vg_checkbox_create(NULL, "Subscribe to newsletter");
    state->newsletter_check->base.x = 30;
    state->newsletter_check->base.y = 280;
    state->newsletter_check->base.width = 200;
    state->newsletter_check->base.height = 24;
    state->newsletter_check->font = state->font;
    state->newsletter_check->font_size = 13;

    state->terms_check = vg_checkbox_create(NULL, "I agree to the terms");
    state->terms_check->base.x = 30;
    state->terms_check->base.y = 310;
    state->terms_check->base.width = 200;
    state->terms_check->base.height = 24;
    state->terms_check->font = state->font;
    state->terms_check->font_size = 13;

    // Sliders
    state->volume_label = vg_label_create(NULL, "Volume: 50%");
    state->volume_label->base.x = 410;
    state->volume_label->base.y = 275;
    state->volume_label->base.width = 100;
    state->volume_label->base.height = 20;
    vg_label_set_font(state->volume_label, state->font, 12);

    state->volume_slider = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    state->volume_slider->base.x = 520;
    state->volume_slider->base.y = 275;
    state->volume_slider->base.width = 240;
    state->volume_slider->base.height = 20;
    vg_slider_set_range(state->volume_slider, 0, 100);
    vg_slider_set_value(state->volume_slider, 50);
    vg_slider_set_on_change(state->volume_slider, on_volume_change, NULL);

    state->brightness_label = vg_label_create(NULL, "Brightness: 75%");
    state->brightness_label->base.x = 410;
    state->brightness_label->base.y = 310;
    state->brightness_label->base.width = 100;
    state->brightness_label->base.height = 20;
    vg_label_set_font(state->brightness_label, state->font, 12);

    state->brightness_slider = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    state->brightness_slider->base.x = 520;
    state->brightness_slider->base.y = 310;
    state->brightness_slider->base.width = 240;
    state->brightness_slider->base.height = 20;
    vg_slider_set_range(state->brightness_slider, 0, 100);
    vg_slider_set_value(state->brightness_slider, 75);
    vg_slider_set_on_change(state->brightness_slider, on_brightness_change, NULL);

    // Progress bar
    state->download_progress = vg_progressbar_create(NULL);
    state->download_progress->base.x = 30;
    state->download_progress->base.y = 390;
    state->download_progress->base.width = 500;
    state->download_progress->base.height = 24;
    state->download_progress->font = state->font;
    state->download_progress->font_size = 12;
    state->download_progress->show_percentage = true;

    state->start_btn = vg_button_create(NULL, "Start");
    state->start_btn->base.x = 550;
    state->start_btn->base.y = 388;
    state->start_btn->base.width = 100;
    state->start_btn->base.height = 28;
    vg_button_set_font(state->start_btn, state->font, 13);
    vg_button_set_style(state->start_btn, VG_BUTTON_STYLE_PRIMARY);

    state->cancel_btn = vg_button_create(NULL, "Cancel");
    state->cancel_btn->base.x = 660;
    state->cancel_btn->base.y = 388;
    state->cancel_btn->base.width = 100;
    state->cancel_btn->base.height = 28;
    vg_button_set_font(state->cancel_btn, state->font, 13);
    vg_button_set_style(state->cancel_btn, VG_BUTTON_STYLE_DANGER);

    // Submit button
    state->submit_btn = vg_button_create(NULL, "Submit Form");
    state->submit_btn->base.x = 30;
    state->submit_btn->base.y = 480;
    state->submit_btn->base.width = 140;
    state->submit_btn->base.height = 36;
    vg_button_set_font(state->submit_btn, state->font, 14);
    vg_button_set_style(state->submit_btn, VG_BUTTON_STYLE_PRIMARY);

    // Status label
    state->status_label = vg_label_create(NULL, "Ready");
    state->status_label->base.x = 190;
    state->status_label->base.y = 488;
    state->status_label->base.width = 500;
    state->status_label->base.height = 24;
    vg_label_set_font(state->status_label, state->font, 13);

    state->running = true;
    return true;
}

//=============================================================================
// Main
//=============================================================================

/// @brief Run the interactive widget showcase until close or Escape.
/// @param argc Process argument count; currently ignored.
/// @param argv Process argument vector; currently ignored.
/// @return Zero after normal completion, or one when initialization fails.
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("ZannaGUI Widget Showcase\n");
    printf("========================\n");
    printf("Press ESC to exit\n\n");

    memset(&g_state, 0, sizeof(g_state));

    if (!init_showcase(&g_state)) {
        return 1;
    }

    while (g_state.running) {
        handle_events(&g_state);
        update_animation(&g_state);
        render_showcase(&g_state);
        vgfx_update(g_state.window);
    }

    vgfx_destroy_window(g_state.window);
    printf("Showcase exited cleanly.\n");
    return 0;
}
