//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui_codeeditor.c
/// @brief Implements runtime bindings for CodeEditor state, editing, geometry, and buffers.
///
/// @details
/// The graphics-enabled path validates editor and buffer handles, translates
/// runtime strings and pixel data, exposes retained gutter/folding/cursor state,
/// and reports bounded performance counters. Graphics-disabled definitions
/// preserve the public ABI with deterministic inert results.
///
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/rt_gui_codeeditor.c
// Purpose: Runtime bindings for the ZannaGUI CodeEditor widget. Implements
//   syntax highlighting (Zia and BASIC keyword/type color tables), gutter icon
//   management, code folding, multiple cursors, edit operations, and completion
//   helpers. MessageBox, FileDialog, and FindBar live in separate files.
//
// Key invariants:
//   - Syntax highlight colors use ARGB 0xAARRGGBB format matching the VS Code
//     dark-theme palette defined at the top of this file.
//   - rt_codeeditor_get_selected_text() returns a freshly allocated C string
//     (from vg_codeeditor_get_selection); the caller must free it.
//
// Ownership/Lifetime:
//   - Selected-text C strings are malloc'd by the vg layer; the caller frees them.
//
// Links: src/runtime/graphics/rt_gui_internal.h (internal types/globals),
//        src/runtime/graphics/rt_gui_messagebox.c (MessageBox dialogs),
//        src/runtime/graphics/rt_gui_filedialog.c (FileDialog dialogs),
//        src/runtime/graphics/rt_gui_findbar.c (FindBar widget),
//        src/lib/gui/src/widgets/vg_codeeditor.c (underlying widget)
//
//===----------------------------------------------------------------------===//

#include "rt_error.h"
#include "rt_gui_codeeditor_internal.h"
#include "rt_gui_internal.h"
#include "rt_map.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include <ctype.h>

/// @brief Saturate a low-level unsigned editor counter to the public signed integer domain.
/// @details Performance counters intentionally never wrap in the runtime API; values beyond the
///          language's `i64` maximum remain pinned at `INT64_MAX` in both individual getters and
///          the consolidated stats Map.
/// @param value Low-level monotonic counter value.
/// @return Exact signed value when representable, otherwise `INT64_MAX`.
static int64_t rt_codeeditor_perf_i64(uint64_t value) {
    return rt_gui_saturating_u64_to_i64(value);
}

/// @brief Allocate the versioned public CodeEditor performance-statistics Map.
/// @details All nine raw lower-layer counters are emitted under stable camelCase keys even in a
///          graphics-disabled build or for an invalid editor. Keeping schema construction in one
///          helper prevents enabled/headless drift and lets diagnostics consume maps uniformly.
/// @param total_height_scans Lines visited while accumulating total document height.
/// @param total_visual_row_scans Lines visited while accumulating total wrapped visual rows.
/// @param visual_row_scans Lines visited by visual-row queries.
/// @param locate_visual_row_scans Lines visited while locating a visual row.
/// @param line_highlight_calls Syntax-highlighter line invocations.
/// @param syntax_state_line_scans Lines scanned to reconstruct cached syntax state.
/// @param highlight_span_checks Highlight spans inspected during paint.
/// @param full_text_copies Full-document text materializations.
/// @param full_text_copy_bytes Bytes copied by full-document materializations.
/// @return New managed Map with `schemaVersion=1`, or NULL on root allocation failure.
static void *rt_codeeditor_perf_stats_map(uint64_t total_height_scans,
                                          uint64_t total_visual_row_scans,
                                          uint64_t visual_row_scans,
                                          uint64_t locate_visual_row_scans,
                                          uint64_t line_highlight_calls,
                                          uint64_t syntax_state_line_scans,
                                          uint64_t highlight_span_checks,
                                          uint64_t full_text_copies,
                                          uint64_t full_text_copy_bytes) {
    void *map = rt_map_new();
    if (!map)
        return NULL;
    rt_map_set_int(map, rt_const_cstr("schemaVersion"), 1);
    rt_map_set_int(
        map, rt_const_cstr("totalHeightLinearScans"), rt_codeeditor_perf_i64(total_height_scans));
    rt_map_set_int(map,
                   rt_const_cstr("totalVisualRowLinearScans"),
                   rt_codeeditor_perf_i64(total_visual_row_scans));
    rt_map_set_int(
        map, rt_const_cstr("visualRowLinearScans"), rt_codeeditor_perf_i64(visual_row_scans));
    rt_map_set_int(map,
                   rt_const_cstr("locateVisualRowLinearScans"),
                   rt_codeeditor_perf_i64(locate_visual_row_scans));
    rt_map_set_int(
        map, rt_const_cstr("lineHighlightCalls"), rt_codeeditor_perf_i64(line_highlight_calls));
    rt_map_set_int(map,
                   rt_const_cstr("syntaxStateLineScans"),
                   rt_codeeditor_perf_i64(syntax_state_line_scans));
    rt_map_set_int(
        map, rt_const_cstr("highlightSpanChecks"), rt_codeeditor_perf_i64(highlight_span_checks));
    rt_map_set_int(map, rt_const_cstr("fullTextCopies"), rt_codeeditor_perf_i64(full_text_copies));
    rt_map_set_int(
        map, rt_const_cstr("fullTextCopyBytes"), rt_codeeditor_perf_i64(full_text_copy_bytes));
    return map;
}

#ifdef ZANNA_ENABLE_GRAPHICS

// CodeEditor Enhancements - Gutter & Line Numbers (Phase 4)
//=============================================================================

/// @brief `CodeEditor.SetShowLineNumbers(show)` — toggle the line-number gutter.
/// @param editor CodeEditor widget handle.
/// @param show Non-zero to show line numbers; zero to hide them.
void rt_codeeditor_set_show_line_numbers(void *editor, int64_t show) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->show_line_numbers = show != 0;
    vg_codeeditor_refresh_layout_state(ce);
}

/// @brief `CodeEditor.GetShowLineNumbers` — read the line-number visibility flag.
/// @param editor CodeEditor widget handle.
/// @return 1 when line numbers are visible, otherwise 0.
int64_t rt_codeeditor_get_show_line_numbers(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return ce->show_line_numbers ? 1 : 0;
}

/// @brief `CodeEditor.SetLineNumberWidth(width)` — set gutter width in characters.
///
/// Internally stored as pixels (`width * char_width`) so layout doesn't
/// have to keep recomputing it.
/// @param editor CodeEditor widget handle.
/// @param width Requested logical gutter width; non-positive values restore automatic sizing.
void rt_codeeditor_set_line_number_width(void *editor, int64_t width) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->line_number_width_override =
        width > 0 ? (float)rt_gui_clamp_i64_to_i32(width, 0, INT32_MAX) : 0.0f;
    vg_codeeditor_refresh_layout_state(ce);
}

/// @brief Convert an `rt_pixels` ARGB buffer into a `vg_icon_t`.
///
/// Repacks the source buffer (top-byte-alpha ARGB stored as uint32)
/// into the RGBA byte order vg expects. Validates dimensions to
/// guard against integer overflow (W*H*4 > SIZE_MAX). Traps on
/// allocation failure.
/// @param pixels Runtime Pixels handle; NULL produces an empty icon.
/// @return Owned toolkit icon value, or `VG_ICON_NONE` on invalid input/failure.
static vg_icon_t rt_codeeditor_icon_from_pixels(void *pixels) {
    vg_icon_t icon = {0};
    if (!pixels)
        return icon;

    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    const uint32_t *raw = rt_pixels_raw_buffer(pixels);
    if (width <= 0 || height <= 0 || !raw)
        return icon;
    if ((uintmax_t)width > (uintmax_t)SIZE_MAX || (uintmax_t)height > (uintmax_t)SIZE_MAX) {
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "CodeEditor.SetGutterIcon: icon too large");
        return icon;
    }

    size_t rgba_size = 0;
    if (!rt_gui_rgba_size_i64(width, height, &rgba_size)) {
        rt_trap_raise_kind(
            RT_TRAP_KIND_OVERFLOW, Err_Overflow, -1, "CodeEditor.SetGutterIcon: icon too large");
        return icon;
    }
    size_t pixel_count = rgba_size / 4u;

    uint8_t *rgba = (uint8_t *)malloc(rgba_size);
    if (!rgba) {
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "CodeEditor.SetGutterIcon: allocation failed");
        return icon;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t px = raw[i];
        rgba[i * 4 + 0] = (uint8_t)((px >> 24) & 0xFF);
        rgba[i * 4 + 1] = (uint8_t)((px >> 16) & 0xFF);
        rgba[i * 4 + 2] = (uint8_t)((px >> 8) & 0xFF);
        rgba[i * 4 + 3] = (uint8_t)(px & 0xFF);
    }

    icon = vg_icon_from_pixels(rgba, (uint32_t)width, (uint32_t)height);
    free(rgba);
    if (icon.type == VG_ICON_NONE)
        rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                           Err_RuntimeError,
                           -1,
                           "CodeEditor.SetGutterIcon: allocation failed");
    return icon;
}

/// @brief `CodeEditor.SetGutterIcon(line, pixels, slot)` — paint an icon in the gutter.
///
/// `slot` selects an icon "channel" so multiple icons can stack on
/// the same line: 0=breakpoint, 1=warning, 2=error, 3=info. Setting
/// the same line+slot replaces the existing icon. Geometric growth
/// for the icons array. Default per-slot tint colors are red/orange/
/// red/blue.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based source line.
/// @param pixels Runtime Pixels handle, or NULL to clear the slot.
/// @param slot Gutter icon channel.
void rt_codeeditor_set_gutter_icon(void *editor, int64_t line, void *pixels, int64_t slot) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (ce->gutter_icon_count < 0 || ce->gutter_icon_cap < 0 ||
        ce->gutter_icon_count > ce->gutter_icon_cap)
        return;
    int type = 0;
    if (!rt_codeeditor_gutter_slot_checked(slot, &type))
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    if (!pixels) {
        rt_codeeditor_clear_gutter_icon(editor, line_i, slot);
        return;
    }
    /* Update existing icon on same line+type if present */
    for (int i = 0; i < ce->gutter_icon_count; i++) {
        if (ce->gutter_icons[i].line == line_i && ce->gutter_icons[i].type == type) {
            vg_icon_t new_image = rt_codeeditor_icon_from_pixels(pixels);
            if (new_image.type == VG_ICON_NONE)
                return;
            vg_icon_destroy(&ce->gutter_icons[i].image);
            ce->gutter_icons[i].image = new_image;
            ce->base.needs_paint = true;
            return;
        }
    }
    if (ce->gutter_icon_count >= ce->gutter_icon_cap) {
        int new_cap = 0;
        if (!rt_gui_next_collection_capacity_i32(
                ce->gutter_icon_cap, ce->gutter_icon_count, 8, sizeof(*ce->gutter_icons), &new_cap))
            return;
        void *p = realloc(ce->gutter_icons, (size_t)new_cap * sizeof(*ce->gutter_icons));
        if (!p) {
            rt_trap_raise_kind(RT_TRAP_KIND_RUNTIME_ERROR,
                               Err_RuntimeError,
                               -1,
                               "CodeEditor.SetGutterIcon: allocation failed");
            return;
        }
        ce->gutter_icons = p;
        ce->gutter_icon_cap = new_cap;
    }
    /* Default color per type */
    static const uint32_t s_type_colors[] = {0xFFE81123, 0xFFFFB900, 0xFFE81123, 0xFF0078D4};
    vg_icon_t new_image = rt_codeeditor_icon_from_pixels(pixels);
    if (new_image.type == VG_ICON_NONE)
        return;
    struct vg_gutter_icon *icon = &ce->gutter_icons[ce->gutter_icon_count++];
    icon->line = line_i;
    icon->type = type;
    icon->style = 0;
    icon->color = s_type_colors[type];
    icon->image = new_image;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.SetGutterBar(line, colorRGB, slot)` — add/update a change
///        bar (thin vertical bar at the gutter's left edge) on a line. Used for
///        SCM diff markers; coexists with disc icons in other slots.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based source line.
/// @param color Packed RGB bar color.
/// @param slot Gutter channel used to identify replacement state.
void rt_codeeditor_set_gutter_bar(void *editor, int64_t line, int64_t color, int64_t slot) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (ce->gutter_icon_count < 0 || ce->gutter_icon_cap < 0 ||
        ce->gutter_icon_count > ce->gutter_icon_cap)
        return;
    int type = 0;
    if (!rt_codeeditor_gutter_slot_checked(slot, &type))
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    uint32_t rgb = (uint32_t)(color & 0xFFFFFFFF);
    /* Update an existing bar on the same line+slot. */
    for (int i = 0; i < ce->gutter_icon_count; i++) {
        if (ce->gutter_icons[i].line == line_i && ce->gutter_icons[i].type == type) {
            ce->gutter_icons[i].style = 1;
            ce->gutter_icons[i].color = rgb;
            vg_icon_destroy(&ce->gutter_icons[i].image);
            memset(&ce->gutter_icons[i].image, 0, sizeof(ce->gutter_icons[i].image));
            ce->base.needs_paint = true;
            return;
        }
    }
    if (ce->gutter_icon_count >= ce->gutter_icon_cap) {
        int new_cap = 0;
        if (!rt_gui_next_collection_capacity_i32(
                ce->gutter_icon_cap, ce->gutter_icon_count, 8, sizeof(*ce->gutter_icons), &new_cap))
            return;
        void *p = realloc(ce->gutter_icons, (size_t)new_cap * sizeof(*ce->gutter_icons));
        if (!p)
            return;
        ce->gutter_icons = p;
        ce->gutter_icon_cap = new_cap;
    }
    struct vg_gutter_icon *icon = &ce->gutter_icons[ce->gutter_icon_count++];
    icon->line = line_i;
    icon->type = type;
    icon->style = 1;
    icon->color = rgb;
    memset(&icon->image, 0, sizeof(icon->image));
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.ClearGutterIcon(line, slot)` — remove one icon entry.
///
/// Swap-with-last compaction. No-op if no matching icon exists.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based source line.
/// @param slot Gutter channel to clear.
void rt_codeeditor_clear_gutter_icon(void *editor, int64_t line, int64_t slot) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int type = 0;
    if (!rt_codeeditor_gutter_slot_checked(slot, &type))
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    for (int i = 0; i < ce->gutter_icon_count; i++) {
        if (ce->gutter_icons[i].line == line_i && ce->gutter_icons[i].type == type) {
            int last = --ce->gutter_icon_count;
            vg_icon_destroy(&ce->gutter_icons[i].image);
            if (i != last) {
                ce->gutter_icons[i] = ce->gutter_icons[last];
            }
            memset(&ce->gutter_icons[last], 0, sizeof(ce->gutter_icons[last]));
            ce->base.needs_paint = true;
            return;
        }
    }
}

