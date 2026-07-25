//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_minimap.c
// Purpose: Revision-observing minimap widget with a bounded direct-mapped line
//          raster-summary cache and interactive viewport navigation.
// Key invariants:
//   - Line-summary lookup is O(1), bounded by maximum_cached_lines, and never
//     scales retained memory with document length.
//   - Text/layout changes invalidate line summaries; scroll-only changes retain
//     them and repaint only the viewport-dependent surface.
//   - scale controls the vertical pixels-per-line ratio (clamped to [0.05, 0.5]).
//   - show_viewport controls drawing of the viewport indicator rectangle.
//   - buffer_dirty is set whenever the document or widget size changes.
// Ownership/Lifetime:
//   - render_buffer, line_cache, and markers are owned and freed by the minimap.
//   - The widget does not own the linked vg_codeeditor_t.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements the code-editor minimap, bounded line-summary caching,
///        viewport observation, rendering, and drag navigation.
/// @details The minimap borrows its editor, observes editor revision and
///          viewport fields, and stores only a fixed-size direct-mapped cache.
///          Stale or destroyed editor handles are detached before use.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void minimap_destroy(vg_widget_t *widget);
static void minimap_measure(vg_widget_t *widget, float available_width, float available_height);
static void minimap_paint(vg_widget_t *widget, void *canvas);
static bool minimap_handle_event(vg_widget_t *widget, vg_event_t *event);
static int minimap_document_line_count(vg_minimap_t *minimap);
static int minimap_line_from_local_y(vg_minimap_t *minimap, float local_y);
static void minimap_scroll_editor_to_line(vg_minimap_t *minimap, int line);
static int minimap_trimmed_line_bounds(const char *text,
                                       int max_columns,
                                       int *out_first,
                                       int *out_last);
static vg_codeeditor_t *minimap_live_editor(vg_minimap_t *minimap);
static void minimap_clear_line_cache(vg_minimap_t *minimap);

//=============================================================================
// Minimap VTable
//=============================================================================

static vg_widget_vtable_t g_minimap_vtable = {.destroy = minimap_destroy,
                                              .measure = minimap_measure,
                                              .arrange = NULL,
                                              .paint = minimap_paint,
                                              .handle_event = minimap_handle_event,
                                              .can_focus = NULL,
                                              .on_focus = NULL};

//=============================================================================
// Minimap Implementation
//=============================================================================

/// @brief Create a minimap widget linked to the given code editor.
///
/// @details Default scale is 0.1 (1 pixel per 10 lines), char_width=1,
///          line_height=2, show_viewport=true.  The widget has no parent; attach
///          it with vg_widget_add_child if needed.
///
/// @param editor The code editor to visualise; may be NULL (set later via
///               vg_minimap_set_editor).
/// @return Newly allocated vg_minimap_t, or NULL on allocation failure.
vg_minimap_t *vg_minimap_create(vg_codeeditor_t *editor) {
    vg_minimap_t *minimap = calloc(1, sizeof(vg_minimap_t));
    if (!minimap)
        return NULL;

    vg_widget_init(&minimap->base, VG_WIDGET_CUSTOM, &g_minimap_vtable);

    minimap->editor = editor;
    minimap->char_width = 1;
    minimap->line_height = 2;
    minimap->show_viewport = true;
    minimap->scale = 0.1f;
    minimap->viewport_color = 0xFF78D6FF;
    minimap->bg_color = 0xFF111827;
    minimap->text_color = 0xFF3B82F6;
    minimap->buffer_dirty = true;
    minimap->source_revision = 1u;
    (void)vg_minimap_set_maximum_cached_lines(minimap, 2048u);
    if (editor && vg_widget_is_live(&editor->base)) {
        minimap->observed_text_revision = editor->revision;
        minimap->observed_layout_revision = editor->layout_generation;
        minimap->observed_scroll_x = editor->scroll_x;
        minimap->observed_scroll_y = editor->scroll_y;
        minimap->observed_visible_first_line = editor->visible_first_line;
        minimap->observed_visible_line_count = editor->visible_line_count;
        minimap->observed_line_count = editor->line_count;
    }

    return minimap;
}

