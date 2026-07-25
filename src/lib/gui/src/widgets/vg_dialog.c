//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_dialog.c
/// @brief Implements modal and non-modal dialogs with responsive content layout.
///
/// @details Dialogs provide a scalable title bar, close control, status or
/// custom icon, wrapped message, adopted content widget, scrolling overflow,
/// and preset or custom button sets. Modal dialogs register their base widget
/// as the modal root while open.
///
/// Titles, messages, custom button labels, and custom icons are owned by the
/// dialog. Embedded content is adopted into the base widget hierarchy and is
/// cleaned up through that hierarchy. Close callbacks are snapshotted before
/// invocation and guarded against recursive close calls. Callbacks must not
/// destroy the dialog synchronously; destruction should be scheduled after
/// `vg_dialog_close()` returns.
///
/// All geometric helpers honor theme UI scale for high-DPI layouts.
///
/// @see vg_ide_widgets.h
/// @see vg_theme.h
/// @see vg_event.h
/// @see vg_widget.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widget.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Constants
//=============================================================================

#define DIALOG_DEFAULT_MIN_WIDTH 300
#define DIALOG_DEFAULT_MIN_HEIGHT 150
#define DIALOG_DEFAULT_MAX_WIDTH 800
#define DIALOG_DEFAULT_MAX_HEIGHT 600
#define DIALOG_TITLE_BAR_HEIGHT 32
#define DIALOG_BUTTON_BAR_HEIGHT 48
#define DIALOG_CONTENT_PADDING 16
#define DIALOG_BUTTON_PADDING 8
#define DIALOG_BUTTON_HEIGHT 28
#define DIALOG_BUTTON_MIN_WIDTH 80
#define DIALOG_CLOSE_BUTTON_SIZE 24
#define DIALOG_ICON_SIZE 32
#define DIALOG_SCREEN_MARGIN 24

//=============================================================================
// Forward Declarations
//=============================================================================

static void dialog_destroy(vg_widget_t *widget);
static void dialog_measure(vg_widget_t *widget, float available_width, float available_height);
static void dialog_arrange(vg_widget_t *widget, float x, float y, float width, float height);
static void dialog_paint(vg_widget_t *widget, void *canvas);
static bool dialog_handle_event(vg_widget_t *widget, vg_event_t *event);

//=============================================================================
// Dialog VTable
//=============================================================================

static vg_widget_vtable_t g_dialog_vtable = {.destroy = dialog_destroy,
                                             .measure = dialog_measure,
                                             .arrange = dialog_arrange,
                                             .paint = dialog_paint,
                                             .handle_event = dialog_handle_event,
                                             .can_focus = NULL,
                                             .on_focus = NULL};

//=============================================================================
// Helper Functions
//=============================================================================

typedef struct {
    const char *label;
    vg_dialog_result_t result;
    bool is_default;
    bool is_cancel;
} preset_button_t;

static void get_preset_buttons(vg_dialog_buttons_t preset,
                               const preset_button_t **buttons,
                               size_t *count);
static float get_button_width(vg_dialog_t *dlg, const char *label);

static const preset_button_t g_ok_buttons[] = {{"OK", VG_DIALOG_RESULT_OK, true, false}};

static const preset_button_t g_ok_cancel_buttons[] = {
    {"OK", VG_DIALOG_RESULT_OK, true, false}, {"Cancel", VG_DIALOG_RESULT_CANCEL, false, true}};

static const preset_button_t g_yes_no_buttons[] = {{"Yes", VG_DIALOG_RESULT_YES, true, false},
                                                   {"No", VG_DIALOG_RESULT_NO, false, true}};

static const preset_button_t g_yes_no_cancel_buttons[] = {
    {"Yes", VG_DIALOG_RESULT_YES, true, false},
    {"No", VG_DIALOG_RESULT_NO, false, false},
    {"Cancel", VG_DIALOG_RESULT_CANCEL, false, true}};

static const preset_button_t g_retry_cancel_buttons[] = {
    {"Retry", VG_DIALOG_RESULT_RETRY, true, false},
    {"Cancel", VG_DIALOG_RESULT_CANCEL, false, true}};

/// @brief Returns the current positive UI scale factor.
///
/// @return Theme UI scale, or `1.0` when no valid scale is available.
static float dialog_ui_scale(void) {
    vg_theme_t *theme = vg_theme_get_current();
    return (theme && theme->ui_scale > 0.0f) ? theme->ui_scale : 1.0f;
}

/// @brief Returns the outer content padding for the current theme and scale.
///
/// @return Positive padding in framebuffer units.
static float dialog_outer_padding(void) {
    vg_theme_t *theme = vg_theme_get_current();
    float s = dialog_ui_scale();
    float theme_pad = theme ? theme->spacing.lg : (float)DIALOG_CONTENT_PADDING;
    return theme_pad > 0.0f ? theme_pad : (float)DIALOG_CONTENT_PADDING * s;
}

/// @brief Returns the gap between message, content, and button sections.
///
/// @return Positive section gap in framebuffer units.
static float dialog_section_gap(void) {
    vg_theme_t *theme = vg_theme_get_current();
    float s = dialog_ui_scale();
    float gap = theme ? theme->spacing.md : (float)DIALOG_BUTTON_PADDING;
    return gap > 0.0f ? gap : (float)DIALOG_BUTTON_PADDING * s;
}

/// @brief Returns the scaled title-bar height.
///
/// @return Title-bar height in framebuffer units.
static float dialog_title_height(void) {
    float s = dialog_ui_scale();
    return (float)DIALOG_TITLE_BAR_HEIGHT * s + 6.0f * s;
}

/// @brief Returns the scaled bottom button-bar height.
///
/// @return Button-bar height in framebuffer units.
static float dialog_button_bar_height(void) {
    float s = dialog_ui_scale();
    return (float)DIALOG_BUTTON_BAR_HEIGHT * s + 6.0f * s;
}

/// @brief Returns the scaled height of an individual dialog button.
///
/// @return Button height in framebuffer units.
static float dialog_button_height(void) {
    float s = dialog_ui_scale();
    return (float)DIALOG_BUTTON_HEIGHT * s + 4.0f * s;
}

/// @brief Returns the square close-button size for the current UI scale.
///
/// @return Close-button width and height in framebuffer units.
static float dialog_close_size(void) {
    return (float)DIALOG_CLOSE_BUTTON_SIZE * dialog_ui_scale();
}

/// @brief Returns the square content-icon size for the current UI scale.
///
/// @return Icon width and height in framebuffer units.
static float dialog_icon_size(void) {
    return (float)DIALOG_ICON_SIZE * dialog_ui_scale();
}

/// @brief Returns the scaled corner radius for dialog surfaces.
///
/// @return Corner radius in pixels, never less than three.
static int dialog_corner_radius(void) {
    float s = dialog_ui_scale();
    int radius = (int)(8.0f * s);
    if (radius < 3)
        radius = 3;
    return radius;
}

/// @brief Fills a rounded rectangle through the shared anti-aliased renderer.
///
/// @param win Destination window.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width.
/// @param h Height.
/// @param radius Corner radius.
/// @param color Packed fill color.
static void dialog_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_fill(win, (float)x, (float)y, (float)w, (float)h, (float)radius, color);
}

/// @brief Strokes a rounded rectangle through the shared anti-aliased renderer.
///
/// @param win Destination window.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width.
/// @param h Height.
/// @param radius Corner radius.
/// @param color Packed one-pixel stroke color.
static void dialog_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_stroke(
        win, (float)x, (float)y, (float)w, (float)h, (float)radius, 1.0f, color);
}