/// @brief `CodeEditor.ClearAllGutterIcons(slot)` — remove every icon of a given type.
///
/// In-place compaction by writing kept entries to `[0..w)` and clearing
/// the trailing slots. Useful for "clear all breakpoints" type ops.
/// @param editor CodeEditor widget handle.
/// @param slot Gutter channel to clear across all lines.
void rt_codeeditor_clear_all_gutter_icons(void *editor, int64_t slot) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int type = 0;
    if (!rt_codeeditor_gutter_slot_checked(slot, &type))
        return;
    int w = 0;
    int original_count = ce->gutter_icon_count;
    for (int i = 0; i < ce->gutter_icon_count; i++) {
        if (ce->gutter_icons[i].type != type) {
            ce->gutter_icons[w++] = ce->gutter_icons[i];
            continue;
        }
        vg_icon_destroy(&ce->gutter_icons[i].image);
    }
    ce->gutter_icon_count = w;
    for (int i = w; i < original_count; i++)
        memset(&ce->gutter_icons[i], 0, sizeof(ce->gutter_icons[i]));
    ce->base.needs_paint = true;
}

// Gutter click tracking — per-editor state (not global statics) so
// multiple CodeEditor instances each track their own gutter clicks.

/// @brief Legacy global setter — currently a no-op, kept for ABI stability.
///
/// The vg layer paint callback doesn't pass the editor pointer
/// through, so we can't route clicks to the right editor here. A
/// future refactor would pass the editor through; until then, gutter
/// state is per-editor and updated directly inside the widget code.
/// @param line Ignored legacy zero-based line.
/// @param slot Ignored legacy gutter slot.
void rt_gui_set_gutter_click(int64_t line, int64_t slot) {
    RT_ASSERT_MAIN_THREAD();
    // Legacy global entry point — forwards to a per-editor setter.
    // The vg layer paint callback doesn't know which editor was clicked,
    // so we broadcast to the most-recently-focused editor via s_current_app.
    // A future improvement would pass the editor pointer through the callback.
    (void)line;
    (void)slot;
}

/// @brief Legacy global clear — currently a no-op (state lives per-editor).
void rt_gui_clear_gutter_click(void) {
    RT_ASSERT_MAIN_THREAD();
    // No-op: per-editor state is cleared when WasGutterClicked consumes an edge.
}

/// @brief `CodeEditor.WasGutterClicked` — edge-detect: true once per click.
///
/// Returns the latched click flag once and clears the click payload. Callers
/// that need the click line or slot should read those coordinates before
/// consuming the edge flag.
/// @param editor CodeEditor widget handle.
/// @return 1 once when a gutter click is pending, otherwise 0.
int64_t rt_codeeditor_was_gutter_clicked(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    if (!ce->gutter_clicked)
        return 0;
    ce->gutter_clicked = false;
    ce->gutter_click_read = true;
    ce->gutter_clicked_line = -1;
    ce->gutter_clicked_slot = -1;
    return 1;
}

/// @brief `CodeEditor.TakeGutterClick` — atomically consume the pending gutter click.
/// @details Returns a map with `clicked`, `line`, and `slot` keys. Unlike the
///          legacy separate getters, this API has no read-order hazard: the
///          payload is captured before the edge flag is cleared.
/// @param editor CodeEditor handle.
/// @return Runtime map describing the click; `clicked=false` uses line/slot -1.
void *rt_codeeditor_take_gutter_click(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    void *result = rt_map_new();
    if (!result)
        return NULL;
    if (!ce || !ce->gutter_clicked) {
        rt_map_set_bool(result, rt_const_cstr("clicked"), 0);
        rt_map_set_int(result, rt_const_cstr("line"), -1);
        rt_map_set_int(result, rt_const_cstr("slot"), -1);
        return result;
    }
    int line = ce->gutter_clicked_line;
    int slot = ce->gutter_clicked_slot;
    ce->gutter_clicked = false;
    ce->gutter_click_read = true;
    ce->gutter_clicked_line = -1;
    ce->gutter_clicked_slot = -1;
    rt_map_set_bool(result, rt_const_cstr("clicked"), 1);
    rt_map_set_int(result, rt_const_cstr("line"), line);
    rt_map_set_int(result, rt_const_cstr("slot"), slot);
    return result;
}

/// @brief `CodeEditor.GetGutterClickedLine` — line number of the most recent click.
///
/// Returns -1 for NULL receiver or no pending gutter click. The payload is
/// available until `WasGutterClicked` consumes the edge flag.
/// @param editor CodeEditor widget handle.
/// @return Zero-based clicked line, or -1 when no click is pending.
int64_t rt_codeeditor_get_gutter_clicked_line(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce || !ce->gutter_clicked || ce->gutter_clicked_line < 0)
        return -1;
    return ce->gutter_clicked_line;
}

/// @brief `CodeEditor.GetGutterClickedSlot` — slot index of the most recent click.
/// @param editor CodeEditor widget handle.
/// @return Clicked gutter slot, or -1 when no click is pending.
int64_t rt_codeeditor_get_gutter_clicked_slot(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce || !ce->gutter_clicked || ce->gutter_clicked_slot < 0)
        return -1;
    return ce->gutter_clicked_slot;
}

/// @brief `CodeEditor.SetShowFoldGutter(show)` — toggle the fold-region gutter.
///
/// The fold gutter sits next to the line-number gutter and shows
/// triangle indicators next to foldable regions.
/// @param editor CodeEditor widget handle.
/// @param show Non-zero to show fold indicators; zero to hide them.
void rt_codeeditor_set_show_fold_gutter(void *editor, int64_t show) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->show_fold_gutter = show != 0;
    vg_codeeditor_refresh_layout_state(ce);
}

//=============================================================================
// CodeEditor Enhancements - Code Folding (Phase 4)
//=============================================================================

