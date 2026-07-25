//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_progressbar.c
// Purpose: ProgressBar widget implementation — determinate bar, circular ring,
//          and indeterminate animation styles with optional percentage label.
// Key invariants:
//   - value is always clamped to [0.0, 1.0]; 0.0 = empty, 1.0 = full.
//   - Indeterminate animation_phase is advanced in vg_progressbar_tick and
//     wraps at 1.0.
// Ownership/Lifetime:
//   - No heap-allocated fields beyond the widget itself.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements determinate linear and circular progress indicators plus
///        an explicitly ticked indeterminate animation.
/// @details Progress values are finite and clamped, theme colors provide default
///          tracks and fills, and optional percentage text participates in
///          measurement for linear bars.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define VG_PROGRESS_PI 3.14159265358979323846f

//=============================================================================
// Forward declarations
//=============================================================================

static void progressbar_measure(vg_widget_t *widget, float avail_w, float avail_h);
static void progressbar_arrange(vg_widget_t *widget, float x, float y, float w, float h);
static void progressbar_paint(vg_widget_t *widget, void *canvas);
static void progressbar_paint_circular(vg_widget_t *widget, void *canvas);
static void progressbar_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);

//=============================================================================
// VTable
//=============================================================================

static vg_widget_vtable_t g_progressbar_vtable = {
    .destroy = NULL,
    .measure = progressbar_measure,
    .arrange = progressbar_arrange,
    .paint = progressbar_paint,
    .handle_event = NULL,
    .can_focus = NULL,
    .on_focus = NULL,
};

//=============================================================================
// VTable Implementations
//=============================================================================

/// @brief VTable measure: sizes the bar to 100 px wide and the taller of 8 px, the percentage label
/// height, or 32% of theme input height.
/// @param widget Progress widget base to measure.
/// @param avail_w Offered width, unused before constraints.
/// @param avail_h Offered height, unused before constraints.
static void progressbar_measure(vg_widget_t *widget, float avail_w, float avail_h) {
    vg_progressbar_t *pb = (vg_progressbar_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    (void)avail_w;
    (void)avail_h;
    if (pb->style == VG_PROGRESS_CIRCULAR) {
        widget->measured_width = 48.0f;
        widget->measured_height = 48.0f;
        vg_widget_apply_constraints(widget);
        return;
    }
    widget->measured_width = 100.0f;
    widget->measured_height = 8.0f;
    if (pb->show_percentage && pb->font) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(pb->font, pb->font_size, &metrics);
        float text_height = (float)metrics.line_height + 6.0f;
        if (text_height > widget->measured_height)
            widget->measured_height = text_height;
    } else if (theme && theme->input.height * 0.32f > widget->measured_height) {
        widget->measured_height = theme->input.height * 0.32f;
    }
    vg_widget_apply_constraints(widget);
}

/// @brief VTable arrange: stores the assigned position and dimensions; no children to arrange.
/// @param widget Progress widget base to arrange.
/// @param x Assigned left coordinate.
/// @param y Assigned top coordinate.
/// @param w Assigned width.
/// @param h Assigned height.
static void progressbar_arrange(vg_widget_t *widget, float x, float y, float w, float h) {
    widget->x = x;
    widget->y = y;
    widget->width = w;
    widget->height = h;
}

/// @brief Fills a rounded rectangle with @p radius corner arcs; degrades to a plain rect when
/// radius is zero or the rect is too small.
/// @param win Target graphics window.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param radius Corner radius.
/// @param color Packed fill color.
static void progressbar_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_fill(win, (float)x, (float)y, (float)w, (float)h, (float)radius, color);
}