/// @brief Copies a byte range into a newly allocated null-terminated string.
///
/// @param text Source byte range containing at least @p len bytes.
/// @param len Number of bytes to copy.
/// @return Owned string, or null on allocation failure.
static char *dialog_dup_range(const char *text, size_t len) {
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Frees wrapped lines and their pointer array.
///
/// @param lines Owned line array returned by `dialog_wrap_text()`; may be null.
/// @param line_count Number of entries to release.
static void dialog_free_wrapped_lines(char **lines, int line_count) {
    if (!lines)
        return;
    for (int i = 0; i < line_count; i++) {
        free(lines[i]);
    }
    free(lines);
}

/// @brief Wraps dialog text to a maximum rendered width.
///
/// @details Explicit newlines are preserved, breakable whitespace is preferred,
/// and an oversized first character still advances the input. When requested,
/// each produced line and the pointer array are heap-owned by the caller.
///
/// @param dlg Dialog supplying the live font and font size.
/// @param text Null-terminated input text.
/// @param max_width Maximum line width; non-positive values permit unbounded
///                  measurement.
/// @param[out] out_lines Optional receiver for an owned array of owned strings.
/// @param[out] out_max_width Optional receiver for the widest measured line.
/// @return Number of logical lines, at least one after successful non-empty
///         processing, or zero when prerequisites or allocation are missing.
static int dialog_wrap_text(
    vg_dialog_t *dlg, const char *text, float max_width, char ***out_lines, float *out_max_width) {
    if (out_lines)
        *out_lines = NULL;
    if (out_max_width)
        *out_max_width = 0.0f;
    if (!dlg || !text || !text[0] || !dlg->font)
        return 0;

    int cap = 4;
    int count = 0;
    char **lines = out_lines ? (char **)calloc((size_t)cap, sizeof(char *)) : NULL;
    if (out_lines && !lines)
        return 0;
    size_t text_len = strlen(text);
    size_t start = 0;

    while (start <= text_len) {
        if (text[start] == '\0') {
            if (count == 0 && out_lines && lines) {
                lines[count++] = vg_strdup("");
            }
            break;
        }
        if (text[start] == '\n') {
            if (out_lines && lines) {
                if (count >= cap) {
                    int new_cap = cap * 2;
                    char **new_lines = (char **)realloc(lines, (size_t)new_cap * sizeof(char *));
                    if (!new_lines) {
                        dialog_free_wrapped_lines(lines, count);
                        return 0;
                    }
                    memset(new_lines + cap, 0, (size_t)(new_cap - cap) * sizeof(char *));
                    lines = new_lines;
                    cap = new_cap;
                }
                lines[count] = vg_strdup("");
                if (!lines[count]) {
                    dialog_free_wrapped_lines(lines, count);
                    return 0;
                }
                count++;
            } else {
                count++;
            }
            start++;
            continue;
        }

        size_t line_end = start;
        size_t best_end = start;
        size_t best_next = start;
        float best_width = 0.0f;
        bool found_break = false;

        while (line_end < text_len && text[line_end] != '\n') {
            size_t candidate_end = line_end + 1;
            char *candidate = dialog_dup_range(text + start, candidate_end - start);
            if (!candidate)
                break;

            vg_text_metrics_t metrics = {0};
            vg_font_measure_text(dlg->font, dlg->font_size, candidate, &metrics);
            free(candidate);

            if (max_width > 0.0f && metrics.width > max_width) {
                break;
            }

            best_end = candidate_end;
            best_next = candidate_end;
            best_width = metrics.width;
            found_break = true;
            if (text[line_end] == ' ' || text[line_end] == '\t') {
                best_end = line_end;
                best_next = candidate_end;
            }
            line_end = candidate_end;
        }

        if (!found_break) {
            best_end = start + 1;
            best_next = best_end;
            char *candidate = dialog_dup_range(text + start, best_end - start);
            if (candidate) {
                vg_text_metrics_t metrics = {0};
                vg_font_measure_text(dlg->font, dlg->font_size, candidate, &metrics);
                best_width = metrics.width;
                free(candidate);
            }
        }

        while (best_end > start && (text[best_end - 1] == ' ' || text[best_end - 1] == '\t')) {
            best_end--;
        }

        if (out_lines && lines) {
            if (count >= cap) {
                int new_cap = cap * 2;
                char **new_lines = (char **)realloc(lines, (size_t)new_cap * sizeof(char *));
                if (!new_lines) {
                    dialog_free_wrapped_lines(lines, count);
                    return 0;
                }
                memset(new_lines + cap, 0, (size_t)(new_cap - cap) * sizeof(char *));
                lines = new_lines;
                cap = new_cap;
            }
            lines[count] = dialog_dup_range(text + start, best_end - start);
            if (!lines[count]) {
                dialog_free_wrapped_lines(lines, count);
                return 0;
            }
            count++;
        } else {
            count++;
        }

        if (out_max_width && best_width > *out_max_width)
            *out_max_width = best_width;

        size_t prev_start = start;
        start = best_next;
        while (text[start] == ' ' || text[start] == '\t')
            start++;
        if (start <= prev_start) {
            if (text[start] == '\0')
                break;
            start = prev_start + 1;
        }
        if (text[start] == '\n') {
            start++;
            if (text[start] == '\0' && out_lines && lines) {
                if (count >= cap) {
                    int new_cap = cap * 2;
                    char **new_lines = (char **)realloc(lines, (size_t)new_cap * sizeof(char *));
                    if (!new_lines) {
                        dialog_free_wrapped_lines(lines, count);
                        return 0;
                    }
                    memset(new_lines + cap, 0, (size_t)(new_cap - cap) * sizeof(char *));
                    lines = new_lines;
                    cap = new_cap;
                }
                lines[count] = vg_strdup("");
                if (!lines[count]) {
                    dialog_free_wrapped_lines(lines, count);
                    return 0;
                }
                count++;
            } else if (text[start] == '\0') {
                count++;
            }
        }
    }

    if (out_lines) {
        *out_lines = lines;
    } else if (lines) {
        dialog_free_wrapped_lines(lines, count);
    }
    return count > 0 ? count : 1;
}

/// @brief Measures the dialog's wrapped message block.
///
/// @param dlg Dialog supplying message and font properties.
/// @param max_width Maximum width passed to the wrapping algorithm.
/// @param[out] out_width Optional receiver for the widest line.
/// @param[out] out_height Optional receiver for total line-block height.
/// @param[out] out_line_height Optional receiver for one line's height.
/// @param[out] out_line_count Optional receiver for the wrapped line count.
static void dialog_measure_message_block(vg_dialog_t *dlg,
                                         float max_width,
                                         float *out_width,
                                         float *out_height,
                                         float *out_line_height,
                                         int *out_line_count) {
    if (out_width)
        *out_width = 0.0f;
    if (out_height)
        *out_height = 0.0f;
    if (out_line_height)
        *out_line_height = 0.0f;
    if (out_line_count)
        *out_line_count = 0;

    if (!dlg || !dlg->message || !dlg->message[0] || !dlg->font)
        return;

    vg_font_metrics_t font_metrics = {0};
    vg_font_get_metrics(dlg->font, dlg->font_size, &font_metrics);
    float line_height =
        font_metrics.line_height > 0 ? (float)font_metrics.line_height : dlg->font_size;
    float max_line_width = 0.0f;
    int line_count = dialog_wrap_text(dlg, dlg->message, max_width, NULL, &max_line_width);

    if (out_width)
        *out_width = max_line_width;
    if (out_height)
        *out_height = line_height * (float)line_count;
    if (out_line_height)
        *out_line_height = line_height;
    if (out_line_count)
        *out_line_count = line_count;
}

/// @brief Measures the complete button row including inter-button gaps.
///
/// @param dlg Dialog whose preset or custom button definitions are measured.
/// @return Total button-row width in framebuffer units.
static float dialog_measure_buttons_width(vg_dialog_t *dlg) {
    float total_width = 0.0f;
    float gap = dialog_section_gap();
    const preset_button_t *preset_buttons = NULL;
    size_t button_count = 0;
    get_preset_buttons(dlg->button_preset, &preset_buttons, &button_count);

    if (dlg->button_preset == VG_DIALOG_BUTTONS_CUSTOM && dlg->custom_buttons) {
        for (size_t i = 0; i < dlg->custom_button_count; i++) {
            total_width += get_button_width(dlg, dlg->custom_buttons[i].label);
            if (i + 1 < dlg->custom_button_count)
                total_width += gap;
        }
    } else if (preset_buttons) {
        for (size_t i = 0; i < button_count; i++) {
            total_width += get_button_width(dlg, preset_buttons[i].label);
            if (i + 1 < button_count)
                total_width += gap;
        }
    }

    return total_width;
}

/// @brief Computes the effective maximum width within the available viewport.
///
/// @param dlg Dialog supplying configured minimum and maximum widths.
/// @param available_width Available viewport width, or a non-positive value to
///                        use only configured limits.
/// @return Effective maximum width, never below the configured minimum.
static float dialog_available_width_limit(vg_dialog_t *dlg, float available_width) {
    float max_width = (float)dlg->max_width;
    if (available_width > 0.0f) {
        float clamped = available_width - (float)DIALOG_SCREEN_MARGIN * 2.0f;
        if (clamped > 0.0f && clamped < max_width) {
            max_width = clamped;
        }
    }
    if (max_width < (float)dlg->min_width) {
        max_width = (float)dlg->min_width;
    }
    return max_width;
}

/// @brief Computes the effective maximum height within the available viewport.
///
/// @param dlg Dialog supplying configured minimum and maximum heights.
/// @param available_height Available viewport height, or a non-positive value
///                         to use only configured limits.
/// @return Effective maximum height, never below the configured minimum.
static float dialog_available_height_limit(vg_dialog_t *dlg, float available_height) {
    float max_height = (float)dlg->max_height;
    if (available_height > 0.0f) {
        float clamped = available_height - (float)DIALOG_SCREEN_MARGIN * 2.0f;
        if (clamped > 0.0f && clamped < max_height) {
            max_height = clamped;
        }
    }
    if (max_height < (float)dlg->min_height) {
        max_height = (float)dlg->min_height;
    }
    return max_height;
}

/// @brief Resolves a preset identifier to immutable button definitions.
///
/// @param preset Button-set preset to resolve.
/// @param[out] buttons Receives a borrowed static array, or null for no preset.
/// @param[out] count Receives the number of definitions.
static void get_preset_buttons(vg_dialog_buttons_t preset,
                               const preset_button_t **buttons,
                               size_t *count) {
    switch (preset) {
        case VG_DIALOG_BUTTONS_OK:
            *buttons = g_ok_buttons;
            *count = 1;
            break;
        case VG_DIALOG_BUTTONS_OK_CANCEL:
            *buttons = g_ok_cancel_buttons;
            *count = 2;
            break;
        case VG_DIALOG_BUTTONS_YES_NO:
            *buttons = g_yes_no_buttons;
            *count = 2;
            break;
        case VG_DIALOG_BUTTONS_YES_NO_CANCEL:
            *buttons = g_yes_no_cancel_buttons;
            *count = 3;
            break;
        case VG_DIALOG_BUTTONS_RETRY_CANCEL:
            *buttons = g_retry_cancel_buttons;
            *count = 2;
            break;
        default:
            *buttons = NULL;
            *count = 0;
            break;
    }
}

/// @brief Measures a button label subject to the scaled minimum width.
///
/// @param dlg Dialog supplying font properties and padding scale.
/// @param label Button label to measure; may be null.
/// @return Button width in framebuffer units.
static float get_button_width(vg_dialog_t *dlg, const char *label) {
    float width = (float)DIALOG_BUTTON_MIN_WIDTH * dialog_ui_scale();
    if (dlg->font && label) {
        vg_text_metrics_t metrics;
        vg_font_measure_text(dlg->font, dlg->font_size, label, &metrics);
        width = metrics.width + dialog_outer_padding() * 1.4f;
        if (width < (float)DIALOG_BUTTON_MIN_WIDTH * dialog_ui_scale()) {
            width = (float)DIALOG_BUTTON_MIN_WIDTH * dialog_ui_scale();
        }
    }
    return width;
}

/// @brief Maps a standard dialog icon to its display glyph.
///
/// @param icon Standard dialog icon identifier.
/// @return Borrowed immutable UTF-8 glyph string, or null for no known icon.
static const char *get_icon_glyph(vg_dialog_icon_t icon) {
    switch (icon) {
        case VG_DIALOG_ICON_INFO:
            return "ℹ";
        case VG_DIALOG_ICON_WARNING:
            return "⚠";
        case VG_DIALOG_ICON_ERROR:
            return "✗";
        case VG_DIALOG_ICON_QUESTION:
            return "?";
        default:
            return NULL;
    }
}

/// @brief Resolves the screen-space origin of a widget's parent.
///
/// @param widget Widget whose parent is queried; may be null.
/// @param[out] x Optional receiver for the screen X coordinate.
/// @param[out] y Optional receiver for the screen Y coordinate.
static void get_parent_screen_origin(vg_widget_t *widget, float *x, float *y) {
    float sx = 0.0f;
    float sy = 0.0f;
    if (widget && widget->parent) {
        vg_widget_get_screen_bounds(widget->parent, &sx, &sy, NULL, NULL);
    }
    if (x)
        *x = sx;
    if (y)
        *y = sy;
}

//=============================================================================
// Dialog Implementation
//=============================================================================

/// @brief Creates a closed modal dialog with an OK button preset.
///
/// @details Theme colors and default size constraints are applied, but no font
/// is retained initially. Call `vg_dialog_show()` or
/// `vg_dialog_show_centered()` after configuring content and font.
///
/// @param title Title text to copy; may be null.
/// @return Newly allocated dialog, or null on allocation failure.
vg_dialog_t *vg_dialog_create(const char *title) {
    vg_dialog_t *dlg = calloc(1, sizeof(vg_dialog_t));
    if (!dlg)
        return NULL;

    // Initialize base widget
    vg_widget_init(&dlg->base, VG_WIDGET_DIALOG, &g_dialog_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Title bar
    dlg->title = title ? vg_strdup(title) : NULL;
    if (title && !dlg->title) {
        vg_widget_destroy(&dlg->base);
        return NULL;
    }
    dlg->show_close_button = true;
    dlg->draggable = true;

    // Content
    dlg->content = NULL;
    dlg->icon = VG_DIALOG_ICON_NONE;
    dlg->custom_icon.type = VG_ICON_NONE;
    dlg->message = NULL;

    // Buttons
    dlg->button_preset = VG_DIALOG_BUTTONS_OK;
    dlg->custom_buttons = NULL;
    dlg->custom_button_count = 0;

    // Sizing
    dlg->min_width = DIALOG_DEFAULT_MIN_WIDTH;
    dlg->min_height = DIALOG_DEFAULT_MIN_HEIGHT;
    dlg->max_width = DIALOG_DEFAULT_MAX_WIDTH;
    dlg->max_height = DIALOG_DEFAULT_MAX_HEIGHT;
    dlg->resizable = false;

    // Modal
    dlg->modal = true;
    dlg->modal_parent = NULL;

    // Font
    dlg->font = NULL;
    dlg->font_size = theme->typography.size_normal;
    dlg->title_font_size = theme->typography.size_large;

    // Colors
    dlg->bg_color = vg_color_blend(theme->colors.bg_primary, theme->colors.bg_secondary, 0.18f);
    dlg->title_bg_color =
        vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_tertiary, 0.42f);
    dlg->title_text_color = theme->colors.fg_primary;
    dlg->text_color = theme->colors.fg_primary;
    dlg->button_bg_color =
        vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.32f);
    dlg->button_hover_color = theme->colors.bg_hover;
    dlg->overlay_color = vg_color_darken(theme->colors.bg_primary, 0.75f);

    // State
    dlg->result = VG_DIALOG_RESULT_NONE;
    dlg->is_open = false;
    dlg->is_dragging = false;
    dlg->drag_offset_x = 0;
    dlg->drag_offset_y = 0;
    dlg->hovered_button = -1;
    dlg->closing_in_progress = false;

    // Callbacks
    dlg->user_data = NULL;
    dlg->on_result_user_data = NULL;
    dlg->on_close_user_data = NULL;
    dlg->on_result = NULL;
    dlg->on_close = NULL;

    // Set size constraints
    dlg->base.constraints.min_width = (float)dlg->min_width;
    dlg->base.constraints.min_height = (float)dlg->min_height;

    return dlg;
}

