//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_notification.c
// Purpose: Notification manager widget that displays a stack of transient toast
//          notifications at a configurable screen corner. Each notification
//          fades and slides in on creation and fades out on dismiss or timeout.
// Key invariants:
//   - notifications[] is a flat pointer array; fully dismissed entries (opacity
//     and slide_progress both ≤ 0) are compacted out during each update tick.
//   - created_at == 0 is a sentinel meaning "not yet started"; the first
//     vg_notification_manager_update call sets it to the current clock value.
//   - opacity and slide_progress are driven entirely by vg_notification_manager_update;
//     paint reads them as-is without touching animation state.
//   - notification_bounds_for_index recomputes positions from scratch every
//     paint/hit-test call to handle the slide animation correctly.
// Ownership/Lifetime:
//   - All vg_notification_t structs are heap-allocated by vg_notification_show*
//     and freed inside vg_notification_manager_update when fully faded out.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_widget.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements transient notification storage, wrapping, animated layout,
///        painting, hit testing, scheduling, and dismissal.
/// @details The manager owns copied notification content and advances animation
///          state only from explicit update calls. Geometry is derived through
///          one shared routine so visual bounds, paint, and action hit testing
///          remain consistent.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void notification_manager_destroy(vg_widget_t *widget);
static void notification_manager_measure(vg_widget_t *widget,
                                         float available_width,
                                         float available_height);
static void notification_manager_paint(vg_widget_t *widget, void *canvas);
static bool notification_manager_handle_event(vg_widget_t *widget, vg_event_t *event);
static uint32_t notification_fade_color(uint32_t color, uint32_t backdrop, float opacity);
static void notification_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
static void notification_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
static char *notification_dup_range(const char *text, size_t len);
static int notification_wrap_text(vg_notification_manager_t *mgr,
                                  const char *text,
                                  float font_size,
                                  float max_width,
                                  char ***out_lines,
                                  float *out_max_width);
static void notification_free_lines(char **lines, int line_count);
static float notification_line_height(vg_notification_manager_t *mgr, float font_size);
static float notification_text_block_height(vg_notification_manager_t *mgr,
                                            const char *text,
                                            float font_size,
                                            float max_width);
static float notification_measure_height(vg_notification_manager_t *mgr,
                                         vg_notification_t *notif,
                                         float *out_action_h);
static void notification_request_dismiss(vg_notification_t *notif, uint64_t now_ms);

//=============================================================================
// Notification Manager VTable
//=============================================================================

static vg_widget_vtable_t g_notification_manager_vtable = {.destroy = notification_manager_destroy,
                                                           .measure = notification_manager_measure,
                                                           .arrange = NULL,
                                                           .paint = notification_manager_paint,
                                                           .handle_event =
                                                               notification_manager_handle_event,
                                                           .can_focus = NULL,
                                                           .on_focus = NULL};

//=============================================================================
// Notification Helpers
//=============================================================================

/// @brief Free all heap strings inside notif and the notif struct itself.
/// @param notif Owned notification to destroy; `NULL` is ignored.
static void free_notification(vg_notification_t *notif) {
    if (!notif)
        return;
    free(notif->title);
    free(notif->message);
    free(notif->action_label);
    free(notif);
}

/// @brief Map a VG_NOTIFICATION_* type to its accent colour from the manager's palette.
/// @param mgr Manager supplying configured type colors.
/// @param type Notification semantic type.
/// @return Packed accent color; unknown values use the information color.
static uint32_t type_to_color(vg_notification_manager_t *mgr, vg_notification_type_t type) {
    switch (type) {
        case VG_NOTIFICATION_INFO:
            return mgr->info_color;
        case VG_NOTIFICATION_SUCCESS:
            return mgr->success_color;
        case VG_NOTIFICATION_WARNING:
            return mgr->warning_color;
        case VG_NOTIFICATION_ERROR:
            return mgr->error_color;
        default:
            return mgr->info_color;
    }
}

/// @brief Lerp a 24-bit RGB colour toward backdrop by (1-opacity), discarding the alpha channel.
/// @param color Foreground packed color.
/// @param backdrop Background packed color.
/// @param opacity Foreground contribution, clamped by endpoint branches.
/// @return Blended 24-bit RGB value.
static uint32_t notification_fade_color(uint32_t color, uint32_t backdrop, float opacity) {
    uint32_t rgb = color & 0x00FFFFFFu;
    if (opacity <= 0.0f)
        return backdrop & 0x00FFFFFFu;
    if (opacity >= 1.0f)
        return rgb;
    return vg_color_blend(backdrop & 0x00FFFFFFu, rgb, opacity);
}

/// @brief Fill a rounded rectangle, falling back to a plain rect when radius is zero or too large.
/// @param win Target graphics window.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param radius Corner radius in pixels.
/// @param color Packed fill color.
static void notification_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_fill(win, (float)x, (float)y, (float)w, (float)h, (float)radius, color);
}

/// @brief Stroke a rounded-rectangle border via the shared anti-aliased core.
/// @param win Target graphics window.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param radius Corner radius in pixels.
/// @param color Packed stroke color.
static void notification_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_stroke(
        win, (float)x, (float)y, (float)w, (float)h, (float)radius, 1.0f, color);
}