/// @brief VTable paint: draws the track, then the fill (determinate bar or indeterminate sliding
/// block), and optionally the percentage label.
/// @param widget Arranged progress widget base to paint.
/// @param canvas Backend canvas used for primitives and optional text.
static void progressbar_paint(vg_widget_t *widget, void *canvas) {
    vg_progressbar_t *pb = (vg_progressbar_t *)widget;
    vgfx_window_t win = (vgfx_window_t)canvas;
    vg_theme_t *theme = vg_theme_get_current();
    int32_t x = (int32_t)widget->x, y = (int32_t)widget->y;
    int32_t w = (int32_t)widget->width, h = (int32_t)widget->height;

    if (pb->style == VG_PROGRESS_CIRCULAR) {
        progressbar_paint_circular(widget, canvas);
        return;
    }

    uint32_t track_color =
        pb->track_color
            ? pb->track_color
            : vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.45f);
    uint32_t fill_color = pb->fill_color ? pb->fill_color : theme->colors.accent_primary;
    int32_t radius = (int32_t)pb->corner_radius;
    if (radius < 2)
        radius = 2;

    progressbar_fill_round_rect(win, x, y, w, h, radius, track_color);
    vgfx_rect(win, x, y, w, h, theme->colors.border_secondary);
    if (w > 2 && h > 2) {
        vgfx_fill_rect(win, x + 1, y + 1, w - 2, 1, vg_color_lighten(track_color, 0.08f));
    }

    if (pb->style == VG_PROGRESS_BAR) {
        float clamped = pb->value < 0.0f ? 0.0f : (pb->value > 1.0f ? 1.0f : pb->value);
        int32_t fill_w = (int32_t)(clamped * (float)w);
        if (fill_w > 0)
            progressbar_fill_round_rect(win, x, y, fill_w, h, radius, fill_color);
    } else if (pb->style == VG_PROGRESS_INDETERMINATE) {
        float phase = pb->animation_phase - (int)pb->animation_phase;
        int32_t block_w = w / 4;
        int32_t block_x = x + (int32_t)(phase * (float)(w + block_w)) - block_w;
        if (block_x < x)
            block_x = x;
        int32_t block_end = block_x + block_w;
        if (block_end > x + w)
            block_end = x + w;
        if (block_end > block_x)
            progressbar_fill_round_rect(
                win, block_x, y, block_end - block_x, h, radius, fill_color);
    }

    if (pb->show_percentage && pb->font && pb->style == VG_PROGRESS_BAR) {
        char buf[8];
        int pct = (int)(pb->value * 100.0f + 0.5f);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        vg_text_metrics_t text_metrics = {0};
        vg_font_measure_text(pb->font, pb->font_size, buf, &text_metrics);
        vg_font_metrics_t font_metrics = {0};
        vg_font_get_metrics(pb->font, pb->font_size, &font_metrics);
        float text_x = widget->x + (widget->width - text_metrics.width) * 0.5f;
        float text_y = widget->y +
                       (widget->height - (font_metrics.ascent - font_metrics.descent)) * 0.5f +
                       font_metrics.ascent;
        uint32_t text_color = pct >= 50 ? 0x00FFFFFF : theme->colors.fg_primary;
        int32_t clip_w = w - 4;
        int32_t clip_h = h - 2;
        if (clip_w > 0 && clip_h > 0) {
            vgfx_set_clip(win, x + 2, y + 1, clip_w, clip_h);
            vg_font_draw_text(canvas, pb->font, pb->font_size, text_x, text_y, buf, text_color);
            vgfx_clear_clip(win);
        }
    }
}

/// @brief Draw a determinate circular/ring progress indicator.
/// @details The track uses concentric circle strokes while the value arc is
///          approximated by filled dots over 96 angular segments.
/// @param widget Arranged circular progress widget base.
/// @param canvas Backend canvas used for ring and percentage rendering.
static void progressbar_paint_circular(vg_widget_t *widget, void *canvas) {
    vg_progressbar_t *pb = (vg_progressbar_t *)widget;
    vgfx_window_t win = (vgfx_window_t)canvas;
    vg_theme_t *theme = vg_theme_get_current();
    int32_t x = (int32_t)widget->x, y = (int32_t)widget->y;
    int32_t w = (int32_t)widget->width, h = (int32_t)widget->height;
    int32_t size = w < h ? w : h;
    if (size <= 4)
        return;

    uint32_t track_color =
        pb->track_color
            ? pb->track_color
            : vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.45f);
    uint32_t fill_color = pb->fill_color ? pb->fill_color : theme->colors.accent_primary;
    int32_t cx = x + w / 2;
    int32_t cy = y + h / 2;
    int32_t thickness = size / 10;
    if (thickness < 3)
        thickness = 3;
    if (thickness > 10)
        thickness = 10;
    int32_t radius = size / 2 - thickness;
    if (radius <= 1)
        return;

    for (int32_t i = 0; i < thickness; i++) {
        int32_t r = radius - thickness / 2 + i;
        if (r > 0)
            vgfx_circle(win, cx, cy, r, track_color);
    }

    float clamped = pb->value < 0.0f ? 0.0f : (pb->value > 1.0f ? 1.0f : pb->value);
    int total_segments = 96;
    int fill_segments = (int)(clamped * (float)total_segments + 0.5f);
    int dot_radius = thickness / 2;
    if (dot_radius < 1)
        dot_radius = 1;
    if (clamped > 0.0f) {
        if (fill_segments < 1)
            fill_segments = 1;
        for (int i = 0; i <= fill_segments; i++) {
            float t = (float)i / (float)total_segments;
            float angle = -VG_PROGRESS_PI * 0.5f + t * VG_PROGRESS_PI * 2.0f;
            int32_t px = cx + (int32_t)roundf(cosf(angle) * (float)radius);
            int32_t py = cy + (int32_t)roundf(sinf(angle) * (float)radius);
            vgfx_fill_circle(win, px, py, dot_radius, fill_color);
        }
    }

    if (pb->show_percentage && pb->font) {
        char buf[8];
        int pct = (int)(clamped * 100.0f + 0.5f);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        vg_text_metrics_t text_metrics = {0};
        vg_font_measure_text(pb->font, pb->font_size, buf, &text_metrics);
        vg_font_metrics_t font_metrics = {0};
        vg_font_get_metrics(pb->font, pb->font_size, &font_metrics);
        float text_x = widget->x + (widget->width - text_metrics.width) * 0.5f;
        float text_y = widget->y +
                       (widget->height - (font_metrics.ascent - font_metrics.descent)) * 0.5f +
                       font_metrics.ascent;
        vg_font_draw_text(
            canvas, pb->font, pb->font_size, text_x, text_y, buf, theme->colors.fg_primary);
    }
}

