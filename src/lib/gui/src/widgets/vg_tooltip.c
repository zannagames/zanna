//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements tooltip presentation and the process-wide hover manager.
/// @details Tooltip widgets measure and paint wrapped text in a transient,
///          noninteractive surface. The global manager reuses one lazily
///          allocated tooltip, tracks the hovered widget with borrowed
///          pointers, and exposes scheduler deadlines for delayed showing,
///          delayed hiding, and optional duration-based dismissal.
///
///          Widget destruction and subtree hiding must notify the manager so
///          borrowed hover and anchor pointers cannot outlive their targets.
///          Tooltip visibility is mirrored in both the specialized
///          `is_visible` field and the base widget's `visible` state.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_tooltip.c
// Purpose: Tooltip widget and global tooltip manager. Tooltips appear after a
//          configurable hover delay, follow the cursor or anchor to a widget,
//          and disappear on mouse-leave after a hide delay.
// Key invariants:
//   - g_tooltip_manager is a process-global singleton; vg_tooltip_manager_get
//     always returns a pointer to it.
//   - pending_show == true means we are counting down show_delay_ms before
//     calling vg_tooltip_show_at; the manager drives this in its update tick.
//   - active_tooltip is lazy-allocated on first hover and reused thereafter
//     (text and position are updated in-place rather than recreated).
//   - tooltip_widget_is_descendant_of walks the parent chain, so hiding a
//     container also clears tooltips anchored to its descendants.
//   - is_visible and base.visible are kept in sync.
// Ownership/Lifetime:
//   - active_tooltip is owned by the manager; never freed explicitly (it
//     outlives all tooltips since the manager is global).
//   - tooltip->text is heap-allocated by vg_tooltip_set_text and freed in
//     tooltip_destroy.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_widget.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void tooltip_destroy(vg_widget_t *widget);
static void tooltip_measure(vg_widget_t *widget, float available_width, float available_height);
static void tooltip_paint(vg_widget_t *widget, void *canvas);
static uint32_t tooltip_apply_alpha(uint32_t color, uint32_t backdrop);
static void tooltip_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
static void tooltip_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);

/// @brief Test whether a widget belongs to an ancestor's subtree.
/// @param widget Candidate descendant; may be `NULL`.
/// @param ancestor Candidate ancestor; may be `NULL`.
/// @return `true` when @p widget is @p ancestor or reaches it by following
///         parent links.
static bool tooltip_widget_is_descendant_of(const vg_widget_t *widget,
                                            const vg_widget_t *ancestor) {
    for (const vg_widget_t *current = widget; current; current = current->parent) {
        if (current == ancestor)
            return true;
    }
    return false;
}