/// @brief Releases dialog-specific resources during widget destruction.
///
/// @details Input capture and modal-root ownership are released before owned
/// strings, icon resources, and custom button definitions are destroyed.
///
/// @param widget Dialog base widget being destroyed.
static void dialog_destroy(vg_widget_t *widget) {
    vg_dialog_t *dlg = (vg_dialog_t *)widget;

    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    if (vg_widget_get_modal_root() == widget)
        vg_widget_set_modal_root(NULL);

    free(dlg->title);
    free(dlg->message);
    vg_icon_destroy(&dlg->custom_icon);

    if (dlg->custom_buttons) {
        for (size_t i = 0; i < dlg->custom_button_count; i++) {
            free(dlg->custom_buttons[i].label);
        }
        free(dlg->custom_buttons);
    }
}

/// @brief Measures all dialog sections within available limits.
///
/// @details Measurement combines fixed title and button bars with wrapped
/// message, optional icon, and measured embedded content. Button width can
/// widen the body, and configured or viewport-derived constraints clamp the
/// final result.
///
/// @param widget Dialog base widget whose measured size is updated.
/// @param available_width Available viewport width.
/// @param available_height Available viewport height.
static void dialog_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_dialog_t *dlg = (vg_dialog_t *)widget;
    float title_h = dialog_title_height();
    float button_bar_h = dialog_button_bar_height();
    float padding = dialog_outer_padding();
    float gap = dialog_section_gap();
    float icon_size = dialog_icon_size();
    bool has_icon = dlg->icon != VG_DIALOG_ICON_NONE || dlg->custom_icon.type != VG_ICON_NONE;

    float max_width = dialog_available_width_limit(dlg, available_width);
    float max_height = dialog_available_height_limit(dlg, available_height);

    float body_width_limit = max_width - padding * 2.0f;
    if (has_icon)
        body_width_limit -= icon_size + gap;
    if (body_width_limit < 80.0f)
        body_width_limit = 80.0f;

    float message_width = 0.0f;
    float message_height = 0.0f;
    dialog_measure_message_block(
        dlg, body_width_limit, &message_width, &message_height, NULL, NULL);

    float content_width = 0.0f;
    float content_height = 0.0f;
    if (dlg->content) {
        float content_max_height = max_height - title_h - button_bar_h - padding * 2.0f;
        if (message_height > 0.0f)
            content_max_height -= message_height + gap;
        if (content_max_height < 40.0f)
            content_max_height = 40.0f;
        vg_widget_measure(dlg->content, body_width_limit, content_max_height);
        content_width = dlg->content->measured_width;
        content_height = dlg->content->measured_height;
    }

    float body_content_width = message_width;
    if (content_width > body_content_width)
        body_content_width = content_width;
    if (body_content_width < 160.0f * dialog_ui_scale())
        body_content_width = 160.0f * dialog_ui_scale();

    float buttons_width = dialog_measure_buttons_width(dlg);
    float total_width = padding * 2.0f + body_content_width;
    if (has_icon)
        total_width += icon_size + gap;
    if (buttons_width > 0.0f && buttons_width + padding * 2.0f > total_width)
        total_width = buttons_width + padding * 2.0f;

    float body_height = 0.0f;
    if (message_height > 0.0f)
        body_height += message_height;
    if (dlg->content && content_height > 0.0f) {
        if (body_height > 0.0f)
            body_height += gap;
        body_height += content_height;
    }
    if (has_icon && icon_size > body_height)
        body_height = icon_size;

    float total_height = title_h + button_bar_h + padding * 2.0f + body_height;

    if (total_width < (float)dlg->min_width)
        total_width = (float)dlg->min_width;
    if (total_width > max_width)
        total_width = max_width;
    if (total_height < (float)dlg->min_height)
        total_height = (float)dlg->min_height;
    if (total_height > max_height)
        total_height = max_height;

    widget->measured_width = total_width;
    widget->measured_height = total_height;
}