/// @brief `CodeEditor.AddFoldRegion(startLine, endLine)` — register a foldable region.
///
/// Regions can be folded/unfolded individually via `Fold(line)`/`Unfold(line)`
/// or in bulk via `FoldAll`/`UnfoldAll`. Initial state is unfolded.
/// Existing regions with the same start line are updated in place. Overlapping
/// regions with different starts are ignored so hidden-line and navigation math
/// never has to reconcile ambiguous fold ownership.
/// @param editor CodeEditor widget handle.
/// @param start_line Zero-based first line of the fold.
/// @param end_line Zero-based inclusive final line.
void rt_codeeditor_add_fold_region(void *editor, int64_t start_line, int64_t end_line) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (ce->fold_region_count < 0 || ce->fold_region_cap < 0 ||
        ce->fold_region_count > ce->fold_region_cap)
        return;
    if (start_line < 0)
        start_line = 0;
    if (end_line >= ce->line_count)
        end_line = ce->line_count - 1;
    if (end_line <= start_line)
        return;
    int start_i = rt_gui_clamp_i64_to_i32(start_line, 0, INT32_MAX);
    int end_i = rt_gui_clamp_i64_to_i32(end_line, 0, INT32_MAX);
    for (int i = 0; i < ce->fold_region_count; i++) {
        if (ce->fold_regions[i].start_line == start_i) {
            bool was_folded = ce->fold_regions[i].folded;
            ce->fold_regions[i].end_line = end_i;
            ce->fold_regions[i].folded = false;
            if (was_folded)
                vg_codeeditor_refresh_layout_state(ce);
            else
                ce->base.needs_paint = true;
            return;
        }
        if (start_i <= ce->fold_regions[i].end_line && end_i >= ce->fold_regions[i].start_line)
            return;
    }
    if (ce->fold_region_count >= ce->fold_region_cap) {
        int new_cap = 0;
        if (!rt_gui_next_collection_capacity_i32(
                ce->fold_region_cap, ce->fold_region_count, 8, sizeof(*ce->fold_regions), &new_cap))
            return;
        void *p = realloc(ce->fold_regions, (size_t)new_cap * sizeof(*ce->fold_regions));
        if (!p)
            return;
        ce->fold_regions = p;
        ce->fold_region_cap = new_cap;
    }
    struct vg_fold_region *r = &ce->fold_regions[ce->fold_region_count++];
    r->start_line = start_i;
    r->end_line = end_i;
    r->folded = false;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.RemoveFoldRegion(startLine)` — drop a fold region.
///
/// Identified by the start line. Swap-with-last compaction. No-op if
/// no region starts at the given line.
/// @param editor CodeEditor widget handle.
/// @param start_line Zero-based fold start to remove.
void rt_codeeditor_remove_fold_region(void *editor, int64_t start_line) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int start_i = rt_gui_clamp_i64_to_i32(start_line, 0, INT32_MAX);
    for (int i = 0; i < ce->fold_region_count; i++) {
        if (ce->fold_regions[i].start_line == start_i) {
            ce->fold_regions[i] = ce->fold_regions[--ce->fold_region_count];
            vg_codeeditor_refresh_layout_state(ce);
            return;
        }
    }
}

/// @brief `CodeEditor.ClearFoldRegions` — drop every registered fold region.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_clear_fold_regions(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    free(ce->fold_regions);
    ce->fold_regions = NULL;
    ce->fold_region_count = 0;
    ce->fold_region_cap = 0;
    vg_codeeditor_refresh_layout_state(ce);
}

/// @brief `CodeEditor.Fold(line)` — collapse the region starting at `line`.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based fold start line.
void rt_codeeditor_fold(void *editor, int64_t line) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    for (int i = 0; i < ce->fold_region_count; i++) {
        if (ce->fold_regions[i].start_line == line_i) {
            ce->fold_regions[i].folded = true;
            vg_codeeditor_refresh_layout_state(ce);
            return;
        }
    }
}

/// @brief `CodeEditor.Unfold(line)` — expand the region starting at `line`.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based fold start line.
void rt_codeeditor_unfold(void *editor, int64_t line) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    for (int i = 0; i < ce->fold_region_count; i++) {
        if (ce->fold_regions[i].start_line == line_i) {
            ce->fold_regions[i].folded = false;
            vg_codeeditor_refresh_layout_state(ce);
            return;
        }
    }
}

/// @brief `CodeEditor.ToggleFold(line)` — flip the folded state of one region.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based fold start line.
void rt_codeeditor_toggle_fold(void *editor, int64_t line) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    for (int i = 0; i < ce->fold_region_count; i++) {
        if (ce->fold_regions[i].start_line == line_i) {
            ce->fold_regions[i].folded = !ce->fold_regions[i].folded;
            vg_codeeditor_refresh_layout_state(ce);
            return;
        }
    }
}

/// @brief `CodeEditor.IsFolded(line)` — true iff the region starting at `line` is collapsed.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based fold start line.
/// @return 1 when the matching region is folded, otherwise 0.
int64_t rt_codeeditor_is_folded(void *editor, int64_t line) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    for (int i = 0; i < ce->fold_region_count; i++) {
        if (ce->fold_regions[i].start_line == line_i)
            return ce->fold_regions[i].folded ? 1 : 0;
    }
    return 0;
}

/// @brief `CodeEditor.FoldAll` — collapse every fold region.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_fold_all(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    for (int i = 0; i < ce->fold_region_count; i++)
        ce->fold_regions[i].folded = true;
    vg_codeeditor_refresh_layout_state(ce);
}

/// @brief `CodeEditor.UnfoldAll` — expand every fold region.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_unfold_all(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    for (int i = 0; i < ce->fold_region_count; i++)
        ce->fold_regions[i].folded = false;
    vg_codeeditor_refresh_layout_state(ce);
}

/// @brief `CodeEditor.SetAutoFoldDetection(enable)` — derive fold regions from indentation.
///
/// When enabled, immediately walks the buffer looking for places where
/// the next line is more indented than the current line and registers
/// a fold region from the start of the indented block to the line where
/// indentation drops back. Blank lines extend the fold (don't break it).
/// Replaces any manually-added regions. No effect if the buffer is empty.
/// @param editor CodeEditor widget handle.
/// @param enable Non-zero to enable indentation-derived regions.
void rt_codeeditor_set_auto_fold_detection(void *editor, int64_t enable) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->auto_fold_detection = enable != 0;

    // When enabling, immediately detect fold regions from indentation.
    if (ce->auto_fold_detection && ce->line_count > 0) {
        // Clear existing fold regions
        free(ce->fold_regions);
        ce->fold_regions = NULL;
        ce->fold_region_count = 0;
        ce->fold_region_cap = 0;

        // Indent-based fold detection: a fold starts when the next line's
        // indentation increases, and ends when it returns to the start level.
        for (int i = 0; i < ce->line_count - 1; i++) {
            // Count leading spaces/tabs for this line and next
            const char *cur = ce->lines[i].text;
            const char *nxt = ce->lines[i + 1].text;
            int cur_indent = 0, nxt_indent = 0;
            while (cur[cur_indent] == ' ' || cur[cur_indent] == '\t')
                cur_indent++;
            while (nxt[nxt_indent] == ' ' || nxt[nxt_indent] == '\t')
                nxt_indent++;

            // Skip blank lines
            if ((size_t)cur_indent >= ce->lines[i].length)
                continue;

            // Fold region starts when indentation increases
            if (nxt_indent > cur_indent) {
                int start_line = i;
                int base_indent = cur_indent;

                // Find end: where indentation returns to base level or below
                int end_line = i + 1;
                for (int j = i + 2; j < ce->line_count; j++) {
                    const char *line = ce->lines[j].text;
                    int indent = 0;
                    while (line[indent] == ' ' || line[indent] == '\t')
                        indent++;
                    if ((size_t)indent >= ce->lines[j].length) {
                        end_line = j; // blank line extends the fold
                        continue;
                    }
                    if (indent <= base_indent)
                        break;
                    end_line = j;
                }

                if (end_line > start_line) {
                    // Add fold region via realloc
                    if (ce->fold_region_count >= ce->fold_region_cap) {
                        int new_cap = 0;
                        if (!rt_gui_next_collection_capacity_i32(ce->fold_region_cap,
                                                                 ce->fold_region_count,
                                                                 16,
                                                                 sizeof(*ce->fold_regions),
                                                                 &new_cap))
                            break;
                        void *p =
                            realloc(ce->fold_regions, (size_t)new_cap * sizeof(*ce->fold_regions));
                        if (!p)
                            break;
                        ce->fold_regions = p;
                        ce->fold_region_cap = new_cap;
                    }
                    struct vg_fold_region *r = &ce->fold_regions[ce->fold_region_count++];
                    r->start_line = start_line;
                    r->end_line = end_line;
                    r->folded = false;

                    // Skip past this fold region
                    i = end_line - 1;
                }
            }
        }
    }
    vg_codeeditor_refresh_layout_state(ce);
}

//=============================================================================
// CodeEditor Enhancements - Multiple Cursors (Phase 4)
//=============================================================================

/// @brief Clamp a `(line, col)` pair to a valid in-buffer position.
///
/// Negative coordinates clamp to 0; out-of-bounds line clamps to the
/// last line; out-of-bounds column clamps to the line's length. Used
/// before storing user-supplied cursor positions to avoid OOB reads.
/// @param ce Borrowed live CodeEditor.
/// @param line In/out zero-based line.
/// @param col In/out zero-based column.
static void rt_codeeditor_clamp_position(vg_codeeditor_t *ce, int *line, int *col) {
    if (!ce || !line || !col || ce->line_count <= 0)
        return;

    if (*line < 0)
        *line = 0;
    if (*line >= ce->line_count)
        *line = ce->line_count - 1;
    if (*col < 0)
        *col = 0;
    int line_len = rt_codeeditor_line_length_i32(ce, *line);
    if (*col > line_len)
        *col = line_len;
}

/// @brief `CodeEditor.GetCursorCount` — number of active cursors (always >= 1).
///
/// Always 1 + extras; the primary cursor is always present. Returns 1
/// for NULL receiver to match the "at least one cursor" invariant.
/// @param editor CodeEditor widget handle.
/// @return Number of active cursors, at least 1.
int64_t rt_codeeditor_get_cursor_count(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 1;
    return 1 + ce->extra_cursor_count;
}

/// @brief Check whether a cursor already exists at the given editor position.
/// @details Multi-cursor commands may discover the same occurrence through
///          overlapping selections or repeated invocation. Keeping cursor
///          positions unique prevents duplicate edits from being applied at the
///          same byte offset.
/// @param ce Editor whose primary and secondary cursors are inspected.
/// @param line Zero-based, clamped source line.
/// @param col Zero-based, clamped byte column.
/// @return true when the primary cursor or an existing extra cursor matches.
static bool rt_codeeditor_cursor_exists_at(const vg_codeeditor_t *ce, int line, int col) {
    if (!ce)
        return false;
    if (ce->cursor_line == line && ce->cursor_col == col)
        return true;
    for (int i = 0; i < ce->extra_cursor_count; i++) {
        if (ce->extra_cursors[i].line == line && ce->extra_cursors[i].col == col)
            return true;
    }
    return false;
}

/// @brief `CodeEditor.AddCursor(line, col)` — add a secondary cursor.
///
/// Index 0 is reserved for the primary cursor; new cursors get
/// indices 1, 2, … Position is clamped to a valid buffer position and
/// duplicate cursor positions are ignored. Geometric growth doubles capacity
/// starting at 4.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based source line.
/// @param col Zero-based character column.
void rt_codeeditor_add_cursor(void *editor, int64_t line, int64_t col) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (ce->extra_cursor_count < 0 || ce->extra_cursor_cap < 0 ||
        ce->extra_cursor_count > ce->extra_cursor_cap)
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    int col_i = rt_gui_clamp_i64_to_i32(col, 0, INT32_MAX);
    rt_codeeditor_clamp_position(ce, &line_i, &col_i);
    if (rt_codeeditor_cursor_exists_at(ce, line_i, col_i))
        return;
    if (ce->extra_cursor_count >= ce->extra_cursor_cap) {
        int new_cap = 0;
        if (!rt_gui_next_collection_capacity_i32(ce->extra_cursor_cap,
                                                 ce->extra_cursor_count,
                                                 4,
                                                 sizeof(*ce->extra_cursors),
                                                 &new_cap))
            return;
        void *p = realloc(ce->extra_cursors, (size_t)new_cap * sizeof(*ce->extra_cursors));
        if (!p)
            return;
        ce->extra_cursors = p;
        ce->extra_cursor_cap = new_cap;
    }
    struct vg_extra_cursor *c = &ce->extra_cursors[ce->extra_cursor_count++];
    c->line = line_i;
    c->col = col_i;
    memset(&c->selection, 0, sizeof(c->selection));
    c->has_selection = false;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.RemoveCursor(index)` — drop a secondary cursor.
///
/// Index 0 (primary) cannot be removed (that cursor is intrinsic to
/// the editor). Indices 1+ refer to entries in the `extra_cursors`
/// array. Shifts remaining cursors down to keep the array dense.
/// @param editor CodeEditor widget handle.
/// @param index Cursor index; zero is ignored.
void rt_codeeditor_remove_cursor(void *editor, int64_t index) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (index <= 0 || index > INT32_MAX)
        return;
    int idx = (int)index - 1; /* index 0 = primary cursor (not in extra array) */
    if (idx >= ce->extra_cursor_count)
        return;
    /* Shift remaining cursors down */
    for (int i = idx; i < ce->extra_cursor_count - 1; i++)
        ce->extra_cursors[i] = ce->extra_cursors[i + 1];
    ce->extra_cursor_count--;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.ClearExtraCursors` — remove every secondary cursor.
///
/// Primary cursor stays. Useful for "Escape" key handling in
/// multi-cursor editing modes.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_clear_extra_cursors(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    free(ce->extra_cursors);
    ce->extra_cursors = NULL;
    ce->extra_cursor_count = 0;
    ce->extra_cursor_cap = 0;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.GetCursorLineAt(index)` — line of the i-th cursor.
///
/// Index 0 is the primary cursor; 1+ are extras. Out-of-range
/// returns 0 (defensive default).
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @return Zero-based line, or 0 for invalid input.
int64_t rt_codeeditor_get_cursor_line_at(void *editor, int64_t index) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    if (index == 0)
        return ce->cursor_line;
    if (index < 0 || index > INT32_MAX)
        return 0;
    int extra_idx = (int)index - 1;
    if (extra_idx >= 0 && extra_idx < ce->extra_cursor_count)
        return ce->extra_cursors[extra_idx].line;
    return 0;
}

/// @brief `CodeEditor.GetCursorColAt(index)` — column of the i-th cursor.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @return Zero-based character column, or 0 for invalid input.
int64_t rt_codeeditor_get_cursor_col_at(void *editor, int64_t index) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    if (index == 0)
        return rt_codeeditor_byte_col_to_char_col(ce, ce->cursor_line, ce->cursor_col);
    if (index < 0 || index > INT32_MAX)
        return 0;
    int extra_idx = (int)index - 1;
    if (extra_idx >= 0 && extra_idx < ce->extra_cursor_count)
        return rt_codeeditor_byte_col_to_char_col(
            ce, ce->extra_cursors[extra_idx].line, ce->extra_cursors[extra_idx].col);
    return 0;
}

/// @brief `CodeEditor.CursorLine` — convenience for the primary cursor's line.
/// @param editor CodeEditor widget handle.
/// @return Primary cursor's zero-based line.
int64_t rt_codeeditor_get_cursor_line(void *editor) {
    return rt_codeeditor_get_cursor_line_at(editor, 0);
}

/// @brief `CodeEditor.CursorCol` — convenience for the primary cursor's column.
/// @param editor CodeEditor widget handle.
/// @return Primary cursor's zero-based character column.
int64_t rt_codeeditor_get_cursor_col(void *editor) {
    return rt_codeeditor_get_cursor_col_at(editor, 0);
}

/// @brief `CodeEditor.ScrollTopLine` — source line nearest the viewport top.
/// @param editor CodeEditor widget handle.
/// @return Zero-based top source line, or 0 for an invalid handle.
int64_t rt_codeeditor_get_scroll_top_line(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return vg_codeeditor_get_scroll_top_line(ce);
}

/// @brief Set `CodeEditor.ScrollTopLine`.
/// @param editor CodeEditor widget handle.
/// @param line Requested zero-based top source line.
void rt_codeeditor_set_scroll_top_line(void *editor, int64_t line) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    vg_codeeditor_set_scroll_top_line(ce, line_i);
}

/// @brief `CodeEditor.SetCursorPositionAt(index, line, col)` — move one cursor.
///
/// Index 0 routes to the underlying widget's `set_cursor` (which also
/// scrolls the viewport). Index 1+ updates the extras directly.
/// Position is clamped; selection is cleared.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @param line Zero-based source line.
/// @param col Zero-based character column.
void rt_codeeditor_set_cursor_position_at(void *editor, int64_t index, int64_t line, int64_t col) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int line_i = rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX);
    int col_i = rt_gui_clamp_i64_to_i32(col, 0, INT32_MAX);
    if (index == 0) {
        vg_codeeditor_set_cursor(ce, line_i, col_i);
        return;
    }
    if (index < 0 || index > INT32_MAX)
        return;
    int extra_idx = (int)index - 1;
    if (extra_idx < 0 || extra_idx >= ce->extra_cursor_count)
        return;
    rt_codeeditor_clamp_position(ce, &line_i, &col_i);
    col_i = rt_codeeditor_char_col_to_byte_col(ce, line_i, col_i);
    ce->extra_cursors[extra_idx].line = line_i;
    ce->extra_cursors[extra_idx].col = col_i;
    ce->extra_cursors[extra_idx].has_selection = false;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.SetCursorSelection(index, sLine, sCol, eLine, eCol)` — set a selection.
///
/// Both start and end positions are clamped. The cursor for that
/// index moves to the end of the selection (matching standard
/// editor behavior where shift+click extends from the existing
/// cursor to the click position).
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @param start_line Zero-based selection start line.
/// @param start_col Zero-based selection start character column.
/// @param end_line Zero-based selection end line.
/// @param end_col Zero-based selection end character column.
void rt_codeeditor_set_cursor_selection(void *editor,
                                        int64_t index,
                                        int64_t start_line,
                                        int64_t start_col,
                                        int64_t end_line,
                                        int64_t end_col) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    int s_line = rt_gui_clamp_i64_to_i32(start_line, 0, INT32_MAX);
    int s_col = rt_gui_clamp_i64_to_i32(start_col, 0, INT32_MAX);
    int e_line = rt_gui_clamp_i64_to_i32(end_line, 0, INT32_MAX);
    int e_col = rt_gui_clamp_i64_to_i32(end_col, 0, INT32_MAX);
    rt_codeeditor_clamp_position(ce, &s_line, &s_col);
    rt_codeeditor_clamp_position(ce, &e_line, &e_col);

    if (index == 0) {
        vg_codeeditor_set_selection(ce, s_line, s_col, e_line, e_col);
        return;
    }

    if (index < 0 || index > INT32_MAX)
        return;
    int extra_idx = (int)index - 1;
    if (extra_idx < 0 || extra_idx >= ce->extra_cursor_count)
        return;

    s_col = rt_codeeditor_char_col_to_byte_col(ce, s_line, s_col);
    e_col = rt_codeeditor_char_col_to_byte_col(ce, e_line, e_col);
    ce->extra_cursors[extra_idx].selection.start_line = s_line;
    ce->extra_cursors[extra_idx].selection.start_col = s_col;
    ce->extra_cursors[extra_idx].selection.end_line = e_line;
    ce->extra_cursors[extra_idx].selection.end_col = e_col;
    ce->extra_cursors[extra_idx].line = e_line;
    ce->extra_cursors[extra_idx].col = e_col;
    ce->extra_cursors[extra_idx].has_selection = s_line != e_line || s_col != e_col;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.CursorHasSelection(index)` — whether the i-th cursor has a selection.