/// @brief Duplicate an exact byte range as a null-terminated string.
/// @param text Source buffer containing at least @p len readable bytes.
/// @param len Number of bytes to copy, excluding the added terminator.
/// @return Newly allocated string owned by the caller, or `NULL` on allocation
///         failure.
static char *tooltip_dup_range(const char *text, size_t len) {
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Wrap tooltip text into measured display lines.
/// @details Explicit newlines are honored, spaces and tabs are preferred break
///          points, and an over-wide segment advances by at least one byte.
///          When line storage is requested, both the pointer array and each
///          string are heap allocated for the caller. A measurement-only call
///          avoids retaining line allocations.
/// @param tooltip Tooltip supplying text, font, padding, and maximum width.
/// @param out_lines Optional destination for the allocated line array.
/// @param out_max_width Optional destination for the widest measured line.
/// @return Number of logical display lines; returns at least one after valid
///         tooltip/font input and zero when required input is absent.
static int tooltip_wrap_text(vg_tooltip_t *tooltip, char ***out_lines, float *out_max_width) {
    if (out_lines)
        *out_lines = NULL;
    if (out_max_width)
        *out_max_width = 0.0f;
    if (!tooltip || !tooltip->text || !tooltip->font)
        return 0;

    float max_text_width = 0.0f;
    if (tooltip->max_width > tooltip->padding * 2) {
        max_text_width = (float)tooltip->max_width - (float)tooltip->padding * 2.0f;
    }

    int cap = 4;
    int count = 0;
    char **lines = out_lines ? (char **)calloc((size_t)cap, sizeof(char *)) : NULL;
    const char *text = tooltip->text;
    size_t text_len = strlen(text);
    size_t start = 0;

    while (start <= text_len) {
        if (text[start] == '\0') {
            if (count == 0 && out_lines && lines) {
                lines[count++] = vg_strdup("");
            }
            break;
        }

        size_t line_end = start;
        size_t best_end = start;
        size_t best_next = start;
        float best_width = 0.0f;
        bool found_break = false;

        while (line_end < text_len && text[line_end] != '\n') {
            size_t candidate_end = line_end + 1;
            char *candidate = tooltip_dup_range(text + start, candidate_end - start);
            if (!candidate)
                break;

            vg_text_metrics_t metrics = {0};
            vg_font_measure_text(tooltip->font, tooltip->font_size, candidate, &metrics);
            free(candidate);

            if (max_text_width > 0.0f && metrics.width > max_text_width) {
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
            char *candidate = tooltip_dup_range(text + start, best_end - start);
            if (candidate) {
                vg_text_metrics_t metrics = {0};
                vg_font_measure_text(tooltip->font, tooltip->font_size, candidate, &metrics);
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
                if (!new_lines)
                    break;
                memset(new_lines + cap, 0, (size_t)(new_cap - cap) * sizeof(char *));
                lines = new_lines;
                cap = new_cap;
            }
            lines[count] = tooltip_dup_range(text + start, best_end - start);
            if (!lines[count])
                break;
            count++;
        } else {
            count++;
        }

        if (best_width > 0.0f && out_max_width && best_width > *out_max_width)
            *out_max_width = best_width;

        size_t prev_start = start;
        start = best_next;
        while (text[start] == ' ' || text[start] == '\t')
            start++;
        // Progress guard: if no advance happened, force one. Without this the
        // loop can spin forever on input where best_next == prev_start (e.g. a
        // whitespace-only suffix that wraps into a zero-length segment).
        if (start <= prev_start) {
            if (text[start] == '\0')
                break;
            start = prev_start + 1;
        }
        if (text[start] == '\n') {
            start++;
            if (out_lines && lines) {
                if (count >= cap) {
                    int new_cap = cap * 2;
                    char **new_lines = (char **)realloc(lines, (size_t)new_cap * sizeof(char *));
                    if (!new_lines)
                        break;
                    memset(new_lines + cap, 0, (size_t)(new_cap - cap) * sizeof(char *));
                    lines = new_lines;
                    cap = new_cap;
                }
                lines[count] = vg_strdup("");
                if (!lines[count])
                    break;
                count++;
            } else {
                count++;
            }
        }
    }

    if (out_lines) {
        *out_lines = lines;
    } else if (lines) {
        for (int i = 0; i < count; i++)
            free(lines[i]);
        free(lines);
    }
    return count > 0 ? count : 1;
}

/// @brief Resolve a packed color's alpha against an opaque backdrop.
/// @details Alpha values of zero and 255 are treated as already-resolved RGB
///          for compatibility with theme colors; intermediate alpha values are
///          blended over @p backdrop.
/// @param color Packed ARGB-style value with alpha in the high byte.
/// @param backdrop Packed RGB backdrop.
/// @return Fully resolved 24-bit RGB color.
static uint32_t tooltip_apply_alpha(uint32_t color, uint32_t backdrop) {
    uint8_t alpha = (uint8_t)(color >> 24);
    uint32_t rgb = color & 0x00FFFFFFu;
    if (alpha == 0 || alpha == 0xFF)
        return rgb;
    return vg_color_blend(backdrop, rgb, (float)alpha / 255.0f);
}

/// @brief Fill a tooltip rounded rectangle through the shared drawing core.
/// @param win Destination graphics window.
/// @param x Left edge in framebuffer coordinates.
/// @param y Top edge in framebuffer coordinates.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param radius Requested corner radius.
/// @param color Packed fill color.
static void tooltip_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_fill(win, (float)x, (float)y, (float)w, (float)h, (float)radius, color);
}

/// @brief Stroke a one-unit tooltip rounded-rectangle border.
/// @param win Destination graphics window.
/// @param x Left edge in framebuffer coordinates.
/// @param y Top edge in framebuffer coordinates.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param radius Requested corner radius.
/// @param color Packed border color.
static void tooltip_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_stroke(
        win, (float)x, (float)y, (float)w, (float)h, (float)radius, 1.0f, color);
}