/// @brief Arranges the dialog and its embedded content.
///
/// @details Content is placed below the wrapped message and beside any icon.
/// When its measured height exceeds the available body region, it is arranged
/// at full height with a clamped scroll offset for paint-time clipping.
///
/// @param widget Dialog base widget to arrange.
/// @param x Allocated left coordinate.
/// @param y Allocated top coordinate.
/// @param width Allocated width.
/// @param height Allocated height.
static void dialog_arrange(vg_widget_t *widget, float x, float y, float width, float height) {
    vg_dialog_t *dlg = (vg_dialog_t *)widget;
    float title_h = dialog_title_height();
    float button_bar_h = dialog_button_bar_height();
    float padding = dialog_outer_padding();
    float gap = dialog_section_gap();
    float icon_size = dialog_icon_size();
    bool has_icon = dlg->icon != VG_DIALOG_ICON_NONE || dlg->custom_icon.type != VG_ICON_NONE;

    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;

    if (dlg->content) {
        float body_x = x + padding;
        float body_y = y + title_h + padding;
        float body_w = width - padding * 2.0f;
        float body_h = height - title_h - button_bar_h - padding * 2.0f;
        if (has_icon) {
            body_x += icon_size + gap;
            body_w -= icon_size + gap;
        }
        if (body_w < 0.0f)
            body_w = 0.0f;
        if (body_h < 0.0f)
            body_h = 0.0f;

        float message_height = 0.0f;
        dialog_measure_message_block(dlg, body_w, NULL, &message_height, NULL, NULL);
        float content_y = body_y;
        if (message_height > 0.0f) {
            content_y += message_height + gap;
        }

        float remaining_h = body_y + body_h - content_y;
        if (remaining_h < 0.0f)
            remaining_h = 0.0f;
        float full_h =
            dlg->content->measured_height > 0.0f ? dlg->content->measured_height : remaining_h;
        if (full_h <= remaining_h) {
            // Fits: no scrolling.
            dlg->content_scroll_y = 0.0f;
            vg_widget_arrange(dlg->content, body_x, content_y, body_w, full_h);
        } else {
            // Overflows: arrange at full height and offset by the scroll amount;
            // the paint pass clips the content to the body region.
            float max_scroll = full_h - remaining_h;
            if (dlg->content_scroll_y < 0.0f)
                dlg->content_scroll_y = 0.0f;
            if (dlg->content_scroll_y > max_scroll)
                dlg->content_scroll_y = max_scroll;
            vg_widget_arrange(
                dlg->content, body_x, content_y - dlg->content_scroll_y, body_w, full_h);
        }
    }
}