/// @brief Release all render, line-cache, and marker storage.
/// @param widget Minimap widget base being destroyed.
static void minimap_destroy(vg_widget_t *widget) {
    vg_minimap_t *minimap = (vg_minimap_t *)widget;
    free(minimap->render_buffer);
    free(minimap->line_cache);
    free(minimap->markers);
    minimap->render_buffer = NULL;
    minimap->line_cache = NULL;
    minimap->markers = NULL;
}

/// @brief Destroy the minimap widget, freeing its pixel buffer and markers.
///
/// @param minimap The minimap to destroy; may be NULL.
void vg_minimap_destroy(vg_minimap_t *minimap) {
    if (!minimap)
        return;
    vg_widget_destroy(&minimap->base);
}

/// @brief VTable measure: uses preferred_width constraint (default 96 px) for width and claims all
/// available height.
/// @param widget Minimap widget base to measure.
/// @param available_width Offered width; the preferred-width policy takes precedence.
/// @param available_height Height claimed when positive, otherwise a 160-pixel fallback.
static void minimap_measure(vg_widget_t *widget, float available_width, float available_height) {
    (void)available_width;

    float width =
        widget->constraints.preferred_width > 0.0f ? widget->constraints.preferred_width : 96.0f;
    if (widget->constraints.min_width > 0.0f && width < widget->constraints.min_width)
        width = widget->constraints.min_width;
    if (widget->constraints.max_width > 0.0f && width > widget->constraints.max_width)
        width = widget->constraints.max_width;

    widget->measured_width = width;
    widget->measured_height = available_height > 0.0f ? available_height : 160.0f;
    if (widget->constraints.min_height > 0.0f &&
        widget->measured_height < widget->constraints.min_height) {
        widget->measured_height = widget->constraints.min_height;
    }
}

/// @brief Clears the buffer_dirty flag; placeholder for future per-pixel RGBA buffer rasterisation.
/// @param minimap Minimap whose deferred buffer work is acknowledged.
static void render_minimap_buffer(vg_minimap_t *minimap) {
    if (!minimap)
        return;
    minimap->buffer_dirty = false;
}

/// @brief Validate and return the minimap's borrowed code-editor handle.
/// @details A stale handle or wrong widget type is detached so later callers do
///          not repeatedly dereference invalid editor state.
/// @param minimap Minimap whose editor link is inspected.
/// @return Borrowed live code editor, or `NULL`.
static vg_codeeditor_t *minimap_live_editor(vg_minimap_t *minimap) {
    if (!minimap || !minimap->editor)
        return NULL;
    vg_widget_t *widget = (vg_widget_t *)minimap->editor;
    if (!vg_widget_is_live(widget) || widget->type != VG_WIDGET_CODEEDITOR) {
        minimap->editor = NULL;
        return NULL;
    }
    return minimap->editor;
}

/// @brief Advance the minimap's saturating observed-source revision.
/// @param minimap Minimap whose source or viewport changed.
static void minimap_advance_source_revision(vg_minimap_t *minimap) {
    if (minimap && minimap->source_revision != UINT64_MAX)
        ++minimap->source_revision;
}

/// @brief Invalidate every direct-mapped cached line summary without freeing storage.
/// @param minimap Minimap cache to clear.
static void minimap_clear_line_cache(vg_minimap_t *minimap) {
    if (!minimap)
        return;
    if (minimap->line_cache && minimap->maximum_cached_lines > 0u) {
        memset(minimap->line_cache,
               0,
               (size_t)minimap->maximum_cached_lines * sizeof(*minimap->line_cache));
    }
    minimap->cached_line_count = 0u;
}