///
/// Index 0 reads the editor's main `has_selection` flag; extras keep
/// their own per-cursor flag set by `SetCursorSelection`.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @return 1 when that cursor has a non-empty selection, otherwise 0.
int64_t rt_codeeditor_cursor_has_selection(void *editor, int64_t index) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    if (index == 0)
        return ce->has_selection ? 1 : 0;
    if (index < 0 || index > INT32_MAX)
        return 0;
    int extra_idx = (int)index - 1;
    if (extra_idx < 0 || extra_idx >= ce->extra_cursor_count)
        return 0;
    return ce->extra_cursors[extra_idx].has_selection ? 1 : 0;
}

/// @brief Fetch the selection range for cursor @p index (0 = primary caret,
///        ≥1 = extra multi-cursor) into @p out.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @param out Receives the normalized byte-column selection.
/// @return true if that cursor has an active selection; false otherwise.
static bool rt_codeeditor_get_selection_at(void *editor, int64_t index, vg_selection_t *out) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce || !out)
        return false;

    if (index == 0) {
        if (!ce->has_selection)
            return false;
        *out = ce->selection;
    } else {
        if (index < 0 || index > INT32_MAX)
            return false;
        int extra_idx = (int)index - 1;
        if (extra_idx < 0 || extra_idx >= ce->extra_cursor_count ||
            !ce->extra_cursors[extra_idx].has_selection)
            return false;
        *out = ce->extra_cursors[extra_idx].selection;
    }

    rt_codeeditor_normalize_selection(out);
    return true;
}

/// @brief `CodeEditor.GetSelectionStartLineAt(index)` — normalized selection start line.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @return Zero-based start line, or 0 when no selection exists.
int64_t rt_codeeditor_get_selection_start_line_at(void *editor, int64_t index) {
    vg_selection_t selection;
    if (!rt_codeeditor_get_selection_at(editor, index, &selection))
        return 0;
    return selection.start_line;
}

/// @brief `CodeEditor.GetSelectionStartColAt(index)` — normalized selection start column.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @return Zero-based character column, or 0 when no selection exists.
int64_t rt_codeeditor_get_selection_start_col_at(void *editor, int64_t index) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    vg_selection_t selection;
    if (!rt_codeeditor_get_selection_at(editor, index, &selection))
        return 0;
    return rt_codeeditor_byte_col_to_char_col(ce, selection.start_line, selection.start_col);
}

/// @brief `CodeEditor.GetSelectionEndLineAt(index)` — normalized selection end line.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @return Zero-based end line, or 0 when no selection exists.
int64_t rt_codeeditor_get_selection_end_line_at(void *editor, int64_t index) {
    vg_selection_t selection;
    if (!rt_codeeditor_get_selection_at(editor, index, &selection))
        return 0;
    return selection.end_line;
}

/// @brief `CodeEditor.GetSelectionEndColAt(index)` — normalized selection end column.
/// @param editor CodeEditor widget handle.
/// @param index Zero-based cursor index.
/// @return Zero-based character column, or 0 when no selection exists.
int64_t rt_codeeditor_get_selection_end_col_at(void *editor, int64_t index) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    vg_selection_t selection;
    if (!rt_codeeditor_get_selection_at(editor, index, &selection))
        return 0;
    return rt_codeeditor_byte_col_to_char_col(ce, selection.end_line, selection.end_col);
}

// Edit-history and clipboard ops — thin wrappers around the underlying
// `vg_codeeditor_*` widget API. NULL receiver is a no-op (or zero return).

/// @brief `CodeEditor.Undo` — pop one entry from the undo stack.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_undo(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (ce)
        vg_codeeditor_undo(ce);
}

/// @brief `CodeEditor.Redo` — re-apply one undone entry.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_redo(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (ce)
        vg_codeeditor_redo(ce);
}

/// @brief `CodeEditor.CanUndo` — true when the undo stack has an available entry.
/// @param editor CodeEditor widget handle.
/// @return 1 when undo is available, otherwise 0.
int64_t rt_codeeditor_can_undo(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return (ce->history && ce->history->current_index > 0) ? 1 : 0;
}

/// @brief `CodeEditor.CanRedo` — true when the redo stack has an available entry.
/// @param editor CodeEditor widget handle.
/// @return 1 when redo is available, otherwise 0.
int64_t rt_codeeditor_can_redo(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return (ce->history && ce->history->current_index < ce->history->count) ? 1 : 0;
}

/// @brief `CodeEditor.Copy` — copy selection to the system clipboard. Returns 1 on success.
/// @param editor CodeEditor widget handle.
/// @return 1 on success, otherwise 0.
int64_t rt_codeeditor_copy(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return vg_codeeditor_copy(ce) ? 1 : 0;
}

/// @brief `CodeEditor.Cut` — copy selection then delete. Returns 1 on success.
/// @param editor CodeEditor widget handle.
/// @return 1 on success, otherwise 0.
int64_t rt_codeeditor_cut(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return vg_codeeditor_cut(ce) ? 1 : 0;
}

/// @brief `CodeEditor.Paste` — insert clipboard text at cursor. Returns 1 on success.
/// @param editor CodeEditor widget handle.
/// @return 1 on success, otherwise 0.
int64_t rt_codeeditor_paste(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return vg_codeeditor_paste(ce) ? 1 : 0;
}

/// @brief `CodeEditor.SelectAll` — selection from buffer start to end.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_select_all(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (ce)
        vg_codeeditor_select_all(ce);
}

/// @brief `CodeEditor.SetTabSize` — set tab width in spaces.
/// @param editor CodeEditor widget handle.
/// @param size Tab width clamped to 1 through 16 spaces.
void rt_codeeditor_set_tab_size(void *editor, int64_t size) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (size < 1)
        size = 1;
    if (size > 16)
        size = 16;
    ce->tab_width = (int)size;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.GetTabSize` — return tab width in spaces.
/// @param editor CodeEditor widget handle.
/// @return Tab width, or 0 for an invalid handle.
int64_t rt_codeeditor_get_tab_size(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return ce->tab_width;
}

/// @brief `CodeEditor.SetInsertSpaces` — choose soft tabs vs hard tabs.
/// @param editor CodeEditor widget handle.
/// @param enabled Non-zero for spaces; zero for tab characters.
void rt_codeeditor_set_insert_spaces(void *editor, int64_t enabled) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->use_spaces = enabled != 0;
}

/// @brief `CodeEditor.GetInsertSpaces` — return soft-tab setting.
/// @param editor CodeEditor widget handle.
/// @return 1 when inserting spaces, otherwise 0.
int64_t rt_codeeditor_get_insert_spaces(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return ce->use_spaces ? 1 : 0;
}

/// @brief `CodeEditor.SetWordWrap` — toggle display-only word wrapping.
/// @param editor CodeEditor widget handle.
/// @param enabled Non-zero to wrap visual rows; zero for horizontal scrolling.
void rt_codeeditor_set_word_wrap(void *editor, int64_t enabled) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->word_wrap = enabled != 0;
    if (ce->word_wrap)
        ce->scroll_x = 0.0f;
    vg_codeeditor_refresh_layout_state(ce);
}

/// @brief Enable or disable ligature shaping for one editor (ADR 0137).
/// @param editor CodeEditor widget handle.
/// @param enabled Non-zero to enable ligature shaping.
void rt_codeeditor_set_ligatures_enabled(void *editor, int64_t enabled) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    vg_codeeditor_set_ligatures_enabled(ce, enabled != 0);
}

/// @brief Return whether one editor renders ligatures.
/// @param editor CodeEditor widget handle.
/// @return 1 when ligatures are enabled, otherwise 0.
int64_t rt_codeeditor_get_ligatures_enabled(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    return ce && vg_codeeditor_get_ligatures_enabled(ce) ? 1 : 0;
}

/// @brief `CodeEditor.GetWordWrap` — return display-only word wrapping state.
/// @param editor CodeEditor widget handle.
/// @return 1 when word wrapping is enabled, otherwise 0.
int64_t rt_codeeditor_get_word_wrap(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return ce->word_wrap ? 1 : 0;
}

/// @brief `CodeEditor.SetWhitespaceMode` — set space/tab marker rendering
///        (0=none, 1=boundary, 2=all). Out-of-range values clamp to none.
/// @param editor CodeEditor widget handle.
/// @param mode Whitespace rendering mode.
void rt_codeeditor_set_whitespace_mode(void *editor, int64_t mode) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (mode < VG_WHITESPACE_NONE || mode > VG_WHITESPACE_ALL)
        mode = VG_WHITESPACE_NONE;
    ce->render_whitespace_mode = (int)mode;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.GetWhitespaceMode` — return the whitespace marker mode.
/// @param editor CodeEditor widget handle.
/// @return Current whitespace rendering mode, or 0 for an invalid handle.
int64_t rt_codeeditor_get_whitespace_mode(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return (int64_t)ce->render_whitespace_mode;
}

