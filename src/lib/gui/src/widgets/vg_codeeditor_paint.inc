//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file vg_codeeditor_paint.inc
/// @brief CodeEditor painting, gutter, selection, cursor, syntax highlight,
/// diagnostics, folds, inlay hints, and overlay rendering.
///
/// @details This include fragment is consumed only by vg_codeeditor.c and
/// separates the visual rendering path from editing, history, lifecycle, input,
/// and API-facing code.
///
// File: src/lib/gui/src/widgets/vg_codeeditor_paint.inc
// Purpose: Gutter icon rendering, fold markers, highlights, inlay hints, text painting, cursor
// drawing, and scrollbar painting.
//
// Key invariants:
//   - Included only by src/lib/gui/src/widgets/vg_codeeditor.c.
//   - Fragment order preserves the original static helper visibility and
//     vtable wiring from the monolithic source file.
//   - Public CodeEditor behavior and exported symbols remain unchanged.
//
// Ownership/Lifetime:
//   - Ownership follows vg_codeeditor_t rules from vg_ide_widgets_editor.h;
//     text lines, history, highlights, fold regions, inlay hints, and extra
//     cursor storage are freed by the lifecycle fragment.
//
// Links: src/lib/gui/src/widgets/vg_codeeditor.c,
//        src/lib/gui/include/vg_ide_widgets_editor.h
//
//===----------------------------------------------------------------------===//

/// @brief Scales and alpha-blends a raster gutter icon into the active framebuffer.
///
/// @details The source image is sampled with nearest-neighbour scaling and
/// composited directly into the canvas framebuffer. Both the framebuffer bounds
/// and the canvas clip rectangle are honored. Fully transparent pixels are
/// skipped, while fully opaque pixels take the fast replacement path.
///
/// @param canvas Destination canvas whose framebuffer receives the icon.
/// @param icon Gutter icon containing an RGBA image. A null icon, a non-image
///             icon, or an image without pixels is ignored.
/// @param dst_x Left edge of the destination rectangle in framebuffer pixels.
/// @param dst_y Top edge of the destination rectangle in framebuffer pixels.
/// @param dst_w Destination width in pixels; non-positive values are ignored.
/// @param dst_h Destination height in pixels; non-positive values are ignored.
static void codeeditor_draw_gutter_icon_image(vgfx_window_t canvas,
                                              const struct vg_gutter_icon *icon,
                                              int32_t dst_x,
                                              int32_t dst_y,
                                              int32_t dst_w,
                                              int32_t dst_h) {
    if (!icon || icon->image.type != VG_ICON_IMAGE || !icon->image.data.image.pixels ||
        dst_w <= 0 || dst_h <= 0)
        return;

    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(canvas, &fb))
        return;

    const uint8_t *src_pixels = icon->image.data.image.pixels;
    int src_w = (int)icon->image.data.image.width;
    int src_h = (int)icon->image.data.image.height;
    int clip_x = 0;
    int clip_y = 0;
    int clip_w = fb.width;
    int clip_h = fb.height;
    if (src_w <= 0 || src_h <= 0)
        return;

    (void)vgfx_get_clip(canvas, &clip_x, &clip_y, &clip_w, &clip_h);

    for (int row = 0; row < dst_h; row++) {
        int src_y = row * src_h / dst_h;
        if (src_y >= src_h)
            src_y = src_h - 1;
        int fb_y = dst_y + row;
        if (fb_y < 0 || fb_y >= fb.height || fb_y < clip_y || fb_y >= clip_y + clip_h)
            continue;

        for (int col = 0; col < dst_w; col++) {
            int src_x = col * src_w / dst_w;
            if (src_x >= src_w)
                src_x = src_w - 1;
            int fb_x = dst_x + col;
            if (fb_x < 0 || fb_x >= fb.width || fb_x < clip_x || fb_x >= clip_x + clip_w)
                continue;

            const uint8_t *src = &src_pixels[(src_y * src_w + src_x) * 4];
            uint8_t alpha = src[3];
            if (alpha == 0)
                continue;

            int fb_idx = fb_y * fb.stride + fb_x * 4;
            if (alpha == 255) {
                fb.pixels[fb_idx + 0] = src[0];
                fb.pixels[fb_idx + 1] = src[1];
                fb.pixels[fb_idx + 2] = src[2];
                fb.pixels[fb_idx + 3] = 0xFF;
                continue;
            }

            uint8_t dst_r = fb.pixels[fb_idx + 0];
            uint8_t dst_g = fb.pixels[fb_idx + 1];
            uint8_t dst_b = fb.pixels[fb_idx + 2];
            uint8_t inv_alpha = (uint8_t)(255 - alpha);
            fb.pixels[fb_idx + 0] = (uint8_t)((src[0] * alpha + dst_r * inv_alpha) / 255);
            fb.pixels[fb_idx + 1] = (uint8_t)((src[1] * alpha + dst_g * inv_alpha) / 255);
            fb.pixels[fb_idx + 2] = (uint8_t)((src[2] * alpha + dst_b * inv_alpha) / 255);
            fb.pixels[fb_idx + 3] = 0xFF;
        }
    }
}

/// @brief Draws the expansion marker for a fold region that starts on a line.
///
/// @details A folded region is represented by a centered `+`; an expanded
/// region is represented by `-`. Nothing is drawn when the line does not begin
/// a fold region or the fold gutter is unavailable.
///
/// @param editor Editor that owns the fold regions and rendering attributes.
/// @param canvas Destination drawing context.
/// @param fold_gutter_x Left edge of the fold-marker gutter.
/// @param fold_gutter_width Width available for centering the marker.
/// @param line_y Top coordinate of the displayed line.
/// @param font_metrics Metrics used to place the marker on the text baseline.
/// @param line Zero-based logical line whose fold region is queried.
static void codeeditor_draw_fold_marker(vg_codeeditor_t *editor,
                                        void *canvas,
                                        float fold_gutter_x,
                                        float fold_gutter_width,
                                        float line_y,
                                        const vg_font_metrics_t *font_metrics,
                                        int line) {
    const struct vg_fold_region *region = codeeditor_fold_region_starting_at(editor, line);
    if (!editor || !region || fold_gutter_width <= 0.0f || !font_metrics)
        return;

    char marker[2] = {region->folded ? '+' : '-', '\0'};
    vg_text_metrics_t marker_metrics = {0};
    vg_font_measure_text(editor->font, editor->font_size, marker, &marker_metrics);
    float marker_x = fold_gutter_x + (fold_gutter_width - marker_metrics.width) * 0.5f;
    float marker_y = line_y + font_metrics->ascent;
    vg_font_draw_text(canvas,
                      editor->font,
                      editor->font_size,
                      marker_x,
                      marker_y,
                      marker,
                      editor->line_number_color);
}