/// @brief Synchronize source-content, layout, and viewport observations from the editor.
/// @details Content or layout changes clear cached line summaries and dirty the
///          render buffer. Viewport-only changes retain summaries. Either kind
///          advances the combined source revision and invalidates paint.
/// @param minimap Minimap to synchronize with its borrowed editor.
/// @return `true` when any observed state changed; `false` for no change or no
///         live editor.
bool vg_minimap_sync_source(vg_minimap_t *minimap) {
    if (!minimap)
        return false;
    vg_codeeditor_t *editor = minimap_live_editor(minimap);
    if (!editor)
        return false;

    const bool content_changed = minimap->observed_text_revision != editor->revision ||
                                 minimap->observed_layout_revision != editor->layout_generation ||
                                 minimap->observed_line_count != editor->line_count;
    const bool viewport_changed =
        minimap->observed_scroll_x != editor->scroll_x ||
        minimap->observed_scroll_y != editor->scroll_y ||
        minimap->observed_visible_first_line != editor->visible_first_line ||
        minimap->observed_visible_line_count != editor->visible_line_count;
    if (!content_changed && !viewport_changed)
        return false;

    minimap->observed_text_revision = editor->revision;
    minimap->observed_layout_revision = editor->layout_generation;
    minimap->observed_scroll_x = editor->scroll_x;
    minimap->observed_scroll_y = editor->scroll_y;
    minimap->observed_visible_first_line = editor->visible_first_line;
    minimap->observed_visible_line_count = editor->visible_line_count;
    minimap->observed_line_count = editor->line_count;
    if (content_changed) {
        minimap_clear_line_cache(minimap);
        minimap->buffer_dirty = true;
    }
    minimap_advance_source_revision(minimap);
    vg_widget_note_revision(&minimap->base);
    vg_widget_invalidate(&minimap->base);
    return true;
}

/// @brief Synchronize and return the minimap's combined observed-source revision.
/// @param minimap Minimap to query.
/// @return Saturating revision after synchronization, or zero for `NULL`.
uint64_t vg_minimap_get_source_revision(vg_minimap_t *minimap) {
    if (!minimap)
        return 0u;
    (void)vg_minimap_sync_source(minimap);
    return minimap->source_revision;
}

/// @brief Atomically replace the bounded direct-mapped line cache.
/// @details A zero limit disables caching and releases storage. For nonzero
///          limits, replacement allocation completes before the old cache is
///          discarded.
/// @param minimap Minimap whose cache capacity is changed.
/// @param maximum_lines Maximum number of line summaries retained.
/// @return `true` after applying the limit; `false` with the old cache intact
///         on invalid input, overflow, or allocation failure.
bool vg_minimap_set_maximum_cached_lines(vg_minimap_t *minimap, uint32_t maximum_lines) {
    if (!minimap)
        return false;
    if (maximum_lines == minimap->maximum_cached_lines)
        return true;
    if (maximum_lines == 0u) {
        free(minimap->line_cache);
        minimap->line_cache = NULL;
        minimap->maximum_cached_lines = 0u;
        minimap->cached_line_count = 0u;
        vg_widget_note_revision(&minimap->base);
        vg_widget_invalidate(&minimap->base);
        return true;
    }
    if ((size_t)maximum_lines > SIZE_MAX / sizeof(*minimap->line_cache))
        return false;
    vg_minimap_line_cache_entry_t *replacement =
        (vg_minimap_line_cache_entry_t *)calloc((size_t)maximum_lines, sizeof(*replacement));
    if (!replacement)
        return false;
    free(minimap->line_cache);
    minimap->line_cache = replacement;
    minimap->maximum_cached_lines = maximum_lines;
    minimap->cached_line_count = 0u;
    vg_widget_note_revision(&minimap->base);
    vg_widget_invalidate(&minimap->base);
    return true;
}

/// @brief Return the count of currently valid direct-mapped cache slots.
/// @param minimap Minimap cache to inspect.
/// @return Number of valid slots, or zero for `NULL`.
uint32_t vg_minimap_get_cached_line_count(const vg_minimap_t *minimap) {
    return minimap ? minimap->cached_line_count : 0u;
}