/// @brief `CodeEditor.SetShowIndentGuides` — toggle faint indentation guides.
/// @param editor CodeEditor widget handle.
/// @param enabled Non-zero to render indentation guides.
void rt_codeeditor_set_show_indent_guides(void *editor, int64_t enabled) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->show_indent_guides = enabled != 0;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.GetShowIndentGuides` — return the indent-guide setting.
/// @param editor CodeEditor widget handle.
/// @return 1 when indentation guides are visible, otherwise 0.
int64_t rt_codeeditor_get_show_indent_guides(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return ce->show_indent_guides ? 1 : 0;
}

/// @brief `CodeEditor.SetReadOnly` — toggle text mutation for the editor.
/// @details Read-only mode keeps navigation, selection, copy, scrolling, and
///          highlighting active, but the widget rejects text insertion,
///          deletion, paste, cut, undo, and redo paths that would mutate text.
/// @param editor CodeEditor handle.
/// @param enabled Non-zero to make the buffer read-only.
void rt_codeeditor_set_read_only(void *editor, int64_t enabled) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    ce->read_only = enabled != 0;
    ce->cursor_visible = !ce->read_only;
    ce->base.needs_paint = true;
}

/// @brief `CodeEditor.GetReadOnly` — return whether text mutation is disabled.
/// @param editor CodeEditor handle.
/// @return 1 when read-only, 0 otherwise.
int64_t rt_codeeditor_get_read_only(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    return ce->read_only ? 1 : 0;
}

//=============================================================================
// CodeEditor Completion Helpers
//=============================================================================

#define RT_CODEEDITOR_SCROLLBAR_WIDTH 12.0f

/// @brief Test whether a line is hidden inside a folded region.
/// @details Walks the fold-region list and returns 1 if `line` falls strictly
///          inside any active fold (`start_line < line <= end_line`). The
///          fold's start line itself stays visible — it carries the fold-icon
///          glyph and shows the collapsed-summary text — so the check is
///          asymmetric on purpose.
/// @param ce Borrowed CodeEditor.
/// @param line Zero-based source line.
/// @return 1 if the line is currently hidden by an active fold, 0 otherwise.
static int rt_codeeditor_line_is_hidden(const vg_codeeditor_t *ce, int line) {
    if (!ce)
        return 0;
    for (int i = 0; i < ce->fold_region_count; i++) {
        const struct vg_fold_region *region = &ce->fold_regions[i];
        if (region->folded && line > region->start_line && line <= region->end_line)
            return 1;
    }
    return 0;
}

/// @brief Resolve a possibly-hidden line to the visible line that anchors it.
/// @details When code-folding hides a line, cursor / scroll / hit-test
///          operations need a visible "stand-in" — the start line of the
///          enclosing fold, since that's the line currently on screen.
///          If `line` isn't hidden, it's returned unchanged after a clamp
///          to `[0, line_count - 1]`. If it's hidden, the function picks
///          the *outermost* containing fold's start line (smallest
///          `start_line` of any fold whose range covers `line`), so nested
///          folds collapse correctly to the topmost visible anchor.
/// @param ce Borrowed CodeEditor.
/// @param line Possibly hidden or out-of-range source line.
/// @return Clamped visible line index suitable for cursor / scroll math.
static int rt_codeeditor_visible_anchor_line(const vg_codeeditor_t *ce, int line) {
    if (!ce || ce->line_count <= 0)
        return 0;
    if (line < 0)
        line = 0;
    if (line >= ce->line_count)
        line = ce->line_count - 1;
    if (!rt_codeeditor_line_is_hidden(ce, line))
        return line;

    int best_start = line;
    int found = 0;
    for (int i = 0; i < ce->fold_region_count; i++) {
        const struct vg_fold_region *region = &ce->fold_regions[i];
        if (region->folded && line > region->start_line && line <= region->end_line) {
            if (!found || region->start_line < best_start) {
                best_start = region->start_line;
                found = 1;
            }
        }
    }
    return found ? best_start : line;
}

/// @brief Compute how many monospace characters fit in one wrapped row.
/// @details Returns 0 when word-wrap is disabled (caller should treat that as
///          "no wrap budget — emit the full line as one row"). When word-wrap
///          is on, returns at least 1 even for absurdly narrow viewports so
///          the caller's division never trips on a zero divisor.
/// @param ce Borrowed CodeEditor.
/// @param content_width Available text width in pixels.
/// @return Characters per row (>= 1 with word-wrap on; 0 with word-wrap off).
static int rt_codeeditor_chars_per_row(const vg_codeeditor_t *ce, float content_width) {
    if (!ce || !ce->word_wrap || !isfinite(ce->char_width) || ce->char_width <= 0.0f)
        return 0;
    if (!isfinite(content_width) || content_width <= 0.0f)
        return 1;
    double chars_f = (double)content_width / (double)ce->char_width;
    int chars = rt_gui_saturating_f64_to_i32(chars_f);
    return chars > 0 ? chars : 1;
}

/// @brief Compute how many visual rows a single source line occupies under word-wrap.
/// @details Ceiling-divides line length by `chars_per_row`. Returns 1 (a
///          single-row fallback) when word-wrap is off, when the line is
///          empty, or when the chars-per-row computation produces 0 (so
///          callers always get a positive row count for any in-range line).
/// @param ce Borrowed CodeEditor.
/// @param line Zero-based source line.
/// @param content_width Available text width in pixels.
/// @return Row count; always >= 1 for valid lines.
static int rt_codeeditor_wrapped_rows_for_line(const vg_codeeditor_t *ce,
                                               int line,
                                               float content_width) {
    if (!ce || line < 0 || line >= ce->line_count)
        return 1;
    int chars_per_row = rt_codeeditor_chars_per_row(ce, content_width);
    if (chars_per_row <= 0)
        return 1;
    size_t len = ce->lines[line].length;
    if (len == 0)
        return 1;
    size_t rows = (len + (size_t)chars_per_row - 1) / (size_t)chars_per_row;
    return rows > (size_t)INT_MAX ? INT_MAX : (int)rows;
}

/// @brief Combine fold-hiding and word-wrap to get the on-screen row count for a line.
/// @details Three cases:
///          - Line is hidden by a fold → returns 0 (consumes no vertical space).
///          - Word-wrap is off → returns 1 (one source line = one screen row).
///          - Word-wrap is on → defers to `rt_codeeditor_wrapped_rows_for_line`.
///          This is the canonical helper for all visual-row arithmetic in
///          the editor; cursor positioning, scrollbar math, and hit-testing
///          all sum these counts to convert between source-line space and
///          screen-row space.
/// @param ce Borrowed CodeEditor.
/// @param line Zero-based source line.
/// @param content_width Available text width in pixels.
/// @return Visual row count for the line (0 if hidden, >= 1 otherwise).
static int rt_codeeditor_visual_rows_for_line(const vg_codeeditor_t *ce,
                                              int line,
                                              float content_width) {
    if (!ce || line < 0 || line >= ce->line_count || rt_codeeditor_line_is_hidden(ce, line))
        return 0;
    if (!ce->word_wrap)
        return 1;
    return rt_codeeditor_wrapped_rows_for_line(ce, line, content_width);
}

/// @brief Compute the editor's text-content width, accounting for the vertical scrollbar.
/// @details This is a fixed-point convergence loop because word-wrap and the
///          vertical-scrollbar's presence are mutually dependent: narrower
///          content (because the scrollbar took space) produces more wrapped
///          rows, which can push content past the viewport height and *make*
///          the scrollbar appear, which narrows the content again. Without
///          word-wrap the answer is just `base.width - gutter_width` and the
///          loop is skipped.
///
///          The convergence is bounded at 3 passes — empirically the answer
///          stabilises within 2 (the only oscillation possible is "no
///          scrollbar / yes scrollbar"; once that flips, the next iteration
///          sees the same width and the loop breaks via equality check).
///          The pass cap defends against pathological cases where the cap
///          line height makes the test oscillate; bounded iteration is
///          better than risking an infinite loop in the paint path.
/// @param ce Borrowed mutable CodeEditor whose width cache may be refreshed.
/// @return Pixel width available for text after gutter and (if needed) scrollbar.
/// @note The converged width is cached on the editor and invalidated by layout
///       generation, widget width/height, and word-wrap state. This avoids
///       repeated full-buffer row scans from hover and cursor hit-tests in the
///       same visual layout.
static float rt_codeeditor_content_draw_width(vg_codeeditor_t *ce) {
    if (!ce)
        return 0.0f;

    float base_width = ce->base.width - ce->gutter_width;
    if (base_width < 0.0f)
        base_width = 0.0f;
    if (ce->runtime_content_width_cache_valid &&
        ce->runtime_content_width_generation == ce->layout_generation &&
        ce->runtime_content_width_base_width == base_width &&
        ce->runtime_content_width_viewport_height == ce->base.height &&
        ce->runtime_content_width_word_wrap == ce->word_wrap)
        return ce->runtime_content_width;

    if (!ce->word_wrap) {
        ce->runtime_content_width_cache_valid = true;
        ce->runtime_content_width_generation = ce->layout_generation;
        ce->runtime_content_width_base_width = base_width;
        ce->runtime_content_width_viewport_height = ce->base.height;
        ce->runtime_content_width_word_wrap = ce->word_wrap;
        ce->runtime_content_width = base_width;
        return base_width;
    }

    float content_width = base_width;
    for (int pass = 0; pass < 3; pass++) {
        int64_t total_rows = 0;
        for (int i = 0; i < ce->line_count; i++) {
            int rows = rt_codeeditor_visual_rows_for_line(ce, i, content_width);
            total_rows = total_rows > INT64_MAX - rows ? INT64_MAX : total_rows + rows;
        }
        float total_height = (float)total_rows * ce->line_height;
        float next_width =
            base_width - ((total_height > ce->base.height) ? RT_CODEEDITOR_SCROLLBAR_WIDTH : 0.0f);
        if (next_width < 0.0f)
            next_width = 0.0f;
        if (next_width == content_width)
            break;
        content_width = next_width;
    }
    ce->runtime_content_width_cache_valid = true;
    ce->runtime_content_width_generation = ce->layout_generation;
    ce->runtime_content_width_base_width = base_width;
    ce->runtime_content_width_viewport_height = ce->base.height;
    ce->runtime_content_width_word_wrap = ce->word_wrap;
    ce->runtime_content_width = content_width;
    return content_width;
}

/// @brief Within a single source line, map column → (wrapped row, column-in-row).
/// @details For a line of length L wrapped at C chars-per-row, column `col`
///          normally lives at `(col / C, col % C)`. The one subtlety is the
///          end-of-line cursor: when `col == L` *and* L is a non-zero exact
///          multiple of C, the simple division produces `row = L/C` (one
///          past the last row), which would paint the cursor on a phantom
///          row below the line. The special-case branch maps that situation
///          back to the actual last row's trailing position.
///          When word-wrap is off (chars_per_row == 0), `row_index` stays 0
///          and `col_in_row == col`. Out parameters may be NULL.
/// @param ce Borrowed CodeEditor.
/// @param content_width Available text width in pixels.
/// @param line Zero-based source line.
/// @param col Zero-based byte column.
/// @param out_row_index Optional destination for wrapped row within the line.
/// @param out_col_in_row Optional destination for column within the wrapped row.
static void rt_codeeditor_visual_offset_for_position(const vg_codeeditor_t *ce,
                                                     float content_width,
                                                     int line,
                                                     int col,
                                                     int *out_row_index,
                                                     int *out_col_in_row) {
    if (out_row_index)
        *out_row_index = 0;
    if (out_col_in_row)
        *out_col_in_row = 0;
    if (!ce || line < 0 || line >= ce->line_count)
        return;
    line = rt_codeeditor_visible_anchor_line(ce, line);

    int chars_per_row = rt_codeeditor_chars_per_row(ce, content_width);
    int row_index = 0;
    int col_in_row = col;
    if (chars_per_row > 0) {
        size_t len = ce->lines[line].length;
        row_index = col / chars_per_row;
        if (col > 0 && (size_t)col == len && len > 0 && (len % (size_t)chars_per_row) == 0) {
            row_index = (int)((len - 1) / (size_t)chars_per_row);
        }
        col_in_row = col - row_index * chars_per_row;
        if (col_in_row < 0)
            col_in_row = 0;
    }

    if (out_row_index)
        *out_row_index = row_index;
    if (out_col_in_row)
        *out_col_in_row = col_in_row;
}

/// @brief Convert a (line, col) position into an absolute visual row index.
/// @details Sums the visual row counts of every preceding line (skipping
///          folded-out lines, which contribute 0) and adds the wrapped-row
///          offset within the target line. The result is the row coordinate
///          to use against scroll position and viewport height — i.e., what
///          you'd subtract `scroll_y / line_height` from to get the screen
///          row.
///          The leading clamp + `visible_anchor_line` resolution ensures
///          callers passing an out-of-range or fold-hidden line still get
///          a meaningful row number rather than triggering OOB reads on
///          the per-line iteration.
/// @param ce Borrowed CodeEditor.
/// @param content_width Available text width in pixels.
/// @param line Zero-based source line.
/// @param col Zero-based byte column.
/// @return Absolute visual row index (always >= 0).
static int rt_codeeditor_visual_row_for_position(const vg_codeeditor_t *ce,
                                                 float content_width,
                                                 int line,
                                                 int col) {
    if (!ce || ce->line_count <= 0)
        return 0;
    if (line < 0)
        line = 0;
    if (line >= ce->line_count)
        line = ce->line_count - 1;
    line = rt_codeeditor_visible_anchor_line(ce, line);

    int64_t visual_row = 0;
    for (int i = 0; i < line; i++) {
        int rows = rt_codeeditor_visual_rows_for_line(ce, i, content_width);
        visual_row = visual_row > INT64_MAX - rows ? INT64_MAX : visual_row + rows;
    }

    int row_index = 0;
    rt_codeeditor_visual_offset_for_position(ce, content_width, line, col, &row_index, NULL);
    visual_row = visual_row > INT64_MAX - row_index ? INT64_MAX : visual_row + row_index;
    return visual_row > INT_MAX ? INT_MAX : (int)visual_row;
}

/// @brief Map a 0-based visual row index back to a logical `(line, row_in_line)` pair.
/// @details Inverse of `rt_codeeditor_total_visual_row_for_position`. Walks the line
///          array accumulating visual row counts (accounting for word-wrap) until the
///          target visual row is consumed, then uses `rt_codeeditor_visual_offset_for_position`
///          to find the exact sub-line offset within the found logical line.
/// @param ce Borrowed CodeEditor.
/// @param content_width Available text width in pixels.
/// @param visual_row Zero-based absolute visual row.
/// @param out_line Optional destination for the source line.
/// @param out_row_in_line Optional destination for wrapped row within the source line.
static void rt_codeeditor_locate_visual_row(const vg_codeeditor_t *ce,
                                            float content_width,
                                            int visual_row,
                                            int *out_line,
                                            int *out_row_in_line) {
    if (out_line)
        *out_line = 0;
    if (out_row_in_line)
        *out_row_in_line = 0;
    if (!ce || ce->line_count <= 0)
        return;

    if (visual_row < 0)
        visual_row = 0;

    int64_t accumulated = 0;
    for (int line = 0; line < ce->line_count; line++) {
        int row_count = rt_codeeditor_visual_rows_for_line(ce, line, content_width);
        if (row_count == 0)
            continue;
        int64_t next = accumulated > INT64_MAX - row_count ? INT64_MAX : accumulated + row_count;
        if ((int64_t)visual_row < next) {
            if (out_line)
                *out_line = line;
            if (out_row_in_line)
                *out_row_in_line = (int)((int64_t)visual_row - accumulated);
            return;
        }
        accumulated = next;
    }

    if (out_line)
        *out_line = rt_codeeditor_visible_anchor_line(ce, ce->line_count - 1);
    if (out_row_in_line) {
        int last_rows = rt_codeeditor_visual_rows_for_line(
            ce, rt_codeeditor_visible_anchor_line(ce, ce->line_count - 1), content_width);
        *out_row_in_line = last_rows > 0 ? last_rows - 1 : 0;
    }
}

/// @brief Get the screen-absolute X pixel coordinate of the primary cursor.
/// @details Combines the widget's screen-space origin, gutter width, and
///          cursor column × character width.
/// @param editor CodeEditor widget handle.
/// @return Absolute caret X coordinate, or 0 for an invalid handle.
int64_t rt_codeeditor_get_cursor_pixel_x(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    float ax = 0, ay = 0;
    vg_widget_get_screen_bounds(&ce->base, &ax, &ay, NULL, NULL);
    (void)ay;
    float px = ax + ce->gutter_width;
    if (ce->word_wrap) {
        float content_width = rt_codeeditor_content_draw_width(ce);
        int col_in_row = ce->cursor_col;
        rt_codeeditor_visual_offset_for_position(
            ce, content_width, ce->cursor_line, ce->cursor_col, NULL, &col_in_row);
        px += (float)col_in_row * ce->char_width;
    } else {
        px += (float)(ce->cursor_col) * ce->char_width - ce->scroll_x;
    }
    return rt_gui_saturating_f64_to_i64((double)px);
}

/// @brief Get the screen-absolute Y pixel coordinate of the primary cursor.
/// @details Combines the widget's screen-space origin with the cursor's
///          visible line offset scaled by line height.
/// @param editor CodeEditor widget handle.
/// @return Absolute caret Y coordinate, or 0 for an invalid handle.
int64_t rt_codeeditor_get_cursor_pixel_y(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    float ax = 0, ay = 0;
    vg_widget_get_screen_bounds(&ce->base, &ax, &ay, NULL, NULL);
    (void)ax;
    float py = ay;
    float content_width = rt_codeeditor_content_draw_width(ce);
    int visual_row =
        rt_codeeditor_visual_row_for_position(ce, content_width, ce->cursor_line, ce->cursor_col);
    py += (float)visual_row * ce->line_height - ce->scroll_y;
    return rt_gui_saturating_f64_to_i64((double)py);
}

/// @brief Return the 0-based editor line at a screen-absolute Y coordinate.
/// @param editor CodeEditor widget handle.
/// @param y Absolute screen Y coordinate.
/// @return Zero-based source line, or -1 when unavailable.
int64_t rt_codeeditor_get_line_at_pixel(void *editor, int64_t y) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return -1;
    if (ce->line_count <= 0 || ce->line_height <= 0.0f)
        return -1;

    float ax = 0, ay = 0;
    vg_widget_get_screen_bounds(&ce->base, &ax, &ay, NULL, NULL);
    (void)ax;

    double local_y = (double)y - (double)ay + (double)ce->scroll_y;
    float content_width = rt_codeeditor_content_draw_width(ce);
    int visual_row = isfinite(ce->line_height) && ce->line_height > 0.0f
                         ? rt_gui_saturating_f64_to_i32(local_y / (double)ce->line_height)
                         : 0;
    int line = 0;
    rt_codeeditor_locate_visual_row(ce, content_width, visual_row, &line, NULL);
    if (line < 0)
        line = 0;
    if (line >= ce->line_count)
        line = ce->line_count - 1;
    return line;
}

/// @brief Return the 0-based editor column at a screen-absolute X/Y coordinate.
/// @param editor CodeEditor widget handle.
/// @param x Absolute screen X coordinate.
/// @param y Absolute screen Y coordinate.
/// @return Zero-based byte column clamped to the selected line, or -1 when unavailable.
int64_t rt_codeeditor_get_col_at_pixel(void *editor, int64_t x, int64_t y) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return -1;
    if (ce->line_count <= 0 || ce->char_width <= 0.0f)
        return -1;

    int64_t line64 = rt_codeeditor_get_line_at_pixel(editor, y);
    if (line64 < 0)
        return -1;
    int line = (int)line64;

    float ax = 0, ay = 0;
    vg_widget_get_screen_bounds(&ce->base, &ax, &ay, NULL, NULL);
    (void)ay;

    double local_x = (double)x - (double)ax - (double)ce->gutter_width;
    int col = 0;
    if (ce->word_wrap) {
        float content_width = rt_codeeditor_content_draw_width(ce);
        int chars_per_row = rt_codeeditor_chars_per_row(ce, content_width);
        int visual_row =
            isfinite(ce->line_height) && ce->line_height > 0.0f
                ? rt_gui_saturating_f64_to_i32(((double)y - (double)ay + (double)ce->scroll_y) /
                                               (double)ce->line_height)
                : 0;
        int row_in_line = 0;
        rt_codeeditor_locate_visual_row(ce, content_width, visual_row, NULL, &row_in_line);
        int col_in_row = isfinite(ce->char_width) && ce->char_width > 0.0f
                             ? rt_gui_saturating_f64_to_i32(local_x / (double)ce->char_width + 0.5)
                             : 0;
        if (col_in_row < 0)
            col_in_row = 0;
        int64_t wide_col = (int64_t)row_in_line * (int64_t)chars_per_row + col_in_row;
        col = rt_gui_clamp_i64_to_i32(wide_col, 0, INT32_MAX);
    } else {
        local_x += ce->scroll_x;
        col = rt_gui_saturating_f64_to_i32(local_x / (double)ce->char_width + 0.5);
    }
    if (col < 0)
        col = 0;
    int line_len = rt_codeeditor_line_length_i32(ce, line);
    if (col > line_len)
        col = line_len;
    return col;
}

/// @brief Insert text at the primary cursor position.
/// @param editor CodeEditor widget handle.
/// @param text Runtime string to insert.
void rt_codeeditor_insert_at_cursor(void *editor, rt_string text) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce || !text)
        return;
    char *cstr = rt_string_to_gui_cstr(text);
    if (!cstr)
        return;
    vg_codeeditor_insert_text(ce, cstr);
    free(cstr);
}

/// @brief Insert text at the primary cursor, then place the caret @p caret_offset characters
///        into the inserted text. Captures the pre-insert position, inserts, then advances by
///        the offset (counting newlines) and sets the cursor — so the caret lands inside a
///        multi-line insertion without the caller walking the text by hand.
/// @param editor CodeEditor widget handle.
/// @param text Runtime string to insert.
/// @param caret_offset Number of inserted Unicode characters before the final caret.
void rt_codeeditor_insert_and_place_cursor(void *editor, rt_string text, int64_t caret_offset) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce || !text)
        return;
    int64_t line = rt_codeeditor_get_cursor_line_at(editor, 0);
    int64_t col = rt_codeeditor_get_cursor_col_at(editor, 0);
    char *cstr = rt_string_to_gui_cstr(text);
    if (!cstr)
        return;
    vg_codeeditor_insert_text(ce, cstr);
    rt_codeeditor_advance_position(cstr, caret_offset, &line, &col);
    free(cstr);
    rt_codeeditor_set_cursor_position_at(editor, 0, line, col);
}

/// @brief Classify a byte as part of an identifier-oriented editor word.
/// @param c Byte to classify.
/// @return Non-zero for ASCII alphanumerics, underscore, or any non-ASCII byte.
static int rt_codeeditor_identifier_byte(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

/// @brief Return the identifier word under the primary cursor.
/// @details Scans left and right from cursor_col over ASCII identifier bytes
///          plus non-ASCII UTF-8 continuation/lead bytes so multibyte words
///          are not split in the middle.
/// @param editor CodeEditor widget handle.
/// @return Owned word text, or an empty runtime string when unavailable.
rt_string rt_codeeditor_get_word_at_cursor(void *editor) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return rt_str_empty();
    if (ce->cursor_line < 0 || ce->cursor_line >= ce->line_count)
        return rt_str_empty();
    const char *text = ce->lines[ce->cursor_line].text;
    int len = rt_codeeditor_line_length_i32(ce, ce->cursor_line);
    int col = ce->cursor_col < len ? ce->cursor_col : len;

    /* scan left to find word start */
    int start = col;
    while (start > 0 && rt_codeeditor_identifier_byte((unsigned char)text[start - 1]))
        --start;

    /* scan right to find word end */
    int end = col;
    while (end < len && rt_codeeditor_identifier_byte((unsigned char)text[end]))
        ++end;

    return rt_gui_string_from_bytes_bounded(text + start, (size_t)(end - start));
}

/// @brief Replace the identifier word under the primary cursor with new_text.
/// @details Selects the same word range that get_word_at_cursor() would return,
///          then inserts the replacement via vg_codeeditor_insert_text.
/// @param editor CodeEditor widget handle.
/// @param new_text Runtime replacement string.
void rt_codeeditor_replace_word_at_cursor(void *editor, rt_string new_text) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return;
    if (ce->cursor_line < 0 || ce->cursor_line >= ce->line_count)
        return;
    const char *text = ce->lines[ce->cursor_line].text;
    int len = rt_codeeditor_line_length_i32(ce, ce->cursor_line);
    int col = ce->cursor_col < len ? ce->cursor_col : len;

    /* find word boundaries */
    int start = col;
    while (start > 0 && rt_codeeditor_identifier_byte((unsigned char)text[start - 1]))
        --start;
    int end = col;
    while (end < len && rt_codeeditor_identifier_byte((unsigned char)text[end]))
        ++end;

    char *cstr = rt_string_to_gui_cstr(new_text);
    if (cstr) {
        /* select the word, then insert the replacement (replaces selection) */
        int start_col = rt_codeeditor_byte_col_to_char_col(ce, ce->cursor_line, start);
        int end_col = rt_codeeditor_byte_col_to_char_col(ce, ce->cursor_line, end);
        vg_codeeditor_set_selection(ce, ce->cursor_line, start_col, ce->cursor_line, end_col);
        vg_codeeditor_insert_text(ce, cstr);
        free(cstr);
    }
}

/// @brief Return the text of a single line (0-based index).
/// @param editor CodeEditor widget handle.
/// @param line_index Zero-based logical line index.
/// @return Owned line text, or an empty runtime string for an invalid handle or index.
rt_string rt_codeeditor_get_line(void *editor, int64_t line_index) {
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return rt_str_empty();
    if (line_index < 0 || line_index >= (int64_t)ce->line_count)
        return rt_str_empty();
    vg_code_line_t *line = &ce->lines[(int)line_index];
    return rt_gui_string_from_bytes_bounded(line->text, line->length);
}

/// @brief Clear low-level editor performance counters.
/// @param editor CodeEditor widget handle whose counters should be reset.
void rt_codeeditor_reset_perf_stats(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (ce)
        vg_codeeditor_reset_perf_stats(ce);
}

/// @brief Snapshot all low-level CodeEditor performance counters into one versioned Map.
/// @details The lower widget returns a value copy, providing a mutually consistent snapshot on the
///          GUI thread. Invalid editors still return the complete schema with zero counters so
///          telemetry collection does not need a separate capability branch.
/// @param editor Live CodeEditor handle; invalid handles yield zero-valued statistics.
/// @return New managed Map with schemaVersion=1 and nine stable raw-counter keys, or NULL on OOM.
void *rt_codeeditor_get_perf_stats(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return rt_codeeditor_perf_stats_map(0, 0, 0, 0, 0, 0, 0, 0, 0);
    vg_codeeditor_perf_stats_t stats = vg_codeeditor_get_perf_stats(ce);
    return rt_codeeditor_perf_stats_map(stats.total_height_linear_scans,
                                        stats.total_visual_row_linear_scans,
                                        stats.visual_row_linear_scans,
                                        stats.locate_visual_row_linear_scans,
                                        stats.line_highlight_calls,
                                        stats.syntax_state_line_scans,
                                        stats.highlight_span_checks,
                                        stats.full_text_copies,
                                        stats.full_text_copy_bytes);
}

/// @brief Return full-buffer materialization count.
/// @param editor CodeEditor widget handle.
/// @return Saturated signed count of full-buffer text copies, or zero for an invalid handle.
int64_t rt_codeeditor_get_full_text_copy_count(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    vg_codeeditor_perf_stats_t stats = vg_codeeditor_get_perf_stats(ce);
    return rt_codeeditor_perf_i64(stats.full_text_copies);
}

/// @brief Return aggregate line visits from layout/scroll visual-row scans.
/// @param editor CodeEditor widget handle.
/// @return Saturated sum of the four layout scan counters, or zero for an invalid handle.
int64_t rt_codeeditor_get_layout_linear_scan_count(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    vg_codeeditor_perf_stats_t stats = vg_codeeditor_get_perf_stats(ce);
    uint64_t total = stats.total_height_linear_scans;
    if (UINT64_MAX - total < stats.total_visual_row_linear_scans)
        total = UINT64_MAX;
    else
        total += stats.total_visual_row_linear_scans;
    if (UINT64_MAX - total < stats.visual_row_linear_scans)
        total = UINT64_MAX;
    else
        total += stats.visual_row_linear_scans;
    if (UINT64_MAX - total < stats.locate_visual_row_linear_scans)
        total = UINT64_MAX;
    else
        total += stats.locate_visual_row_linear_scans;
    return rt_codeeditor_perf_i64(total);
}

/// @brief Return syntax-highlighter invocation count.
/// @param editor CodeEditor widget handle.
/// @return Saturated syntax-highlight invocation count, or zero for an invalid handle.
int64_t rt_codeeditor_get_syntax_highlight_call_count(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    vg_codeeditor_perf_stats_t stats = vg_codeeditor_get_perf_stats(ce);
    return rt_codeeditor_perf_i64(stats.line_highlight_calls);
}

/// @brief Return cached syntax-state line scan count.
/// @param editor CodeEditor widget handle.
/// @return Saturated syntax-state scan count, or zero for an invalid handle.
int64_t rt_codeeditor_get_syntax_state_line_scan_count(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    vg_codeeditor_perf_stats_t stats = vg_codeeditor_get_perf_stats(ce);
    return rt_codeeditor_perf_i64(stats.syntax_state_line_scans);
}

/// @brief Return highlight span checks performed during paint.
/// @param editor CodeEditor widget handle.
/// @return Saturated highlight-span check count, or zero for an invalid handle.
int64_t rt_codeeditor_get_highlight_span_check_count(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    vg_codeeditor_perf_stats_t stats = vg_codeeditor_get_perf_stats(ce);
    return rt_codeeditor_perf_i64(stats.highlight_span_checks);
}

/// @brief Return bytes copied by full-buffer materializations.
/// @param editor CodeEditor widget handle.
/// @return Saturated byte count, or zero for an invalid handle.
int64_t rt_codeeditor_get_full_text_copy_byte_count(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    if (!ce)
        return 0;
    vg_codeeditor_perf_stats_t stats = vg_codeeditor_get_perf_stats(ce);
    return rt_codeeditor_perf_i64(stats.full_text_copy_bytes);
}

//=============================================================================
// EditorBuffer — detachable per-document editor state (Zanna.GUI.EditorBuffer)
//=============================================================================

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));

#define RT_EDITORBUFFER_MAGIC 0x45444255464652ULL /* "EDBUFFR" */

typedef struct rt_editorbuffer_data {
    uint64_t magic;
    vg_editor_buffer_t *buf; /* NULL once consumed by an attach */
} rt_editorbuffer_data_t;

/// @brief Authenticate an EditorBuffer handle via its magic tag.
/// @param handle Candidate runtime EditorBuffer object.
/// @return Validated buffer wrapper, or `NULL` when the handle is absent or has the wrong tag.
static rt_editorbuffer_data_t *rt_editorbuffer_checked(void *handle) {
    rt_editorbuffer_data_t *d = (rt_editorbuffer_data_t *)handle;
    return (d && d->magic == RT_EDITORBUFFER_MAGIC) ? d : NULL;
}

/// @brief GC finalizer: free the owned buffer if it was never attached.
/// @param obj EditorBuffer wrapper being finalized.
static void rt_editorbuffer_finalize(void *obj) {
    rt_editorbuffer_data_t *d = rt_editorbuffer_checked(obj);
    if (!d)
        return;
    if (d->buf) {
        vg_editor_buffer_destroy(d->buf);
        d->buf = NULL;
    }
    d->magic = 0;
}

/// @brief Wrap a detached buffer in a GC handle (takes ownership).
/// @param buf Detached editor buffer whose ownership transfers to the wrapper.
/// @return New managed EditorBuffer handle, or `NULL` if allocation fails.
static void *rt_editorbuffer_wrap(vg_editor_buffer_t *buf) {
    rt_editorbuffer_data_t *d =
        (rt_editorbuffer_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_editorbuffer_data_t));
    if (!d) {
        vg_editor_buffer_destroy(buf);
        return NULL;
    }
    d->magic = RT_EDITORBUFFER_MAGIC;
    d->buf = buf;
    rt_obj_set_finalizer(d, rt_editorbuffer_finalize);
    return d;
}

/// @brief `EditorBuffer.New` — detached buffer initialised from text.
/// @param text Initial document contents; a null runtime string creates an empty buffer.
/// @return New managed EditorBuffer handle, or `NULL` when allocation fails.
void *rt_editorbuffer_new(rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return NULL;
    vg_editor_buffer_t *buf = vg_editor_buffer_create(ctext);
    if (ctext)
        free(ctext);
    if (!buf)
        return NULL;
    return rt_editorbuffer_wrap(buf);
}

/// @brief `EditorBuffer.get_Text` — full document text.
/// @param handle EditorBuffer handle to query.
/// @return Owned document text, or an empty runtime string for an invalid or consumed handle.
rt_string rt_editorbuffer_get_text(void *handle) {
    RT_ASSERT_MAIN_THREAD();
    rt_editorbuffer_data_t *d = rt_editorbuffer_checked(handle);
    if (!d || !d->buf)
        return rt_str_empty();
    char *t = vg_editor_buffer_get_text(d->buf);
    if (!t)
        return rt_str_empty();
    rt_string s = rt_gui_string_from_cstr_bounded(t);
    free(t);
    return s;
}

/// @brief `EditorBuffer.get_Revision` — content revision.
/// @param handle EditorBuffer handle to query.
/// @return Current content revision, or zero for an invalid or consumed handle.
int64_t rt_editorbuffer_get_revision(void *handle) {
    RT_ASSERT_MAIN_THREAD();
    rt_editorbuffer_data_t *d = rt_editorbuffer_checked(handle);
    if (!d || !d->buf)
        return 0;
    return rt_gui_saturating_u64_to_i64(vg_editor_buffer_get_revision(d->buf));
}

/// @brief `EditorBuffer.IsModified`.
/// @param handle EditorBuffer handle to query.
/// @return `1` when the detached document is modified; otherwise `0`.
int64_t rt_editorbuffer_is_modified(void *handle) {
    RT_ASSERT_MAIN_THREAD();
    rt_editorbuffer_data_t *d = rt_editorbuffer_checked(handle);
    if (!d || !d->buf)
        return 0;
    return vg_editor_buffer_is_modified(d->buf) ? 1 : 0;
}

/// @brief `EditorBuffer.ClearModified`.
/// @param handle EditorBuffer handle whose modified flag should be cleared.
void rt_editorbuffer_clear_modified(void *handle) {
    RT_ASSERT_MAIN_THREAD();
    rt_editorbuffer_data_t *d = rt_editorbuffer_checked(handle);
    if (d && d->buf)
        vg_editor_buffer_clear_modified(d->buf);
}

/// @brief `CodeEditor.AttachBuffer` — swap the editor's document for @p bufHandle
///        and return the editor's previous document as a new EditorBuffer. The
///        passed buffer is consumed.
/// @param editor CodeEditor widget receiving the detached document.
/// @param bufHandle EditorBuffer handle consumed by a successful swap.
/// @return Managed handle for the editor's previous document, or `NULL` on invalid input or
/// failure.
void *rt_codeeditor_attach_buffer(void *editor, void *bufHandle) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_handle_checked(editor);
    rt_editorbuffer_data_t *d = rt_editorbuffer_checked(bufHandle);
    if (!ce || !d || !d->buf)
        return NULL;
    vg_editor_buffer_t *prev = vg_codeeditor_swap_buffer(ce, d->buf);
    d->buf = NULL; /* consumed by swap (its shell was freed); block double-free */
    if (!prev)
        return NULL;
    return rt_editorbuffer_wrap(prev);
}

#else /* !ZANNA_ENABLE_GRAPHICS */

//=============================================================================
// CodeEditor Stubs (graphics disabled)
//=============================================================================
//
// Every public CodeEditor API has a no-op stub below so headless / server
// builds (without ZANNA_ENABLE_GRAPHICS) link cleanly. Each stub:
//   - swallows its arguments via `(void)` casts to silence unused warnings
//   - returns a "neutral" value (0/-1/empty string) for getter signatures
//
// Callers that try to actually use a CodeEditor in a headless build will
// see no errors but also no output — matching the silent-stub pattern
// used elsewhere in the runtime.
//=============================================================================


/// @brief Stub: `CodeEditor.SetShowLineNumbers` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param show Ignored visibility flag.
void rt_codeeditor_set_show_line_numbers(void *editor, int64_t show) {
    (void)editor;
    (void)show;
}

/// @brief Stub: returns the default visible line-number state in headless builds.
/// @param editor Ignored CodeEditor handle.
/// @return Always `1`, matching the graphical editor default.
int64_t rt_codeeditor_get_show_line_numbers(void *editor) {
    (void)editor;
    return 1;
}

/// @brief Stub: `CodeEditor.SetLineNumberWidth` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param width Ignored gutter width.
void rt_codeeditor_set_line_number_width(void *editor, int64_t width) {
    (void)editor;
    (void)width;
}

/// @brief Stub: `CodeEditor.SetGutterIcon` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored zero-based line index.
/// @param pixels Ignored pixel buffer handle.
/// @param slot Ignored gutter slot.
void rt_codeeditor_set_gutter_icon(void *editor, int64_t line, void *pixels, int64_t slot) {
    (void)editor;
    (void)line;
    (void)pixels;
    (void)slot;
}

/// @brief Stub: `CodeEditor.SetGutterBar` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored zero-based line index.
/// @param color Ignored packed bar color.
/// @param slot Ignored gutter slot.
void rt_codeeditor_set_gutter_bar(void *editor, int64_t line, int64_t color, int64_t slot) {
    (void)editor;
    (void)line;
    (void)color;
    (void)slot;
}

/// @brief Stub: `CodeEditor.ClearGutterIcon` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored zero-based line index.
/// @param slot Ignored gutter slot.
void rt_codeeditor_clear_gutter_icon(void *editor, int64_t line, int64_t slot) {
    (void)editor;
    (void)line;
    (void)slot;
}

/// @brief Stub: `CodeEditor.ClearAllGutterIcons` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param slot Ignored gutter slot.
void rt_codeeditor_clear_all_gutter_icons(void *editor, int64_t slot) {
    (void)editor;
    (void)slot;
}

/// @brief Stub: internal gutter-click injection is a no-op without graphics.
/// @param line Ignored clicked line index.
/// @param slot Ignored clicked gutter slot.
void rt_gui_set_gutter_click(int64_t line, int64_t slot) {
    (void)line;
    (void)slot;
}

/// @brief Stub: internal gutter-click clear is a no-op without graphics.
void rt_gui_clear_gutter_click(void) {}

/// @brief Stub: returns 0 (no gutter can be clicked in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_was_gutter_clicked(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: return an empty atomic gutter-click snapshot without graphics.
/// @param editor Ignored CodeEditor handle.
/// @return New Map containing `clicked=false`, `line=-1`, and `slot=-1`, or `NULL` on OOM.
void *rt_codeeditor_take_gutter_click(void *editor) {
    (void)editor;
    void *result = rt_map_new();
    if (!result)
        return NULL;
    rt_map_set_bool(result, rt_const_cstr("clicked"), 0);
    rt_map_set_int(result, rt_const_cstr("line"), -1);
    rt_map_set_int(result, rt_const_cstr("slot"), -1);
    return result;
}

/// @brief Stub: returns -1 (no gutter click available in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `-1`.
int64_t rt_codeeditor_get_gutter_clicked_line(void *editor) {
    (void)editor;
    return -1;
}

/// @brief Stub: returns -1 (no gutter click available in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `-1`.
int64_t rt_codeeditor_get_gutter_clicked_slot(void *editor) {
    (void)editor;
    return -1;
}

/// @brief Stub: `CodeEditor.SetShowFoldGutter` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param show Ignored visibility flag.
void rt_codeeditor_set_show_fold_gutter(void *editor, int64_t show) {
    (void)editor;
    (void)show;
}

/// @brief Stub: `CodeEditor.AddFoldRegion` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param start_line Ignored first line of the fold region.
/// @param end_line Ignored last line of the fold region.
void rt_codeeditor_add_fold_region(void *editor, int64_t start_line, int64_t end_line) {
    (void)editor;
    (void)start_line;
    (void)end_line;
}

/// @brief Stub: `CodeEditor.RemoveFoldRegion` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param start_line Ignored fold-region start line.
void rt_codeeditor_remove_fold_region(void *editor, int64_t start_line) {
    (void)editor;
    (void)start_line;
}

/// @brief Stub: `CodeEditor.ClearFoldRegions` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_clear_fold_regions(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.Fold` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored fold-region line.
void rt_codeeditor_fold(void *editor, int64_t line) {
    (void)editor;
    (void)line;
}