//=============================================================================
// Tooltip VTable
//=============================================================================

static vg_widget_vtable_t g_tooltip_vtable = {.destroy = tooltip_destroy,
                                              .measure = tooltip_measure,
                                              .arrange = NULL,
                                              .paint = tooltip_paint,
                                              .handle_event = NULL,
                                              .can_focus = NULL,
                                              .on_focus = NULL};

//=============================================================================
// Global Tooltip Manager
//=============================================================================

static vg_tooltip_manager_t g_tooltip_manager = {0};

/// @brief Read the platform monotonic clock in milliseconds.
/// @return Milliseconds from the platform-specific monotonic epoch.
static uint64_t tooltip_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/// @brief Access the process-wide tooltip manager.
/// @return Stable pointer to the zero-initialized global manager singleton.
vg_tooltip_manager_t *vg_tooltip_manager_get(void) {
    return &g_tooltip_manager;
}

//=============================================================================
// Tooltip Implementation
//=============================================================================

/// @brief Create a tooltip widget with theme-derived defaults (500 ms show delay, cursor-follow
/// mode).
///
/// @details The returned widget is initially invisible and borrows the current
///          theme's regular font. Text storage remains empty until assigned by
///          `vg_tooltip_set_text()`.
/// @return Newly allocated invisible tooltip, or NULL on allocation failure.
vg_tooltip_t *vg_tooltip_create(void) {
    vg_tooltip_t *tooltip = calloc(1, sizeof(vg_tooltip_t));
    if (!tooltip)
        return NULL;

    vg_widget_init(&tooltip->base, VG_WIDGET_CUSTOM, &g_tooltip_vtable);

    vg_theme_t *theme = vg_theme_get_current();

    // Defaults
    tooltip->show_delay_ms = 500;
    tooltip->hide_delay_ms = 100;
    tooltip->duration_ms = 0; // Stay until leave

    tooltip->position_mode = VG_TOOLTIP_FOLLOW_CURSOR;
    tooltip->offset_x = 10;
    tooltip->offset_y = 20;

    tooltip->max_width = 300;
    tooltip->padding = 8;
    tooltip->corner_radius = 6;
    tooltip->bg_color = 0xE0222A36;
    tooltip->text_color = 0x00FFFFFF;
    tooltip->border_color = 0xCC66758A;

    tooltip->font = theme->typography.font_regular;
    tooltip->font_size = theme->typography.size_small;
    tooltip->is_visible = false;

    return tooltip;
}

/// @brief Release tooltip-owned text during base-widget destruction.
/// @param widget Tooltip base widget being destroyed.
static void tooltip_destroy(vg_widget_t *widget) {
    vg_tooltip_t *tooltip = (vg_tooltip_t *)widget;
    free(tooltip->text);
}

/// @brief Destroy the tooltip widget and free its text string.
///
/// @details Destruction is delegated through the base widget so the tooltip
///          vtable releases specialized resources.
/// @param tooltip Tooltip to destroy; may be NULL (no-op).
void vg_tooltip_destroy(vg_tooltip_t *tooltip) {
    if (!tooltip)
        return;
    vg_widget_destroy(&tooltip->base);
}

/// @brief Measure the tooltip from wrapped text, font metrics, and padding.
/// @details The external available-size constraints are intentionally ignored;
///          `max_width` supplies the text constraint. Missing text or font
///          produces a zero-sized tooltip.
/// @param widget Tooltip base widget receiving measured dimensions.
/// @param available_width Parent width constraint; unused.
/// @param available_height Parent height constraint; unused.
static void tooltip_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_tooltip_t *tooltip = (vg_tooltip_t *)widget;
    (void)available_width;
    (void)available_height;

    if (!tooltip->text || !tooltip->font) {
        widget->measured_width = 0;
        widget->measured_height = 0;
        return;
    }

    float max_line_width = 0.0f;
    int line_count = tooltip_wrap_text(tooltip, NULL, &max_line_width);
    vg_font_metrics_t font_metrics = {0};
    vg_font_get_metrics(tooltip->font, tooltip->font_size, &font_metrics);
    float line_height =
        font_metrics.line_height > 0 ? (float)font_metrics.line_height : tooltip->font_size;

    widget->measured_width = max_line_width + (float)tooltip->padding * 2.0f;
    if (tooltip->max_width > 0 && widget->measured_width > (float)tooltip->max_width) {
        widget->measured_width = (float)tooltip->max_width;
    }
    widget->measured_height = line_height * (float)line_count + (float)tooltip->padding * 2.0f;
}