/// @brief Returns the line count of the linked editor, or 1 if no editor is attached or the editor
/// is empty.
/// @param minimap Minimap whose live editor is queried.
/// @return Positive logical line count.
static int minimap_document_line_count(vg_minimap_t *minimap) {
    vg_codeeditor_t *editor = minimap_live_editor(minimap);
    if (!editor || editor->line_count <= 0)
        return 1;
    return editor->line_count;
}

/// @brief Converts widget-local @p local_y to a document line index proportional to the inner
/// drawing area height.
/// @param minimap Minimap providing widget geometry and document length.
/// @param local_y Pointer coordinate relative to the widget.
/// @return Clamped zero-based document line.
static int minimap_line_from_local_y(vg_minimap_t *minimap, float local_y) {
    if (!minimap_live_editor(minimap))
        return 0;

    float inner_top = 6.0f;
    float inner_height = minimap->base.height - 12.0f;
    if (inner_height <= 1.0f)
        return 0;

    if (local_y < inner_top)
        local_y = inner_top;
    if (local_y > inner_top + inner_height)
        local_y = inner_top + inner_height;

    int line_count = minimap_document_line_count(minimap);
    float t = (local_y - inner_top) / inner_height;
    int line = (int)(t * (float)line_count);
    if (line < 0)
        line = 0;
    if (line >= line_count)
        line = line_count - 1;
    return line;
}

/// @brief Scrolls the linked editor so that @p line is centred in the visible area.
/// @param minimap Minimap linked to the target editor.
/// @param line Desired zero-based document line.
static void minimap_scroll_editor_to_line(vg_minimap_t *minimap, int line) {
    vg_codeeditor_t *editor = minimap_live_editor(minimap);
    if (!editor)
        return;

    int line_count = editor->line_count > 0 ? editor->line_count : 1;
    int visible_lines = editor->visible_line_count > 0 ? editor->visible_line_count : 1;
    int target = line - visible_lines / 2;
    int max_first = line_count - visible_lines;

    if (target < 0)
        target = 0;
    if (max_first < 0)
        max_first = 0;
    if (target > max_first)
        target = max_first;

    vg_codeeditor_scroll_to_line(editor, target);
    minimap->base.needs_paint = true;
}

/// @brief Scans @p text for the first and last non-whitespace byte positions, writing them into @p
/// out_first and @p out_last; returns 0 for blank lines.
/// @param text Null-terminated line bytes, or `NULL`.
/// @param max_columns Maximum prefix bytes to inspect.
/// @param out_first Optional destination for the first non-space/tab index.
/// @param out_last Optional destination for the last non-space/tab index.
/// @return Nonzero when the inspected prefix contains visible bytes.
static int minimap_trimmed_line_bounds(const char *text,
                                       int max_columns,
                                       int *out_first,
                                       int *out_last) {
    int first = -1;
    int last = -1;

    if (!text) {
        if (out_first)
            *out_first = -1;
        if (out_last)
            *out_last = -1;
        return 0;
    }

    if (max_columns < 1)
        max_columns = 1;

    for (int i = 0; i < max_columns && text[i]; i++) {
        if (text[i] != ' ' && text[i] != '\t') {
            if (first < 0)
                first = i;
            last = i;
        }
    }

    if (out_first)
        *out_first = first;
    if (out_last)
        *out_last = last;
    return first >= 0 && last >= first;
}