/// @brief Heap-allocate a NUL-terminated copy of the first len bytes of text.
/// @param text Source containing at least @p len readable bytes.
/// @param len Number of bytes to copy.
/// @return Newly allocated string, or `NULL` on allocation failure.
static char *notification_dup_range(const char *text, size_t len) {
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Word-wrap text to fit max_width, returning the line count and optionally allocating
/// out_lines[].
/// @details Explicit newlines and whitespace break opportunities are honored.
///          When line storage is requested, both the pointer array and each line
///          are caller-owned and released with notification_free_lines().
/// @param mgr Manager supplying the active font.
/// @param text Null-terminated text to wrap.
/// @param font_size Font size used for measurement.
/// @param max_width Maximum line width in pixels.
/// @param out_lines Optional destination for an allocated line array.
/// @param out_max_width Optional destination for the widest measured line.
/// @return Produced line count, with one fallback line for empty allocation output.
static int notification_wrap_text(vg_notification_manager_t *mgr,
                                  const char *text,
                                  float font_size,
                                  float max_width,
                                  char ***out_lines,
                                  float *out_max_width) {
    if (out_lines)
        *out_lines = NULL;
    if (out_max_width)
        *out_max_width = 0.0f;
    if (!mgr || !mgr->font || !text || !text[0])
        return 0;

    int cap = 4;
    int count = 0;
    char **lines = out_lines ? (char **)calloc((size_t)cap, sizeof(char *)) : NULL;
    size_t text_len = strlen(text);
    size_t start = 0;

    while (start <= text_len) {
        if (text[start] == '\0') {
            if (count == 0 && lines)
                lines[count++] = vg_strdup("");
            break;
        }

        size_t line_end = start;
        size_t best_end = start;
        size_t best_next = start;
        float best_width = 0.0f;
        bool found_break = false;

        while (line_end < text_len && text[line_end] != '\n') {
            size_t candidate_end = line_end + 1;
            char *candidate = notification_dup_range(text + start, candidate_end - start);
            if (!candidate)
                break;
            vg_text_metrics_t metrics = {0};
            vg_font_measure_text(mgr->font, font_size, candidate, &metrics);
            free(candidate);
            if (max_width > 0.0f && metrics.width > max_width)
                break;

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
            char *candidate = notification_dup_range(text + start, best_end - start);
            if (candidate) {
                vg_text_metrics_t metrics = {0};
                vg_font_measure_text(mgr->font, font_size, candidate, &metrics);
                best_width = metrics.width;
                free(candidate);
            }
        }

        while (best_end > start && (text[best_end - 1] == ' ' || text[best_end - 1] == '\t'))
            best_end--;

        if (lines) {
            if (count >= cap) {
                int new_cap = cap * 2;
                char **new_lines = (char **)realloc(lines, (size_t)new_cap * sizeof(char *));
                if (!new_lines)
                    break;
                memset(new_lines + cap, 0, (size_t)(new_cap - cap) * sizeof(char *));
                lines = new_lines;
                cap = new_cap;
            }
            lines[count] = notification_dup_range(text + start, best_end - start);
            if (!lines[count])
                break;
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
        if (text[start] == '\n')
            start++;
    }

    if (out_lines) {
        *out_lines = lines;
    } else if (lines) {
        notification_free_lines(lines, count);
    }
    return count > 0 ? count : 1;
}

/// @brief Free a lines[] array produced by notification_wrap_text.
/// @param lines Owned line-pointer array; `NULL` is ignored.
/// @param line_count Number of initialized entries to release.
static void notification_free_lines(char **lines, int line_count) {
    if (!lines)
        return;
    for (int i = 0; i < line_count; i++)
        free(lines[i]);
    free(lines);
}

/// @brief Return the font's line_height in pixels, falling back to font_size when no font is set.
/// @param mgr Manager supplying the active font.
/// @param font_size Requested font size and fallback height.
/// @return Positive font line height when available, otherwise @p font_size.
static float notification_line_height(vg_notification_manager_t *mgr, float font_size) {
    vg_font_metrics_t metrics = {0};
    if (!mgr || !mgr->font)
        return font_size;
    vg_font_get_metrics(mgr->font, font_size, &metrics);
    return metrics.line_height > 0 ? (float)metrics.line_height : font_size;
}

/// @brief Return the total pixel height of text wrapped to max_width at font_size.
/// @param mgr Manager supplying font metrics.
/// @param text Text block to measure.
/// @param font_size Measurement size.
/// @param max_width Wrapping width.
/// @return Wrapped line count multiplied by line height.
static float notification_text_block_height(vg_notification_manager_t *mgr,
                                            const char *text,
                                            float font_size,
                                            float max_width) {
    int line_count = notification_wrap_text(mgr, text, font_size, max_width, NULL, NULL);
    return notification_line_height(mgr, font_size) * (float)line_count;
}

/// @brief Compute the total card height for notif including title, message, action button, and
/// padding.
/// @param mgr Manager supplying dimensions and typography.
/// @param notif Notification content to measure.
/// @param out_action_h Optional destination for the action-button height.
/// @return Required card height, or zero for invalid input.
static float notification_measure_height(vg_notification_manager_t *mgr,
                                         vg_notification_t *notif,
                                         float *out_action_h) {
    if (!mgr || !notif)
        return 0.0f;

    float content_width = (float)mgr->notification_width - (float)(mgr->padding * 2 + 16);
    float notif_height = (float)mgr->padding * 2.0f;
    float action_h = notification_line_height(mgr, mgr->font_size) + 8.0f;

    if (notif->title && notif->title[0]) {
        notif_height +=
            notification_text_block_height(mgr, notif->title, mgr->title_font_size, content_width);
    }
    if (notif->message && notif->message[0]) {
        if (notif->title && notif->title[0])
            notif_height += 6.0f;
        notif_height +=
            notification_text_block_height(mgr, notif->message, mgr->font_size, content_width);
    }
    if (notif->action_label && notif->action_label[0]) {
        if ((notif->title && notif->title[0]) || (notif->message && notif->message[0]))
            notif_height += 10.0f;
        notif_height += action_h;
    }

    if (out_action_h)
        *out_action_h = action_h;
    return notif_height;
}

/// @brief Mark notif as dismissed and record now_ms as the start of the fade-out animation.
/// @param notif Notification whose exit animation is requested.
/// @param now_ms Current clock value, or zero to defer timestamping until update.
static void notification_request_dismiss(vg_notification_t *notif, uint64_t now_ms) {
    if (!notif)
        return;
    notif->dismissed = true;
    if (notif->dismiss_started_at == 0 && now_ms > 0)
        notif->dismiss_started_at = now_ms;
}

/// @brief Compute the screen-space bounding rect (and optional action-button rect) for the
/// notification at target_index.
/// @details Earlier visible notifications contribute their measured heights and
///          spacing. The selected corner controls stacking direction, and slide
///          progress offsets the resulting card.
/// @param mgr Manager containing the notification stack.
/// @param target_index Physical array index to locate.
/// @param out_x Optional destination for card left coordinate.
/// @param out_y Optional destination for card top coordinate.
/// @param out_w Optional destination for card width.
/// @param out_h Optional destination for card height.
/// @param action_x Optional destination for action-button left coordinate.
/// @param action_y Optional destination for action-button top coordinate.
/// @param action_w Optional destination for action-button width.
/// @param action_h Optional destination for action-button height.
/// @return `true` when the target is within the currently materialized visible stack.
static bool notification_bounds_for_index(vg_notification_manager_t *mgr,
                                          size_t target_index,
                                          float *out_x,
                                          float *out_y,
                                          float *out_w,
                                          float *out_h,
                                          float *action_x,
                                          float *action_y,
                                          float *action_w,
                                          float *action_h) {
    if (!mgr || target_index >= mgr->notification_count)
        return false;

    float x, y;
    bool from_top = true;
    bool from_right = true;

    switch (mgr->position) {
        case VG_NOTIFICATION_TOP_LEFT:
            x = mgr->base.x + mgr->margin;
            y = mgr->base.y + mgr->margin;
            from_right = false;
            break;
        case VG_NOTIFICATION_TOP_RIGHT:
            x = mgr->base.x + mgr->base.width - mgr->margin - mgr->notification_width;
            y = mgr->base.y + mgr->margin;
            break;
        case VG_NOTIFICATION_BOTTOM_LEFT:
            x = mgr->base.x + mgr->margin;
            y = mgr->base.y + mgr->base.height - mgr->margin;
            from_top = false;
            from_right = false;
            break;
        case VG_NOTIFICATION_BOTTOM_RIGHT:
            x = mgr->base.x + mgr->base.width - mgr->margin - mgr->notification_width;
            y = mgr->base.y + mgr->base.height - mgr->margin;
            from_top = false;
            break;
        case VG_NOTIFICATION_TOP_CENTER:
            x = mgr->base.x + (mgr->base.width - mgr->notification_width) / 2;
            y = mgr->base.y + mgr->margin;
            break;
        case VG_NOTIFICATION_BOTTOM_CENTER:
            x = mgr->base.x + (mgr->base.width - mgr->notification_width) / 2;
            y = mgr->base.y + mgr->base.height - mgr->margin;
            from_top = false;
            break;
        default:
            x = mgr->base.x + mgr->base.width - mgr->margin - mgr->notification_width;
            y = mgr->base.y + mgr->margin;
            break;
    }

    size_t visible_count = 0;
    for (size_t i = 0; i < mgr->notification_count; i++) {
        vg_notification_t *notif = mgr->notifications[i];
        if (!notif)
            continue;
        if (visible_count >= mgr->max_visible)
            break;

        float action_h_px = 0.0f;
        float notif_height = notification_measure_height(mgr, notif, &action_h_px);
        float slide_t = notif->slide_progress;
        if (slide_t < 0.0f)
            slide_t = 0.0f;
        if (slide_t > 1.0f)
            slide_t = 1.0f;

        float notif_x = x + (1.0f - slide_t) * 24.0f * (from_right ? 1.0f : -1.0f);
        float notif_y =
            (from_top ? y : y - notif_height) + (1.0f - slide_t) * 8.0f * (from_top ? -1.0f : 1.0f);
        if (i == target_index) {
            if (out_x)
                *out_x = notif_x;
            if (out_y)
                *out_y = notif_y;
            if (out_w)
                *out_w = (float)mgr->notification_width;
            if (out_h)
                *out_h = notif_height;
            if (action_x)
                *action_x = notif_x + (float)mgr->padding + 8.0f;
            if (action_y)
                *action_y = notif_y + notif_height - (float)mgr->padding - action_h_px;
            if (action_w)
                *action_w = (float)mgr->notification_width - (float)(mgr->padding * 2 + 16);
            if (action_h)
                *action_h = action_h_px;
            return true;
        }

        if (from_top)
            y += notif_height + mgr->spacing;
        else
            y -= notif_height + mgr->spacing;
        visible_count++;
    }

    return false;
}

/// @brief Compute the exact retained-damage union for currently paintable toast cards.
/// @details Mirrors the visibility and `max_visible` rules in notification_manager_paint, then
///          conservatively includes the level-2 soft shadow used by that paint path. See the
///          public declaration for output and ownership semantics.
/// @param mgr Manager whose animated cards are measured.
/// @param x Optional destination for union left coordinate.
/// @param y Optional destination for union top coordinate.
/// @param width Optional destination for union width.
/// @param height Optional destination for union height.
/// @return `true` when at least one finite, positive-area card contributes.
bool vg_notification_manager_get_visual_bounds(
    vg_notification_manager_t *mgr, float *x, float *y, float *width, float *height) {
    float out_x = 0.0f;
    float out_y = 0.0f;
    float out_w = 0.0f;
    float out_h = 0.0f;
    bool any = false;
    size_t visible_count = 0;

    if (mgr) {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        for (size_t i = 0; i < mgr->notification_count; ++i) {
            vg_notification_t *notif = mgr->notifications[i];
            if (!notif)
                continue;
            if (visible_count >= mgr->max_visible)
                break;
            visible_count++;
            if (!isfinite(notif->opacity) || notif->opacity <= 0.0f)
                continue;

            float nx = 0.0f, ny = 0.0f, nw = 0.0f, nh = 0.0f;
            if (!notification_bounds_for_index(
                    mgr, i, &nx, &ny, &nw, &nh, NULL, NULL, NULL, NULL) ||
                !isfinite(nx) || !isfinite(ny) || !isfinite(nw) || !isfinite(nh) || nw <= 0.0f ||
                nh <= 0.0f) {
                continue;
            }
            float nr = nx + nw;
            float nb = ny + nh;
            if (!isfinite(nr) || !isfinite(nb))
                continue;
            if (!any) {
                x0 = nx;
                y0 = ny;
                x1 = nr;
                y1 = nb;
                any = true;
            } else {
                if (nx < x0)
                    x0 = nx;
                if (ny < y0)
                    y0 = ny;
                if (nr > x1)
                    x1 = nr;
                if (nb > y1)
                    y1 = nb;
            }
        }

        if (any) {
            float left = 1.0f, top = 1.0f, right = 1.0f, bottom = 1.0f;
            const vg_theme_t *theme = vg_theme_get_current();
            if (theme && theme->elevation.level2.alpha > 0 &&
                isfinite(theme->elevation.level2.blur) && theme->elevation.level2.blur > 0.0f) {
                int blur_radius = (int)(theme->elevation.level2.blur * 0.5f + 0.5f);
                if (blur_radius < 1)
                    blur_radius = 1;
                float padding = (float)(blur_radius * 3 + 1);
                float shadow_left = padding - (float)theme->elevation.level2.dx;
                float shadow_top = padding - (float)theme->elevation.level2.dy;
                float shadow_right = padding + (float)theme->elevation.level2.dx;
                float shadow_bottom = padding + (float)theme->elevation.level2.dy;
                if (shadow_left > left)
                    left = shadow_left;
                if (shadow_top > top)
                    top = shadow_top;
                if (shadow_right > right)
                    right = shadow_right;
                if (shadow_bottom > bottom)
                    bottom = shadow_bottom;
            }
            out_x = x0 - left;
            out_y = y0 - top;
            out_w = (x1 + right) - out_x;
            out_h = (y1 + bottom) - out_y;
        }
    }

    if (x)
        *x = out_x;
    if (y)
        *y = out_y;
    if (width)
        *width = out_w;
    if (height)
        *height = out_h;
    return any;
}

//=============================================================================
// Notification Manager Implementation
//=============================================================================

/// @brief Create a notification manager widget with default styling and a top-right position.
///
/// @return Newly allocated manager, or NULL on allocation failure.
vg_notification_manager_t *vg_notification_manager_create(void) {
    vg_notification_manager_t *mgr = calloc(1, sizeof(vg_notification_manager_t));
    if (!mgr)
        return NULL;

    vg_widget_init(&mgr->base, VG_WIDGET_CUSTOM, &g_notification_manager_vtable);

    vg_theme_t *theme = vg_theme_get_current();

    // Defaults
    mgr->position = VG_NOTIFICATION_TOP_RIGHT;
    mgr->max_visible = 5;
    mgr->notification_width = 350;
    mgr->spacing = 8;
    mgr->margin = 16;
    mgr->padding = 12;

    mgr->font = theme->typography.font_regular;
    mgr->font_size = theme->typography.size_normal;
    mgr->title_font_size = theme->typography.size_normal + 2;

    mgr->info_color = 0xFF2196F3;    // Blue
    mgr->success_color = 0xFF4CAF50; // Green
    mgr->warning_color = 0xFFFFC107; // Amber
    mgr->error_color = 0xFFF44336;   // Red
    mgr->bg_color = 0xF0212934;
    mgr->text_color = theme->colors.fg_primary;

    mgr->fade_duration_ms = 200;
    mgr->slide_duration_ms = 300;

    mgr->next_id = 1;

    return mgr;
}

/// @brief vtable destroy — free all pending notification structs and the notifications[] array.
/// @param widget Notification-manager widget base being destroyed.
static void notification_manager_destroy(vg_widget_t *widget) {
    vg_notification_manager_t *mgr = (vg_notification_manager_t *)widget;

    for (size_t i = 0; i < mgr->notification_count; i++) {
        free_notification(mgr->notifications[i]);
    }
    free(mgr->notifications);
}

/// @brief Destroy the notification manager, freeing all pending notifications.
///
/// @param mgr Manager to destroy; may be NULL (no-op).
void vg_notification_manager_destroy(vg_notification_manager_t *mgr) {
    if (!mgr)
        return;
    vg_widget_destroy(&mgr->base);
}

/// @brief vtable measure — reports available_width × available_height; the manager overlays the
/// entire parent.
/// @param widget Notification-manager widget base to measure.
/// @param available_width Parent overlay width.
/// @param available_height Parent overlay height.
static void notification_manager_measure(vg_widget_t *widget,
                                         float available_width,
                                         float available_height) {
    (void)available_width;
    (void)available_height;

    // Notification manager fills the whole window
    widget->measured_width = available_width;
    widget->measured_height = available_height;
}

/// @brief vtable paint — render all visible notifications at their animated screen positions.
/// @details Applies semantic accent colors, backdrop-relative fade colors,
///          elevation shadows, wrapping, clipping, and optional action buttons
///          to at most `max_visible` cards.
/// @param widget Arranged notification-manager widget base.
/// @param canvas Backend canvas used for card and text rendering.
static void notification_manager_paint(vg_widget_t *widget, void *canvas) {
    vg_notification_manager_t *mgr = (vg_notification_manager_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    if (mgr->notification_count == 0)
        return;

    size_t visible_count = 0;
    for (size_t i = 0; i < mgr->notification_count; i++) {
        vg_notification_t *notif = mgr->notifications[i];
        if (!notif)
            continue;
        if (visible_count >= mgr->max_visible)
            break;

        float notif_x = 0.0f, notif_y = 0.0f, notif_w = 0.0f, notif_h = 0.0f;
        float action_x = 0.0f, action_y = 0.0f, action_w = 0.0f, action_h = 0.0f;
        if (!notification_bounds_for_index(mgr,
                                           i,
                                           &notif_x,
                                           &notif_y,
                                           &notif_w,
                                           &notif_h,
                                           &action_x,
                                           &action_y,
                                           &action_w,
                                           &action_h)) {
            continue;
        }

        float opacity = notif->opacity;
        if (opacity <= 0.0f) {
            visible_count++;
            continue;
        }
        if (opacity > 1.0f)
            opacity = 1.0f;

        uint32_t backdrop = theme->colors.bg_primary;
        uint32_t type_color = type_to_color(mgr, notif->type);
        uint32_t card_bg = notification_fade_color(mgr->bg_color, backdrop, opacity);
        uint32_t card_border = notification_fade_color(
            vg_color_blend(type_color, theme->colors.border_primary, 0.55f), backdrop, opacity);
        uint32_t accent_color = notification_fade_color(type_color, backdrop, opacity);
        uint32_t title_color =
            notification_fade_color(vg_color_lighten(mgr->text_color, 0.08f), backdrop, opacity);
        uint32_t body_color = notification_fade_color(mgr->text_color, backdrop, opacity);
        uint32_t action_bg = notification_fade_color(
            vg_color_blend(type_color, mgr->bg_color, 0.78f), backdrop, opacity);
        uint32_t action_border = notification_fade_color(
            vg_color_blend(type_color, theme->colors.border_primary, 0.35f), backdrop, opacity);
        uint32_t action_text =
            notification_fade_color(vg_color_lighten(type_color, 0.14f), backdrop, opacity);

        int32_t x = (int32_t)notif_x;
        int32_t y = (int32_t)notif_y;
        int32_t w = (int32_t)notif_w;
        int32_t h = (int32_t)notif_h;
        int32_t radius = 10;
        int32_t accent_w = 5;

        vg_elevation_t nel = theme->elevation.level2;
        uint8_t nshadow_a = (uint8_t)((float)nel.alpha * opacity);
        vg_draw_round_rect_shadow(win,
                                  (float)x,
                                  (float)y,
                                  (float)w,
                                  (float)h,
                                  (float)radius,
                                  nel.blur,
                                  nel.dx,
                                  nel.dy,
                                  nshadow_a,
                                  theme->elevation.shadow_rgb);
        notification_fill_round_rect(win, x, y, w, h, radius, card_bg);
        notification_stroke_round_rect(win, x, y, w, h, radius, card_border);
        notification_fill_round_rect(win, x, y, accent_w, h, radius / 2, accent_color);
        if (w > 12) {
            vgfx_fill_rect(
                win,
                x + 10,
                y + 1,
                w - 20,
                1,
                notification_fade_color(vg_color_lighten(card_bg, 0.07f), backdrop, opacity));
        }

        float content_x = notif_x + (float)mgr->padding + 10.0f;
        float content_y = notif_y + (float)mgr->padding;
        float content_w = notif_w - (float)(mgr->padding * 2) - 18.0f;
        if (content_w < 8.0f)
            content_w = 8.0f;

        int32_t clip_x = x + 6;
        int32_t clip_y = y + 4;
        int32_t clip_w = w - 12;
        int32_t clip_h = h - 8;
        if (clip_w > 0 && clip_h > 0)
            vgfx_set_clip(win, clip_x, clip_y, clip_w, clip_h);

        if (mgr->font) {
            if (notif->title && notif->title[0]) {
                char **lines = NULL;
                int line_count = notification_wrap_text(
                    mgr, notif->title, mgr->title_font_size, content_w, &lines, NULL);
                float line_h = notification_line_height(mgr, mgr->title_font_size);
                vg_font_metrics_t metrics = {0};
                vg_font_get_metrics(mgr->font, mgr->title_font_size, &metrics);
                for (int line = 0; line < line_count; line++) {
                    const char *text = (lines && lines[line]) ? lines[line] : "";
                    vg_font_draw_text(canvas,
                                      mgr->font,
                                      mgr->title_font_size,
                                      content_x,
                                      content_y + (float)metrics.ascent,
                                      text,
                                      title_color);
                    content_y += line_h;
                }
                notification_free_lines(lines, line_count);
            }

            if (notif->message && notif->message[0]) {
                if (notif->title && notif->title[0])
                    content_y += 6.0f;
                char **lines = NULL;
                int line_count = notification_wrap_text(
                    mgr, notif->message, mgr->font_size, content_w, &lines, NULL);
                float line_h = notification_line_height(mgr, mgr->font_size);
                vg_font_metrics_t metrics = {0};
                vg_font_get_metrics(mgr->font, mgr->font_size, &metrics);
                for (int line = 0; line < line_count; line++) {
                    const char *text = (lines && lines[line]) ? lines[line] : "";
                    vg_font_draw_text(canvas,
                                      mgr->font,
                                      mgr->font_size,
                                      content_x,
                                      content_y + (float)metrics.ascent,
                                      text,
                                      body_color);
                    content_y += line_h;
                }
                notification_free_lines(lines, line_count);
            }

            if (notif->action_label && notif->action_label[0]) {
                int32_t ar = (int32_t)(action_h * 0.5f);
                notification_fill_round_rect(win,
                                             (int32_t)action_x,
                                             (int32_t)action_y,
                                             (int32_t)action_w,
                                             (int32_t)action_h,
                                             ar,
                                             action_bg);
                notification_stroke_round_rect(win,
                                               (int32_t)action_x,
                                               (int32_t)action_y,
                                               (int32_t)action_w,
                                               (int32_t)action_h,
                                               ar,
                                               action_border);
                vg_font_metrics_t metrics = {0};
                vg_text_metrics_t text_metrics = {0};
                vg_font_get_metrics(mgr->font, mgr->font_size, &metrics);
                vg_font_measure_text(mgr->font, mgr->font_size, notif->action_label, &text_metrics);
                float label_x = action_x + 12.0f;
                if (text_metrics.width + 24.0f < action_w) {
                    label_x = action_x + (action_w - text_metrics.width) * 0.5f;
                }
                float label_y = action_y +
                                (action_h - (float)(metrics.ascent - metrics.descent)) * 0.5f +
                                (float)metrics.ascent;
                vg_font_draw_text(canvas,
                                  mgr->font,
                                  mgr->font_size,
                                  label_x,
                                  label_y,
                                  notif->action_label,
                                  action_text);
            }
        }

        if (clip_w > 0 && clip_h > 0)
            vgfx_clear_clip(win);
        visible_count++;
    }
}

/// @brief vtable handle_event — on click, hit-test all visible notifications; invoke action
/// callback or dismiss.
/// @param widget Notification manager receiving pointer input.
/// @param event Click or mouse-down event with screen coordinates.
/// @return `true` when a card was hit and dismissal was requested.
static bool notification_manager_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_notification_manager_t *mgr = (vg_notification_manager_t *)widget;

    if (event->type == VG_EVENT_CLICK || event->type == VG_EVENT_MOUSE_DOWN) {
        for (size_t i = 0; i < mgr->notification_count; i++) {
            vg_notification_t *notif = mgr->notifications[i];
            if (!notif || notif->dismissed)
                continue;
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
            float ax = 0.0f, ay = 0.0f, aw = 0.0f, ah = 0.0f;
            if (!notification_bounds_for_index(mgr, i, &x, &y, &w, &h, &ax, &ay, &aw, &ah)) {
                continue;
            }
            float px = event->mouse.screen_x;
            float py = event->mouse.screen_y;
            if (px < x || px >= x + w || py < y || py >= y + h)
                continue;
            if (notif->action_label && px >= ax && px < ax + aw && py >= ay && py < ay + ah &&
                notif->action_callback) {
                notif->action_callback(notif->id, notif->action_user_data);
            }
            notification_request_dismiss(notif, 0);
            mgr->base.needs_paint = true;
            return true;
        }
    }

    return false;
}

/// @brief Advance animation state for all notifications and compact out fully-dismissed entries.
///
/// @details Must be called every frame (or at least once per animation tick) with the current
///          wall-clock time. Drives fade-in, fade-out, and slide animations. Notifications
///          whose opacity and slide_progress both reach 0 are freed and removed.
///
/// @param mgr    Manager to update; may be NULL (no-op).
/// @param now_ms Current wall-clock time in milliseconds.
void vg_notification_manager_update(vg_notification_manager_t *mgr, uint64_t now_ms) {
    if (!mgr)
        return;

    size_t old_count = mgr->notification_count;
    bool needs_repaint = false;
    for (size_t i = 0; i < mgr->notification_count; i++) {
        vg_notification_t *notif = mgr->notifications[i];
        if (!notif)
            continue;

        if (notif->dismissed && notif->created_at == 0 && notif->dismiss_started_at == 0) {
            notif->opacity = 0.0f;
            notif->slide_progress = 0.0f;
            continue;
        }

        if (notif->created_at == 0) {
            notif->created_at = now_ms ? now_ms : 1;
            needs_repaint = true;
        }

        if (notif->duration_ms > 0 && !notif->dismissed) {
            uint64_t elapsed = now_ms >= notif->created_at ? (now_ms - notif->created_at) : 0;
            if (elapsed >= notif->duration_ms) {
                notification_request_dismiss(notif, notif->created_at + notif->duration_ms);
                needs_repaint = true;
            }
        }

        if (notif->dismissed) {
            if (notif->dismiss_started_at == 0)
                notif->dismiss_started_at = now_ms;
            uint64_t dismiss_at = notif->dismiss_started_at;
            uint64_t elapsed = now_ms >= dismiss_at ? (now_ms - dismiss_at) : 0;
            if (mgr->fade_duration_ms > 0) {
                float t = (float)elapsed / (float)mgr->fade_duration_ms;
                if (t > 1.0f)
                    t = 1.0f;
                notif->opacity = 1.0f - t;
            } else {
                notif->opacity = 0.0f;
            }
            if (mgr->slide_duration_ms > 0) {
                float t = (float)elapsed / (float)mgr->slide_duration_ms;
                if (t > 1.0f)
                    t = 1.0f;
                notif->slide_progress = 1.0f - t;
            } else {
                notif->slide_progress = 0.0f;
            }
            if (notif->opacity < 0.0f)
                notif->opacity = 0.0f;
            if (notif->slide_progress < 0.0f)
                notif->slide_progress = 0.0f;
            if (notif->opacity > 0.0f || notif->slide_progress > 0.0f)
                needs_repaint = true;
        } else {
            uint64_t elapsed = now_ms >= notif->created_at ? (now_ms - notif->created_at) : 0;
            if (mgr->fade_duration_ms > 0) {
                notif->opacity = (float)elapsed / (float)mgr->fade_duration_ms;
                if (notif->opacity > 1.0f)
                    notif->opacity = 1.0f;
            } else {
                notif->opacity = 1.0f;
            }
            if (mgr->slide_duration_ms > 0) {
                notif->slide_progress = (float)elapsed / (float)mgr->slide_duration_ms;
                if (notif->slide_progress > 1.0f)
                    notif->slide_progress = 1.0f;
            } else {
                notif->slide_progress = 1.0f;
            }
            if (notif->opacity < 1.0f || notif->slide_progress < 1.0f)
                needs_repaint = true;
        }
    }

    size_t write_idx = 0;
    for (size_t i = 0; i < mgr->notification_count; i++) {
        vg_notification_t *notif = mgr->notifications[i];
        bool keep = notif != NULL;
        if (notif && notif->dismissed && notif->opacity <= 0.0f && notif->slide_progress <= 0.0f) {
            keep = false;
        }
        if (keep) {
            mgr->notifications[write_idx++] = mgr->notifications[i];
        } else {
            free_notification(mgr->notifications[i]);
        }
    }
    mgr->notification_count = write_idx;
    if (needs_repaint || old_count != write_idx)
        mgr->base.needs_paint = true;
}

/// @brief Convert one absolute notification timestamp to a relative scheduler delay.
/// @param now_ms Current scheduler time.
/// @param target_ms Absolute transition time on the same clock.
/// @return Zero when due, otherwise a positive delay saturated to INT64_MAX.
static int64_t notification_relative_deadline_ms(uint64_t now_ms, uint64_t target_ms) {
    if (target_ms <= now_ms)
        return 0;
    uint64_t remaining = target_ms - now_ms;
    return remaining > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)remaining;
}

/// @brief Return the nearest notification animation tick or auto-dismiss deadline.
/// @param mgr Notification manager to inspect; NULL has no deadline.
/// @param now_ms Current scheduler time in milliseconds.
/// @return Milliseconds until work, zero when due, or -1 when no timer is active.
int64_t vg_notification_manager_next_deadline_ms(const vg_notification_manager_t *mgr,
                                                 uint64_t now_ms) {
    if (!mgr)
        return -1;
    int64_t deadline = -1;
    for (size_t i = 0; i < mgr->notification_count; ++i) {
        const vg_notification_t *notif = mgr->notifications[i];
        if (!notif)
            continue;
        if (notif->created_at == 0)
            return 0;

        bool entering =
            !notif->dismissed && (notif->opacity < 1.0f || notif->slide_progress < 1.0f);
        bool exiting = notif->dismissed && (notif->opacity > 0.0f || notif->slide_progress > 0.0f);
        if (entering || exiting) {
            if (deadline < 0 || deadline > 16)
                deadline = 16;
            continue;
        }
        if (!notif->dismissed && notif->duration_ms > 0) {
            uint64_t duration = (uint64_t)notif->duration_ms;
            uint64_t target = notif->created_at > UINT64_MAX - duration
                                  ? UINT64_MAX
                                  : notif->created_at + duration;
            int64_t candidate = notification_relative_deadline_ms(now_ms, target);
            if (candidate == 0)
                return 0;
            if (deadline < 0 || candidate < deadline)
                deadline = candidate;
        }
    }
    return deadline;
}

/// @brief Show a notification without an action button.
///
/// @param mgr         Manager to add to; may be NULL (returns 0).
/// @param type        VG_NOTIFICATION_INFO/SUCCESS/WARNING/ERROR.
/// @param title       Optional bold title text; may be NULL.
/// @param message     Optional body text; may be NULL.
/// @param duration_ms Auto-dismiss delay in ms; 0 for persistent (no auto-dismiss).
/// @return            Unique notification ID, or 0 on failure.
uint32_t vg_notification_show(vg_notification_manager_t *mgr,
                              vg_notification_type_t type,
                              const char *title,
                              const char *message,
                              uint32_t duration_ms) {
    return vg_notification_show_with_action(
        mgr, type, title, message, duration_ms, NULL, NULL, NULL);
}

/// @brief Show a notification with an optional action button that invokes action_callback when
/// clicked.
///
/// @param mgr             Manager to add to; may be NULL (returns 0).
/// @param type            VG_NOTIFICATION_INFO/SUCCESS/WARNING/ERROR.
/// @param title           Optional bold title text; may be NULL.
/// @param message         Optional body text; may be NULL.
/// @param duration_ms     Auto-dismiss delay in ms; 0 for persistent.
/// @param action_label    Button label text; may be NULL (no button rendered).
/// @param action_callback Invoked with (notification_id, user_data) when button is clicked; may be
/// NULL.
/// @param user_data       Opaque pointer forwarded to action_callback.
/// @return                Unique notification ID, or 0 on failure.
uint32_t vg_notification_show_with_action(vg_notification_manager_t *mgr,
                                          vg_notification_type_t type,
                                          const char *title,
                                          const char *message,
                                          uint32_t duration_ms,
                                          const char *action_label,
                                          void (*action_callback)(uint32_t, void *),
                                          void *user_data) {
    if (!mgr)
        return 0;

    uint32_t id = mgr->next_id;
    bool found_id = false;
    size_t max_attempts = mgr->notification_count + 1u;
    for (size_t attempts = 0; attempts < max_attempts; attempts++) {
        if (id == 0)
            id = 1;
        bool collision = false;
        for (size_t i = 0; i < mgr->notification_count; i++) {
            if (mgr->notifications[i] && mgr->notifications[i]->id == id) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            found_id = true;
            break;
        }
        id++;
    }
    if (!found_id)
        return 0;
    mgr->next_id = id + 1;
    if (mgr->next_id == 0)
        mgr->next_id = 1;

    vg_notification_t *notif = calloc(1, sizeof(vg_notification_t));
    if (!notif)
        return 0;

    notif->id = id;
    notif->type = type;
    notif->title = title ? vg_strdup(title) : NULL;
    notif->message = message ? vg_strdup(message) : NULL;
    notif->duration_ms = duration_ms;
    notif->created_at = 0; // Sentinel — populated on first manager_update tick.
    notif->action_label = action_label ? vg_strdup(action_label) : NULL;
    notif->action_callback = action_callback;
    notif->action_user_data = user_data;
    notif->opacity = 0.0f;
    notif->slide_progress = 0.0f;
    notif->dismiss_started_at = 0;
    notif->dismissed = false;
    if ((title && !notif->title) || (message && !notif->message) ||
        (action_label && !notif->action_label)) {
        free_notification(notif);
        return 0;
    }

    // Add to array
    if (mgr->notification_count >= mgr->notification_capacity) {
        size_t new_cap = mgr->notification_capacity * 2;
        if (new_cap < 8)
            new_cap = 8;
        if (new_cap <= mgr->notification_capacity ||
            new_cap > SIZE_MAX / sizeof(vg_notification_t *)) {
            free_notification(notif);
            return 0;
        }
        vg_notification_t **new_notifs =
            realloc(mgr->notifications, new_cap * sizeof(vg_notification_t *));
        if (!new_notifs) {
            free_notification(notif);
            return 0;
        }
        mgr->notifications = new_notifs;
        mgr->notification_capacity = new_cap;
    }

    mgr->notifications[mgr->notification_count++] = notif;
    mgr->base.needs_paint = true;

    return notif->id;
}

/// @brief Begin the fade-out animation for the notification with the given id.
///
/// @param mgr Manager containing the notification; may be NULL (no-op).
/// @param id  Notification ID returned by vg_notification_show*; 0 is ignored.
void vg_notification_dismiss(vg_notification_manager_t *mgr, uint32_t id) {
    if (!mgr)
        return;

    for (size_t i = 0; i < mgr->notification_count; i++) {
        if (mgr->notifications[i] && mgr->notifications[i]->id == id) {
            notification_request_dismiss(mgr->notifications[i], 0);
            mgr->base.needs_paint = true;
            return;
        }
    }
}

/// @brief Begin the fade-out animation for every active notification.
///
/// @param mgr Manager to clear; may be NULL (no-op).
void vg_notification_dismiss_all(vg_notification_manager_t *mgr) {
    if (!mgr)
        return;

    for (size_t i = 0; i < mgr->notification_count; i++) {
        if (mgr->notifications[i]) {
            notification_request_dismiss(mgr->notifications[i], 0);
        }
    }
    mgr->base.needs_paint = true;
}

/// @brief Set which screen corner (or centre) new notifications stack from.
///
/// @param mgr      Manager to configure; may be NULL (no-op).
/// @param position VG_NOTIFICATION_TOP_RIGHT, _TOP_LEFT, _BOTTOM_RIGHT, _BOTTOM_LEFT, _TOP_CENTER,
/// or _BOTTOM_CENTER.
void vg_notification_manager_set_position(vg_notification_manager_t *mgr,
                                          vg_notification_position_t position) {
    if (!mgr)
        return;
    mgr->position = position;
    mgr->base.needs_paint = true;
}

/// @brief Set the font and body size; title_font_size is set to size + 2.
///
/// @param mgr  Manager to configure; may be NULL (no-op).
/// @param font Font for body and title text.
/// @param size Body point size (title rendered at size + 2).
void vg_notification_manager_set_font(vg_notification_manager_t *mgr, vg_font_t *font, float size) {
    if (!mgr)
        return;
    if (mgr->font == font && mgr->font_size == size && mgr->title_font_size == size + 2)
        return;
    mgr->font = font;
    mgr->font_size = size;
    mgr->title_font_size = size + 2;
    mgr->base.needs_paint = true;
}