/// @brief Paint a visible tooltip and its wrapped text.
/// @details Theme elevation supplies the shadow, translucent configured colors
///          are resolved against theme backdrops, and text is clipped to the
///          padded content rectangle. Temporary wrapped-line storage is freed
///          before returning.
/// @param widget Tooltip base widget to paint.
/// @param canvas Graphics canvas represented by a `vgfx_window_t`.
static void tooltip_paint(vg_widget_t *widget, void *canvas) {
    vg_tooltip_t *tooltip = (vg_tooltip_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    if (!tooltip->is_visible || !tooltip->text)
        return;

    vgfx_window_t win = (vgfx_window_t)canvas;
    int32_t x = (int32_t)widget->x;
    int32_t y = (int32_t)widget->y;
    int32_t w = (int32_t)widget->measured_width;
    int32_t h = (int32_t)widget->measured_height;
    int32_t radius = (int32_t)tooltip->corner_radius;
    uint32_t bg =
        tooltip_apply_alpha(tooltip->bg_color, theme ? theme->colors.bg_primary : 0x00202020u);
    uint32_t border = tooltip_apply_alpha(tooltip->border_color,
                                          theme ? theme->colors.border_primary : 0x00555555u);

    vg_elevation_t tt_el = theme ? theme->elevation.level2 : (vg_elevation_t){10.0f, 0, 4, 64};
    vg_draw_round_rect_shadow(win,
                              (float)x,
                              (float)y,
                              (float)w,
                              (float)h,
                              (float)radius,
                              tt_el.blur,
                              tt_el.dx,
                              tt_el.dy,
                              tt_el.alpha,
                              theme ? theme->elevation.shadow_rgb : 0x000000u);
    tooltip_fill_round_rect(win, x, y, w, h, radius, bg);
    if (w > radius * 2)
        vgfx_fill_rect(win, x + radius, y + 1, w - radius * 2, 1, vg_color_lighten(bg, 0.08f));
    tooltip_stroke_round_rect(win, x, y, w, h, radius, border);

    if (!tooltip->font || !tooltip->text[0])
        return;

    char **lines = NULL;
    float ignored_width = 0.0f;
    int line_count = tooltip_wrap_text(tooltip, &lines, &ignored_width);
    vg_font_metrics_t font_metrics = {0};
    vg_font_get_metrics(tooltip->font, tooltip->font_size, &font_metrics);
    float line_height =
        font_metrics.line_height > 0 ? (float)font_metrics.line_height : tooltip->font_size;
    float text_x = widget->x + (float)tooltip->padding;
    float text_y = widget->y + (float)tooltip->padding + (float)font_metrics.ascent;
    int32_t clip_x = x + (int32_t)tooltip->padding;
    int32_t clip_y = y + (int32_t)tooltip->padding;
    int32_t clip_w = w - (int32_t)tooltip->padding * 2;
    int32_t clip_h = h - (int32_t)tooltip->padding * 2;

    if (clip_w > 0 && clip_h > 0)
        vgfx_set_clip(win, clip_x, clip_y, clip_w, clip_h);

    for (int i = 0; i < line_count; i++) {
        const char *line = (lines && lines[i]) ? lines[i] : "";
        vg_font_draw_text(canvas,
                          tooltip->font,
                          tooltip->font_size,
                          text_x,
                          text_y + (float)i * line_height,
                          line,
                          tooltip->text_color);
    }

    if (clip_w > 0 && clip_h > 0)
        vgfx_clear_clip(win);

    if (lines) {
        for (int i = 0; i < line_count; i++)
            free(lines[i]);
        free(lines);
    }
}

/// @brief Replace the tooltip's display text, invalidating layout.
///
/// @details The input is copied before the previous allocation is released, so
///          allocation failure leaves the existing text unchanged.
/// @param tooltip Tooltip to modify; may be NULL (no-op).
/// @param text    New text string, duplicated internally; may be NULL to clear.
void vg_tooltip_set_text(vg_tooltip_t *tooltip, const char *text) {
    if (!tooltip)
        return;

    char *copy = NULL;
    if (text) {
        copy = vg_strdup(text);
        if (!copy)
            return;
    }

    free(tooltip->text);
    tooltip->text = copy;
    tooltip->base.needs_layout = true;
    tooltip->base.needs_paint = true;
}

/// @brief Show the tooltip, positioning it at (x, y) plus offset or anchored to anchor_widget.
///
/// @details Runs a measure pass, computes screen position (clamped to the root widget's bounds
///          in anchor mode), marks the tooltip visible, and stamps `show_timer`
///          on the transition from hidden to visible. Anchored tooltips move
///          above their anchor when they would extend below the root.
///
/// @param tooltip Tooltip to show; may be NULL (no-op).
/// @param x       Cursor or anchor X in screen coordinates (used in follow-cursor mode).
/// @param y       Cursor or anchor Y in screen coordinates.
void vg_tooltip_show_at(vg_tooltip_t *tooltip, int x, int y) {
    if (!tooltip)
        return;

    bool was_visible = tooltip->is_visible;
    vg_widget_measure(&tooltip->base, 0.0f, 0.0f);
    if (tooltip->position_mode == VG_TOOLTIP_ANCHOR_WIDGET && tooltip->anchor_widget) {
        float ax = 0.0f, ay = 0.0f, aw = 0.0f, ah = 0.0f;
        vg_widget_get_screen_bounds(tooltip->anchor_widget, &ax, &ay, &aw, &ah);
        tooltip->base.x = ax + (float)tooltip->offset_x;
        tooltip->base.y = ay + ah + (float)tooltip->offset_y;

        vg_widget_t *root = tooltip->anchor_widget;
        while (root->parent)
            root = root->parent;
        float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
        vg_widget_get_screen_bounds(root, &vx, &vy, &vw, &vh);
        if (vw > 0.0f && tooltip->base.x + tooltip->base.measured_width > vx + vw)
            tooltip->base.x = vx + vw - tooltip->base.measured_width - 2.0f;
        if (vh > 0.0f && tooltip->base.y + tooltip->base.measured_height > vy + vh)
            tooltip->base.y = ay - tooltip->base.measured_height - (float)tooltip->offset_y;
        if (tooltip->base.x < vx)
            tooltip->base.x = vx;
        if (tooltip->base.y < vy)
            tooltip->base.y = vy;
    } else {
        tooltip->base.x = (float)x + (float)tooltip->offset_x;
        tooltip->base.y = (float)y + (float)tooltip->offset_y;
    }
    tooltip->is_visible = true;
    tooltip->base.visible = true;
    if (!was_visible)
        tooltip->show_timer = tooltip_now_ms();
    tooltip->hide_timer = 0;
    tooltip->base.needs_paint = true;
}

/// @brief Immediately hide the tooltip.
///
/// @details Specialized and base-widget visibility flags are cleared together
///          and a repaint is requested. Timing configuration and text remain
///          available for reuse.
/// @param tooltip Tooltip to hide; may be NULL (no-op).
void vg_tooltip_hide(vg_tooltip_t *tooltip) {
    if (!tooltip)
        return;

    tooltip->is_visible = false;
    tooltip->base.visible = false;
    tooltip->base.needs_paint = true;
}

/// @brief Set the anchor widget and switch the tooltip to VG_TOOLTIP_ANCHOR_WIDGET mode.
///
/// @details The anchor is borrowed and must be cleared through tooltip-manager
///          lifecycle notifications if its widget is hidden or destroyed.
/// @param tooltip Tooltip to configure; may be NULL (no-op).
/// @param anchor  Widget to anchor below; the tooltip appears at anchor's bottom-left + offset.
void vg_tooltip_set_anchor(vg_tooltip_t *tooltip, vg_widget_t *anchor) {
    if (!tooltip)
        return;

    tooltip->anchor_widget = anchor;
    tooltip->position_mode = VG_TOOLTIP_ANCHOR_WIDGET;
}

/// @brief Configure the show delay, hide delay, and optional auto-dismiss duration.
///
/// @details Values are stored verbatim and apply to subsequent manager
///          transitions; this function does not reschedule an already pending
///          deadline.
/// @param tooltip       Tooltip to configure; may be NULL (no-op).
/// @param show_delay_ms Milliseconds to wait after hover before showing (0 = immediate).
/// @param hide_delay_ms Milliseconds to wait after mouse-leave before hiding (0 = immediate).
/// @param duration_ms   Milliseconds to stay visible before auto-hiding (0 = stay until leave).
void vg_tooltip_set_timing(vg_tooltip_t *tooltip,
                           uint32_t show_delay_ms,
                           uint32_t hide_delay_ms,
                           uint32_t duration_ms) {
    if (!tooltip)
        return;

    tooltip->show_delay_ms = show_delay_ms;
    tooltip->hide_delay_ms = hide_delay_ms;
    tooltip->duration_ms = duration_ms;
}

//=============================================================================
// Tooltip Manager Implementation
//=============================================================================

/// @brief Drive pending-show and auto-hide timers; call every frame with the current clock value.
///
/// @details A due pending hover displays the shared tooltip at the most recent
///          cursor position. For visible tooltips, an explicit leave-driven
///          hide deadline takes precedence over duration-based dismissal.
/// @param mgr    Manager to update; may be NULL (no-op).
/// @param now_ms Current monotonic time in milliseconds.
void vg_tooltip_manager_update(vg_tooltip_manager_t *mgr, uint64_t now_ms) {
    if (!mgr)
        return;

    // Check if pending show should trigger
    if (mgr->pending_show && mgr->hovered_widget) {
        if (mgr->active_tooltip &&
            now_ms >= mgr->hover_start_time + mgr->active_tooltip->show_delay_ms) {
            vg_tooltip_show_at(mgr->active_tooltip, mgr->cursor_x, mgr->cursor_y);
            mgr->pending_show = false;
        }
    }

    if (mgr->active_tooltip && mgr->active_tooltip->is_visible) {
        if (mgr->active_tooltip->hide_timer > 0 && now_ms >= mgr->active_tooltip->hide_timer) {
            vg_tooltip_hide(mgr->active_tooltip);
            mgr->active_tooltip->hide_timer = 0;
        } else if (mgr->active_tooltip->duration_ms > 0 && mgr->active_tooltip->show_timer > 0 &&
                   now_ms >= mgr->active_tooltip->show_timer + mgr->active_tooltip->duration_ms) {
            vg_tooltip_hide(mgr->active_tooltip);
        }
    }
}

/// @brief Convert an absolute tooltip timestamp into a bounded relative deadline.
/// @details Past targets are reported as immediately due, while future
///          differences too large for the signed scheduler representation are
///          saturated.
/// @param now_ms Current scheduler time.
/// @param target_ms Absolute transition time on the same clock.
/// @return Zero when due, otherwise a positive delay saturated to INT64_MAX.
static int64_t tooltip_relative_deadline_ms(uint64_t now_ms, uint64_t target_ms) {
    if (target_ms <= now_ms)
        return 0;
    uint64_t remaining = target_ms - now_ms;
    return remaining > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)remaining;
}