/// @brief Paints the complete open dialog and its embedded content.
///
/// @details The pass draws an optional modal backdrop, elevated panel, title
/// bar, close control, standard or custom icon, wrapped message, clipped child
/// content, and preset or custom buttons. Closed dialogs do not paint.
///
/// @param widget Dialog base widget to render.
/// @param canvas Destination drawing context.
static void dialog_paint(vg_widget_t *widget, void *canvas) {
    vg_dialog_t *dlg = (vg_dialog_t *)widget;

    if (!dlg->is_open)
        return;

    vgfx_window_t win = (vgfx_window_t)canvas;
    vg_theme_t *theme = vg_theme_get_current();
    float padding = dialog_outer_padding();
    float gap = dialog_section_gap();
    float title_h = dialog_title_height();
    float button_bar_h = dialog_button_bar_height();
    float button_h = dialog_button_height();
    float close_size = dialog_close_size();
    float icon_size = dialog_icon_size();
    bool has_icon = dlg->icon != VG_DIALOG_ICON_NONE || dlg->custom_icon.type != VG_ICON_NONE;
    int radius = dialog_corner_radius();

    // Draw full-screen dim overlay so the dialog floats above all other widgets
    if (dlg->modal && dlg->overlay_color) {
        int32_t win_w = 0, win_h = 0;
        if (vgfx_get_size(win, &win_w, &win_h)) {
            vgfx_fill_rect(win, 0, 0, win_w, win_h, dlg->overlay_color);
        }
    }

    int32_t x = (int32_t)widget->x;
    int32_t y = (int32_t)widget->y;
    int32_t w = (int32_t)widget->width;
    int32_t h = (int32_t)widget->height;

    uint32_t panel_bg = dlg->bg_color ? dlg->bg_color : theme->colors.bg_primary;
    uint32_t header_bg = dlg->title_bg_color
                             ? dlg->title_bg_color
                             : vg_color_blend(panel_bg, theme->colors.bg_secondary, 0.65f);
    uint32_t border_color = theme->colors.border_primary;
    uint32_t highlight = vg_color_lighten(panel_bg, 0.06f);

    // Real soft drop shadow (floating elevation) beneath the dialog panel.
    vg_elevation_t el = theme->elevation.level3;
    vg_draw_round_rect_shadow(win,
                              (float)x,
                              (float)y,
                              (float)w,
                              (float)h,
                              (float)radius,
                              el.blur,
                              el.dx,
                              el.dy,
                              el.alpha,
                              theme->elevation.shadow_rgb);
    dialog_fill_round_rect(win, x, y, w, h, radius, panel_bg);
    vgfx_fill_rect(win, x + 1, y + 1, w - 2, (int32_t)title_h, header_bg);
    vgfx_fill_rect(win, x + 1, y + 1, w - 2, 1, highlight);
    vgfx_fill_rect(win, x + 1, y + (int32_t)title_h - 1, w - 2, 1, border_color);
    dialog_stroke_round_rect(win, x, y, w, h, radius, border_color);

    if (dlg->title && dlg->font) {
        vg_font_metrics_t title_metrics = {0};
        vg_font_get_metrics(dlg->font, dlg->title_font_size, &title_metrics);
        float title_x = (float)x + padding;
        float title_y = (float)y +
                        (title_h - (float)(title_metrics.ascent - title_metrics.descent)) * 0.5f +
                        (float)title_metrics.ascent;
        int32_t title_clip_w = (int32_t)((float)w - padding * 2.0f - close_size - gap);
        if (title_clip_w < 0)
            title_clip_w = 0;
        vgfx_set_clip(win, (int32_t)title_x, y + 1, title_clip_w, (int32_t)title_h - 2);
        vg_font_draw_text(canvas,
                          dlg->font,
                          dlg->title_font_size,
                          title_x,
                          title_y,
                          dlg->title,
                          dlg->title_text_color);
        vgfx_clear_clip(win);
    }

    if (dlg->show_close_button && dlg->font) {
        int32_t close_x = x + w - (int32_t)padding - (int32_t)close_size;
        int32_t close_y = y + (int32_t)((title_h - close_size) * 0.5f);
        dialog_fill_round_rect(win,
                               close_x,
                               close_y,
                               (int32_t)close_size,
                               (int32_t)close_size,
                               radius / 2,
                               vg_color_blend(header_bg, theme->colors.bg_hover, 0.45f));
        dialog_stroke_round_rect(win,
                                 close_x,
                                 close_y,
                                 (int32_t)close_size,
                                 (int32_t)close_size,
                                 radius / 2,
                                 border_color);
        vg_font_metrics_t close_metrics = {0};
        vg_font_get_metrics(dlg->font, dlg->font_size, &close_metrics);
        float close_cx = (float)close_x + close_size * 0.5f;
        float close_cy =
            (float)close_y +
            (close_size - (float)(close_metrics.ascent - close_metrics.descent)) * 0.5f +
            (float)close_metrics.ascent;
        vg_font_draw_text(canvas,
                          dlg->font,
                          dlg->font_size,
                          close_cx - dlg->font_size * 0.3f,
                          close_cy,
                          "X",
                          dlg->title_text_color);
    }

    float body_x = (float)x + padding;
    float body_y = (float)y + title_h + padding;
    float body_w = (float)w - padding * 2.0f;
    float body_h = (float)h - title_h - button_bar_h - padding * 2.0f;

    if (has_icon) {
        dialog_fill_round_rect(win,
                               (int32_t)body_x,
                               (int32_t)body_y,
                               (int32_t)icon_size,
                               (int32_t)icon_size,
                               radius / 2,
                               vg_color_blend(panel_bg, theme->colors.accent_primary, 0.22f));
        dialog_stroke_round_rect(win,
                                 (int32_t)body_x,
                                 (int32_t)body_y,
                                 (int32_t)icon_size,
                                 (int32_t)icon_size,
                                 radius / 2,
                                 vg_color_blend(border_color, theme->colors.accent_primary, 0.35f));
    }

    float content_x = body_x;
    float content_y = body_y;
    if (has_icon) {
        content_x += icon_size + gap;
        body_w -= icon_size + gap;
    }
    if (body_w < 0.0f)
        body_w = 0.0f;
    if (body_h < 0.0f)
        body_h = 0.0f;

    if (dlg->icon != VG_DIALOG_ICON_NONE) {
        const char *glyph = get_icon_glyph(dlg->icon);
        if (glyph && dlg->font) {
            vg_font_metrics_t icon_metrics = {0};
            float icon_font_size = icon_size * 0.75f;
            vg_font_get_metrics(dlg->font, icon_font_size, &icon_metrics);
            vg_font_draw_text(
                canvas,
                dlg->font,
                icon_font_size,
                body_x + icon_size * 0.28f,
                body_y + (icon_size - (float)(icon_metrics.ascent - icon_metrics.descent)) * 0.5f +
                    (float)icon_metrics.ascent,
                glyph,
                theme->colors.accent_primary);
        }
    }

    if (dlg->message && dlg->font) {
        char **lines = NULL;
        float ignored_width = 0.0f;
        int line_count = dialog_wrap_text(dlg, dlg->message, body_w, &lines, &ignored_width);
        vg_font_metrics_t font_metrics = {0};
        vg_font_get_metrics(dlg->font, dlg->font_size, &font_metrics);
        float line_height =
            font_metrics.line_height > 0 ? (float)font_metrics.line_height : dlg->font_size;
        float text_y = content_y + (float)font_metrics.ascent;

        vgfx_set_clip(
            win, (int32_t)content_x, (int32_t)content_y, (int32_t)body_w, (int32_t)body_h);
        if (line_count <= 0 || !lines) {
            vg_font_draw_text(canvas,
                              dlg->font,
                              dlg->font_size,
                              content_x,
                              text_y,
                              dlg->message,
                              dlg->text_color);
            line_count = 1;
        } else {
            for (int i = 0; i < line_count; i++) {
                const char *line = lines[i] ? lines[i] : "";
                vg_font_draw_text(canvas,
                                  dlg->font,
                                  dlg->font_size,
                                  content_x,
                                  text_y + (float)i * line_height,
                                  line,
                                  dlg->text_color);
            }
        }
        vgfx_clear_clip(win);
        dialog_free_wrapped_lines(lines, line_count);
        content_y += line_height * (float)line_count;
        if (dlg->content)
            content_y += gap;
    }

    if (dlg->content) {
        float clip_h = body_y + body_h - content_y;
        if (clip_h < 0.0f)
            clip_h = 0.0f;
        vgfx_set_clip(
            win, (int32_t)content_x, (int32_t)content_y, (int32_t)body_w, (int32_t)clip_h);
        vg_widget_paint(dlg->content, canvas);
        vgfx_clear_clip(win);
    }

    int32_t btn_bar_y = y + h - (int32_t)button_bar_h;
    vgfx_fill_rect(win, x + 1, btn_bar_y, w - 2, (int32_t)button_bar_h - 1, dlg->title_bg_color);
    vgfx_fill_rect(win, x + 1, btn_bar_y, w - 2, 1, border_color);

    float button_y = (float)btn_bar_y + (button_bar_h - button_h) * 0.5f;
    float button_x = (float)x + (float)w - padding;

    const preset_button_t *buttons;
    size_t button_count;
    get_preset_buttons(dlg->button_preset, &buttons, &button_count);

    if (dlg->button_preset == VG_DIALOG_BUTTONS_CUSTOM && dlg->custom_buttons) {
        for (int i = (int)dlg->custom_button_count - 1; i >= 0; i--) {
            vg_dialog_button_def_t *btn = &dlg->custom_buttons[i];
            float btn_w = get_button_width(dlg, btn->label);
            button_x -= btn_w;
            bool is_default = btn->is_default;
            uint32_t btn_bg = is_default ? theme->colors.accent_primary : dlg->button_bg_color;
            if (dlg->hovered_button == i) {
                btn_bg = vg_color_lighten(btn_bg, is_default ? 0.08f : 0.05f);
            }
            uint32_t btn_border =
                is_default
                    ? vg_color_blend(theme->colors.accent_primary, theme->colors.border_focus, 0.4f)
                    : theme->colors.border_primary;
            uint32_t btn_fg = is_default ? 0x00FFFFFF : dlg->text_color;

            dialog_fill_round_rect(win,
                                   (int32_t)button_x,
                                   (int32_t)button_y,
                                   (int32_t)btn_w,
                                   (int32_t)button_h,
                                   radius / 2,
                                   btn_bg);
            dialog_stroke_round_rect(win,
                                     (int32_t)button_x,
                                     (int32_t)button_y,
                                     (int32_t)btn_w,
                                     (int32_t)button_h,
                                     radius / 2,
                                     btn_border);

            if (btn->label && dlg->font) {
                vg_text_metrics_t metrics;
                vg_font_measure_text(dlg->font, dlg->font_size, btn->label, &metrics);
                float text_x = button_x + (btn_w - metrics.width) / 2.0f;
                vg_font_metrics_t button_metrics = {0};
                vg_font_get_metrics(dlg->font, dlg->font_size, &button_metrics);
                float text_y =
                    button_y +
                    (button_h - (float)(button_metrics.ascent - button_metrics.descent)) * 0.5f +
                    (float)button_metrics.ascent;
                vgfx_set_clip(
                    win, (int32_t)button_x, (int32_t)button_y, (int32_t)btn_w, (int32_t)button_h);
                vg_font_draw_text(
                    canvas, dlg->font, dlg->font_size, text_x, text_y, btn->label, btn_fg);
                vgfx_clear_clip(win);
            }

            button_x -= gap;
        }
    } else if (buttons) {
        for (int i = (int)button_count - 1; i >= 0; i--) {
            const preset_button_t *btn = &buttons[i];
            float btn_w = get_button_width(dlg, btn->label);
            button_x -= btn_w;
            bool is_default = btn->is_default;
            uint32_t btn_bg = is_default ? theme->colors.accent_primary : dlg->button_bg_color;
            if (dlg->hovered_button == i) {
                btn_bg = vg_color_lighten(btn_bg, is_default ? 0.08f : 0.05f);
            }
            uint32_t btn_border =
                is_default
                    ? vg_color_blend(theme->colors.accent_primary, theme->colors.border_focus, 0.4f)
                    : theme->colors.border_primary;
            uint32_t btn_fg = is_default ? 0x00FFFFFF : dlg->text_color;

            dialog_fill_round_rect(win,
                                   (int32_t)button_x,
                                   (int32_t)button_y,
                                   (int32_t)btn_w,
                                   (int32_t)button_h,
                                   radius / 2,
                                   btn_bg);
            dialog_stroke_round_rect(win,
                                     (int32_t)button_x,
                                     (int32_t)button_y,
                                     (int32_t)btn_w,
                                     (int32_t)button_h,
                                     radius / 2,
                                     btn_border);

            if (btn->label && dlg->font) {
                vg_text_metrics_t metrics;
                vg_font_measure_text(dlg->font, dlg->font_size, btn->label, &metrics);
                float text_x = button_x + (btn_w - metrics.width) / 2.0f;
                vg_font_metrics_t button_metrics = {0};
                vg_font_get_metrics(dlg->font, dlg->font_size, &button_metrics);
                float text_y =
                    button_y +
                    (button_h - (float)(button_metrics.ascent - button_metrics.descent)) * 0.5f +
                    (float)button_metrics.ascent;
                vgfx_set_clip(
                    win, (int32_t)button_x, (int32_t)button_y, (int32_t)btn_w, (int32_t)button_h);
                vg_font_draw_text(
                    canvas, dlg->font, dlg->font_size, text_x, text_y, btn->label, btn_fg);
                vgfx_clear_clip(win);
            }

            button_x -= gap;
        }
    }
}

/// @brief Finds the dialog button under local coordinates.
///
/// @param dlg Dialog whose preset or custom button row is hit-tested.
/// @param px Horizontal coordinate relative to the dialog.
/// @param py Vertical coordinate relative to the dialog.
/// @return Zero-based button index, or `-1` outside all buttons.
static int find_button_at(vg_dialog_t *dlg, float px, float py) {
    float w = dlg->base.width;
    float h = dlg->base.height;
    float button_bar_h = dialog_button_bar_height();
    float button_h = dialog_button_height();
    float padding = dialog_outer_padding();
    float gap = dialog_section_gap();

    float button_bar_y = h - button_bar_h;
    if (py < button_bar_y || py > h)
        return -1;

    float button_y = button_bar_y + (button_bar_h - button_h) / 2.0f;
    float button_x = w - padding;

    const preset_button_t *buttons;
    size_t button_count;
    get_preset_buttons(dlg->button_preset, &buttons, &button_count);

    if (dlg->button_preset == VG_DIALOG_BUTTONS_CUSTOM && dlg->custom_buttons) {
        for (int i = (int)dlg->custom_button_count - 1; i >= 0; i--) {
            float btn_w = get_button_width(dlg, dlg->custom_buttons[i].label);
            button_x -= btn_w;

            if (px >= button_x && px < button_x + btn_w && py >= button_y &&
                py < button_y + button_h) {
                return i;
            }

            button_x -= gap;
        }
    } else if (buttons) {
        for (int i = (int)button_count - 1; i >= 0; i--) {
            float btn_w = get_button_width(dlg, buttons[i].label);
            button_x -= btn_w;

            if (px >= button_x && px < button_x + btn_w && py >= button_y &&
                py < button_y + button_h) {
                return i;
            }

            button_x -= gap;
        }
    }

    return -1;
}