/// @brief Stub: `CodeEditor.Unfold` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored fold-region line.
void rt_codeeditor_unfold(void *editor, int64_t line) {
    (void)editor;
    (void)line;
}

/// @brief Stub: `CodeEditor.ToggleFold` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored fold-region line.
void rt_codeeditor_toggle_fold(void *editor, int64_t line) {
    (void)editor;
    (void)line;
}

/// @brief Stub: returns 0 (no fold state exists in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored fold-region line.
/// @return Always `0`.
int64_t rt_codeeditor_is_folded(void *editor, int64_t line) {
    (void)editor;
    (void)line;
    return 0;
}

/// @brief Stub: `CodeEditor.FoldAll` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_fold_all(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.UnfoldAll` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_unfold_all(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.SetAutoFoldDetection` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param enable Ignored automatic-detection flag.
void rt_codeeditor_set_auto_fold_detection(void *editor, int64_t enable) {
    (void)editor;
    (void)enable;
}

/// @brief Stub: returns the primary cursor count in headless builds.
/// @param editor Ignored CodeEditor handle.
/// @return Always `1`, representing the primary cursor.
int64_t rt_codeeditor_get_cursor_count(void *editor) {
    (void)editor;
    return 1;
}

/// @brief Stub: `CodeEditor.AddCursor` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored zero-based line.
/// @param col Ignored zero-based character column.
void rt_codeeditor_add_cursor(void *editor, int64_t line, int64_t col) {
    (void)editor;
    (void)line;
    (void)col;
}