/// @brief Add a bounded duration to a tooltip timestamp without unsigned wraparound.
/// @details Overflow produces `UINT64_MAX`, preserving ordering without
///          wrapping a deadline into the past.
/// @param start_ms Absolute start time.
/// @param duration_ms Relative duration.
/// @return Saturating absolute timestamp.
static uint64_t tooltip_saturating_timestamp(uint64_t start_ms, uint32_t duration_ms) {
    return start_ms > UINT64_MAX - (uint64_t)duration_ms ? UINT64_MAX
                                                         : start_ms + (uint64_t)duration_ms;
}

/// @brief Return the nearest pending tooltip timer without mutating the manager.
/// @details Pending show, delayed hide, and duration expiry are considered;
///          their absolute timestamps are converted into the scheduler's
///          signed relative-delay convention.
/// @param mgr Tooltip manager to inspect; NULL has no deadline.
/// @param now_ms Current scheduler time in milliseconds.
/// @return Milliseconds until work, zero when due, or -1 when no timer is active.
int64_t vg_tooltip_manager_next_deadline_ms(const vg_tooltip_manager_t *mgr, uint64_t now_ms) {
    if (!mgr || !mgr->active_tooltip)
        return -1;

    const vg_tooltip_t *tooltip = mgr->active_tooltip;
    int64_t deadline = -1;
    if (mgr->pending_show && mgr->hovered_widget) {
        uint64_t target =
            tooltip_saturating_timestamp(mgr->hover_start_time, tooltip->show_delay_ms);
        deadline = tooltip_relative_deadline_ms(now_ms, target);
    }
    if (tooltip->is_visible && tooltip->hide_timer > 0) {
        int64_t candidate = tooltip_relative_deadline_ms(now_ms, tooltip->hide_timer);
        if (deadline < 0 || candidate < deadline)
            deadline = candidate;
    }
    if (tooltip->is_visible && tooltip->duration_ms > 0 && tooltip->show_timer > 0) {
        uint64_t target = tooltip_saturating_timestamp(tooltip->show_timer, tooltip->duration_ms);
        int64_t candidate = tooltip_relative_deadline_ms(now_ms, target);
        if (deadline < 0 || candidate < deadline)
            deadline = candidate;
    }
    return deadline;
}