/// @brief Draws a continuation ellipsis after a folded line's visible text.
///
/// @details The ellipsis is positioned after @p start_col using the editor's
/// fixed character width. The helper is a no-op unless @p line begins a
/// currently folded region.
///
/// @param editor Editor that owns the fold region and text style.
/// @param canvas Destination drawing context.
/// @param content_x Horizontal origin of the editor content area.
/// @param baseline_y Text baseline for the ellipsis.
/// @param line Zero-based logical line whose fold state is queried.
/// @param display_col Display column after which the ellipsis is placed.
static void codeeditor_draw_fold_ellipsis(vg_codeeditor_t *editor,
                                          void *canvas,
                                          float content_x,
                                          float baseline_y,
                                          int line,
                                          size_t display_col) {
    const struct vg_fold_region *region = codeeditor_fold_region_starting_at(editor, line);
    if (!editor || !region || !region->folded)
        return;

    float ellipsis_x = content_x + (float)display_col * editor->char_width + 4.0f;
    vg_font_draw_text(canvas,
                      editor->font,
                      editor->font_size,
                      ellipsis_x,
                      baseline_y,
                      "...",
                      editor->line_number_color);
}

/// @brief Draws a highlight span as an underline without obscuring its text.
///
/// @param canvas Destination drawing context; null is ignored.
/// @param x Left edge of the highlighted span.
/// @param y Top edge of the containing text row.
/// @param width Width of the highlighted span; non-positive values are ignored.
/// @param line_height Height of the containing row, used to place the underline.
/// @param color Packed color used for the underline.
static void codeeditor_draw_highlight_underline(
    void *canvas, float x, float y, float width, float line_height, uint32_t color) {
    if (!canvas || width <= 0.0f || line_height <= 0.0f)
        return;

    float underline_height = line_height >= 8.0f ? 2.0f : 1.0f;
    float underline_y = y + line_height - underline_height - 1.0f;
    if (underline_y < y)
        underline_y = y;

    int32_t draw_width = (int32_t)(width + 0.5f);
    int32_t draw_height = (int32_t)(underline_height + 0.5f);
    if (draw_width < 1)
        draw_width = 1;
    if (draw_height < 1)
        draw_height = 1;

    vgfx_fill_rect((vgfx_window_t)canvas,
                   (int32_t)x,
                   (int32_t)underline_y,
                   draw_width,
                   draw_height,
                   (vgfx_color_t)color);
}

/// @brief Paints all inlay hints that belong to one displayed line segment.
///
/// @details The editor's sorted hint array is scanned from the first hint on
/// @p line. Hint columns are clamped to the line bounds and, for wrapped rows,
/// filtered to the requested segment. Each hint receives a translucent
/// background sized from its measured text followed by the hint text itself.
///
/// @param editor Editor that owns the hints, source lines, font, and colors.
/// @param canvas Destination drawing context.
/// @param line Zero-based logical line containing the segment.
/// @param segment_start Byte column at which the displayed segment begins.
/// @param segment_len Number of bytes represented by the segment.
/// @param content_x Horizontal origin of the segment's text.
/// @param row_y Top coordinate of the displayed row.
/// @param scroll_x Horizontal scroll offset to subtract from hint positions.
/// @param baseline_y Text baseline on which hint labels are drawn.
/// @param wrapped Whether columns must be translated relative to a wrapped
///                segment rather than the complete logical line.
static void codeeditor_paint_inlay_hints_for_segment(vg_codeeditor_t *editor,
                                                     void *canvas,
                                                     int line,
                                                     int segment_start,
                                                     int segment_len,
                                                     float content_x,
                                                     float row_y,
                                                     float scroll_x,
                                                     float baseline_y,
                                                     bool wrapped) {
    if (!editor || !canvas || !editor->font || editor->inlay_hint_count <= 0)
        return;

    int line_len = 0;
    if (line >= 0 && line < editor->line_count)
        line_len = (int)editor->lines[line].length;
    int segment_end = segment_start + segment_len;
    float hint_size = editor->font_size * 0.9f;
    if (hint_size <= 0.0f)
        hint_size = editor->font_size;

    int first_hint = codeeditor_first_inlay_hint_for_line(editor, line);
    if (first_hint < 0)
        return;

    for (int i = first_hint; i < editor->inlay_hint_count; i++) {
        const struct vg_inlay_hint *hint = &editor->inlay_hints[i];
        if (hint->line != line)
            break;
        if (!hint->text || hint->text[0] == '\0')
            continue;

        int col = hint->col;
        if (col < 0)
            col = 0;
        if (col > line_len)
            col = line_len;

        if (wrapped) {
            bool at_segment_end = col == line_len && line_len <= segment_end;
            if ((col < segment_start || col > segment_end) && !at_segment_end)
                continue;
        }

        size_t hint_display = codeeditor_display_columns_before_byte(
            &editor->lines[line], (size_t)col, editor->tab_width);
        size_t segment_display =
            wrapped ? codeeditor_display_columns_before_byte(
                          &editor->lines[line], (size_t)segment_start, editor->tab_width)
                    : 0u;
        if (hint_display < segment_display)
            hint_display = segment_display;
        float x = content_x + (float)(hint_display - segment_display) * editor->char_width -
                  scroll_x + 4.0f;
        vg_text_metrics_t metrics = {0};
        vg_font_measure_text(editor->font, hint_size, hint->text, &metrics);
        if (metrics.width > 0.0f) {
            uint32_t bg = (editor->current_line_bg & 0x00FFFFFFu) | 0x66000000u;
            vgfx_fill_rect((vgfx_window_t)canvas,
                           (int32_t)(x - 2.0f),
                           (int32_t)(row_y + 2.0f),
                           (int32_t)(metrics.width + 4.0f),
                           (int32_t)(editor->line_height - 4.0f),
                           bg);
        }
        vg_font_draw_text(canvas, editor->font, hint_size, x, baseline_y, hint->text, hint->color);
    }
}