/// @brief Return cached trimmed bounds for a line, populating its direct-mapped slot on miss.
/// @param minimap Minimap owning the bounded cache.
/// @param line Non-negative source line number.
/// @param text Borrowed line text.
/// @param max_columns Maximum byte columns to scan.
/// @param out_first Receives first non-whitespace byte column.
/// @param out_last Receives last non-whitespace byte column.
/// @return Non-zero when the represented line segment is non-blank.
static int minimap_cached_line_bounds(vg_minimap_t *minimap,
                                      int line,
                                      const char *text,
                                      int max_columns,
                                      int *out_first,
                                      int *out_last) {
    if (!minimap || !minimap->line_cache || minimap->maximum_cached_lines == 0u || line < 0)
        return minimap_trimmed_line_bounds(text, max_columns, out_first, out_last);
    const uint32_t slot = (uint32_t)line % minimap->maximum_cached_lines;
    vg_minimap_line_cache_entry_t *entry = &minimap->line_cache[slot];
    if (!entry->valid || entry->line != line || entry->scan_columns != max_columns) {
        const bool was_valid = entry->valid;
        entry->line = line;
        entry->scan_columns = max_columns;
        (void)minimap_trimmed_line_bounds(
            text, max_columns, &entry->first_column, &entry->last_column);
        entry->valid = true;
        if (!was_valid && minimap->cached_line_count < minimap->maximum_cached_lines)
            ++minimap->cached_line_count;
    }
    if (out_first)
        *out_first = entry->first_column;
    if (out_last)
        *out_last = entry->last_column;
    return entry->first_column >= 0 && entry->last_column >= entry->first_column;
}

/// @brief VTable paint: draws background, then proportional text bars for each document line,
/// marker overlays, and the viewport highlight rectangle.
/// @details Sampling is capped independently of document length. Each sampled
///          line becomes a proportional horizontal bar based on its trimmed
///          byte span; markers and observed editor viewport are layered above.
/// @param widget Arranged minimap widget base to paint.
/// @param canvas Backend canvas used for rectangle primitives.
static void minimap_paint(vg_widget_t *widget, void *canvas) {
    vg_minimap_t *minimap = (vg_minimap_t *)widget;
    (void)vg_minimap_sync_source(minimap);
    vg_codeeditor_t *editor = minimap_live_editor(minimap);
    if (!editor)
        return;

    if (minimap->buffer_dirty)
        render_minimap_buffer(minimap);

    vgfx_window_t win = (vgfx_window_t)canvas;
    int32_t panel_x = (int32_t)widget->x;
    int32_t panel_y = (int32_t)widget->y;
    int32_t panel_w = (int32_t)widget->width;
    int32_t panel_h = (int32_t)widget->height;
    if (panel_w <= 0 || panel_h <= 0)
        return;

    vgfx_fill_rect(win, panel_x, panel_y, panel_w, panel_h, minimap->bg_color);
    vgfx_rect(win, panel_x, panel_y, panel_w, panel_h, 0xFF243244);

    int32_t inner_x = panel_x + 6;
    int32_t inner_y = panel_y + 6;
    int32_t inner_w = panel_w - 12;
    int32_t inner_h = panel_h - 12;
    if (inner_w <= 2 || inner_h <= 2)
        return;

    vgfx_fill_rect(win, inner_x, inner_y, inner_w, inner_h, 0xFF0B1220);

    int line_count = minimap_document_line_count(minimap);
    float scale = minimap->scale;
    if (scale < 0.05f)
        scale = 0.05f;
    if (scale > 0.5f)
        scale = 0.5f;

    float visible_columns = 120.0f * (0.1f / scale);
    if (visible_columns < 24.0f)
        visible_columns = 24.0f;
    int scan_columns = (int)(visible_columns + 1.0f);
    if (scan_columns < 24)
        scan_columns = 24;
    if (scan_columns > 240)
        scan_columns = 240;

    int max_lines_to_draw = inner_h * 2;
    if (max_lines_to_draw < 1)
        max_lines_to_draw = 1;
    if (max_lines_to_draw > 1024)
        max_lines_to_draw = 1024;
    int line_step = (line_count + max_lines_to_draw - 1) / max_lines_to_draw;
    if (line_step < 1)
        line_step = 1;

    for (int line = 0; line < editor->line_count; line += line_step) {
        int32_t y0 = inner_y + (line * inner_h) / line_count;
        int end_line = line + line_step;
        if (end_line > line_count)
            end_line = line_count;
        int32_t y1 = inner_y + (end_line * inner_h) / line_count;
        int32_t bar_h = y1 - y0;
        if (bar_h < 1)
            bar_h = 1;

        int first = -1;
        int last = -1;
        if (!minimap_cached_line_bounds(
                minimap, line, editor->lines[line].text, scan_columns, &first, &last))
            continue;

        float start_ratio = (float)first / visible_columns;
        float end_ratio = (float)(last + 1) / visible_columns;
        if (start_ratio > 1.0f)
            start_ratio = 1.0f;
        if (end_ratio > 1.0f)
            end_ratio = 1.0f;
        if (end_ratio < start_ratio)
            end_ratio = start_ratio;

        int32_t x0 = inner_x + 2 + (int32_t)(start_ratio * (float)(inner_w - 4));
        int32_t x1 = inner_x + 2 + (int32_t)(end_ratio * (float)(inner_w - 4));
        int32_t bar_w = x1 - x0;
        if (bar_w < 2)
            bar_w = 2;
        if (x0 + bar_w > inner_x + inner_w - 2)
            bar_w = (inner_x + inner_w - 2) - x0;
        if (bar_w > 0)
            vgfx_fill_rect(win, x0, y0, bar_w, bar_h, minimap->text_color);
    }

    for (int i = 0; i < minimap->marker_count; i++) {
        int line = minimap->markers[i].line;
        if (line < 0 || line >= line_count)
            continue;
        int32_t y = inner_y + (line * inner_h) / line_count;
        vgfx_fill_rect(win, inner_x, y, inner_w, 2, minimap->markers[i].color);
    }

    if (minimap->show_viewport) {
        int visible_first = editor->visible_first_line;
        int visible_count = editor->visible_line_count > 0 ? editor->visible_line_count : 1;
        int32_t viewport_y = inner_y + (visible_first * inner_h) / line_count;
        int32_t viewport_end = inner_y + ((visible_first + visible_count) * inner_h) / line_count;
        int32_t viewport_h = viewport_end - viewport_y;
        if (viewport_h < 6)
            viewport_h = 6;
        if (viewport_y + viewport_h > inner_y + inner_h)
            viewport_h = (inner_y + inner_h) - viewport_y;
        if (viewport_h > 0) {
            if (visible_count < line_count) {
                vgfx_fill_rect(win, inner_x, viewport_y, inner_w, viewport_h, 0xFF132235);
            }
            vgfx_rect(win, inner_x, viewport_y, inner_w, viewport_h, minimap->viewport_color);
        }
    }
}