/// @brief Notify the manager that the cursor is hovering over widget at screen position (x, y).
///
/// @details On widget change, hides any active tooltip and starts the show delay countdown.
///          On same-widget move, updates cursor position and re-shows if already visible.
///          Text changes restart the delay while hidden or remeasure an already
///          visible tooltip in place. The manager borrows @p widget.
///
/// @param mgr    Manager to notify; may be NULL (no-op).
/// @param widget Currently hovered widget; may be NULL (same as calling on_leave).
/// @param x      Cursor screen X.
/// @param y      Cursor screen Y.
void vg_tooltip_manager_on_hover(vg_tooltip_manager_t *mgr, vg_widget_t *widget, int x, int y) {
    if (!mgr)
        return;

    mgr->cursor_x = x;
    mgr->cursor_y = y;

    if (widget != mgr->hovered_widget) {
        // Widget changed
        if (mgr->active_tooltip) {
            vg_tooltip_hide(mgr->active_tooltip);
        }

        mgr->hovered_widget = widget;
        if (widget && widget->tooltip_text && widget->tooltip_text[0]) {
            if (!mgr->active_tooltip) {
                mgr->active_tooltip = vg_tooltip_create();
            }
            if (mgr->active_tooltip) {
                vg_tooltip_set_text(mgr->active_tooltip, widget->tooltip_text);
                mgr->active_tooltip->position_mode = VG_TOOLTIP_FOLLOW_CURSOR;
                mgr->active_tooltip->anchor_widget = NULL;
                mgr->active_tooltip->hide_timer = 0;
                mgr->pending_show = true;
            } else {
                mgr->pending_show = false;
            }
        } else {
            mgr->pending_show = false;
        }
        mgr->hover_start_time = tooltip_now_ms();
        return;
    }

    if (!widget || !widget->tooltip_text || !widget->tooltip_text[0]) {
        if (mgr->active_tooltip) {
            vg_tooltip_hide(mgr->active_tooltip);
        }
        mgr->pending_show = false;
        return;
    }

    if (!mgr->active_tooltip) {
        mgr->active_tooltip = vg_tooltip_create();
    }
    if (!mgr->active_tooltip) {
        mgr->pending_show = false;
        return;
    }

    bool text_changed =
        !mgr->active_tooltip->text || strcmp(mgr->active_tooltip->text, widget->tooltip_text) != 0;
    if (text_changed) {
        vg_tooltip_set_text(mgr->active_tooltip, widget->tooltip_text);
        if (mgr->active_tooltip->is_visible) {
            vg_tooltip_show_at(mgr->active_tooltip, mgr->cursor_x, mgr->cursor_y);
            mgr->pending_show = false;
        } else {
            mgr->hover_start_time = tooltip_now_ms();
            mgr->pending_show = true;
        }
    } else if (mgr->active_tooltip->is_visible &&
               mgr->active_tooltip->position_mode == VG_TOOLTIP_FOLLOW_CURSOR) {
        mgr->active_tooltip->hide_timer = 0;
        vg_tooltip_show_at(mgr->active_tooltip, mgr->cursor_x, mgr->cursor_y);
    } else if (!mgr->active_tooltip->is_visible) {
        mgr->active_tooltip->hide_timer = 0;
        mgr->hover_start_time = tooltip_now_ms();
        if (mgr->active_tooltip->show_delay_ms == 0) {
            mgr->pending_show = false;
            vg_tooltip_show_at(mgr->active_tooltip, mgr->cursor_x, mgr->cursor_y);
        } else {
            mgr->pending_show = true;
        }
    }
}