/// @brief Tests whether local coordinates fall within the title bar.
///
/// @param dlg Dialog whose title rectangle is tested.
/// @param px Horizontal local coordinate.
/// @param py Vertical local coordinate.
/// @return `true` inside the title bar; otherwise `false`.
static bool is_in_title_bar(vg_dialog_t *dlg, float px, float py) {
    return px >= 0.0f && px < dlg->base.width && py >= 0.0f && py < dialog_title_height();
}

/// @brief Tests whether local coordinates fall within the close button.
///
/// @param dlg Dialog whose close control is tested.
/// @param px Horizontal local coordinate.
/// @param py Vertical local coordinate.
/// @return `true` inside an enabled close control; otherwise `false`.
static bool is_on_close_button(vg_dialog_t *dlg, float px, float py) {
    if (!dlg->show_close_button)
        return false;

    float size = dialog_close_size();
    float pad = dialog_outer_padding();
    float x = dlg->base.width - pad - size;
    float y = (dialog_title_height() - size) / 2.0f;

    return px >= x && px < x + size && py >= y && py < y + size;
}

/// @brief Resolves and activates a dialog button by index.
///
/// @param dlg Dialog that owns the preset or custom button.
/// @param button_index Zero-based button index to activate.
static void trigger_button_click(vg_dialog_t *dlg, int button_index) {
    vg_dialog_result_t result = VG_DIALOG_RESULT_NONE;

    if (dlg->button_preset == VG_DIALOG_BUTTONS_CUSTOM && dlg->custom_buttons) {
        if (button_index >= 0 && button_index < (int)dlg->custom_button_count) {
            result = dlg->custom_buttons[button_index].result;
        }
    } else {
        const preset_button_t *buttons;
        size_t button_count;
        get_preset_buttons(dlg->button_preset, &buttons, &button_count);
        if (buttons && button_index >= 0 && button_index < (int)button_count) {
            result = buttons[button_index].result;
        }
    }

    if (result != VG_DIALOG_RESULT_NONE) {
        vg_dialog_close(dlg, result);
    }
}

/// @brief Handles dialog dragging, buttons, scrolling, and keyboard dismissal.
///
/// @details Pointer input updates button hover, activates close and dialog
/// buttons, drags enabled title bars, and scrolls overflowing embedded content.
/// Enter selects the default button and Escape selects a cancel button or closes
/// with a cancel result. Unhandled input is consumed by modal dialogs.
///
/// @param widget Dialog base widget receiving the event.
/// @param event Event to inspect.
/// @return `true` when the dialog handles the event or modal policy blocks it.
static bool dialog_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_dialog_t *dlg = (vg_dialog_t *)widget;

    if (!dlg->is_open)
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_WHEEL: {
            // Scroll the content area when it is taller than the body region.
            if (dlg->content) {
                dlg->content_scroll_y -= event->wheel.delta_y * 24.0f;
                widget->needs_layout = true;
                widget->needs_paint = true;
                return true;
            }
            return false;
        }
        case VG_EVENT_MOUSE_MOVE: {
            float px = event->mouse.x;
            float py = event->mouse.y;

            // Handle dragging
            if (dlg->is_dragging) {
                float parent_sx = 0.0f;
                float parent_sy = 0.0f;
                get_parent_screen_origin(widget, &parent_sx, &parent_sy);
                widget->x = event->mouse.screen_x - parent_sx - (float)dlg->drag_offset_x;
                widget->y = event->mouse.screen_y - parent_sy - (float)dlg->drag_offset_y;
                if (dlg->modal_parent) {
                    float max_x =
                        dlg->modal_parent->width - widget->width - (float)DIALOG_SCREEN_MARGIN;
                    float max_y = dlg->modal_parent->height - dialog_title_height() -
                                  (float)DIALOG_SCREEN_MARGIN;
                    float min_x = (float)DIALOG_SCREEN_MARGIN;
                    float min_y = (float)DIALOG_SCREEN_MARGIN;
                    if (widget->x < min_x)
                        widget->x = min_x;
                    if (widget->y < min_y)
                        widget->y = min_y;
                    if (widget->x > max_x)
                        widget->x = max_x;
                    if (widget->y > max_y)
                        widget->y = max_y;
                }
                widget->needs_paint = true;
                widget->needs_layout = true;
                return true;
            }

            // Update hovered button
            int new_hovered = find_button_at(dlg, px, py);
            if (new_hovered != dlg->hovered_button) {
                dlg->hovered_button = new_hovered;
                widget->needs_paint = true;
            }

            return dlg->modal; // Consume if modal
        }

        case VG_EVENT_MOUSE_DOWN: {
            float px = event->mouse.x;
            float py = event->mouse.y;

            // Check close button
            if (is_on_close_button(dlg, px, py)) {
                vg_dialog_close(dlg, VG_DIALOG_RESULT_CANCEL);
                return true;
            }

            // Check button click
            int button = find_button_at(dlg, px, py);
            if (button >= 0) {
                trigger_button_click(dlg, button);
                return true;
            }

            // Start dragging
            if (dlg->draggable && is_in_title_bar(dlg, px, py)) {
                dlg->is_dragging = true;
                float widget_sx = 0.0f;
                float widget_sy = 0.0f;
                vg_widget_get_screen_bounds(widget, &widget_sx, &widget_sy, NULL, NULL);
                dlg->drag_offset_x = (int)(event->mouse.screen_x - widget_sx);
                dlg->drag_offset_y = (int)(event->mouse.screen_y - widget_sy);
                vg_widget_set_input_capture(widget);
                return true;
            }

            return dlg->modal;
        }

        case VG_EVENT_MOUSE_UP:
            dlg->is_dragging = false;
            if (vg_widget_get_input_capture() == widget)
                vg_widget_release_input_capture();
            return dlg->modal;

        case VG_EVENT_KEY_DOWN:
            // Handle Enter (default button) and Escape (cancel button)
            if (event->key.key == VG_KEY_ENTER) {
                // Find default button
                if (dlg->button_preset == VG_DIALOG_BUTTONS_CUSTOM && dlg->custom_buttons) {
                    for (size_t i = 0; i < dlg->custom_button_count; i++) {
                        if (dlg->custom_buttons[i].is_default) {
                            trigger_button_click(dlg, (int)i);
                            return true;
                        }
                    }
                } else {
                    const preset_button_t *buttons;
                    size_t button_count;
                    get_preset_buttons(dlg->button_preset, &buttons, &button_count);
                    if (buttons) {
                        for (size_t i = 0; i < button_count; i++) {
                            if (buttons[i].is_default) {
                                trigger_button_click(dlg, (int)i);
                                return true;
                            }
                        }
                    }
                }
            } else if (event->key.key == VG_KEY_ESCAPE) {
                // Find cancel button
                if (dlg->button_preset == VG_DIALOG_BUTTONS_CUSTOM && dlg->custom_buttons) {
                    for (size_t i = 0; i < dlg->custom_button_count; i++) {
                        if (dlg->custom_buttons[i].is_cancel) {
                            trigger_button_click(dlg, (int)i);
                            return true;
                        }
                    }
                } else {
                    const preset_button_t *buttons;
                    size_t button_count;
                    get_preset_buttons(dlg->button_preset, &buttons, &button_count);
                    if (buttons) {
                        for (size_t i = 0; i < button_count; i++) {
                            if (buttons[i].is_cancel) {
                                trigger_button_click(dlg, (int)i);
                                return true;
                            }
                        }
                    }
                }
                // If no cancel button, just close
                vg_dialog_close(dlg, VG_DIALOG_RESULT_CANCEL);
                return true;
            }
            return dlg->modal;

        default:
            return dlg->modal;
    }
}

//=============================================================================
// Dialog API
//=============================================================================