/// @brief VTable handle_event: handles mouse-down to start a drag-scroll and mouse-move while
/// dragging, translating Y to a document line.
/// @param widget Minimap widget receiving input.
/// @param event Pointer event to interpret.
/// @return `true` when a drag-navigation event was consumed.
static bool minimap_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_minimap_t *minimap = (vg_minimap_t *)widget;
    if (!minimap_live_editor(minimap))
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_DOWN:
            minimap->dragging = true;
            minimap->drag_start_y = (int)event->mouse.y;
            minimap_scroll_editor_to_line(minimap,
                                          minimap_line_from_local_y(minimap, event->mouse.y));
            return true;

        case VG_EVENT_MOUSE_UP:
            minimap->dragging = false;
            return true;

        case VG_EVENT_MOUSE_MOVE:
            if (minimap->dragging) {
                minimap->drag_start_y = (int)event->mouse.y;
                minimap_scroll_editor_to_line(minimap,
                                              minimap_line_from_local_y(minimap, event->mouse.y));
                return true;
            }
            break;

        default:
            break;
    }

    return false;
}

/// @brief Associate a different code editor with the minimap and invalidate the buffer.
///
/// @param minimap The minimap to update.
/// @param editor  The new editor to visualise; may be NULL to detach.
void vg_minimap_set_editor(vg_minimap_t *minimap, vg_codeeditor_t *editor) {
    if (!minimap)
        return;
    if (editor && (!vg_widget_is_live(&editor->base) || editor->base.type != VG_WIDGET_CODEEDITOR))
        editor = NULL;
    if (minimap->editor == editor)
        return;

    minimap->editor = editor;
    minimap_clear_line_cache(minimap);
    minimap->buffer_dirty = true;
    if (editor) {
        minimap->observed_text_revision = editor->revision;
        minimap->observed_layout_revision = editor->layout_generation;
        minimap->observed_scroll_x = editor->scroll_x;
        minimap->observed_scroll_y = editor->scroll_y;
        minimap->observed_visible_first_line = editor->visible_first_line;
        minimap->observed_visible_line_count = editor->visible_line_count;
        minimap->observed_line_count = editor->line_count;
    } else {
        minimap->observed_text_revision = 0u;
        minimap->observed_layout_revision = 0u;
        minimap->observed_scroll_x = 0.0f;
        minimap->observed_scroll_y = 0.0f;
        minimap->observed_visible_first_line = 0;
        minimap->observed_visible_line_count = 0;
        minimap->observed_line_count = 0;
    }
    minimap_advance_source_revision(minimap);
    vg_widget_note_revision(&minimap->base);
    vg_widget_invalidate(&minimap->base);
}