/// @brief Notify the manager that the cursor has left the hovered widget, starting the hide delay.
///
/// @details A visible tooltip either receives an absolute hide deadline or is
///          hidden immediately. Pending shows are canceled and the borrowed
///          hovered-widget pointer is cleared.
/// @param mgr Manager to notify; may be NULL (no-op).
void vg_tooltip_manager_on_leave(vg_tooltip_manager_t *mgr) {
    if (!mgr)
        return;

    if (mgr->active_tooltip) {
        if (mgr->active_tooltip->is_visible && mgr->active_tooltip->hide_delay_ms > 0) {
            mgr->active_tooltip->hide_timer =
                tooltip_now_ms() + (uint64_t)mgr->active_tooltip->hide_delay_ms;
        } else {
            vg_tooltip_hide(mgr->active_tooltip);
        }
    }

    mgr->hovered_widget = NULL;
    mgr->pending_show = false;
}

/// @brief Clear dangling pointers to widget in the global manager after the widget is destroyed.
///
/// @details Direct hover and anchor references are cleared, their active
///          tooltip is hidden, and any pending show is canceled. Callers must
///          notify before the widget storage becomes inaccessible.
/// @param widget Widget being destroyed; may be NULL (no-op).
void vg_tooltip_manager_widget_destroyed(vg_widget_t *widget) {
    if (!widget)
        return;

    vg_tooltip_manager_t *mgr = &g_tooltip_manager;
    if (mgr->hovered_widget == widget) {
        if (mgr->active_tooltip) {
            vg_tooltip_hide(mgr->active_tooltip);
        }
        mgr->hovered_widget = NULL;
        mgr->pending_show = false;
    }
    if (mgr->active_tooltip && mgr->active_tooltip->anchor_widget == widget) {
        mgr->active_tooltip->anchor_widget = NULL;
        vg_tooltip_hide(mgr->active_tooltip);
    }
}