/// @brief Create a progress bar widget.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_progressbar_t, or NULL on allocation failure.
vg_progressbar_t *vg_progressbar_create(vg_widget_t *parent) {
    vg_progressbar_t *progress = calloc(1, sizeof(vg_progressbar_t));
    if (!progress)
        return NULL;

    vg_widget_init(&progress->base, VG_WIDGET_PROGRESS, &g_progressbar_vtable);

    // Default values
    progress->value = 0;
    progress->style = VG_PROGRESS_BAR;

    // Default appearance
    vg_theme_t *theme = vg_theme_get_current();
    progress->track_color =
        vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.45f);
    progress->fill_color = theme->colors.accent_primary;
    progress->corner_radius = 4;
    progress->font = theme->typography.font_regular;
    progress->font_size = theme->typography.size_small;

    if (parent) {
        vg_widget_add_child(parent, &progress->base);
    }

    return progress;
}

/// @brief Set the progress value; clamped to [0.0, 1.0].
///
/// @param progress The progress bar to update.
/// @param value    Progress fraction (0.0 = empty, 1.0 = full).
void vg_progressbar_set_value(vg_progressbar_t *progress, float value) {
    if (!progress)
        return;
    if (!isfinite(value))
        value = 0.0f;
    if (value < 0)
        value = 0;
    if (value > 1)
        value = 1;
    progress->value = value;
    progress->base.needs_paint = true;
}

/// @brief Return the current progress value.
///
/// @param progress The progress bar to query.
/// @return Current value in [0.0, 1.0], or 0 if progress is NULL.
float vg_progressbar_get_value(vg_progressbar_t *progress) {
    return progress ? progress->value : 0;
}

/// @brief Set the visual style of the progress indicator.
///
/// @param progress The progress bar to configure.
/// @param style    VG_PROGRESS_BAR, VG_PROGRESS_CIRCLE, or VG_PROGRESS_INDETERMINATE.
void vg_progressbar_set_style(vg_progressbar_t *progress, vg_progress_style_t style) {
    if (!progress)
        return;
    if (style < VG_PROGRESS_BAR || style > VG_PROGRESS_INDETERMINATE)
        style = VG_PROGRESS_BAR;
    progress->style = style;
    progress->base.needs_paint = true;
}

/// @brief Show or hide the percentage label drawn over the progress bar.
///
/// @param progress The progress bar to configure.
/// @param show     true to show the percentage text; false to hide it.
void vg_progressbar_show_percentage(vg_progressbar_t *progress, bool show) {
    if (!progress)
        return;
    progress->show_percentage = show;
    progress->base.needs_layout = true;
    progress->base.needs_paint = true;
}

/// @brief Set the font used to draw the optional percentage label.
///
/// @param progress The progress bar to configure.
/// @param font     Font handle; may be NULL to clear.
/// @param size     Font size in pixels; values <= 0 keep the theme default.
void vg_progressbar_set_font(vg_progressbar_t *progress, vg_font_t *font, float size) {
    if (!progress)
        return;
    progress->font = font;
    progress->font_size = size > 0.0f ? size : vg_theme_get_current()->typography.size_small;
    progress->base.needs_layout = true;
    progress->base.needs_paint = true;
}

/// @brief Advance the indeterminate animation phase; call once per frame.
///
/// @param progress The progress bar to advance.
/// @param dt       Elapsed time in seconds since the last tick. No-op for
///                 non-indeterminate styles.
void vg_progressbar_tick(vg_progressbar_t *progress, float dt) {
    if (!progress)
        return;
    if (progress->style == VG_PROGRESS_INDETERMINATE) {
        if (!isfinite(dt) || dt <= 0.0f)
            return;
        // Advance animation phase at a moderate speed; wrap at 1.0
        progress->animation_phase += dt * 0.7f;
        if (!isfinite(progress->animation_phase)) {
            progress->animation_phase = 0.0f;
        } else {
            progress->animation_phase = fmodf(progress->animation_phase, 1.0f);
            if (progress->animation_phase < 0.0f)
                progress->animation_phase += 1.0f;
        }
        progress->base.needs_paint = true;
    }
}