/// @brief Set the vertical scale ratio between minimap pixels and document lines.
///
/// @param minimap The minimap to configure.
/// @param scale   Pixels-per-line ratio; clamped to [0.05, 0.5].
void vg_minimap_set_scale(vg_minimap_t *minimap, float scale) {
    if (!minimap)
        return;

    if (scale < 0.05f)
        scale = 0.05f;
    if (scale > 0.5f)
        scale = 0.5f;

    if (minimap->scale == scale)
        return;
    minimap->scale = scale;
    minimap_clear_line_cache(minimap);
    minimap->buffer_dirty = true;
    vg_widget_note_revision(&minimap->base);
    vg_widget_invalidate(&minimap->base);
}

/// @brief Show or hide the viewport indicator rectangle drawn over the minimap.
///
/// @param minimap The minimap to configure.
/// @param show    true to show the viewport highlight; false to hide it.
void vg_minimap_set_show_viewport(vg_minimap_t *minimap, bool show) {
    if (!minimap)
        return;

    if (minimap->show_viewport == show)
        return;
    minimap->show_viewport = show;
    vg_widget_note_revision(&minimap->base);
    vg_widget_invalidate(&minimap->base);
}

/// @brief Mark the minimap's pixel buffer as dirty, forcing a full rebuild on next paint.
///
/// @param minimap The minimap to invalidate.
void vg_minimap_invalidate(vg_minimap_t *minimap) {
    if (!minimap)
        return;

    minimap_clear_line_cache(minimap);
    minimap->buffer_dirty = true;
    vg_widget_invalidate(&minimap->base);
}

/// @brief Invalidate the pixel buffer due to a change in a range of document lines.
///
/// @details Only matching direct-mapped cache slots are discarded. Other line summaries remain
///          reusable, so an incremental editor integration can bound rescanning to changed lines.
///
/// @param minimap    The minimap to invalidate.
/// @param start_line First changed line (zero-based, inclusive).
/// @param end_line   Last changed line (zero-based, inclusive).
void vg_minimap_invalidate_lines(vg_minimap_t *minimap, uint32_t start_line, uint32_t end_line) {
    if (!minimap)
        return;
    if (start_line > end_line) {
        const uint32_t temporary = start_line;
        start_line = end_line;
        end_line = temporary;
    }
    for (uint32_t slot = 0u; slot < minimap->maximum_cached_lines; ++slot) {
        vg_minimap_line_cache_entry_t *entry = &minimap->line_cache[slot];
        if (!entry->valid || entry->line < 0)
            continue;
        const uint32_t line = (uint32_t)entry->line;
        if (line < start_line || line > end_line)
            continue;
        entry->valid = false;
        if (minimap->cached_line_count > 0u)
            --minimap->cached_line_count;
    }
    minimap->buffer_dirty = true;
    vg_widget_invalidate(&minimap->base);
}