/// @brief Draws faint vertical indent guides for one non-wrapped line.
///
/// @details One guide is drawn per complete indentation level from column zero
/// through the line's leading-whitespace depth. Tabs advance to the next tab
/// stop, off-screen guides are skipped, and an unset guide color falls back to
/// a muted dark grey. Wrapped continuation rows are intentionally not handled.
///
/// @param editor Editor that owns the line, tab width, scroll offset, and style.
/// @param canvas Destination drawing context.
/// @param line Zero-based logical line whose indentation is inspected.
/// @param content_x Horizontal origin of the editor content area.
/// @param line_y Top coordinate of the displayed line.
static void codeeditor_paint_indent_guides(
    vg_codeeditor_t *editor, void *canvas, int line, float content_x, float line_y) {
    if (!editor || line < 0 || line >= editor->line_count)
        return;
    const vg_code_line_t *ln = &editor->lines[line];
    const char *text = ln->text;
    if (!text)
        return;
    int tab_w = editor->tab_width > 0 ? editor->tab_width : 4;

    int cols = 0;
    for (int k = 0; k < (int)ln->length; k++) {
        char c = text[k];
        if (c == ' ')
            cols++;
        else if (c == '\t')
            cols += tab_w - (cols % tab_w);
        else
            break;
    }
    int levels = cols / tab_w;
    if (levels <= 0)
        return;

    uint32_t color = editor->indent_guide_color ? editor->indent_guide_color : 0xFF3C3C3Cu;
    for (int lvl = 0; lvl < levels; lvl++) {
        float gx = content_x + (float)(lvl * tab_w) * editor->char_width - editor->scroll_x;
        if (gx < content_x - 0.5f)
            continue; // scrolled left of the content area
        // Anti-aliased hairline (centered on the pixel) instead of a hard 1px
        // fill — softer, less "ladder"-looking between rows.
        vg_draw_line_aa((vgfx_window_t)canvas,
                        gx + 0.5f,
                        line_y,
                        gx + 0.5f,
                        line_y + editor->line_height,
                        1.0f,
                        color);
    }
}

/// @brief Determines whether a whitespace cell should receive a visible marker.
///
/// @details Boundary mode selects only leading or trailing spaces and tabs,
/// while all mode selects every whitespace cell and none mode selects nothing.
/// Factoring the policy out keeps it independent of fonts and windows.
///
/// @param ln Line whose text is inspected; may be null.
/// @param col Zero-based byte column in `ln->text`.
/// @param mode One of the `VG_WHITESPACE_*` display modes.
/// @return `true` when the indexed cell is a space or tab selected by @p mode;
///         otherwise `false`.
static bool codeeditor_whitespace_cell_visible(const vg_code_line_t *ln, int col, int mode) {
    if (mode == VG_WHITESPACE_NONE)
        return false;
    if (!ln || !ln->text || col < 0 || col >= (int)ln->length)
        return false;
    char c = ln->text[col];
    if (c != ' ' && c != '\t')
        return false;
    if (mode == VG_WHITESPACE_ALL)
        return true;
    if (mode != VG_WHITESPACE_BOUNDARY)
        return false;
    // Boundary: leading (before first non-space) or trailing (after last non-space).
    int first_non_ws = 0;
    while (first_non_ws < (int)ln->length &&
           (ln->text[first_non_ws] == ' ' || ln->text[first_non_ws] == '\t'))
        first_non_ws++;
    if (col < first_non_ws)
        return true;
    int last_non_ws = (int)ln->length - 1;
    while (last_non_ws >= 0 && (ln->text[last_non_ws] == ' ' || ln->text[last_non_ws] == '\t'))
        last_non_ws--;
    return col > last_non_ws;
}

/// @brief Paints space and tab markers for one line inside the viewport.
///
/// @details Space cells receive a small centered dot and tab cells receive a
/// thin arrow spanning to the next tab stop. Only cells selected by the
/// configured whitespace mode and intersecting the visible horizontal range
/// are drawn. Visual columns account for tab expansion and UTF-8 continuation
/// bytes so the markers remain aligned with rendered glyphs.
///
/// @param editor Editor that owns the line and whitespace-rendering settings.
/// @param canvas Destination drawing context.
/// @param line Zero-based logical line to inspect.
/// @param content_x Left edge of the editor content viewport.
/// @param content_width Width of the content viewport.
/// @param line_y Top coordinate of the displayed line.
static void codeeditor_paint_whitespace(vg_codeeditor_t *editor,
                                        void *canvas,
                                        int line,
                                        float content_x,
                                        float content_width,
                                        float line_y) {
    if (!editor || editor->render_whitespace_mode == VG_WHITESPACE_NONE)
        return;
    if (line < 0 || line >= editor->line_count)
        return;
    const vg_code_line_t *ln = &editor->lines[line];
    if (!ln->text || ln->length == 0)
        return;

    int mode = editor->render_whitespace_mode;
    int tab_w = editor->tab_width > 0 ? editor->tab_width : 4;
    float cw = editor->char_width;
    if (cw <= 0.0f)
        return;
    uint32_t color = editor->whitespace_color;
    if (!color)
        color = editor->line_number_color;
    if (!color)
        color = 0xFF5A5A5Au;

    float right_limit = content_x + content_width;
    float cy = line_y + editor->line_height * 0.5f;
    float dot_r = cw * 0.10f;
    if (dot_r < 1.0f)
        dot_r = 1.0f;

    for (int k = 0; k < (int)ln->length; k++) {
        char c = ln->text[k];
        if (((unsigned char)c & 0xC0) == 0x80)
            continue; // UTF-8 continuation byte: no new visual column
        int visual_col =
            (int)codeeditor_display_columns_before_byte(ln, (size_t)k, editor->tab_width);
        float x = content_x + (float)visual_col * cw - editor->scroll_x;
        int adv = 1;
        if (c == '\t')
            adv = tab_w - (visual_col % tab_w);

        bool marked = codeeditor_whitespace_cell_visible(ln, k, mode);
        if (marked && x >= content_x - cw && x <= right_limit) {
            if (c == ' ') {
                vg_draw_disc_fill((vgfx_window_t)canvas, x + cw * 0.5f, cy, dot_r, color);
            } else if (c == '\t') {
                float x0 = x + 2.0f;
                float x1 = x + (float)adv * cw - 2.0f;
                if (x1 > x0) {
                    vg_draw_line_aa((vgfx_window_t)canvas, x0, cy, x1, cy, 1.0f, color);
                    // Small arrowhead at the trailing edge.
                    vg_draw_line_aa(
                        (vgfx_window_t)canvas, x1, cy, x1 - 3.0f, cy - 3.0f, 1.0f, color);
                    vg_draw_line_aa(
                        (vgfx_window_t)canvas, x1, cy, x1 - 3.0f, cy + 3.0f, 1.0f, color);
                }
            }
        }
    }
}