/// @brief Stub: `CodeEditor.RemoveCursor` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
void rt_codeeditor_remove_cursor(void *editor, int64_t index) {
    (void)editor;
    (void)index;
}

/// @brief Stub: `CodeEditor.ClearExtraCursors` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_clear_extra_cursors(void *editor) {
    (void)editor;
}

/// @brief Stub: returns 0 (no cursor state in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @return Always `0`.
int64_t rt_codeeditor_get_cursor_line_at(void *editor, int64_t index) {
    (void)editor;
    (void)index;
    return 0;
}

/// @brief Stub: returns 0 (no cursor state in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @return Always `0`.
int64_t rt_codeeditor_get_cursor_col_at(void *editor, int64_t index) {
    (void)editor;
    (void)index;
    return 0;
}

/// @brief Stub: returns 0 (no cursor state in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_cursor_line(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (no cursor state in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_cursor_col(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (no scroll state in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_scroll_top_line(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: `CodeEditor.ScrollTopLine` setter is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param line Ignored top logical line.
void rt_codeeditor_set_scroll_top_line(void *editor, int64_t line) {
    (void)editor;
    (void)line;
}

/// @brief Stub: `CodeEditor.SetCursorPositionAt` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @param line Ignored zero-based line.
/// @param col Ignored zero-based character column.
void rt_codeeditor_set_cursor_position_at(void *editor, int64_t index, int64_t line, int64_t col) {
    (void)editor;
    (void)index;
    (void)line;
    (void)col;
}