/// @brief Hide the active tooltip when a widget subtree containing the hovered or anchored widget
/// is hidden.
///
/// @details Parent-chain checks cover both the supplied root and any of its
///          descendants, preventing a tooltip from remaining visible for a
///          nonvisible subtree.
/// @param widget Root of the subtree being hidden; may be NULL (no-op).
void vg_tooltip_manager_widget_hidden(vg_widget_t *widget) {
    if (!widget)
        return;

    vg_tooltip_manager_t *mgr = &g_tooltip_manager;
    if (mgr->hovered_widget && tooltip_widget_is_descendant_of(mgr->hovered_widget, widget)) {
        if (mgr->active_tooltip) {
            vg_tooltip_hide(mgr->active_tooltip);
        }
        mgr->hovered_widget = NULL;
        mgr->pending_show = false;
    }
    if (mgr->active_tooltip && mgr->active_tooltip->anchor_widget &&
        tooltip_widget_is_descendant_of(mgr->active_tooltip->anchor_widget, widget)) {
        mgr->active_tooltip->anchor_widget = NULL;
        vg_tooltip_hide(mgr->active_tooltip);
    }
}

/// @brief Set the tooltip_text field on a widget; the global manager reads this on hover.
///
/// @details Nonempty input is duplicated before existing storage is released,
///          preserving the old tooltip on allocation failure. This setter does
///          not itself notify or reschedule the global manager.
/// @param widget Widget to configure; may be NULL (no-op).
/// @param text   Tooltip string, duplicated internally; NULL or empty string clears the tooltip.
void vg_widget_set_tooltip_text(vg_widget_t *widget, const char *text) {
    if (!widget)
        return;
    char *copy = NULL;
    if (text && text[0]) {
        copy = vg_strdup(text);
        if (!copy)
            return;
    }
    free(widget->tooltip_text);
    widget->tooltip_text = copy;
}