/// @brief Draw a crisp anti-aliased caret with a soft accent glow behind it.
///
/// @details Replaces the legacy hard 2px rectangle. The glow is a centred
///          (dx=dy=0) soft shadow in the caret colour; because vg_draw caches
///          shadow silhouettes by (w,h,radius,blur) and the caret is a constant
///          size, the glow is computed once and reused every frame. The crisp
///          caret is a slightly rounded AA bar painted on top. Cost is O(1).
///
/// @param canvas Destination drawing context; null is ignored.
/// @param x Horizontal coordinate of the caret's crisp center bar.
/// @param y Top coordinate of the caret.
/// @param h Caret height; non-positive values are ignored.
/// @param color Packed color used for both the glow and crisp bar.
static void codeeditor_draw_caret(void *canvas, float x, float y, float h, uint32_t color) {
    if (!canvas || h <= 0.0f)
        return;
    const float caret_w = 2.0f;
    const float radius = 1.0f;
    vg_draw_round_rect_shadow(
        (vgfx_window_t)canvas, x - 1.0f, y, caret_w + 2.0f, h, radius, 3.0f, 0, 0, 110, color);
    vg_draw_round_rect_fill((vgfx_window_t)canvas, x, y, caret_w, h, radius, color);
}

/// @brief Paint the active-line highlight as a subtle vertical gradient with a
///        rounded accent rail on the left edge.
///
/// @details Replaces the flat fill + hard 3px bar. The band lightens slightly
///          toward the top for a gentle sense of depth; the rail is a rounded AA
///          fill in the accent colour. Drawn once per frame (the cursor's line),
///          so the gradient cost is trivial.
///
/// @param canvas Destination drawing context; null is ignored.
/// @param x Left edge of the highlight band.
/// @param y Top edge of the highlight band.
/// @param w Width of the band; non-positive values are ignored.
/// @param h Height of the band; non-positive values are ignored.
/// @param base_bg Packed base color used at the lower edge of the gradient.
/// @param accent Packed color used for the leading accent rail.
static void codeeditor_draw_current_line(
    void *canvas, float x, float y, float w, float h, uint32_t base_bg, uint32_t accent) {
    if (!canvas || w <= 0.0f || h <= 0.0f)
        return;
    uint32_t top = vg_color_lighten(base_bg, 0.05f);
    vg_draw_round_rect_gradient_v((vgfx_window_t)canvas, x, y, w, h, 0.0f, top, base_bg);
    vg_draw_round_rect_fill((vgfx_window_t)canvas, x, y, 3.0f, h, 1.0f, accent);
}