/// @brief Stub: `CodeEditor.SetCursorSelection` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @param start_line Ignored selection-start line.
/// @param start_col Ignored selection-start character column.
/// @param end_line Ignored selection-end line.
/// @param end_col Ignored selection-end character column.
void rt_codeeditor_set_cursor_selection(void *editor,
                                        int64_t index,
                                        int64_t start_line,
                                        int64_t start_col,
                                        int64_t end_line,
                                        int64_t end_col) {
    (void)editor;
    (void)index;
    (void)start_line;
    (void)start_col;
    (void)end_line;
    (void)end_col;
}

/// @brief Stub: returns 0 (no selection exists in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @return Always `0`.
int64_t rt_codeeditor_cursor_has_selection(void *editor, int64_t index) {
    (void)editor;
    (void)index;
    return 0;
}

/// @brief Stub: returns 0 (no selection exists in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @return Always `0`.
int64_t rt_codeeditor_get_selection_start_line_at(void *editor, int64_t index) {
    (void)editor;
    (void)index;
    return 0;
}

/// @brief Stub: returns 0 (no selection exists in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @return Always `0`.
int64_t rt_codeeditor_get_selection_start_col_at(void *editor, int64_t index) {
    (void)editor;
    (void)index;
    return 0;
}

/// @brief Stub: returns 0 (no selection exists in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @return Always `0`.
int64_t rt_codeeditor_get_selection_end_line_at(void *editor, int64_t index) {
    (void)editor;
    (void)index;
    return 0;
}

/// @brief Stub: returns 0 (no selection exists in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @param index Ignored cursor index.
/// @return Always `0`.
int64_t rt_codeeditor_get_selection_end_col_at(void *editor, int64_t index) {
    (void)editor;
    (void)index;
    return 0;
}

/// @brief Stub: `CodeEditor.Undo` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_undo(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.Redo` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_redo(void *editor) {
    (void)editor;
}

/// @brief Stub: returns 0 (no undo history in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_can_undo(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (no redo history in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_can_redo(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (clipboard unavailable without graphics).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0` to report that no text was copied.
int64_t rt_codeeditor_copy(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (clipboard unavailable without graphics).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0` to report that no text was cut.
int64_t rt_codeeditor_cut(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (clipboard unavailable without graphics).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0` to report that no text was pasted.
int64_t rt_codeeditor_paste(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: `CodeEditor.SelectAll` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_select_all(void *editor) {
    (void)editor;
}

/// @brief Stub: `CodeEditor.SetTabSize` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param size Ignored tab width.
void rt_codeeditor_set_tab_size(void *editor, int64_t size) {
    (void)editor;
    (void)size;
}

/// @brief Stub: returns 0 (no tab size state in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_tab_size(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: `CodeEditor.SetInsertSpaces` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param enabled Ignored insert-spaces flag.
void rt_codeeditor_set_insert_spaces(void *editor, int64_t enabled) {
    (void)editor;
    (void)enabled;
}

/// @brief Stub: returns 1 so headless preference probes match the editor default.
/// @param editor Ignored CodeEditor handle.
/// @return Always `1`.
int64_t rt_codeeditor_get_insert_spaces(void *editor) {
    (void)editor;
    return 1;
}

/// @brief Stub: `CodeEditor.SetWordWrap` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param enabled Ignored word-wrap flag.
void rt_codeeditor_set_word_wrap(void *editor, int64_t enabled) {
    (void)editor;
    (void)enabled;
}

/// @brief Graphics-disabled ligature setter stub.
/// @param editor Ignored CodeEditor handle.
/// @param enabled Ignored ligature flag.
void rt_codeeditor_set_ligatures_enabled(void *editor, int64_t enabled) {
    (void)editor;
    (void)enabled;
}

/// @brief Graphics-disabled ligature getter stub.
/// @param editor Ignored CodeEditor handle.
/// @return Always `1`, matching the graphical editor default.
int64_t rt_codeeditor_get_ligatures_enabled(void *editor) {
    (void)editor;
    return 1;
}

/// @brief Stub: returns 0 (no word-wrap state in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_word_wrap(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: `CodeEditor.SetWhitespaceMode` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param mode Ignored whitespace display mode.
void rt_codeeditor_set_whitespace_mode(void *editor, int64_t mode) {
    (void)editor;
    (void)mode;
}

/// @brief Stub: returns 0 (no whitespace mode in headless builds).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_whitespace_mode(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: `CodeEditor.SetShowIndentGuides` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param enabled Ignored indent-guide visibility flag.
void rt_codeeditor_set_show_indent_guides(void *editor, int64_t enabled) {
    (void)editor;
    (void)enabled;
}

/// @brief Stub: returns 1 so headless probes match the editor default (guides on).
/// @param editor Ignored CodeEditor handle.
/// @return Always `1`.
int64_t rt_codeeditor_get_show_indent_guides(void *editor) {
    (void)editor;
    return 1;
}

/// @brief Stub: `CodeEditor.SetReadOnly` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param enabled Ignored read-only flag.
void rt_codeeditor_set_read_only(void *editor, int64_t enabled) {
    (void)editor;
    (void)enabled;
}

/// @brief Stub: returns 0 (headless CodeEditor storage is not editable anyway).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_read_only(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (no pixel cursor position without graphics).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_cursor_pixel_x(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 (no pixel cursor position without graphics).
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_cursor_pixel_y(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns -1 (no hit-testing without graphics).
/// @param editor Ignored CodeEditor handle.
/// @param y Ignored vertical pixel coordinate.
/// @return Always `-1`.
int64_t rt_codeeditor_get_line_at_pixel(void *editor, int64_t y) {
    (void)editor;
    (void)y;
    return -1;
}

/// @brief Stub: returns -1 (no hit-testing without graphics).
/// @param editor Ignored CodeEditor handle.
/// @param x Ignored horizontal pixel coordinate.
/// @param y Ignored vertical pixel coordinate.
/// @return Always `-1`.
int64_t rt_codeeditor_get_col_at_pixel(void *editor, int64_t x, int64_t y) {
    (void)editor;
    (void)x;
    (void)y;
    return -1;
}

/// @brief Stub: `CodeEditor.InsertAtCursor` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param text Ignored runtime text.
void rt_codeeditor_insert_at_cursor(void *editor, rt_string text) {
    (void)editor;
    (void)text;
}

/// @brief Stub: graphics disabled — no editor to insert into.
/// @param editor Ignored CodeEditor handle.
/// @param text Ignored runtime text.
/// @param caret_offset Ignored caret offset within the text.
void rt_codeeditor_insert_and_place_cursor(void *editor, rt_string text, int64_t caret_offset) {
    (void)editor;
    (void)text;
    (void)caret_offset;
}

/// @brief Stub: returns empty string (no word-at-cursor without graphics).
/// @param editor Ignored CodeEditor handle.
/// @return Empty runtime string.
rt_string rt_codeeditor_get_word_at_cursor(void *editor) {
    (void)editor;
    return rt_str_empty();
}

/// @brief Stub: `CodeEditor.ReplaceWordAtCursor` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param new_text Ignored replacement text.
void rt_codeeditor_replace_word_at_cursor(void *editor, rt_string new_text) {
    (void)editor;
    (void)new_text;
}

/// @brief Stub: returns empty string (no line content without graphics).
/// @param editor Ignored CodeEditor handle.
/// @param line_index Ignored zero-based line index.
/// @return Empty runtime string.
rt_string rt_codeeditor_get_line(void *editor, int64_t line_index) {
    (void)editor;
    (void)line_index;
    return rt_str_empty();
}

/// @brief Stub: `CodeEditor.ResetPerfStats` is a no-op without graphics.
/// @param editor Ignored CodeEditor handle.
void rt_codeeditor_reset_perf_stats(void *editor) {
    (void)editor;
}

/// @brief Return the complete zero-valued performance schema without graphics support.
/// @details The headless runtime preserves the enabled build's Map shape and schema version so
///          diagnostics and tests can consume telemetry portably even though no editor widget can
///          accumulate work.
/// @param editor Ignored graphics-disabled CodeEditor handle.
/// @return New managed Map with schemaVersion=1 and all nine counters set to zero, or NULL on OOM.
void *rt_codeeditor_get_perf_stats(void *editor) {
    (void)editor;
    return rt_codeeditor_perf_stats_map(0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/// @brief Stub: returns 0 without graphics.
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_full_text_copy_count(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 without graphics.
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_layout_linear_scan_count(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 without graphics.
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_syntax_highlight_call_count(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 without graphics.
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_syntax_state_line_scan_count(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 without graphics.
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_highlight_span_check_count(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: returns 0 without graphics.
/// @param editor Ignored CodeEditor handle.
/// @return Always `0`.
int64_t rt_codeeditor_get_full_text_copy_byte_count(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stubs: EditorBuffer is unavailable without graphics.
/// @param text Ignored initial document text.
/// @return Always `NULL`.
void *rt_editorbuffer_new(rt_string text) {
    (void)text;
    return NULL;
}

/// @brief Return empty text because EditorBuffer is unavailable without graphics.
/// @param handle Ignored EditorBuffer handle.
/// @return Empty runtime string.
rt_string rt_editorbuffer_get_text(void *handle) {
    (void)handle;
    return rt_str_empty();
}

/// @brief Return the neutral revision because EditorBuffer is unavailable without graphics.
/// @param handle Ignored EditorBuffer handle.
/// @return Always `0`.
int64_t rt_editorbuffer_get_revision(void *handle) {
    (void)handle;
    return 0;
}

/// @brief Report an unmodified buffer because EditorBuffer is unavailable without graphics.
/// @param handle Ignored EditorBuffer handle.
/// @return Always `0`.
int64_t rt_editorbuffer_is_modified(void *handle) {
    (void)handle;
    return 0;
}

/// @brief Ignore a modified-state reset because EditorBuffer is unavailable without graphics.
/// @param handle Ignored EditorBuffer handle.
void rt_editorbuffer_clear_modified(void *handle) {
    (void)handle;
}

/// @brief Reject buffer attachment because CodeEditor is unavailable without graphics.
/// @param editor Ignored CodeEditor handle.
/// @param bufHandle Ignored EditorBuffer handle.
/// @return Always `NULL`.
void *rt_codeeditor_attach_buffer(void *editor, void *bufHandle) {
    (void)editor;
    (void)bufHandle;
    return NULL;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