/// @brief Replace the dialog's title bar text.
///
/// @param dialog The dialog to update; may be NULL.
/// @param title  New title text; copied internally.  NULL removes the title.
void vg_dialog_set_title(vg_dialog_t *dialog, const char *title) {
    if (!dialog)
        return;
    char *copy = title ? vg_strdup(title) : NULL;
    if (title && !copy)
        return;
    free(dialog->title);
    dialog->title = copy;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Set an optional custom widget displayed in the dialog body below the message.
///
/// @details The dialog adopts content as a child widget.  Passing a new content
///          widget removes the previous one from the child list first.
///
/// @param dialog  The dialog to update; may be NULL.
/// @param content Custom widget to embed in the body; may be NULL to remove.
void vg_dialog_set_content(vg_dialog_t *dialog, vg_widget_t *content) {
    if (!dialog)
        return;
    if (dialog->content == content)
        return;
    if (dialog->content && dialog->content->parent == &dialog->base) {
        vg_widget_remove_child(&dialog->base, dialog->content);
    }
    dialog->content = content;
    if (content) {
        vg_widget_add_child(&dialog->base, content);
    }
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Set the word-wrapped message text shown in the dialog body.
///
/// @param dialog  The dialog to update; may be NULL.
/// @param message Message text; copied internally.  NULL removes the message.
void vg_dialog_set_message(vg_dialog_t *dialog, const char *message) {
    if (!dialog)
        return;
    char *copy = message ? vg_strdup(message) : NULL;
    if (message && !copy)
        return;
    free(dialog->message);
    dialog->message = copy;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Set the standard icon badge shown to the left of the message.
///
/// @param dialog The dialog to update; may be NULL.
/// @param icon   One of VG_DIALOG_ICON_INFO/WARNING/ERROR/QUESTION/NONE.
void vg_dialog_set_icon(vg_dialog_t *dialog, vg_dialog_icon_t icon) {
    if (!dialog)
        return;
    dialog->icon = icon;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Set a custom vg_icon_t badge, overriding the standard icon preset.
///
/// @param dialog The dialog to update; may be NULL.
/// @param icon   Custom icon value; the dialog takes ownership via vg_icon_destroy.
void vg_dialog_set_custom_icon(vg_dialog_t *dialog, vg_icon_t icon) {
    if (!dialog)
        return;
    vg_icon_destroy(&dialog->custom_icon);
    dialog->custom_icon = icon;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Select a preset button set (OK, OK/Cancel, Yes/No, etc.).
///
/// @param dialog  The dialog to update; may be NULL.
/// @param buttons Preset identifier; VG_DIALOG_BUTTONS_CUSTOM is set implicitly by
///                vg_dialog_set_custom_buttons.
void vg_dialog_set_buttons(vg_dialog_t *dialog, vg_dialog_buttons_t buttons) {
    if (!dialog)
        return;
    dialog->button_preset = buttons;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Replace the button set with fully custom button definitions.
///
/// @details Copies the provided definitions and sets button_preset to
///          VG_DIALOG_BUTTONS_CUSTOM.  Labels are strdup'd.
///
/// @param dialog  The dialog to update; may be NULL.
/// @param buttons Array of custom button definitions to copy.
/// @param count   Number of entries in buttons.
void vg_dialog_set_custom_buttons(vg_dialog_t *dialog,
                                  vg_dialog_button_def_t *buttons,
                                  size_t count) {
    if (!dialog)
        return;

    if (count > 0 && !buttons)
        return;
    if (count > SIZE_MAX / sizeof(vg_dialog_button_def_t))
        return;

    vg_dialog_button_def_t *new_buttons = NULL;
    if (count > 0) {
        new_buttons = calloc(count, sizeof(vg_dialog_button_def_t));
        if (!new_buttons)
            return;
        for (size_t i = 0; i < count; i++) {
            if (buttons[i].label) {
                new_buttons[i].label = vg_strdup(buttons[i].label);
                if (!new_buttons[i].label) {
                    for (size_t j = 0; j < i; j++)
                        free(new_buttons[j].label);
                    free(new_buttons);
                    return;
                }
            }
            new_buttons[i].result = buttons[i].result;
            new_buttons[i].is_default = buttons[i].is_default;
            new_buttons[i].is_cancel = buttons[i].is_cancel;
        }
    }

    // Free existing custom buttons
    if (dialog->custom_buttons) {
        for (size_t i = 0; i < dialog->custom_button_count; i++) {
            free(dialog->custom_buttons[i].label);
        }
        free(dialog->custom_buttons);
    }

    dialog->custom_buttons = new_buttons;
    dialog->custom_button_count = count;
    dialog->button_preset = count > 0 ? VG_DIALOG_BUTTONS_CUSTOM : VG_DIALOG_BUTTONS_OK;

    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Enable or disable user resizing of the dialog window.
///
/// @param dialog    The dialog to configure; may be NULL.
/// @param resizable true to allow drag-resize; false (default) for fixed size.
void vg_dialog_set_resizable(vg_dialog_t *dialog, bool resizable) {
    if (!dialog)
        return;
    dialog->resizable = resizable;
    dialog->base.needs_paint = true;
}

/// @brief Set the minimum and maximum pixel size of the dialog.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param min_w  Minimum width in logical pixels.
/// @param min_h  Minimum height in logical pixels.
/// @param max_w  Maximum width in logical pixels.
/// @param max_h  Maximum height in logical pixels.
void vg_dialog_set_size_constraints(
    vg_dialog_t *dialog, uint32_t min_w, uint32_t min_h, uint32_t max_w, uint32_t max_h) {
    if (!dialog)
        return;
    dialog->min_width = min_w;
    dialog->min_height = min_h;
    dialog->max_width = max_w;
    dialog->max_height = max_h;
    dialog->base.constraints.min_width = (float)min_w;
    dialog->base.constraints.min_height = (float)min_h;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

/// @brief Configure whether the dialog is modal and which widget it is modal over.
///
/// @details When modal is true the dialog blocks input to all other widgets by
///          registering itself as the modal root on vg_dialog_show.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param modal  true for modal (default); false for non-modal.
/// @param parent Widget to use as the clamp region for centering; may be NULL.
void vg_dialog_set_modal(vg_dialog_t *dialog, bool modal, vg_widget_t *parent) {
    if (!dialog)
        return;
    dialog->modal = modal;
    dialog->modal_parent = parent;
    dialog->base.needs_paint = true;
}

/// @brief Mark the dialog as open and register it as the modal root if modal.
///
/// @details Resets result to VG_DIALOG_RESULT_NONE.  Use vg_dialog_show_centered
///          to also position the dialog before showing it.
///
/// @param dialog The dialog to show; may be NULL.
void vg_dialog_show(vg_dialog_t *dialog) {
    if (!dialog)
        return;
    dialog->is_open = true;
    dialog->result = VG_DIALOG_RESULT_NONE;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;

    // Register as modal root so event dispatch restricts input to this dialog
    if (dialog->modal)
        vg_widget_set_modal_root(&dialog->base);
}

/// @brief Show the dialog and centre it over relative_to (or the modal parent).
///
/// @details Measures the dialog, computes the centred position, clamps to the
///          clamp region with DIALOG_SCREEN_MARGIN border, then arranges.
///
/// @param dialog      The dialog to show; may be NULL.
/// @param relative_to Widget to centre over; NULL centres over the modal parent.
void vg_dialog_show_centered(vg_dialog_t *dialog, vg_widget_t *relative_to) {
    if (!dialog)
        return;

    vg_dialog_show(dialog);

    float avail_w = 0.0f;
    float avail_h = 0.0f;
    if (relative_to) {
        avail_w = relative_to->width;
        avail_h = relative_to->height;
    } else if (dialog->modal_parent) {
        avail_w = dialog->modal_parent->width;
        avail_h = dialog->modal_parent->height;
    }
    if (avail_w <= 0.0f)
        avail_w = 800.0f;
    if (avail_h <= 0.0f)
        avail_h = 600.0f;

    vg_widget_measure(&dialog->base, avail_w, avail_h);

    float region_x = 0.0f, region_y = 0.0f, region_w = avail_w, region_h = avail_h;
    float center_x, center_y;
    if (relative_to) {
        float sx = 0.0f;
        float sy = 0.0f;
        vg_widget_get_screen_bounds(relative_to, &sx, &sy, NULL, NULL);
        center_x = sx + relative_to->width / 2.0f;
        center_y = sy + relative_to->height / 2.0f;
    } else {
        center_x = region_w * 0.5f;
        center_y = region_h * 0.5f;
    }

    float parent_sx = 0.0f;
    float parent_sy = 0.0f;
    get_parent_screen_origin(&dialog->base, &parent_sx, &parent_sy);

    bool have_clamp_region = false;
    vg_widget_t *clamp_ref = dialog->modal_parent ? dialog->modal_parent : dialog->base.parent;
    if (clamp_ref && clamp_ref->width > 0.0f && clamp_ref->height > 0.0f) {
        vg_widget_get_screen_bounds(clamp_ref, &region_x, &region_y, NULL, NULL);
        region_w = clamp_ref->width;
        region_h = clamp_ref->height;
        have_clamp_region = true;
    } else if (!relative_to) {
        have_clamp_region = true;
    }

    dialog->base.x = center_x - parent_sx - dialog->base.measured_width / 2.0f;
    dialog->base.y = center_y - parent_sy - dialog->base.measured_height / 2.0f;

    if (have_clamp_region) {
        float local_region_x = region_x - parent_sx;
        float local_region_y = region_y - parent_sy;
        float min_x = local_region_x + (float)DIALOG_SCREEN_MARGIN;
        float min_y = local_region_y + (float)DIALOG_SCREEN_MARGIN;
        float max_x =
            local_region_x + region_w - dialog->base.measured_width - (float)DIALOG_SCREEN_MARGIN;
        float max_y =
            local_region_y + region_h - dialog->base.measured_height - (float)DIALOG_SCREEN_MARGIN;

        if (max_x < min_x)
            max_x = min_x;
        if (max_y < min_y)
            max_y = min_y;

        if (dialog->base.x < min_x)
            dialog->base.x = min_x;
        if (dialog->base.y < min_y)
            dialog->base.y = min_y;
        if (dialog->base.x > max_x)
            dialog->base.x = max_x;
        if (dialog->base.y > max_y)
            dialog->base.y = max_y;
    }

    vg_widget_arrange(&dialog->base,
                      dialog->base.x,
                      dialog->base.y,
                      dialog->base.measured_width,
                      dialog->base.measured_height);
}

/// @brief Hide the dialog without firing any result or close callbacks.
///
/// @param dialog The dialog to hide; may be NULL.
void vg_dialog_hide(vg_dialog_t *dialog) {
    if (!dialog)
        return;
    dialog->is_open = false;
    dialog->is_dragging = false;
    if (vg_widget_get_input_capture() == &dialog->base)
        vg_widget_release_input_capture();

    // Release modal lock if this dialog owned it
    if (dialog->modal && vg_widget_get_modal_root() == &dialog->base)
        vg_widget_set_modal_root(NULL);
}

/// @brief Close the dialog with a result code, firing on_result then on_close callbacks.
///
/// @details closing_in_progress prevents a callback that calls vg_dialog_close again
///          from re-firing the result/close handlers.  Callbacks are snapshotted into
///          locals so a re-entrant set_on_close cannot swap the close handler mid-fire.
///          Callbacks MUST NOT free the dialog — schedule destruction after close returns;
///          freeing inside on_result would cause use-after-free when on_close fires.
///
/// @param dialog The dialog to close; may be NULL.
/// @param result The outcome to record and pass to on_result.
void vg_dialog_close(vg_dialog_t *dialog, vg_dialog_result_t result) {
    if (!dialog || dialog->closing_in_progress)
        return;
    dialog->closing_in_progress = true;

    dialog->result = result;
    dialog->is_open = false;
    dialog->is_dragging = false;
    if (vg_widget_get_input_capture() == &dialog->base)
        vg_widget_release_input_capture();

    // Release modal lock if this dialog owned it
    if (dialog->modal && vg_widget_get_modal_root() == &dialog->base)
        vg_widget_set_modal_root(NULL);

    // Snapshot callbacks to defend against re-entrant set_on_result/set_on_close.
    void (*on_result_cb)(vg_dialog_t *, vg_dialog_result_t, void *) = dialog->on_result;
    void *on_result_ud = dialog->on_result_user_data;
    void (*on_close_cb)(vg_dialog_t *, void *) = dialog->on_close;
    void *on_close_ud = dialog->on_close_user_data;

    if (on_result_cb) {
        on_result_cb(dialog, result, on_result_ud);
    }

    if (on_close_cb) {
        on_close_cb(dialog, on_close_ud);
    }

    dialog->closing_in_progress = false;
}

/// @brief Return the result recorded by the most recent vg_dialog_close call.
///
/// @param dialog The dialog to query.
/// @return The last close result, or VG_DIALOG_RESULT_NONE if dialog is NULL or still open.
vg_dialog_result_t vg_dialog_get_result(vg_dialog_t *dialog) {
    if (!dialog)
        return VG_DIALOG_RESULT_NONE;
    return dialog->result;
}

/// @brief Return whether the dialog is currently visible.
///
/// @param dialog The dialog to query.
/// @return true if the dialog is open; false if closed or dialog is NULL.
bool vg_dialog_is_open(vg_dialog_t *dialog) {
    if (!dialog)
        return false;
    return dialog->is_open;
}

/// @brief Register a callback fired with the result code when the dialog closes.
///
/// @param dialog    The dialog to configure; may be NULL.
/// @param callback  Called with (dialog, result, user_data); NULL to unregister.
/// @param user_data Opaque pointer forwarded to callback and stored in user_data.
void vg_dialog_set_on_result(vg_dialog_t *dialog,
                             void (*callback)(vg_dialog_t *, vg_dialog_result_t, void *),
                             void *user_data) {
    if (!dialog)
        return;
    dialog->on_result = callback;
    dialog->on_result_user_data = user_data;
    dialog->user_data = user_data; // legacy alias
}

/// @brief Register a callback fired after on_result when the dialog closes.
///
/// @details When no on_result is registered, on_close's user_data also populates
///          the legacy combined user_data slot for historical compatibility.
///
/// @param dialog    The dialog to configure; may be NULL.
/// @param callback  Called with (dialog, user_data); NULL to unregister.
/// @param user_data Opaque pointer forwarded to callback.
void vg_dialog_set_on_close(vg_dialog_t *dialog,
                            void (*callback)(vg_dialog_t *, void *),
                            void *user_data) {
    if (!dialog)
        return;
    dialog->on_close = callback;
    dialog->on_close_user_data = user_data;
    if (!dialog->on_result) {
        // Preserve historical behavior: when no result handler is set, on_close's
        // user_data also populates the legacy combined slot.
        dialog->user_data = user_data;
    }
}

/// @brief Set the font used for both body text and the title bar.
///
/// @details title_font_size is derived as size + ui_scale.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param font   The font to use; must outlive the dialog.
/// @param size   Body text point size; ≤ 0 falls back to the theme's normal size.
void vg_dialog_set_font(vg_dialog_t *dialog, vg_font_t *font, float size) {
    if (!dialog)
        return;
    float font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    float title_font_size = font_size + dialog_ui_scale();
    if (dialog->font == font && dialog->font_size == font_size &&
        dialog->title_font_size == title_font_size) {
        return;
    }
    dialog->font = font;
    dialog->font_size = font_size;
    dialog->title_font_size = title_font_size;
    dialog->base.needs_layout = true;
    dialog->base.needs_paint = true;
}

//=============================================================================
// Convenience Constructors
//=============================================================================

/// @brief Convenience constructor — create a pre-configured message dialog.
///
/// @param title   Title bar text.
/// @param message Body message text.
/// @param icon    Status icon to display.
/// @param buttons Button preset for the dialog.
/// @return Newly allocated vg_dialog_t, or NULL on allocation failure.
vg_dialog_t *vg_dialog_message(const char *title,
                               const char *message,
                               vg_dialog_icon_t icon,
                               vg_dialog_buttons_t buttons) {
    vg_dialog_t *dlg = vg_dialog_create(title);
    if (!dlg)
        return NULL;

    vg_dialog_set_message(dlg, message);
    vg_dialog_set_icon(dlg, icon);
    vg_dialog_set_buttons(dlg, buttons);

    return dlg;
}

typedef struct {
    void (*callback)(void *);
    void *user_data;
} confirm_data_t;

/// @brief Bridges a confirmation result to its simplified user callback.
///
/// @details The wrapper context is released for every result. The user's
/// callback runs only for `VG_DIALOG_RESULT_YES`.
///
/// @param dialog Confirmation dialog that closed; otherwise unused.
/// @param result Result selected by the user.
/// @param data Owned `confirm_data_t` wrapper to consume.
static void confirm_result_handler(vg_dialog_t *dialog, vg_dialog_result_t result, void *data) {
    confirm_data_t *cd = (confirm_data_t *)data;
    if (result == VG_DIALOG_RESULT_YES && cd && cd->callback) {
        cd->callback(cd->user_data);
    }
    free(cd);
    (void)dialog;
}

/// @brief Convenience constructor — create a Yes/No confirmation dialog.
///
/// @details on_confirm is called only when the user selects Yes.  The
///          confirm_data_t wrapper is heap-allocated and freed by the
///          confirm_result_handler after the callback fires.
///
/// @param title      Title bar text.
/// @param message    Confirmation question text.
/// @param on_confirm Callback fired when the user clicks Yes; may be NULL.
/// @param user_data  Forwarded to on_confirm.
/// @return Newly allocated vg_dialog_t, or NULL on allocation failure.
vg_dialog_t *vg_dialog_confirm(const char *title,
                               const char *message,
                               void (*on_confirm)(void *),
                               void *user_data) {
    vg_dialog_t *dlg = vg_dialog_create(title);
    if (!dlg)
        return NULL;

    vg_dialog_set_message(dlg, message);
    vg_dialog_set_icon(dlg, VG_DIALOG_ICON_QUESTION);
    vg_dialog_set_buttons(dlg, VG_DIALOG_BUTTONS_YES_NO);

    // Set up callback wrapper
    confirm_data_t *cd = malloc(sizeof(confirm_data_t));
    if (cd) {
        cd->callback = on_confirm;
        cd->user_data = user_data;
        vg_dialog_set_on_result(dlg, confirm_result_handler, cd);
    }

    return dlg;
}