/// @brief Paints the complete code-editor widget for its vtable callback.
///
/// @details Rendering includes the background, gutters, fold and diagnostic
/// decorations, current-line and selection layers, syntax-colored text, inlay
/// hints, carets, and both scrollbars. The function derives visible logical
/// lines or wrapped rows from the current scroll state, applies a content clip,
/// and temporarily scopes font ligature shaping to the editor preference.
///
/// @param widget Code-editor base widget to paint.
/// @param canvas Destination drawing context.
static void codeeditor_paint(vg_widget_t *widget, void *canvas) {
    vg_codeeditor_t *editor = (vg_codeeditor_t *)widget;

    if (!editor->font)
        return;

    // Scope ligature shaping to this editor's preference for the duration of
    // the paint (single-threaded paint contract).
    bool ligatures_were_enabled = vg_font_ligatures_enabled();
    vg_font_set_ligatures_enabled(ligatures_were_enabled && editor->ligatures_enabled);

    float content_x = widget->x + editor->gutter_width;
    float content_width = codeeditor_content_draw_width(editor, widget);
    float total_content_height = codeeditor_total_content_height_for_width(editor, content_width);
    float visible_height = widget->height;
    float scrollbar_width = CODEEDITOR_SCROLLBAR_WIDTH;
    float line_number_gutter_width = codeeditor_line_number_gutter_width(editor);
    float fold_gutter_width = codeeditor_fold_gutter_width(editor);
    codeeditor_clamp_scroll(editor, widget);

    int first_visual_row = (int)(editor->scroll_y / editor->line_height);
    codeeditor_locate_visual_row(
        editor, content_width, first_visual_row, &editor->visible_first_line, NULL);
    editor->visible_line_count = (int)(widget->height / editor->line_height) + 2;

    // Draw background
    vgfx_fill_rect((vgfx_window_t)canvas,
                   (int32_t)widget->x,
                   (int32_t)widget->y,
                   (int32_t)widget->width,
                   (int32_t)widget->height,
                   editor->bg_color);

    // Draw gutter background
    if (editor->gutter_width > 0.0f) {
        vgfx_fill_rect((vgfx_window_t)canvas,
                       (int32_t)widget->x,
                       (int32_t)widget->y,
                       (int32_t)editor->gutter_width,
                       (int32_t)widget->height,
                       editor->gutter_bg);
    }

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(editor->font, editor->font_size, &font_metrics);
    codeeditor_sort_highlight_spans(editor);
    codeeditor_sort_inlay_hints(editor);
    int32_t clip_x = (int32_t)content_x;
    int32_t clip_y = (int32_t)widget->y;
    int32_t clip_w = (int32_t)content_width;
    int32_t clip_h = (int32_t)widget->height;

    if (clip_w > 0 && clip_h > 0) {
        vgfx_set_clip((vgfx_window_t)canvas, clip_x, clip_y, clip_w, clip_h);
    }

    if (!editor->word_wrap) {
        float row_offset = editor->scroll_y - (float)first_visual_row * editor->line_height;
        float line_y = widget->y - row_offset;
        for (int i = editor->visible_first_line;
             i >= 0 && i < editor->line_count && line_y < widget->y + widget->height;
             i = codeeditor_next_visible_line(editor, i)) {
            if (codeeditor_line_is_hidden(editor, i))
                continue;

            if (i == editor->cursor_line && (widget->state & VG_STATE_FOCUSED)) {
                codeeditor_draw_current_line(canvas,
                                             content_x,
                                             line_y,
                                             content_width,
                                             editor->line_height,
                                             editor->current_line_bg,
                                             vg_theme_get_current()->colors.accent_primary);
            }

            if (editor->show_indent_guides)
                codeeditor_paint_indent_guides(editor, canvas, i, content_x, line_y);

            if (editor->render_whitespace_mode != VG_WHITESPACE_NONE)
                codeeditor_paint_whitespace(editor, canvas, i, content_x, content_width, line_y);

            codeeditor_paint_highlights_for_line(editor,
                                                 canvas,
                                                 i,
                                                 0,
                                                 (int)editor->lines[i].length,
                                                 content_x,
                                                 line_y,
                                                 editor->scroll_x,
                                                 false);
            codeeditor_paint_pair_highlight_for_segment(editor,
                                                        canvas,
                                                        i,
                                                        0,
                                                        (int)editor->lines[i].length,
                                                        content_x,
                                                        line_y,
                                                        editor->scroll_x,
                                                        false);

            if (editor->has_selection && (widget->state & VG_STATE_FOCUSED)) {
                int sel_start_line = editor->selection.start_line;
                int sel_start_col = editor->selection.start_col;
                int sel_end_line = editor->selection.end_line;
                int sel_end_col = editor->selection.end_col;
                normalize_selection_range(
                    &sel_start_line, &sel_start_col, &sel_end_line, &sel_end_col);
                if (i >= sel_start_line && i <= sel_end_line) {
                    int col_start = (i == sel_start_line) ? sel_start_col : 0;
                    int col_end = (i == sel_end_line) ? sel_end_col : (int)editor->lines[i].length;
                    size_t display_start = codeeditor_display_columns_before_byte(
                        &editor->lines[i], (size_t)col_start, editor->tab_width);
                    size_t display_end = codeeditor_display_columns_before_byte(
                        &editor->lines[i], (size_t)col_end, editor->tab_width);
                    float sel_x =
                        content_x + (float)display_start * editor->char_width - editor->scroll_x;
                    float sel_width = (float)(display_end - display_start) * editor->char_width;
                    vgfx_fill_rect((vgfx_window_t)canvas,
                                   (int32_t)sel_x,
                                   (int32_t)line_y,
                                   (int32_t)sel_width,
                                   (int32_t)editor->line_height,
                                   editor->selection_color);
                }
            }

            if (widget->state & VG_STATE_FOCUSED) {
                for (int c = 0; c < editor->extra_cursor_count; c++) {
                    if (!editor->extra_cursors[c].has_selection)
                        continue;
                    int sel_start_line = editor->extra_cursors[c].selection.start_line;
                    int sel_start_col = editor->extra_cursors[c].selection.start_col;
                    int sel_end_line = editor->extra_cursors[c].selection.end_line;
                    int sel_end_col = editor->extra_cursors[c].selection.end_col;
                    normalize_selection_range(
                        &sel_start_line, &sel_start_col, &sel_end_line, &sel_end_col);
                    if (i < sel_start_line || i > sel_end_line)
                        continue;
                    int col_start = (i == sel_start_line) ? sel_start_col : 0;
                    int col_end = (i == sel_end_line) ? sel_end_col : (int)editor->lines[i].length;
                    size_t display_start = codeeditor_display_columns_before_byte(
                        &editor->lines[i], (size_t)col_start, editor->tab_width);
                    size_t display_end = codeeditor_display_columns_before_byte(
                        &editor->lines[i], (size_t)col_end, editor->tab_width);
                    float sel_x =
                        content_x + (float)display_start * editor->char_width - editor->scroll_x;
                    float sel_width = (float)(display_end - display_start) * editor->char_width;
                    vgfx_fill_rect((vgfx_window_t)canvas,
                                   (int32_t)sel_x,
                                   (int32_t)line_y,
                                   (int32_t)sel_width,
                                   (int32_t)editor->line_height,
                                   editor->selection_color);
                }
            }

            if (editor->lines[i].text && editor->lines[i].length > 0) {
                float text_y = line_y + font_metrics.ascent;
                highlight_line(editor, i);
                float text_x = content_x - editor->scroll_x;
                if (editor->lines[i].colors) {
                    codeeditor_draw_colored_slice(canvas,
                                                  editor->font,
                                                  editor->font_size,
                                                  text_x,
                                                  text_y,
                                                  editor->lines[i].text,
                                                  &editor->lines[i],
                                                  editor->lines[i].colors,
                                                  editor->lines[i].colors_capacity,
                                                  0,
                                                  editor->lines[i].length,
                                                  editor->char_width,
                                                  editor->tab_width,
                                                  editor->text_color);
                } else {
                    vg_font_draw_text(canvas,
                                      editor->font,
                                      editor->font_size,
                                      text_x,
                                      text_y,
                                      editor->lines[i].text,
                                      editor->text_color);
                }
                codeeditor_draw_fold_ellipsis(
                    editor,
                    canvas,
                    content_x - editor->scroll_x,
                    text_y,
                    i,
                    codeeditor_line_display_columns(&editor->lines[i], editor->tab_width));
            }
            codeeditor_paint_inlay_hints_for_segment(editor,
                                                     canvas,
                                                     i,
                                                     0,
                                                     (int)editor->lines[i].length,
                                                     content_x,
                                                     line_y,
                                                     editor->scroll_x,
                                                     line_y + font_metrics.ascent,
                                                     false);

            line_y += editor->line_height;
        }

        if ((widget->state & VG_STATE_FOCUSED) && editor->cursor_visible && !editor->read_only) {
            int visible_cursor_line = codeeditor_visual_row_for_position(
                editor, content_width, editor->cursor_line, editor->cursor_col);
            float cursor_y =
                widget->y + (float)visible_cursor_line * editor->line_height - editor->scroll_y;
            if (cursor_y + editor->line_height > widget->y &&
                cursor_y < widget->y + widget->height) {
                size_t cursor_display_col =
                    codeeditor_display_columns_before_byte(&editor->lines[editor->cursor_line],
                                                           (size_t)editor->cursor_col,
                                                           editor->tab_width);
                float cursor_x =
                    content_x + (float)cursor_display_col * editor->char_width - editor->scroll_x;
                codeeditor_draw_caret(
                    canvas, cursor_x, cursor_y, editor->line_height, editor->cursor_color);
            }

            for (int c = 0; c < editor->extra_cursor_count; c++) {
                int visible_extra_line =
                    codeeditor_visual_row_for_position(editor,
                                                       content_width,
                                                       editor->extra_cursors[c].line,
                                                       editor->extra_cursors[c].col);
                size_t cursor_display_col = codeeditor_display_columns_before_byte(
                    &editor->lines[editor->extra_cursors[c].line],
                    (size_t)editor->extra_cursors[c].col,
                    editor->tab_width);
                float cursor_x =
                    content_x + (float)cursor_display_col * editor->char_width - editor->scroll_x;
                float extra_cursor_y =
                    widget->y + (float)visible_extra_line * editor->line_height - editor->scroll_y;
                if (extra_cursor_y + editor->line_height <= widget->y ||
                    extra_cursor_y >= widget->y + widget->height)
                    continue;
                codeeditor_draw_caret(
                    canvas, cursor_x, extra_cursor_y, editor->line_height, editor->cursor_color);
            }
        }
    } else {
        int chars_per_row = codeeditor_chars_per_row(editor, content_width);
        int wrapped_first_visual_row = (int)(editor->scroll_y / editor->line_height);
        float row_offset = editor->scroll_y - (float)wrapped_first_visual_row * editor->line_height;
        int line = 0;
        int row_in_line = 0;
        codeeditor_locate_visual_row(
            editor, content_width, wrapped_first_visual_row, &line, &row_in_line);

        float row_y = widget->y - row_offset;
        while (line < editor->line_count && row_y < widget->y + widget->height) {
            if (line >= 0 && line < editor->line_count) {
                size_t line_len = editor->lines[line].length;
                int row_count = codeeditor_cached_row_count_for_line(editor, content_width, line);
                if (row_count == 0) {
                    line = codeeditor_next_visible_line(editor, line);
                    row_in_line = 0;
                    continue;
                }
                size_t segment_display_start =
                    chars_per_row > 0 ? (size_t)row_in_line * (size_t)chars_per_row : 0u;
                size_t segment_display_end =
                    chars_per_row > 0
                        ? segment_display_start + (size_t)chars_per_row
                        : codeeditor_line_display_columns(&editor->lines[line], editor->tab_width);
                size_t segment_start_byte = codeeditor_byte_for_display_column(
                    &editor->lines[line], segment_display_start, editor->tab_width);
                size_t segment_end_byte = codeeditor_byte_for_display_column(
                    &editor->lines[line], segment_display_end, editor->tab_width);
                int seg_start =
                    segment_start_byte > (size_t)INT_MAX ? INT_MAX : (int)segment_start_byte;
                int seg_len = segment_end_byte - segment_start_byte > (size_t)INT_MAX
                                  ? INT_MAX
                                  : (int)(segment_end_byte - segment_start_byte);

                if (line == editor->cursor_line && (widget->state & VG_STATE_FOCUSED)) {
                    codeeditor_draw_current_line(canvas,
                                                 content_x,
                                                 row_y,
                                                 content_width,
                                                 editor->line_height,
                                                 editor->current_line_bg,
                                                 vg_theme_get_current()->colors.accent_primary);
                }

                codeeditor_paint_highlights_for_line(
                    editor, canvas, line, seg_start, seg_len, content_x, row_y, 0.0f, true);
                codeeditor_paint_pair_highlight_for_segment(
                    editor, canvas, line, seg_start, seg_len, content_x, row_y, 0.0f, true);

                if (editor->has_selection && (widget->state & VG_STATE_FOCUSED)) {
                    int sel_start_line = editor->selection.start_line;
                    int sel_start_col = editor->selection.start_col;
                    int sel_end_line = editor->selection.end_line;
                    int sel_end_col = editor->selection.end_col;
                    normalize_selection_range(
                        &sel_start_line, &sel_start_col, &sel_end_line, &sel_end_col);
                    if (line >= sel_start_line && line <= sel_end_line) {
                        int sel_start = (line == sel_start_line) ? sel_start_col : 0;
                        int sel_end = (line == sel_end_line) ? sel_end_col : (int)line_len;
                        int overlap_start = sel_start > seg_start ? sel_start : seg_start;
                        int overlap_end =
                            sel_end < seg_start + seg_len ? sel_end : seg_start + seg_len;
                        if (overlap_end > overlap_start) {
                            size_t overlap_display_start = codeeditor_display_columns_before_byte(
                                &editor->lines[line], (size_t)overlap_start, editor->tab_width);
                            size_t overlap_display_end = codeeditor_display_columns_before_byte(
                                &editor->lines[line], (size_t)overlap_end, editor->tab_width);
                            float sel_x =
                                content_x + (float)(overlap_display_start - segment_display_start) *
                                                editor->char_width;
                            float sel_w = (float)(overlap_display_end - overlap_display_start) *
                                          editor->char_width;
                            vgfx_fill_rect((vgfx_window_t)canvas,
                                           (int32_t)sel_x,
                                           (int32_t)row_y,
                                           (int32_t)sel_w,
                                           (int32_t)editor->line_height,
                                           editor->selection_color);
                        }
                    }
                }

                if (widget->state & VG_STATE_FOCUSED) {
                    for (int c = 0; c < editor->extra_cursor_count; c++) {
                        if (!editor->extra_cursors[c].has_selection)
                            continue;
                        int sel_start_line = editor->extra_cursors[c].selection.start_line;
                        int sel_start_col = editor->extra_cursors[c].selection.start_col;
                        int sel_end_line = editor->extra_cursors[c].selection.end_line;
                        int sel_end_col = editor->extra_cursors[c].selection.end_col;
                        normalize_selection_range(
                            &sel_start_line, &sel_start_col, &sel_end_line, &sel_end_col);
                        if (line < sel_start_line || line > sel_end_line)
                            continue;
                        int sel_start = (line == sel_start_line) ? sel_start_col : 0;
                        int sel_end = (line == sel_end_line) ? sel_end_col : (int)line_len;
                        int overlap_start = sel_start > seg_start ? sel_start : seg_start;
                        int overlap_end =
                            sel_end < seg_start + seg_len ? sel_end : seg_start + seg_len;
                        if (overlap_end > overlap_start) {
                            size_t overlap_display_start = codeeditor_display_columns_before_byte(
                                &editor->lines[line], (size_t)overlap_start, editor->tab_width);
                            size_t overlap_display_end = codeeditor_display_columns_before_byte(
                                &editor->lines[line], (size_t)overlap_end, editor->tab_width);
                            float sel_x =
                                content_x + (float)(overlap_display_start - segment_display_start) *
                                                editor->char_width;
                            float sel_w = (float)(overlap_display_end - overlap_display_start) *
                                          editor->char_width;
                            vgfx_fill_rect((vgfx_window_t)canvas,
                                           (int32_t)sel_x,
                                           (int32_t)row_y,
                                           (int32_t)sel_w,
                                           (int32_t)editor->line_height,
                                           editor->selection_color);
                        }
                    }
                }

                highlight_line(editor, line);
                if (editor->lines[line].text && line_len > 0 && seg_len > 0) {
                    float text_y = row_y + font_metrics.ascent;
                    if (editor->lines[line].colors) {
                        codeeditor_draw_colored_slice(canvas,
                                                      editor->font,
                                                      editor->font_size,
                                                      content_x,
                                                      text_y,
                                                      editor->lines[line].text,
                                                      &editor->lines[line],
                                                      editor->lines[line].colors,
                                                      editor->lines[line].colors_capacity,
                                                      (size_t)seg_start,
                                                      (size_t)seg_len,
                                                      editor->char_width,
                                                      editor->tab_width,
                                                      editor->text_color);
                    } else {
                        char stack_buf[256];
                        char *seg_buf = stack_buf;
                        size_t copy_len = (size_t)seg_len;
                        if (copy_len >= sizeof(stack_buf)) {
                            seg_buf = (char *)malloc(copy_len + 1);
                            if (!seg_buf)
                                seg_buf = stack_buf;
                        }
                        if (seg_buf) {
                            if (seg_buf == stack_buf && copy_len >= sizeof(stack_buf)) {
                                copy_len = sizeof(stack_buf) - 1;
                            }
                            memcpy(seg_buf, editor->lines[line].text + seg_start, copy_len);
                            seg_buf[copy_len] = '\0';
                            vg_font_draw_text(canvas,
                                              editor->font,
                                              editor->font_size,
                                              content_x,
                                              text_y,
                                              seg_buf,
                                              editor->text_color);
                            if (seg_buf != stack_buf)
                                free(seg_buf);
                        }
                    }
                }
                if (row_in_line == 0) {
                    size_t segment_display_width =
                        codeeditor_display_columns_before_byte(&editor->lines[line],
                                                               segment_start_byte + (size_t)seg_len,
                                                               editor->tab_width) -
                        segment_display_start;
                    codeeditor_draw_fold_ellipsis(editor,
                                                  canvas,
                                                  content_x,
                                                  row_y + font_metrics.ascent,
                                                  line,
                                                  segment_display_width);
                }
                codeeditor_paint_inlay_hints_for_segment(editor,
                                                         canvas,
                                                         line,
                                                         seg_start,
                                                         seg_len,
                                                         content_x,
                                                         row_y,
                                                         0.0f,
                                                         row_y + font_metrics.ascent,
                                                         true);

                row_in_line++;
                if (row_in_line >= row_count) {
                    line = codeeditor_next_visible_line(editor, line);
                    row_in_line = 0;
                }
            } else {
                line = codeeditor_next_visible_line(editor, line);
                row_in_line = 0;
            }
            row_y += editor->line_height;
        }

        if ((widget->state & VG_STATE_FOCUSED) && editor->cursor_visible && !editor->read_only) {
            int cursor_visual_row = codeeditor_visual_row_for_position(
                editor, content_width, editor->cursor_line, editor->cursor_col);
            float cursor_y =
                widget->y + (float)cursor_visual_row * editor->line_height - editor->scroll_y;
            int col_in_row = editor->cursor_col;
            codeeditor_visual_offset_for_position(
                editor, content_width, editor->cursor_line, editor->cursor_col, NULL, &col_in_row);
            float cursor_x = content_x + (float)col_in_row * editor->char_width;
            codeeditor_draw_caret(
                canvas, cursor_x, cursor_y, editor->line_height, editor->cursor_color);

            for (int c = 0; c < editor->extra_cursor_count; c++) {
                int extra_visual_row =
                    codeeditor_visual_row_for_position(editor,
                                                       content_width,
                                                       editor->extra_cursors[c].line,
                                                       editor->extra_cursors[c].col);
                float extra_y =
                    widget->y + (float)extra_visual_row * editor->line_height - editor->scroll_y;
                int extra_col_in_row = editor->extra_cursors[c].col;
                codeeditor_visual_offset_for_position(editor,
                                                      content_width,
                                                      editor->extra_cursors[c].line,
                                                      editor->extra_cursors[c].col,
                                                      NULL,
                                                      &extra_col_in_row);
                float extra_x = content_x + (float)extra_col_in_row * editor->char_width;
                codeeditor_draw_caret(
                    canvas, extra_x, extra_y, editor->line_height, editor->cursor_color);
            }
        }
    }

    if (clip_w > 0 && clip_h > 0) {
        vgfx_clear_clip((vgfx_window_t)canvas);
    }

    if (editor->gutter_width > 0.0f) {
        float fold_gutter_x = widget->x + line_number_gutter_width;
        if (!editor->word_wrap) {
            float row_offset = editor->scroll_y - (float)first_visual_row * editor->line_height;
            float line_y = widget->y - row_offset;
            for (int i = editor->visible_first_line;
                 i >= 0 && i < editor->line_count && line_y < widget->y + widget->height;
                 i = codeeditor_next_visible_line(editor, i)) {
                if (codeeditor_line_is_hidden(editor, i))
                    continue;

                if (editor->show_line_numbers) {
                    char line_num[16];
                    snprintf(line_num, sizeof(line_num), "%d", i + 1);
                    vg_text_metrics_t num_metrics = {0};
                    vg_font_measure_text(editor->font, editor->font_size, line_num, &num_metrics);
                    float num_x = widget->x + line_number_gutter_width - num_metrics.width - 8.0f;
                    float num_y = line_y + font_metrics.ascent;
                    vg_font_draw_text(canvas,
                                      editor->font,
                                      editor->font_size,
                                      num_x,
                                      num_y,
                                      line_num,
                                      editor->line_number_color);
                }

                // Change bars (SCM diff markers): thin vertical bars at the
                // gutter's left edge. Drawn as a separate pass so they coexist
                // with breakpoint/diagnostic disc icons on the same line.
                for (int g = 0; g < editor->gutter_icon_count; g++) {
                    struct vg_gutter_icon *icon = &editor->gutter_icons[g];
                    if (icon->line != i || icon->style != 1)
                        continue;
                    vg_draw_round_rect_fill((vgfx_window_t)canvas,
                                            widget->x,
                                            line_y,
                                            3.0f,
                                            editor->line_height,
                                            1.0f,
                                            icon->color);
                }

                for (int g = 0; g < editor->gutter_icon_count; g++) {
                    struct vg_gutter_icon *icon = &editor->gutter_icons[g];
                    if (icon->line != i || icon->style == 1)
                        continue;
                    int32_t icon_box = (int32_t)editor->line_height - 4;
                    if (icon_box < 8)
                        icon_box = 8;
                    int32_t icon_x = (int32_t)widget->x + 2;
                    int32_t icon_y =
                        (int32_t)line_y + ((int32_t)editor->line_height - icon_box) / 2;
                    if (icon->image.type == VG_ICON_IMAGE && icon->image.data.image.pixels) {
                        codeeditor_draw_gutter_icon_image(
                            (vgfx_window_t)canvas, icon, icon_x, icon_y, icon_box, icon_box);
                    } else {
                        float icon_r = (float)icon_box * 0.5f;
                        float icon_cx = (float)icon_x + icon_r;
                        float icon_cy = (float)icon_y + icon_r;
                        vg_draw_disc_fill(
                            (vgfx_window_t)canvas, icon_cx, icon_cy, icon_r, icon->color);
                    }
                    break;
                }

                if (editor->show_fold_gutter) {
                    codeeditor_draw_fold_marker(
                        editor, canvas, fold_gutter_x, fold_gutter_width, line_y, &font_metrics, i);
                }

                line_y += editor->line_height;
            }
        } else {
            float row_offset = editor->scroll_y - (float)first_visual_row * editor->line_height;
            int line = 0;
            int row_in_line = 0;
            codeeditor_locate_visual_row(
                editor, content_width, first_visual_row, &line, &row_in_line);
            float row_y = widget->y - row_offset;
            while (line < editor->line_count && row_y < widget->y + widget->height) {
                int row_count = codeeditor_cached_row_count_for_line(editor, content_width, line);
                if (row_count == 0) {
                    line = codeeditor_next_visible_line(editor, line);
                    row_in_line = 0;
                    continue;
                }
                if (row_in_line == 0) {
                    if (editor->show_line_numbers) {
                        char line_num[16];
                        snprintf(line_num, sizeof(line_num), "%d", line + 1);
                        vg_text_metrics_t num_metrics = {0};
                        vg_font_measure_text(
                            editor->font, editor->font_size, line_num, &num_metrics);
                        float num_x =
                            widget->x + line_number_gutter_width - num_metrics.width - 8.0f;
                        float num_y = row_y + font_metrics.ascent;
                        vg_font_draw_text(canvas,
                                          editor->font,
                                          editor->font_size,
                                          num_x,
                                          num_y,
                                          line_num,
                                          editor->line_number_color);
                    }

                    for (int g = 0; g < editor->gutter_icon_count; g++) {
                        struct vg_gutter_icon *icon = &editor->gutter_icons[g];
                        if (icon->line != line)
                            continue;
                        int32_t icon_box = (int32_t)editor->line_height - 4;
                        if (icon_box < 8)
                            icon_box = 8;
                        int32_t icon_x = (int32_t)widget->x + 2;
                        int32_t icon_y =
                            (int32_t)row_y + ((int32_t)editor->line_height - icon_box) / 2;
                        if (icon->image.type == VG_ICON_IMAGE && icon->image.data.image.pixels) {
                            codeeditor_draw_gutter_icon_image(
                                (vgfx_window_t)canvas, icon, icon_x, icon_y, icon_box, icon_box);
                        } else {
                            int32_t icon_r = icon_box / 2;
                            int32_t icon_cx = icon_x + icon_r;
                            int32_t icon_cy = icon_y + icon_r;
                            vgfx_fill_circle((vgfx_window_t)canvas,
                                             icon_cx,
                                             icon_cy,
                                             icon_r,
                                             (vgfx_color_t)icon->color);
                        }
                        break;
                    }

                    if (editor->show_fold_gutter) {
                        codeeditor_draw_fold_marker(editor,
                                                    canvas,
                                                    fold_gutter_x,
                                                    fold_gutter_width,
                                                    row_y,
                                                    &font_metrics,
                                                    line);
                    }
                }

                row_in_line++;
                if (row_in_line >= row_count) {
                    line = codeeditor_next_visible_line(editor, line);
                    row_in_line = 0;
                }
                row_y += editor->line_height;
            }
        }
    }

    if (total_content_height > visible_height) {
        // Scrollbar track background
        float track_x_local = 0.0f;
        float thumb_y_local = 0.0f;
        float thumb_height = 0.0f;
        if (!codeeditor_get_scrollbar_metrics(
                editor, widget, &track_x_local, &thumb_y_local, &thumb_height, NULL, NULL))
            return;
        float track_x = widget->x + track_x_local;
        uint32_t track_color = 0xFF3C3C3C; // Dark gray
        vgfx_fill_rect((vgfx_window_t)canvas,
                       (int32_t)track_x,
                       (int32_t)widget->y,
                       (int32_t)scrollbar_width,
                       (int32_t)visible_height,
                       track_color);
        float thumb_y = widget->y + thumb_y_local;

        // Scrollbar thumb — rounded, anti-aliased pill instead of a hard block.
        uint32_t thumb_color = 0xFF6C6C6C; // Medium gray
        float thumb_w = scrollbar_width - 4.0f;
        vg_draw_round_rect_fill((vgfx_window_t)canvas,
                                track_x + 2.0f,
                                thumb_y,
                                thumb_w,
                                thumb_height,
                                thumb_w * 0.5f,
                                thumb_color);
    }

    // Horizontal scrollbar along the bottom edge (only when the widest line
    // overflows the viewport and word wrap is off).
    {
        float htrack_y_local = 0.0f;
        float hthumb_x_local = 0.0f;
        float hthumb_width = 0.0f;
        if (codeeditor_get_hscrollbar_metrics(
                editor, widget, &htrack_y_local, &hthumb_x_local, &hthumb_width, NULL, NULL)) {
            float htrack_y = widget->y + htrack_y_local;
            float track_span = widget->width - editor->gutter_width - scrollbar_width;
            if (track_span < 0.0f)
                track_span = 0.0f;
            uint32_t track_color = 0xFF3C3C3C;
            vgfx_fill_rect((vgfx_window_t)canvas,
                           (int32_t)(widget->x + editor->gutter_width),
                           (int32_t)htrack_y,
                           (int32_t)track_span,
                           (int32_t)scrollbar_width,
                           track_color);
            uint32_t thumb_color = 0xFF6C6C6C;
            float thumb_h = scrollbar_width - 4.0f;
            vg_draw_round_rect_fill((vgfx_window_t)canvas,
                                    widget->x + hthumb_x_local,
                                    htrack_y + 2.0f,
                                    hthumb_width,
                                    thumb_h,
                                    thumb_h * 0.5f,
                                    thumb_color);
        }
    }

    vg_font_set_ligatures_enabled(ligatures_were_enabled);
}
