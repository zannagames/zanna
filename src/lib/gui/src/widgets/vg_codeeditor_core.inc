//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file vg_codeeditor_core.inc
/// @brief CodeEditor constants, forward declarations, vtable setup, storage
/// helpers, syntax and layout caches, fold helpers, scrollbar math, and layout
/// refresh.
///
/// @details This include fragment is consumed only by vg_codeeditor.c. It keeps
/// shared editor internals in their original declaration order so later
/// fragments retain the same static helper visibility as the monolithic file.
///
// File: src/lib/gui/src/widgets/vg_codeeditor_core.inc
// Purpose: Constants, forward declarations, vtable setup, storage helpers, syntax/highlight caches,
// fold and layout helpers, scrollbar math, and layout refresh.
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

// Cross-platform attribute for unused variables/functions
#ifdef _MSC_VER
#define VG_UNUSED
#else
#define VG_UNUSED __attribute__((unused))
#endif

//=============================================================================
// Constants
//=============================================================================

#define INITIAL_LINE_CAPACITY 64
#define INITIAL_TEXT_CAPACITY 256
#define LINE_CAPACITY_GROWTH 2
#define CURSOR_BLINK_RATE 0.5f
#define CODEEDITOR_SCROLLBAR_WIDTH 12.0f
#define CODEEDITOR_FOLD_GUTTER_MIN_WIDTH 14.0f
// Lines scrolled per mouse-wheel delta unit. Was 1.0f, but on macOS trackpads
// (which emit high-frequency, high-magnitude delta_y values) that produced an
// uncomfortably fast scroll. Half-line per delta unit feels closer to the
// platform-native cadence of Safari, VS Code, and Sublime.
#define CODEEDITOR_MOUSE_WHEEL_LINES 0.3f
// Maximum lines/columns per second the view auto-scrolls when a selection drag
// is held beyond the editor edge. Speed ramps from ~1/3 of this (just past the
// edge) up to this maximum (far past the edge).
#define CODEEDITOR_AUTOSCROLL_MAX_LPS 30.0f
// Prevent malicious or accidental all-document highlight spans from allocating
// unbounded line-index storage. Normal diagnostics/find highlights are far below
// this because they usually touch one source line each.
#define CODEEDITOR_HIGHLIGHT_LINE_INDEX_MAX_ENTRIES 2000000
#define CODEEDITOR_PAIR_HIGHLIGHT_FILL 0x6638BDF8u
#define CODEEDITOR_PAIR_HIGHLIGHT_BORDER 0xFF38BDF8u

//=============================================================================
// Forward Declarations
//=============================================================================

static void codeeditor_destroy(vg_widget_t *widget);
static void codeeditor_measure(vg_widget_t *widget, float available_width, float available_height);
static void codeeditor_paint(vg_widget_t *widget, void *canvas);
static bool codeeditor_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool codeeditor_can_focus(vg_widget_t *widget);
static void codeeditor_on_focus(vg_widget_t *widget, bool gained);
static void codeeditor_draw_highlight_underline(
    void *canvas, float x, float y, float width, float line_height, uint32_t color);
static void codeeditor_paint_pair_highlight_for_segment(vg_codeeditor_t *editor,
                                                        void *canvas,
                                                        int line,
                                                        int segment_start,
                                                        int segment_len,
                                                        float content_x,
                                                        float row_y,
                                                        float scroll_x,
                                                        bool wrapped);

static bool ensure_line_capacity(vg_codeeditor_t *editor, int needed);
static bool ensure_text_capacity(vg_code_line_t *line, size_t needed);
static bool insert_text_at_internal(vg_codeeditor_t *editor, int line, int col, const char *text);
static void delete_text_range_internal(
    vg_codeeditor_t *editor, int start_line, int start_col, int end_line, int end_col);
static void codeeditor_adjust_hidden_cursors(vg_codeeditor_t *editor);
static int codeeditor_prev_byte_boundary(const vg_codeeditor_t *editor, int line, int byte_col);

/// @brief Saturating-add an amount to a performance counter.
/// @param counter Counter to update; NULL is ignored.
/// @param amount Unsigned increment.
static void codeeditor_perf_add(uint64_t *counter, uint64_t amount) {
    if (!counter)
        return;
    if (*counter > UINT64_MAX - amount) {
        *counter = UINT64_MAX;
    } else {
        *counter += amount;
    }
}

/// @brief Advance the layout-cache generation and mark cached rows invalid.
/// @param editor Editor whose content-derived layout cache is invalidated.
static void codeeditor_bump_layout_generation(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    editor->layout_generation =
        editor->layout_generation == UINT64_MAX ? 1 : editor->layout_generation + 1;
    editor->layout_cache_valid = false;
}

/// @brief Recompute the summary flag indicating whether any fold is closed.
/// @param editor Editor whose fold-region array is inspected.
static void codeeditor_refresh_fold_summary(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    editor->has_folded_lines = false;
    for (int i = 0; i < editor->fold_region_count; i++) {
        if (editor->fold_regions[i].folded) {
            editor->has_folded_lines = true;
            return;
        }
    }
}

/// @brief Return whether the editor currently hides any source lines behind folds.
/// @param editor Editor to inspect.
/// @return Cached folded-line summary, or `false` when @p editor is NULL.
static bool codeeditor_has_hidden_lines(const vg_codeeditor_t *editor) {
    return editor ? editor->has_folded_lines : false;
}

/// @brief Clamp a source-line index to the editor's valid line range.
/// @param editor Editor defining the valid range.
/// @param line Candidate zero-based line index.
/// @return Clamped index, or zero for an invalid or empty editor.
static int codeeditor_clamp_line_index(const vg_codeeditor_t *editor, int line) {
    if (!editor || editor->line_count <= 0)
        return 0;
    if (line < 0)
        return 0;
    if (line >= editor->line_count)
        return editor->line_count - 1;
    return line;
}

//=============================================================================
// CodeEditor VTable
//=============================================================================

static vg_widget_vtable_t g_codeeditor_vtable = {.destroy = codeeditor_destroy,
                                                 .measure = codeeditor_measure,
                                                 .arrange = NULL,
                                                 .paint = codeeditor_paint,
                                                 .handle_event = codeeditor_handle_event,
                                                 .can_focus = codeeditor_can_focus,
                                                 .on_focus = codeeditor_on_focus};

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Grows @p *lines to hold at least @p needed entries, doubling capacity; zeros new slots.
/// @param[in,out] lines Pointer to the potentially reallocated line array.
/// @param[in,out] capacity Current capacity updated on successful growth.
/// @param needed Minimum required entry count.
/// @return `true` when the requested capacity is available.
static bool ensure_line_array_capacity(vg_code_line_t **lines, int *capacity, int needed) {
    if (!lines || !capacity || needed < 0)
        return false;
    if (needed <= *capacity)
        return true;

    int old_capacity = *capacity;
    int new_capacity = old_capacity;
    if (new_capacity <= 0)
        new_capacity = INITIAL_LINE_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > INT_MAX / LINE_CAPACITY_GROWTH)
            return false;
        new_capacity *= LINE_CAPACITY_GROWTH;
    }

    if ((size_t)new_capacity > SIZE_MAX / sizeof(vg_code_line_t))
        return false;
    vg_code_line_t *new_lines = realloc(*lines, (size_t)new_capacity * sizeof(vg_code_line_t));
    if (!new_lines)
        return false;

    memset(new_lines + old_capacity,
           0,
           (size_t)(new_capacity - old_capacity) * sizeof(vg_code_line_t));

    *lines = new_lines;
    *capacity = new_capacity;
    return true;
}

/// @brief Ensures editor->lines can hold at least @p needed entries, delegating to
/// ensure_line_array_capacity.
/// @param editor Editor whose line array may grow.
/// @param needed Minimum required line capacity.
/// @return `true` when the requested capacity is available.
static bool ensure_line_capacity(vg_codeeditor_t *editor, int needed) {
    if (!editor)
        return false;
    return ensure_line_array_capacity(&editor->lines, &editor->line_capacity, needed);
}

/// @brief Advance the editor content revision, keeping zero reserved for invalid handles.
/// @param editor Editor whose revision advances; NULL is ignored.
static void codeeditor_bump_revision(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    editor->revision = editor->revision == UINT64_MAX ? 1 : editor->revision + 1;
}

/// @brief The revision codeeditor_bump_revision() will produce on its next call.
/// @param editor Editor whose next revision is predicted.
/// @return Next nonzero revision, or one for a NULL editor.
static uint64_t codeeditor_pending_revision(const vg_codeeditor_t *editor) {
    if (!editor)
        return 1;
    return editor->revision == UINT64_MAX ? 1 : editor->revision + 1;
}

/// @brief Drop all retained sync deltas, freeing their replacement text.
/// @param editor Editor whose journal storage is cleared.
static void codeeditor_journal_reset(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    for (int i = 0; i < editor->journal_count; i++) {
        int idx = (editor->journal_head + i) % VG_EDIT_JOURNAL_CAP;
        free(editor->journal[idx].text);
        editor->journal[idx].text = NULL;
    }
    editor->journal_head = 0;
    editor->journal_count = 0;
    editor->journal_oldest_rev = 0;
}

/// @brief Signal that a mutation could not be captured as a delta, so the next
///        language-service pull must full-sync. Retained deltas are dropped
///        because they now precede a gap and cannot be applied incrementally.
/// @param editor Editor whose journal is invalidated.
static void codeeditor_journal_mark_overflow(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_journal_reset(editor);
    editor->journal_overflowed = true;
}

/// @brief Append one forward content delta (pre-edit range [start..end) replaced
///        by @p text) at @p revision. A full ring or OOM marks overflow instead.
/// @param editor Editor whose incremental journal receives the delta.
/// @param start_line First line of the replaced pre-edit range.
/// @param start_col First byte column of the replaced pre-edit range.
/// @param end_line Final line of the replaced pre-edit range.
/// @param end_col Exclusive final byte column of the replaced pre-edit range.
/// @param text Replacement text copied into the journal; NULL denotes an empty replacement.
/// @param revision Content revision produced by the mutation.
static void codeeditor_journal_push(vg_codeeditor_t *editor,
                                    int start_line,
                                    int start_col,
                                    int end_line,
                                    int end_col,
                                    const char *text,
                                    uint64_t revision) {
    if (!editor || editor->journal_overflowed)
        return; // already need a full sync
    if (editor->journal_count == VG_EDIT_JOURNAL_CAP) {
        codeeditor_journal_mark_overflow(editor);
        return;
    }
    const char *src = text ? text : "";
    size_t len = strlen(src);
    char *copy = malloc(len + 1);
    if (!copy) {
        codeeditor_journal_mark_overflow(editor);
        return;
    }
    memcpy(copy, src, len + 1);

    int tail = (editor->journal_head + editor->journal_count) % VG_EDIT_JOURNAL_CAP;
    vg_edit_delta_t *d = &editor->journal[tail];
    d->revision = revision;
    d->start_line = start_line;
    d->start_col = start_col;
    d->end_line = end_line;
    d->end_col = end_col;
    d->text = copy;
    if (editor->journal_count == 0)
        editor->journal_oldest_rev = revision;
    editor->journal_count++;
}

/// @brief Advance the syntax-cache generation after a language/theme/global-state change.
/// @param editor Editor whose syntax generation advances.
static void codeeditor_bump_highlight_generation(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    editor->highlight_generation =
        editor->highlight_generation == UINT64_MAX ? 1 : editor->highlight_generation + 1;
}

/// @brief Mark one line's cached syntax colors and language state stale.
/// @param editor Editor containing the line.
/// @param line Zero-based line index to invalidate.
static void codeeditor_invalidate_line_highlight(vg_codeeditor_t *editor, int line) {
    if (!editor || line < 0 || line >= editor->line_count)
        return;
    editor->lines[line].highlight_generation = 0;
    editor->lines[line].syntax_state_generation = 0;
}

/// @brief Mark all syntax colors and cached language states stale without touching text revision.
/// @param editor Editor whose complete syntax cache is invalidated.
static void codeeditor_invalidate_all_highlights(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_bump_highlight_generation(editor);
    for (int i = 0; i < editor->line_count; i++) {
        editor->lines[i].highlight_generation = 0;
        editor->lines[i].syntax_state_generation = 0;
    }
}

/// @brief Heuristic for edits that can change downstream lexical state.
/// @param text Inserted or deleted text to inspect.
/// @return `true` when the text contains a newline or block-comment delimiter byte.
static bool codeeditor_text_may_affect_multiline_syntax(const char *text) {
    if (!text)
        return false;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || *p == '/' || *p == '*')
            return true;
    }
    return false;
}

/// @brief Compare highlight spans in document position order.
/// @param lhs Address of the first highlight span.
/// @param rhs Address of the second highlight span.
/// @return A negative, zero, or positive value according to start then end coordinates.
static int highlight_span_compare(const void *lhs, const void *rhs) {
    const struct vg_highlight_span *a = (const struct vg_highlight_span *)lhs;
    const struct vg_highlight_span *b = (const struct vg_highlight_span *)rhs;
    if (a->start_line != b->start_line)
        return a->start_line < b->start_line ? -1 : 1;
    if (a->start_col != b->start_col)
        return a->start_col < b->start_col ? -1 : 1;
    if (a->end_line != b->end_line)
        return a->end_line < b->end_line ? -1 : 1;
    if (a->end_col != b->end_col)
        return a->end_col < b->end_col ? -1 : 1;
    return 0;
}

/// @brief Mark the per-line highlight-span index stale.
/// @param editor Editor whose derived index is invalidated.
static void codeeditor_invalidate_highlight_line_index(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    editor->highlight_line_index_valid = false;
}

/// @brief Release all arrays backing the per-line highlight-span index.
/// @param editor Editor whose index storage is freed and metadata reset.
static void codeeditor_free_highlight_line_index(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    free(editor->highlight_line_offsets);
    free(editor->highlight_line_span_indices);
    free(editor->highlight_line_write_offsets);
    editor->highlight_line_offsets = NULL;
    editor->highlight_line_span_indices = NULL;
    editor->highlight_line_write_offsets = NULL;
    editor->highlight_line_offsets_cap = 0;
    editor->highlight_line_span_indices_cap = 0;
    editor->highlight_line_write_offsets_cap = 0;
    editor->highlight_line_index_line_count = 0;
    editor->highlight_line_index_span_count = 0;
    editor->highlight_line_index_valid = false;
}

/// @brief Sort highlight spans by source range when their order is stale.
/// @param editor Editor whose span array is sorted and line index invalidated.
static void codeeditor_sort_highlight_spans(vg_codeeditor_t *editor) {
    if (!editor || editor->highlight_spans_sorted)
        return;
    if (editor->highlight_span_count <= 1) {
        editor->highlight_spans_sorted = true;
        codeeditor_invalidate_highlight_line_index(editor);
        return;
    }
    qsort(editor->highlight_spans,
          (size_t)editor->highlight_span_count,
          sizeof(*editor->highlight_spans),
          highlight_span_compare);
    editor->highlight_spans_sorted = true;
    codeeditor_invalidate_highlight_line_index(editor);
}

/// @brief Clamp a highlight span's line coverage to the current document.
/// @param editor Editor defining the valid source-line range.
/// @param span Highlight span to project onto lines.
/// @param[out] out_start Optional destination for the first covered line.
/// @param[out] out_end Optional destination for the final covered line.
/// @return `true` when the span overlaps at least one document line.
static bool codeeditor_highlight_line_range_for_span(const vg_codeeditor_t *editor,
                                                     const struct vg_highlight_span *span,
                                                     int *out_start,
                                                     int *out_end) {
    if (!editor || !span || editor->line_count <= 0)
        return false;

    int start = span->start_line;
    int end = span->end_line;
    if (end < 0 || start >= editor->line_count)
        return false;
    if (start < 0)
        start = 0;
    if (end >= editor->line_count)
        end = editor->line_count - 1;
    if (end < start)
        return false;

    if (out_start)
        *out_start = start;
    if (out_end)
        *out_end = end;
    return true;
}

/// @brief Build or reuse the index mapping each source line to intersecting highlight spans.
/// @details The compact index stores per-line offsets into a flat span-index array and is bounded
/// by `CODEEDITOR_HIGHLIGHT_LINE_INDEX_MAX_ENTRIES` to reject pathological all-document coverage.
/// @param editor Editor whose sorted spans and line count define the index.
/// @return `true` when a valid index is available, otherwise `false`.
static bool codeeditor_ensure_highlight_line_index(vg_codeeditor_t *editor) {
    if (!editor)
        return false;

    codeeditor_sort_highlight_spans(editor);

    if (editor->highlight_line_index_valid &&
        editor->highlight_line_index_line_count == editor->line_count &&
        editor->highlight_line_index_span_count == editor->highlight_span_count)
        return true;

    editor->highlight_line_index_valid = false;
    editor->highlight_line_index_line_count = 0;
    editor->highlight_line_index_span_count = 0;

    if (editor->line_count < 0 || editor->line_count == INT_MAX)
        return false;
    int offset_count = editor->line_count + 1;

    if (offset_count > editor->highlight_line_offsets_cap) {
        if ((size_t)offset_count > SIZE_MAX / sizeof(int))
            return false;
        int *offsets =
            (int *)realloc(editor->highlight_line_offsets, (size_t)offset_count * sizeof(int));
        if (!offsets)
            return false;
        editor->highlight_line_offsets = offsets;
        editor->highlight_line_offsets_cap = offset_count;
    }

    memset(editor->highlight_line_offsets, 0, (size_t)offset_count * sizeof(int));

    size_t total_entries = 0;
    for (int s = 0; s < editor->highlight_span_count; s++) {
        int start = 0;
        int end = 0;
        if (!codeeditor_highlight_line_range_for_span(
                editor, &editor->highlight_spans[s], &start, &end))
            continue;
        size_t coverage = (size_t)(end - start + 1);
        if (coverage > CODEEDITOR_HIGHLIGHT_LINE_INDEX_MAX_ENTRIES - total_entries)
            return false;
        total_entries += coverage;
        for (int line = start; line <= end; line++)
            editor->highlight_line_offsets[line + 1]++;
    }

    for (int line = 1; line < offset_count; line++)
        editor->highlight_line_offsets[line] += editor->highlight_line_offsets[line - 1];

    if ((int)total_entries > editor->highlight_line_span_indices_cap) {
        if (total_entries > SIZE_MAX / sizeof(int))
            return false;
        int *indices =
            (int *)realloc(editor->highlight_line_span_indices, total_entries * sizeof(int));
        if (!indices && total_entries > 0)
            return false;
        editor->highlight_line_span_indices = indices;
        editor->highlight_line_span_indices_cap = (int)total_entries;
    }

    if (offset_count > editor->highlight_line_write_offsets_cap) {
        if ((size_t)offset_count > SIZE_MAX / sizeof(int))
            return false;
        int *write_offsets = (int *)realloc(editor->highlight_line_write_offsets,
                                            (size_t)offset_count * sizeof(int));
        if (!write_offsets)
            return false;
        editor->highlight_line_write_offsets = write_offsets;
        editor->highlight_line_write_offsets_cap = offset_count;
    }
    memcpy(editor->highlight_line_write_offsets,
           editor->highlight_line_offsets,
           (size_t)offset_count * sizeof(int));

    for (int s = 0; s < editor->highlight_span_count; s++) {
        int start = 0;
        int end = 0;
        if (!codeeditor_highlight_line_range_for_span(
                editor, &editor->highlight_spans[s], &start, &end))
            continue;
        for (int line = start; line <= end; line++)
            editor->highlight_line_span_indices[editor->highlight_line_write_offsets[line]++] = s;
    }

    editor->highlight_line_index_line_count = editor->line_count;
    editor->highlight_line_index_span_count = editor->highlight_span_count;
    editor->highlight_line_index_valid = true;
    return true;
}

/// @brief Paint underline highlights intersecting one visible line segment.
/// @details Uses the per-line span index when available and clips range geometry to the requested
/// wrapped segment; unwrapped positions include horizontal scrolling.
/// @param editor Editor containing highlight spans and character metrics.
/// @param canvas Graphics window receiving underline primitives.
/// @param line Source line being painted.
/// @param segment_start First byte column in the visible segment.
/// @param segment_len Segment length in byte columns.
/// @param content_x Left content origin.
/// @param row_y Top of the visual row.
/// @param scroll_x Horizontal scroll offset for unwrapped text.
/// @param wrapped Whether segment-local wrapped coordinates are active.
static void codeeditor_paint_highlights_for_line(vg_codeeditor_t *editor,
                                                 void *canvas,
                                                 int line,
                                                 int segment_start,
                                                 int segment_len,
                                                 float content_x,
                                                 float row_y,
                                                 float scroll_x,
                                                 bool wrapped) {
    if (!editor || line < 0 || line >= editor->line_count || editor->highlight_span_count <= 0)
        return;

    bool indexed = codeeditor_ensure_highlight_line_index(editor);
    int begin = 0;
    int end = editor->highlight_span_count;
    if (indexed && editor->highlight_line_offsets &&
        line + 1 <= editor->highlight_line_index_line_count) {
        begin = editor->highlight_line_offsets[line];
        end = editor->highlight_line_offsets[line + 1];
    }

    size_t line_len = editor->lines[line].length;
    for (int entry = begin; entry < end; entry++) {
        codeeditor_perf_add(&editor->perf_stats.highlight_span_checks, 1);
        int span_index = indexed ? editor->highlight_line_span_indices[entry] : entry;
        struct vg_highlight_span *span = &editor->highlight_spans[span_index];
        if (!indexed) {
            if (span->start_line > line)
                break;
            if (line > span->end_line)
                continue;
        }

        int span_start = (line == span->start_line) ? span->start_col : 0;
        int span_end = (line == span->end_line) ? span->end_col : (int)line_len;
        if (span_end <= span_start)
            span_end = span_start + 1;

        if (!wrapped) {
            float span_x = content_x + (float)span_start * editor->char_width - scroll_x;
            float span_w = (float)(span_end - span_start) * editor->char_width;
            codeeditor_draw_highlight_underline(
                canvas, span_x, row_y, span_w, editor->line_height, span->color);
            continue;
        }

        int overlap_start = span_start > segment_start ? span_start : segment_start;
        int overlap_end =
            span_end < segment_start + segment_len ? span_end : segment_start + segment_len;
        if (overlap_end <= overlap_start)
            continue;
        float span_x = content_x + (float)(overlap_start - segment_start) * editor->char_width;
        float span_w = (float)(overlap_end - overlap_start) * editor->char_width;
        codeeditor_draw_highlight_underline(
            canvas, span_x, row_y, span_w, editor->line_height, span->color);
    }
}

/// @brief Minimal cross-line lexical state used to exclude strings and comments.
typedef struct codeeditor_syntax_scan_state {
    bool in_line_comment;
    bool in_block_comment;
    bool in_string;
    bool escaped;
    char string_quote;
} codeeditor_syntax_scan_state_t;

/// @brief Return whether the current byte is source-code punctuation and advance syntax state.
/// @details This lightweight scanner understands line comments (`//`), block
///          comments (`/* ... */`), single-quoted strings, double-quoted strings,
///          and backslash escapes. It intentionally does not try to classify
///          language tokens; it only answers whether delimiter matching should
///          consider the current byte as code.
/// @param state Mutable syntax state carried across lines.
/// @param text Current line text.
/// @param len Current line byte length.
/// @param col Current byte column.
/// @param out_advance Number of bytes the caller should advance.
/// @return true when the byte is outside comments and strings.
static bool codeeditor_syntax_scan_byte(codeeditor_syntax_scan_state_t *state,
                                        const char *text,
                                        int len,
                                        int col,
                                        int *out_advance) {
    if (out_advance)
        *out_advance = 1;
    if (!state || !text || col < 0 || col >= len)
        return false;

    char ch = text[col];
    char next = col + 1 < len ? text[col + 1] : '\0';

    if (state->in_line_comment)
        return false;

    if (state->in_block_comment) {
        if (ch == '*' && next == '/') {
            state->in_block_comment = false;
            if (out_advance)
                *out_advance = 2;
        }
        return false;
    }

    if (state->in_string) {
        if (state->escaped) {
            state->escaped = false;
        } else if (ch == '\\') {
            state->escaped = true;
        } else if (ch == state->string_quote) {
            state->in_string = false;
            state->string_quote = '\0';
        }
        return false;
    }

    if (ch == '/' && next == '/') {
        state->in_line_comment = true;
        if (out_advance)
            *out_advance = 2;
        return false;
    }
    if (ch == '/' && next == '*') {
        state->in_block_comment = true;
        if (out_advance)
            *out_advance = 2;
        return false;
    }
    if (ch == '"' || ch == '\'') {
        state->in_string = true;
        state->escaped = false;
        state->string_quote = ch;
        return false;
    }

    return true;
}

/// @brief Reset line-comment state at a physical line boundary.
/// @details Block comments and strings intentionally survive line boundaries;
///          `//` comments do not.
/// @param state Scanner state to advance to the next physical line.
static void codeeditor_syntax_scan_end_line(codeeditor_syntax_scan_state_t *state) {
    if (state)
        state->in_line_comment = false;
}

/// @brief Return true when a byte position is outside comments and strings.
/// @details Scans once from the start of the document to the requested position,
///          using the same lightweight rules as delimiter matching. The byte at
///          @p col is classified from the scanner state before that byte is
///          consumed.
/// @param editor Editor containing the source document.
/// @param line Target zero-based source line.
/// @param col Target byte column on @p line.
/// @return `true` when the target lies outside comments and strings.
static bool codeeditor_position_is_code_context(const vg_codeeditor_t *editor, int line, int col) {
    if (!editor || line < 0 || line >= editor->line_count)
        return false;
    codeeditor_syntax_scan_state_t state = {0};
    for (int l = 0; l <= line; l++) {
        const vg_code_line_t *code_line = &editor->lines[l];
        int len = code_line->length > (size_t)INT_MAX ? INT_MAX : (int)code_line->length;
        int limit = l == line ? col : len;
        if (limit < 0)
            limit = 0;
        if (limit > len)
            limit = len;
        for (int c = 0; c < limit;) {
            int advance = 1;
            (void)codeeditor_syntax_scan_byte(&state, code_line->text, len, c, &advance);
            if (advance < 1)
                advance = 1;
            c += advance;
        }
        if (l == line)
            break;
        codeeditor_syntax_scan_end_line(&state);
    }
    return !state.in_line_comment && !state.in_block_comment && !state.in_string;
}

/// @brief Source coordinate retained on a delimiter-matching stack.
typedef struct codeeditor_pair_position {
    int line;
    int col;
} codeeditor_pair_position_t;

/// @brief Append one delimiter position to a dynamically-sized stack.
/// @param[in,out] stack Pointer to the potentially reallocated stack.
/// @param[in,out] count Current entry count incremented on success.
/// @param[in,out] cap Current capacity updated on growth.
/// @param line Source line to append.
/// @param col Source byte column to append.
/// @return `true` on success, or `false` for invalid arguments or allocation failure.
static bool codeeditor_pair_stack_push(codeeditor_pair_position_t **stack,
                                       int *count,
                                       int *cap,
                                       int line,
                                       int col) {
    if (!stack || !count || !cap)
        return false;
    if (*count >= *cap) {
        int new_cap = *cap ? *cap * 2 : 32;
        if (new_cap < *cap || new_cap > INT_MAX / (int)sizeof(**stack))
            return false;
        void *p = realloc(*stack, (size_t)new_cap * sizeof(**stack));
        if (!p)
            return false;
        *stack = p;
        *cap = new_cap;
    }
    (*stack)[*count].line = line;
    (*stack)[*count].col = col;
    (*count)++;
    return true;
}

/// @brief Find the nearest unmatched opener before an arbitrary insertion point.
/// @details Scans source text in document order, ignoring comments and strings.
///          Openers push onto a stack and closers pop; the top of the stack at
///          @p line:@p col is the opener that should own an inserted closer.
/// @param editor Editor containing the source document.
/// @param line Insertion-point line, inclusive scan limit.
/// @param col Insertion-point byte column, exclusive scan limit.
/// @param opener Opening delimiter byte.
/// @param closer Closing delimiter byte.
/// @param[out] out_line Optional destination for the unmatched opener line.
/// @param[out] out_col Optional destination for the unmatched opener byte column.
/// @return `true` when an unmatched opener exists before the insertion point.
static bool codeeditor_find_unmatched_opener_before(const vg_codeeditor_t *editor,
                                                    int line,
                                                    int col,
                                                    char opener,
                                                    char closer,
                                                    int *out_line,
                                                    int *out_col) {
    if (out_line)
        *out_line = -1;
    if (out_col)
        *out_col = -1;
    if (!editor || line < 0 || line >= editor->line_count)
        return false;

    codeeditor_syntax_scan_state_t state = {0};
    codeeditor_pair_position_t *stack = NULL;
    int stack_count = 0;
    int stack_cap = 0;

    for (int l = 0; l <= line; l++) {
        const vg_code_line_t *code_line = &editor->lines[l];
        int len = code_line->length > (size_t)INT_MAX ? INT_MAX : (int)code_line->length;
        int limit = l == line ? col : len;
        if (limit < 0)
            limit = 0;
        if (limit > len)
            limit = len;
        for (int c = 0; c < limit;) {
            int advance = 1;
            bool is_code = codeeditor_syntax_scan_byte(&state, code_line->text, len, c, &advance);
            if (is_code) {
                char ch = code_line->text[c];
                if (ch == opener) {
                    if (!codeeditor_pair_stack_push(&stack, &stack_count, &stack_cap, l, c))
                        goto cleanup;
                } else if (ch == closer && stack_count > 0) {
                    stack_count--;
                }
            }
            if (advance < 1)
                advance = 1;
            c += advance;
        }
        if (l == line)
            break;
        codeeditor_syntax_scan_end_line(&state);
    }

    if (stack_count <= 0)
        goto cleanup;
    if (out_line)
        *out_line = stack[stack_count - 1].line;
    if (out_col)
        *out_col = stack[stack_count - 1].col;
    free(stack);
    return true;

cleanup:
    free(stack);
    return false;
}

/// @brief Classify a supported bracket byte and return its matching peer and scan direction.
/// @param ch Candidate bracket byte.
/// @param[out] out_match Optional destination for the paired delimiter.
/// @param[out] out_direction Optional destination for `1` when scanning forward or `-1` backward.
/// @return `true` for parentheses, brackets, or braces.
static bool codeeditor_pair_info(char ch, char *out_match, int *out_direction) {
    char match = '\0';
    int direction = 0;
    switch (ch) {
        case '(':
            match = ')';
            direction = 1;
            break;
        case '[':
            match = ']';
            direction = 1;
            break;
        case '{':
            match = '}';
            direction = 1;
            break;
        case ')':
            match = '(';
            direction = -1;
            break;
        case ']':
            match = '[';
            direction = -1;
            break;
        case '}':
            match = '{';
            direction = -1;
            break;
        default:
            return false;
    }
    if (out_match)
        *out_match = match;
    if (out_direction)
        *out_direction = direction;
    return true;
}

/// @brief Read one byte from a validated document coordinate.
/// @param editor Editor containing the source line.
/// @param line Zero-based line index.
/// @param col Zero-based byte column.
/// @param[out] out_ch Optional destination, cleared before validation.
/// @return `true` when the coordinate names an existing byte.
static bool codeeditor_byte_at(const vg_codeeditor_t *editor, int line, int col, char *out_ch) {
    if (out_ch)
        *out_ch = '\0';
    if (!editor || line < 0 || line >= editor->line_count)
        return false;
    const vg_code_line_t *code_line = &editor->lines[line];
    if (!code_line->text || col < 0 || col >= (int)code_line->length)
        return false;
    if (out_ch)
        *out_ch = code_line->text[col];
    return true;
}

/// @brief Find the closer matching an opening delimiter by scanning forward.
/// @details The scan ignores delimiters inside strings and comments and balances nested peers.
/// @param editor Editor containing the source document.
/// @param line Opening delimiter line.
/// @param col Opening delimiter byte column.
/// @param opener Opening delimiter byte.
/// @param closer Matching closing delimiter byte.
/// @param[out] out_line Optional destination for the peer line.
/// @param[out] out_col Optional destination for the peer byte column.
/// @return `true` when a balanced closer is found.
static bool codeeditor_find_matching_pair_forward(const vg_codeeditor_t *editor,
                                                  int line,
                                                  int col,
                                                  char opener,
                                                  char closer,
                                                  int *out_line,
                                                  int *out_col) {
    if (!editor)
        return false;
    codeeditor_syntax_scan_state_t state = {0};
    bool active = false;
    int depth = 0;
    for (int l = 0; l < editor->line_count; l++) {
        const vg_code_line_t *code_line = &editor->lines[l];
        int len = code_line->length > (size_t)INT_MAX ? INT_MAX : (int)code_line->length;
        for (int c = 0; code_line->text && c < len;) {
            int advance = 1;
            bool is_code = codeeditor_syntax_scan_byte(&state, code_line->text, len, c, &advance);
            if (is_code) {
                char ch = code_line->text[c];
                if (!active) {
                    if (l == line && c == col && ch == opener) {
                        active = true;
                        depth = 1;
                    }
                } else if (ch == opener) {
                    depth++;
                } else if (ch == closer) {
                    depth--;
                    if (depth == 0) {
                        if (out_line)
                            *out_line = l;
                        if (out_col)
                            *out_col = c;
                        return true;
                    }
                }
            }
            if (advance < 1)
                advance = 1;
            c += advance;
        }
        codeeditor_syntax_scan_end_line(&state);
    }
    return false;
}

/// @brief Find the opener matching a closing delimiter by scanning through the target.
/// @details Code-context openers are stacked and ordinary closers pop the stack; the stack top at
/// the target closer is its peer.
/// @param editor Editor containing the source document.
/// @param line Closing delimiter line.
/// @param col Closing delimiter byte column.
/// @param closer Closing delimiter byte.
/// @param opener Matching opening delimiter byte.
/// @param[out] out_line Optional destination for the peer line.
/// @param[out] out_col Optional destination for the peer byte column.
/// @return `true` when a balanced opener is found.
static bool codeeditor_find_matching_pair_backward(const vg_codeeditor_t *editor,
                                                   int line,
                                                   int col,
                                                   char closer,
                                                   char opener,
                                                   int *out_line,
                                                   int *out_col) {
    if (!editor)
        return false;
    codeeditor_syntax_scan_state_t state = {0};
    codeeditor_pair_position_t *stack = NULL;
    int stack_count = 0;
    int stack_cap = 0;
    bool found = false;

    for (int l = 0; l <= line && !found; l++) {
        const vg_code_line_t *code_line = &editor->lines[l];
        int len = code_line->length > (size_t)INT_MAX ? INT_MAX : (int)code_line->length;
        int limit = l == line ? col + 1 : len;
        if (limit > len)
            limit = len;
        for (int c = 0; code_line->text && c < limit;) {
            int advance = 1;
            bool is_code = codeeditor_syntax_scan_byte(&state, code_line->text, len, c, &advance);
            if (is_code) {
                char ch = code_line->text[c];
                if (ch == opener) {
                    if (!codeeditor_pair_stack_push(&stack, &stack_count, &stack_cap, l, c))
                        goto cleanup;
                } else if (ch == closer) {
                    if (l == line && c == col) {
                        if (stack_count > 0) {
                            if (out_line)
                                *out_line = stack[stack_count - 1].line;
                            if (out_col)
                                *out_col = stack[stack_count - 1].col;
                            found = true;
                        }
                        break;
                    }
                    if (stack_count > 0)
                        stack_count--;
                }
            }
            if (advance < 1)
                advance = 1;
            c += advance;
        }
        codeeditor_syntax_scan_end_line(&state);
    }

cleanup:
    free(stack);
    return found;
}

/// @brief Refresh the delimiter-pair match cached for the current cursor and revision.
/// @details The byte at the cursor, or the previous UTF-8 boundary, is classified as a bracket and
/// matched in the appropriate direction. Cache metadata is updated even when no pair exists.
/// @param editor Editor whose pair cache is refreshed.
static void codeeditor_update_pair_match_cache(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    if (editor->pair_match_cache_valid && editor->pair_match_revision == editor->revision &&
        editor->pair_match_cursor_line == editor->cursor_line &&
        editor->pair_match_cursor_col == editor->cursor_col)
        return;

    editor->pair_match_cache_valid = true;
    editor->pair_match_revision = editor->revision;
    editor->pair_match_cursor_line = editor->cursor_line;
    editor->pair_match_cursor_col = editor->cursor_col;
    editor->pair_match_active = false;
    editor->pair_anchor_line = -1;
    editor->pair_anchor_col = -1;
    editor->pair_peer_line = -1;
    editor->pair_peer_col = -1;

    int line = editor->cursor_line;
    int col = editor->cursor_col;
    char ch = '\0';
    if (!codeeditor_byte_at(editor, line, col, &ch) || !codeeditor_pair_info(ch, NULL, NULL)) {
        int prev_col = codeeditor_prev_byte_boundary(editor, line, col);
        if (prev_col == col || !codeeditor_byte_at(editor, line, prev_col, &ch) ||
            !codeeditor_pair_info(ch, NULL, NULL)) {
            return;
        }
        col = prev_col;
    }

    char match = '\0';
    int direction = 0;
    if (!codeeditor_pair_info(ch, &match, &direction))
        return;

    int peer_line = -1;
    int peer_col = -1;
    bool found = direction > 0 ? codeeditor_find_matching_pair_forward(
                                     editor, line, col, ch, match, &peer_line, &peer_col)
                               : codeeditor_find_matching_pair_backward(
                                     editor, line, col, ch, match, &peer_line, &peer_col);
    if (!found)
        return;

    editor->pair_match_active = true;
    editor->pair_anchor_line = line;
    editor->pair_anchor_col = col;
    editor->pair_peer_line = peer_line;
    editor->pair_peer_col = peer_col;
}

/// @brief Draw one delimiter-highlight cell in a visible text segment.
/// @param editor Editor supplying character and line dimensions.
/// @param canvas Graphics window receiving fill and border primitives.
/// @param col Delimiter byte column.
/// @param segment_start First byte column of a wrapped segment.
/// @param segment_len Wrapped segment length.
/// @param content_x Left content origin.
/// @param row_y Top of the visual row.
/// @param scroll_x Horizontal offset for unwrapped text.
/// @param wrapped Whether segment-local wrapped coordinates are active.
static void codeeditor_draw_pair_cell(vg_codeeditor_t *editor,
                                      void *canvas,
                                      int col,
                                      int segment_start,
                                      int segment_len,
                                      float content_x,
                                      float row_y,
                                      float scroll_x,
                                      bool wrapped) {
    if (!editor)
        return;
    if (wrapped && (col < segment_start || col >= segment_start + segment_len))
        return;
    float x = wrapped ? content_x + (float)(col - segment_start) * editor->char_width
                      : content_x + (float)col * editor->char_width - scroll_x;
    float y = row_y + 1.0f;
    float h = editor->line_height - 2.0f;
    if (h < 1.0f)
        h = editor->line_height;
    vgfx_fill_rect((vgfx_window_t)canvas,
                   (int32_t)x,
                   (int32_t)y,
                   (int32_t)editor->char_width,
                   (int32_t)h,
                   CODEEDITOR_PAIR_HIGHLIGHT_FILL);
    vgfx_rect((vgfx_window_t)canvas,
              (int32_t)x,
              (int32_t)y,
              (int32_t)editor->char_width,
              (int32_t)h,
              CODEEDITOR_PAIR_HIGHLIGHT_BORDER);
}

/// @brief Paint cached delimiter peers that intersect one visible line segment.
/// @param editor Editor containing cursor and pair-match state.
/// @param canvas Graphics window receiving pair cells.
/// @param line Source line being painted.
/// @param segment_start First byte column of the segment.
/// @param segment_len Segment byte length.
/// @param content_x Left content origin.
/// @param row_y Top of the visual row.
/// @param scroll_x Horizontal offset for unwrapped text.
/// @param wrapped Whether segment-local wrapped coordinates are active.
static void codeeditor_paint_pair_highlight_for_segment(vg_codeeditor_t *editor,
                                                        void *canvas,
                                                        int line,
                                                        int segment_start,
                                                        int segment_len,
                                                        float content_x,
                                                        float row_y,
                                                        float scroll_x,
                                                        bool wrapped) {
    if (!editor || !(editor->base.state & VG_STATE_FOCUSED))
        return;
    codeeditor_update_pair_match_cache(editor);
    if (!editor->pair_match_active)
        return;
    if (line == editor->pair_anchor_line) {
        codeeditor_draw_pair_cell(editor,
                                  canvas,
                                  editor->pair_anchor_col,
                                  segment_start,
                                  segment_len,
                                  content_x,
                                  row_y,
                                  scroll_x,
                                  wrapped);
    }
    if (line == editor->pair_peer_line) {
        codeeditor_draw_pair_cell(editor,
                                  canvas,
                                  editor->pair_peer_col,
                                  segment_start,
                                  segment_len,
                                  content_x,
                                  row_y,
                                  scroll_x,
                                  wrapped);
    }
}

/// @brief Grows @p line->text to hold at least @p needed bytes, doubling from
/// INITIAL_TEXT_CAPACITY.
/// @param line Line whose owned text allocation may grow.
/// @param needed Required byte capacity including any terminator budget.
/// @return `true` when the requested capacity is available.
static bool ensure_text_capacity(vg_code_line_t *line, size_t needed) {
    if (needed <= line->capacity)
        return true;

    size_t new_capacity = line->capacity;
    if (new_capacity == 0)
        new_capacity = INITIAL_TEXT_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2)
            return false;
        new_capacity *= 2;
    }

    char *new_text = realloc(line->text, new_capacity);
    if (!new_text)
        return false;

    line->text = new_text;
    line->capacity = new_capacity;
    return true;
}

/// @brief Return a line's byte length as a bounded signed integer.
/// @param editor Editor containing the line.
/// @param line Zero-based line index.
/// @return Byte length saturated at `INT_MAX`, or zero for invalid input.
static int codeeditor_line_length_i32(const vg_codeeditor_t *editor, int line) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    return editor->lines[line].length > (size_t)INT_MAX ? INT_MAX : (int)editor->lines[line].length;
}

/// @brief Allocates and copies @p len bytes from @p text into @p line, zero-initializing all other
/// fields.
/// @param line Destination line object.
/// @param text Source bytes, required when @p len is nonzero.
/// @param len Number of bytes to copy.
/// @return `true` when the line is initialized.
static bool init_line_text(vg_code_line_t *line, const char *text, size_t len) {
    if (!line || len > SIZE_MAX - 1 || len > (size_t)INT_MAX)
        return false;
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return false;
    if (len > 0 && text)
        memcpy(copy, text, len);
    copy[len] = '\0';
    memset(line, 0, sizeof(*line));
    line->text = copy;
    line->length = len;
    line->capacity = len + 1;
    return true;
}

/// @brief Allocates a minimal empty text buffer for @p line if it has none; no-op if text already
/// exists.
/// @param line Line that must acquire valid NUL-terminated storage.
/// @return `true` when storage exists.
static bool ensure_line_text_exists(vg_code_line_t *line) {
    if (!line)
        return false;
    if (line->text)
        return true;
    char *empty = (char *)malloc(1);
    if (!empty)
        return false;
    empty[0] = '\0';
    line->text = empty;
    line->length = 0;
    line->capacity = 1;
    return true;
}

/// @brief Frees the text and colors buffers of @p line and zeroes its length/capacity fields.
/// @param line Line to clear; NULL is ignored.
static void free_line(vg_code_line_t *line) {
    if (!line)
        return;
    if (line->text) {
        free(line->text);
        line->text = NULL;
    }
    if (line->colors) {
        free(line->colors);
        line->colors = NULL;
    }
    line->length = 0;
    line->capacity = 0;
    line->colors_capacity = 0;
}

/// @brief Calls free_line on each of the @p count entries in @p lines, then frees the array itself.
/// @param lines Owned line array; NULL is ignored.
/// @param count Number of initialized entries to release.
static void free_line_array(vg_code_line_t *lines, int count) {
    if (!lines)
        return;
    for (int i = 0; i < count; i++)
        free_line(&lines[i]);
    free(lines);
}

/// @brief Returns the byte length of the well-formed UTF-8 sequence starting at @p p (1–4), or 0 on
/// error.
/// @param p Pointer into a NUL-terminated byte string.
/// @return Valid sequence length from one through four, or zero at termination or malformed input.
static size_t codeeditor_utf8_span(const char *p) {
    if (!p || *p == '\0')
        return 0;

    const unsigned char *s = (const unsigned char *)p;
    if ((s[0] & 0x80u) == 0)
        return 1;
    if ((s[0] & 0xE0u) == 0xC0u) {
        if (s[1] == '\0')
            return 0;
        if ((s[1] & 0xC0u) != 0x80u)
            return 0;
        uint32_t cp = ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        return cp >= 0x80u ? 2u : 0u;
    }
    if ((s[0] & 0xF0u) == 0xE0u) {
        if (s[1] == '\0' || s[2] == '\0')
            return 0;
        if ((s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u)
            return 0;
        uint32_t cp = ((uint32_t)(s[0] & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) |
                      (uint32_t)(s[2] & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
            return 0;
        return 3;
    }
    if ((s[0] & 0xF8u) == 0xF0u) {
        if (s[1] == '\0' || s[2] == '\0' || s[3] == '\0')
            return 0;
        if ((s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u || (s[3] & 0xC0u) != 0x80u)
            return 0;
        uint32_t cp = ((uint32_t)(s[0] & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
                      ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
        return (cp >= 0x10000u && cp <= 0x10FFFFu) ? 4u : 0u;
    }
    return 0;
}

/// @brief Convert an internal byte column to a UTF-8 codepoint column.
/// @param editor Editor containing the source line.
/// @param line Zero-based line index.
/// @param byte_col Byte offset to convert, clamped to the line length.
/// @return Number of decoded code points preceding the byte position.
static int codeeditor_byte_col_to_char_col(const vg_codeeditor_t *editor, int line, int byte_col) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    const vg_code_line_t *code_line = &editor->lines[line];
    if (byte_col <= 0)
        return 0;
    if (byte_col > (int)code_line->length)
        byte_col = (int)code_line->length;

    int chars = 0;
    size_t pos = 0;
    while (pos < (size_t)byte_col) {
        size_t span = codeeditor_utf8_span(code_line->text + pos);
        if (span == 0 || pos + span > code_line->length)
            span = 1;
        if (pos + span > (size_t)byte_col)
            break;
        pos += span;
        chars++;
    }
    return chars;
}

/// @brief Convert a public UTF-8 codepoint column to the corresponding internal byte column.
/// @param editor Editor containing the source line.
/// @param line Zero-based line index.
/// @param char_col Code-point column to convert.
/// @return Corresponding byte offset, clamped to the line and `INT_MAX`.
static int codeeditor_char_col_to_byte_col(const vg_codeeditor_t *editor, int line, int char_col) {
    if (!editor || line < 0 || line >= editor->line_count || char_col <= 0)
        return 0;
    const vg_code_line_t *code_line = &editor->lines[line];
    size_t pos = 0;
    int chars = 0;
    while (pos < code_line->length && chars < char_col) {
        size_t span = codeeditor_utf8_span(code_line->text + pos);
        if (span == 0 || pos + span > code_line->length)
            span = 1;
        pos += span;
        chars++;
    }
    return pos > (size_t)INT_MAX ? INT_MAX : (int)pos;
}

/// @brief Return the start byte of the UTF-8 codepoint immediately before @p byte_col.
/// @param editor Editor containing the source line.
/// @param line Zero-based line index.
/// @param byte_col Byte position to step backward from.
/// @return Previous code-point boundary, or zero for invalid input.
static int codeeditor_prev_byte_boundary(const vg_codeeditor_t *editor, int line, int byte_col) {
    if (!editor || line < 0 || line >= editor->line_count || byte_col <= 0)
        return 0;
    const vg_code_line_t *code_line = &editor->lines[line];
    if (byte_col > (int)code_line->length)
        byte_col = (int)code_line->length;

    size_t pos = 0;
    while (pos < (size_t)byte_col) {
        size_t span = codeeditor_utf8_span(code_line->text + pos);
        if (span == 0 || pos + span > code_line->length)
            span = 1;
        if (pos + span >= (size_t)byte_col)
            return (int)pos;
        pos += span;
    }
    return (int)pos;
}

/// @brief Return the byte offset immediately after the UTF-8 codepoint at or after @p byte_col.
/// @param editor Editor containing the source line.
/// @param line Zero-based line index.
/// @param byte_col Byte position to step forward from.
/// @return Next code-point boundary, clamped to the line length.
static int codeeditor_next_byte_boundary(const vg_codeeditor_t *editor, int line, int byte_col) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    const vg_code_line_t *code_line = &editor->lines[line];
    if (byte_col < 0)
        byte_col = 0;
    if (byte_col >= (int)code_line->length)
        return (int)code_line->length;

    size_t pos = 0;
    while (pos < code_line->length) {
        size_t span = codeeditor_utf8_span(code_line->text + pos);
        if (span == 0 || pos + span > code_line->length)
            span = 1;
        if ((size_t)byte_col <= pos || (size_t)byte_col < pos + span)
            return (int)(pos + span);
        pos += span;
    }
    return (int)code_line->length;
}

/// @brief Frees the highlight_spans array and resets its count and capacity to zero.
/// @param editor Editor whose highlight annotations and derived index are cleared.
static void codeeditor_clear_highlight_spans(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    free(editor->highlight_spans);
    editor->highlight_spans = NULL;
    editor->highlight_span_count = 0;
    editor->highlight_span_cap = 0;
    editor->highlight_spans_sorted = true;
    codeeditor_free_highlight_line_index(editor);
}

/// @brief Release every copied inlay-hint string and the hint array.
/// @param editor Editor whose inlay-hint storage and counters are reset.
static void codeeditor_free_inlay_hint_storage(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    for (int i = 0; i < editor->inlay_hint_count; i++)
        free(editor->inlay_hints[i].text);
    free(editor->inlay_hints);
    editor->inlay_hints = NULL;
    editor->inlay_hint_count = 0;
    editor->inlay_hint_cap = 0;
    editor->inlay_hints_sorted = true;
}

/// @brief Compare inlay hints by source line then byte column.
/// @param a Address of the first inlay hint.
/// @param b Address of the second inlay hint.
/// @return A negative, zero, or positive value in document position order.
static int codeeditor_compare_inlay_hints(const void *a, const void *b) {
    const struct vg_inlay_hint *lhs = (const struct vg_inlay_hint *)a;
    const struct vg_inlay_hint *rhs = (const struct vg_inlay_hint *)b;
    if (lhs->line != rhs->line)
        return lhs->line < rhs->line ? -1 : 1;
    if (lhs->col != rhs->col)
        return lhs->col < rhs->col ? -1 : 1;
    return 0;
}

/// @brief Sort inlay hints in document order when their order is stale.
/// @param editor Editor whose hint array may be sorted in place.
static void codeeditor_sort_inlay_hints(vg_codeeditor_t *editor) {
    if (!editor || editor->inlay_hints_sorted)
        return;
    if (editor->inlay_hint_count > 1) {
        qsort(editor->inlay_hints,
              (size_t)editor->inlay_hint_count,
              sizeof(*editor->inlay_hints),
              codeeditor_compare_inlay_hints);
    }
    editor->inlay_hints_sorted = true;
}

/// @brief Find the first sorted inlay hint assigned to a source line.
/// @param editor Editor containing sorted hints.
/// @param line Zero-based source line.
/// @return Hint-array index, or `-1` when the line has no hint.
static int codeeditor_first_inlay_hint_for_line(const vg_codeeditor_t *editor, int line) {
    if (!editor || !editor->inlay_hints || editor->inlay_hint_count <= 0)
        return -1;

    int lo = 0;
    int hi = editor->inlay_hint_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (editor->inlay_hints[mid].line < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= editor->inlay_hint_count || editor->inlay_hints[lo].line != line)
        return -1;
    return lo;
}

/// @brief Compare semantic tokens by line, then start column, then end column.
/// @details Sorted semantic tokens allow the highlighter to binary-search the
///          first token for a visible line instead of scanning every project
///          token for every line being painted.
/// @param a Pointer to the left semantic-token record.
/// @param b Pointer to the right semantic-token record.
/// @return Negative, zero, or positive using qsort ordering semantics.
static int codeeditor_compare_semantic_tokens(const void *a, const void *b) {
    const struct vg_semantic_token *lhs = (const struct vg_semantic_token *)a;
    const struct vg_semantic_token *rhs = (const struct vg_semantic_token *)b;
    if (lhs->line != rhs->line)
        return lhs->line < rhs->line ? -1 : 1;
    if (lhs->start_col != rhs->start_col)
        return lhs->start_col < rhs->start_col ? -1 : 1;
    if (lhs->end_col != rhs->end_col)
        return lhs->end_col < rhs->end_col ? -1 : 1;
    return 0;
}

/// @brief Ensure semantic tokens are ordered for per-line lookup.
/// @details The runtime API accepts tokens one at a time from compiler jobs, so
///          insertion order is not guaranteed. Sorting lazily keeps token-add
///          calls cheap and pays the ordering cost once before paint lookup.
/// @param editor Code editor whose semantic-token array may be sorted in place.
static void codeeditor_sort_semantic_tokens(vg_codeeditor_t *editor) {
    if (!editor || editor->semantic_tokens_sorted)
        return;
    if (editor->semantic_token_count > 1) {
        qsort(editor->semantic_tokens,
              (size_t)editor->semantic_token_count,
              sizeof(*editor->semantic_tokens),
              codeeditor_compare_semantic_tokens);
    }
    editor->semantic_tokens_sorted = true;
}

/// @brief Find the first semantic token on a source line.
/// @details Requires the semantic-token array to be sorted by line. The caller
///          can then iterate forward until the line changes.
/// @param editor Code editor containing sorted semantic tokens.
/// @param line Zero-based source line.
/// @return First token index for @p line, or -1 when the line has no tokens.
static int codeeditor_first_semantic_token_for_line(const vg_codeeditor_t *editor, int line) {
    if (!editor || !editor->semantic_tokens || editor->semantic_token_count <= 0)
        return -1;

    int lo = 0;
    int hi = editor->semantic_token_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (editor->semantic_tokens[mid].line < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= editor->semantic_token_count || editor->semantic_tokens[lo].line != line)
        return -1;
    return lo;
}

/// @brief Ensures the colors buffer for @p line_idx is sized, then invokes the syntax highlighter
/// callback.
/// @param editor Editor containing the line and syntax callback.
/// @param line_idx Zero-based line index to highlight.
static void highlight_line(vg_codeeditor_t *editor, size_t line_idx) {
    if (!editor->syntax_highlighter || line_idx >= (size_t)editor->line_count)
        return;

    vg_code_line_t *line = &editor->lines[line_idx];
    if (line->length == 0)
        return;
    if (line->colors && line->colors_capacity >= line->length &&
        line->highlight_generation == editor->highlight_generation)
        return;

    // Grow the colors buffer if needed
    if (line->colors_capacity < line->length) {
        if (line->length > SIZE_MAX - 16u || line->length + 16u > SIZE_MAX / sizeof(uint32_t)) {
            free(line->colors);
            line->colors = NULL;
            line->colors_capacity = 0;
            return;
        }
        size_t new_cap = line->length + 16;
        uint32_t *nc = (uint32_t *)realloc(line->colors, new_cap * sizeof(uint32_t));
        if (!nc) {
            // Realloc failed: discard stale buffer so the paint path falls back to
            // monochrome rendering rather than reading past the old capacity.
            free(line->colors);
            line->colors = NULL;
            line->colors_capacity = 0;
            return;
        }
        line->colors = nc;
        line->colors_capacity = new_cap;
    }

    codeeditor_perf_add(&editor->perf_stats.line_highlight_calls, 1);
    editor->syntax_highlighter(
        (vg_widget_t *)editor, (int)line_idx, line->text, line->colors, editor->syntax_data);

    // Semantic overlay: the compiler-classified tokens win over the lexical
    // guess for the identifier columns they cover (see ADR 0007). Only this
    // line's tokens are applied; unclassified columns keep their lexical color.
    codeeditor_sort_semantic_tokens(editor);
    int first_semantic = codeeditor_first_semantic_token_for_line(editor, (int)line_idx);
    for (int t = first_semantic; t >= 0 && t < editor->semantic_token_count; t++) {
        const struct vg_semantic_token *st = &editor->semantic_tokens[t];
        if (st->line != (int)line_idx)
            break;
        int s = st->start_col < 0 ? 0 : st->start_col;
        int e = st->end_col > (int)line->length ? (int)line->length : st->end_col;
        for (int c = s; c < e; c++)
            line->colors[c] = st->color;
    }

    line->highlight_generation = editor->highlight_generation;
}

/// @brief Draws the substring text[start..start+len] at (x,y).
/// @details The font API currently accepts NUL-terminated strings rather than
///          byte ranges, so the editor must copy each colored run before
///          drawing it. A 4 KiB stack scratch keeps ordinary long code lines out
///          of the heap path; only unusually large single-color runs allocate.
/// @param canvas Graphics window receiving the text.
/// @param font Font used for rendering.
/// @param font_size Font size in pixels.
/// @param x Baseline origin x coordinate.
/// @param y Baseline origin y coordinate.
/// @param text Complete source line.
/// @param start First byte to copy.
/// @param len Number of bytes to draw.
/// @param color Packed foreground color.
static void codeeditor_draw_text_slice(void *canvas,
                                       vg_font_t *font,
                                       float font_size,
                                       float x,
                                       float y,
                                       const char *text,
                                       size_t start,
                                       size_t len,
                                       uint32_t color) {
    if (!font || !text || len == 0)
        return;

    char stack_buf[4096];
    char *buf = stack_buf;
    size_t copy_len = len;
    if (copy_len >= sizeof(stack_buf)) {
        buf = (char *)malloc(copy_len + 1);
        if (!buf)
            return;
    }
    memcpy(buf, text + start, copy_len);
    buf[copy_len] = '\0';
    vg_font_draw_text(canvas, font, font_size, x, y, buf, color);
    if (buf != stack_buf)
        free(buf);
}

/// @brief Draws text[start..start+len] in color runs derived from @p colors[], falling back to @p
/// fallback_color.
/// @param canvas Graphics window receiving the text.
/// @param font Font used for rendering.
/// @param font_size Font size in pixels.
/// @param origin_x Left origin of the requested slice.
/// @param y Text baseline.
/// @param text Complete source line.
/// @param colors Per-byte foreground colors.
/// @param colors_capacity Number of readable entries in @p colors.
/// @param start First source byte in the slice.
/// @param len Number of source bytes in the slice.
/// @param char_width Fixed horizontal byte-cell advance.
/// @param fallback_color Color used beyond @p colors_capacity.
static void codeeditor_draw_colored_slice(void *canvas,
                                          vg_font_t *font,
                                          float font_size,
                                          float origin_x,
                                          float y,
                                          const char *text,
                                          const uint32_t *colors,
                                          size_t colors_capacity,
                                          size_t start,
                                          size_t len,
                                          float char_width,
                                          uint32_t fallback_color) {
    if (!text || !colors || len == 0)
        return;

    size_t run_start = start;
    uint32_t run_color = run_start < colors_capacity ? colors[run_start] : fallback_color;
    size_t end = start + len;
    for (size_t i = start + 1; i < end; i++) {
        uint32_t color = i < colors_capacity ? colors[i] : fallback_color;
        if (color == run_color)
            continue;
        codeeditor_draw_text_slice(canvas,
                                   font,
                                   font_size,
                                   origin_x + (float)(run_start - start) * char_width,
                                   y,
                                   text,
                                   run_start,
                                   i - run_start,
                                   run_color);
        run_start = i;
        run_color = color;
    }
    codeeditor_draw_text_slice(canvas,
                               font,
                               font_size,
                               origin_x + (float)(run_start - start) * char_width,
                               y,
                               text,
                               run_start,
                               end - run_start,
                               run_color);
}

/// @brief Computes the minimum gutter width needed to display the widest line-number string plus 20
/// px padding.
/// @param editor Editor whose line count and font determine the width.
/// @return Required line-number gutter width, or zero when line numbers are hidden.
static float codeeditor_auto_line_number_gutter_width(const vg_codeeditor_t *editor) {
    if (!editor || !editor->show_line_numbers)
        return 0.0f;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", editor->line_count > 0 ? editor->line_count : 1);
    if (editor->font) {
        vg_text_metrics_t metrics = {0};
        vg_font_measure_text(editor->font, editor->font_size, buf, &metrics);
        return metrics.width + 20.0f;
    }
    return (float)strlen(buf) * editor->char_width + 20.0f;
}

/// @brief Returns the fold-marker gutter width (one line_height wide, clamped to
/// CODEEDITOR_FOLD_GUTTER_MIN_WIDTH).
/// @param editor Editor whose fold-gutter visibility and line height are inspected.
/// @return Fold-gutter width, or zero when disabled.
static float codeeditor_fold_gutter_width(const vg_codeeditor_t *editor) {
    if (!editor || !editor->show_fold_gutter)
        return 0.0f;
    float width =
        editor->line_height > 0.0f ? editor->line_height : CODEEDITOR_FOLD_GUTTER_MIN_WIDTH;
    return width < CODEEDITOR_FOLD_GUTTER_MIN_WIDTH ? CODEEDITOR_FOLD_GUTTER_MIN_WIDTH : width;
}

/// @brief Returns the effective line-number gutter width, honouring line_number_width_override when
/// set.
/// @param editor Editor whose line-number settings are inspected.
/// @return Override width in character cells, automatic width, or zero when disabled.
static float codeeditor_line_number_gutter_width(const vg_codeeditor_t *editor) {
    if (!editor || !editor->show_line_numbers)
        return 0.0f;
    if (editor->line_number_width_override > 0.0f)
        return editor->line_number_width_override * editor->char_width;
    return codeeditor_auto_line_number_gutter_width(editor);
}

/// @brief Returns a const pointer to the fold region whose start_line equals @p line, or NULL if
/// none.
/// @param editor Editor containing fold regions.
/// @param line Candidate fold-start line.
/// @return Borrowed matching region, or NULL.
static const struct vg_fold_region *codeeditor_fold_region_starting_at(
    const vg_codeeditor_t *editor, int line) {
    if (!editor)
        return NULL;
    for (int i = 0; i < editor->fold_region_count; i++) {
        if (editor->fold_regions[i].start_line == line)
            return &editor->fold_regions[i];
    }
    return NULL;
}

/// @brief Returns a mutable pointer to the fold region whose start_line equals @p line, or NULL if
/// none.
/// @param editor Editor containing fold regions.
/// @param line Candidate fold-start line.
/// @return Borrowed mutable matching region, or NULL.
static struct vg_fold_region *codeeditor_fold_region_starting_at_mut(vg_codeeditor_t *editor,
                                                                     int line) {
    if (!editor)
        return NULL;
    for (int i = 0; i < editor->fold_region_count; i++) {
        if (editor->fold_regions[i].start_line == line)
            return &editor->fold_regions[i];
    }
    return NULL;
}

/// @brief Returns true if @p line falls inside a currently folded region (i.e., it is not visible).
/// @param editor Editor containing fold regions.
/// @param line Source line to test.
/// @return `true` when a folded region hides @p line.
static bool codeeditor_line_is_hidden(const vg_codeeditor_t *editor, int line) {
    if (!editor)
        return false;
    for (int i = 0; i < editor->fold_region_count; i++) {
        const struct vg_fold_region *region = &editor->fold_regions[i];
        if (region->folded && line > region->start_line && line <= region->end_line)
            return true;
    }
    return false;
}

/// @brief Returns the nearest visible line at or above @p line, snapping hidden lines to their
/// fold's start_line.
/// @param editor Editor containing fold regions.
/// @param line Candidate line, clamped to the document.
/// @return Visible line or owning folded-region anchor.
static int codeeditor_visible_anchor_line(const vg_codeeditor_t *editor, int line) {
    if (!editor || editor->line_count <= 0)
        return 0;
    if (line < 0)
        line = 0;
    if (line >= editor->line_count)
        line = editor->line_count - 1;
    if (!codeeditor_line_is_hidden(editor, line))
        return line;

    int best_start = line;
    bool found = false;
    for (int i = 0; i < editor->fold_region_count; i++) {
        const struct vg_fold_region *region = &editor->fold_regions[i];
        if (region->folded && line > region->start_line && line <= region->end_line) {
            if (!found || region->start_line < best_start) {
                best_start = region->start_line;
                found = true;
            }
        }
    }
    return found ? best_start : line;
}

/// @brief Returns the index of the next visible (non-hidden) line after @p line, or line_count if
/// none.
/// @param editor Editor containing fold regions.
/// @param line Current source line.
/// @return Next visible line, `line_count` after the document, or `-1` for invalid input.
static int codeeditor_next_visible_line(const vg_codeeditor_t *editor, int line) {
    if (!editor || editor->line_count <= 0)
        return -1;

    int next = line + 1;
    bool changed = true;
    while (changed && next < editor->line_count) {
        changed = false;
        for (int i = 0; i < editor->fold_region_count; i++) {
            const struct vg_fold_region *region = &editor->fold_regions[i];
            if (!region->folded)
                continue;
            if (line == region->start_line && next <= region->end_line) {
                next = region->end_line + 1;
                changed = true;
            } else if (next > region->start_line && next <= region->end_line) {
                next = region->end_line + 1;
                changed = true;
            }
        }
    }
    return next < editor->line_count ? next : editor->line_count;
}

/// @brief Returns the index of the previous visible line before @p line, or -1 if none exists.
/// @param editor Editor containing fold regions.
/// @param line Current source line.
/// @return Previous visible line or `-1`.
VG_UNUSED static int codeeditor_prev_visible_line(const vg_codeeditor_t *editor, int line) {
    if (!editor || editor->line_count <= 0)
        return -1;
    int prev = line - 1;
    bool changed = true;
    while (changed && prev >= 0) {
        changed = false;
        for (int i = 0; i < editor->fold_region_count; i++) {
            const struct vg_fold_region *region = &editor->fold_regions[i];
            if (!region->folded)
                continue;
            if (prev > region->start_line && prev <= region->end_line) {
                prev = region->start_line;
                changed = true;
            }
        }
    }
    return prev >= 0 ? prev : -1;
}

/// @brief Recomputes editor->gutter_width as the sum of the line-number and fold-gutter widths.
/// @param editor Editor whose gutter metric is updated.
static void update_gutter_width(vg_codeeditor_t *editor) {
    if (!editor) {
        return;
    }
    editor->gutter_width =
        codeeditor_line_number_gutter_width(editor) + codeeditor_fold_gutter_width(editor);
}

/// @brief Returns the number of characters that fit per wrap row given @p content_width; 0 if
/// word_wrap is off.
/// @param editor Editor supplying wrap mode and fixed character width.
/// @param content_width Available text width in pixels.
/// @return Character cells per row, at least one when wrapping, or zero when disabled.
static int codeeditor_chars_per_row(const vg_codeeditor_t *editor, float content_width) {
    if (!editor || !editor->word_wrap || editor->char_width <= 0.0f)
        return 0;
    if (content_width <= 0.0f)
        return 1;
    int chars = (int)(content_width / editor->char_width);
    return chars > 0 ? chars : 1;
}

/// @brief Convert a layout content width to a stable cache key.
/// @details Layout row counts only change when the effective pixel width crosses
///          a character boundary.  Rounding the floating width to whole pixels
///          avoids invalidating the prefix-row cache for insignificant float
///          noise from parent layout calculations.
/// @param content_width Available text content width in pixels.
/// @return Rounded non-negative width key stored in the layout cache.
static float codeeditor_layout_width_cache_key(float content_width) {
    if (content_width <= 0.0f)
        return 0.0f;
    if (content_width >= (float)INT_MAX)
        return (float)INT_MAX;
    return (float)(int)(content_width + 0.5f);
}

/// @brief Decode a UTF-8 sequence whose byte span has already been validated.
/// @param text Pointer to the first byte of the sequence.
/// @param span Number of bytes in the sequence.
/// @return Unicode scalar value, or U+FFFD for invalid spans.
static uint32_t codeeditor_decode_span_codepoint(const char *text, size_t span) {
    const unsigned char *s = (const unsigned char *)text;
    if (!s || span == 0)
        return 0xFFFDu;
    if (span == 1)
        return s[0];
    if (span == 2)
        return ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    if (span == 3)
        return ((uint32_t)(s[0] & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) |
               (uint32_t)(s[2] & 0x3Fu);
    if (span == 4)
        return ((uint32_t)(s[0] & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
               ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
    return 0xFFFDu;
}

/// @brief Return a conservative terminal-style display width for one codepoint.
/// @details The editor still renders text with fixed pixel advances.  This
///          helper is used only for wrap-row estimation so wide CJK/fullwidth
///          codepoints and combining marks are not counted as raw UTF-8 bytes.
/// @param cp Unicode scalar value.
/// @return Display column width: 0 for combining marks, 2 for common wide ranges, otherwise 1.
static int codeeditor_codepoint_display_width(uint32_t cp) {
    if ((cp >= 0x0300u && cp <= 0x036Fu) || (cp >= 0x1AB0u && cp <= 0x1AFFu) ||
        (cp >= 0x1DC0u && cp <= 0x1DFFu) || (cp >= 0x20D0u && cp <= 0x20FFu) ||
        (cp >= 0xFE20u && cp <= 0xFE2Fu))
        return 0;
    if ((cp >= 0x1100u && cp <= 0x115Fu) || (cp >= 0x2329u && cp <= 0x232Au) ||
        (cp >= 0x2E80u && cp <= 0xA4CFu) || (cp >= 0xAC00u && cp <= 0xD7A3u) ||
        (cp >= 0xF900u && cp <= 0xFAFFu) || (cp >= 0xFE10u && cp <= 0xFE19u) ||
        (cp >= 0xFE30u && cp <= 0xFE6Fu) || (cp >= 0xFF00u && cp <= 0xFF60u) ||
        (cp >= 0xFFE0u && cp <= 0xFFE6u))
        return 2;
    return 1;
}

/// @brief Count approximate display columns for one editor line.
/// @param line Line whose UTF-8 text should be measured.
/// @return Display columns with tabs expanded to four-column stops.
static size_t codeeditor_line_display_columns(const vg_code_line_t *line) {
    if (!line || !line->text)
        return 0;
    size_t columns = 0;
    for (size_t pos = 0; pos < line->length;) {
        unsigned char ch = (unsigned char)line->text[pos];
        if (ch == '\t') {
            size_t add = 4u - (columns % 4u);
            if (add > SIZE_MAX - columns)
                return SIZE_MAX;
            columns += add;
            pos++;
            continue;
        }
        size_t span = codeeditor_utf8_span(line->text + pos);
        if (span == 0 || span > line->length - pos)
            span = 1;
        int width = codeeditor_codepoint_display_width(
            codeeditor_decode_span_codepoint(line->text + pos, span));
        if ((size_t)width > SIZE_MAX - columns)
            return SIZE_MAX;
        columns += (size_t)width;
        pos += span;
    }
    return columns;
}

/// @brief Returns the number of wrapped rows needed to display @p line given @p content_width
/// (minimum 1).
/// @param editor Editor supplying text and wrap metrics.
/// @param line Zero-based source line.
/// @param content_width Available text width in pixels.
/// @return Required wrapped rows, saturated at `INT_MAX`.
static int codeeditor_wrapped_rows_for_line(const vg_codeeditor_t *editor,
                                            int line,
                                            float content_width) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 1;
    int chars_per_row = codeeditor_chars_per_row(editor, content_width);
    if (chars_per_row <= 0)
        return 1;
    size_t columns = codeeditor_line_display_columns(&editor->lines[line]);
    if (columns == 0)
        return 1;
    size_t rows = (columns + (size_t)chars_per_row - 1) / (size_t)chars_per_row;
    return rows > (size_t)INT_MAX ? INT_MAX : (int)rows;
}

/// @brief Returns the visual row count for @p line (0 if hidden, 1 if no word-wrap, else wrapped
/// row count).
/// @param editor Editor supplying folds and wrap settings.
/// @param line Zero-based source line.
/// @param content_width Available text width in pixels.
/// @return Zero for hidden/invalid lines, otherwise the displayed row count.
static int codeeditor_visual_rows_for_line(const vg_codeeditor_t *editor,
                                           int line,
                                           float content_width) {
    if (!editor || line < 0 || line >= editor->line_count ||
        codeeditor_line_is_hidden(editor, line))
        return 0;
    if (!editor->word_wrap)
        return 1;
    return codeeditor_wrapped_rows_for_line(editor, line, content_width);
}

/// @brief Build or reuse prefix sums of visual rows for every source line.
/// @details The cache key includes layout generation, rounded content width, wrap mode, and line
/// count. Each prefix entry gives the absolute visual row at the corresponding source-line boundary.
/// @param editor Editor whose layout cache is populated.
/// @param content_width Available text width in pixels.
/// @return `true` when a current cache is available.
static bool codeeditor_layout_cache_ensure(vg_codeeditor_t *editor, float content_width) {
    if (!editor)
        return false;

    vg_codeeditor_t *mutable_editor = editor;
    float width_key = codeeditor_layout_width_cache_key(content_width);
    if (mutable_editor->layout_cache_valid &&
        mutable_editor->layout_cache_generation == mutable_editor->layout_generation &&
        mutable_editor->layout_cache_content_width == width_key &&
        mutable_editor->layout_cache_word_wrap == mutable_editor->word_wrap &&
        mutable_editor->layout_cache_line_count == mutable_editor->line_count)
        return true;

    if (mutable_editor->line_count == INT_MAX)
        return false;
    int needed = mutable_editor->line_count + 1;
    if (needed < 1)
        needed = 1;
    if (mutable_editor->layout_cache_capacity < needed) {
        if ((size_t)needed > SIZE_MAX / sizeof(int))
            return false;
        int *rows = realloc(mutable_editor->layout_cache_prefix_rows, (size_t)needed * sizeof(int));
        if (!rows)
            return false;
        mutable_editor->layout_cache_prefix_rows = rows;
        mutable_editor->layout_cache_capacity = needed;
    }

    int total_rows = 0;
    mutable_editor->layout_cache_prefix_rows[0] = 0;
    for (int i = 0; i < mutable_editor->line_count; i++) {
        int line_rows = codeeditor_visual_rows_for_line(mutable_editor, i, content_width);
        if (line_rows < 0)
            line_rows = 0;
        if (line_rows > INT_MAX - total_rows)
            total_rows = INT_MAX;
        else
            total_rows += line_rows;
        mutable_editor->layout_cache_prefix_rows[i + 1] = total_rows;
    }

    mutable_editor->layout_cache_valid = true;
    mutable_editor->layout_cache_generation = mutable_editor->layout_generation;
    mutable_editor->layout_cache_content_width = width_key;
    mutable_editor->layout_cache_word_wrap = mutable_editor->word_wrap;
    mutable_editor->layout_cache_line_count = mutable_editor->line_count;
    mutable_editor->layout_cache_total_visual_rows = total_rows;
    mutable_editor->layout_cache_total_height = (float)total_rows * mutable_editor->line_height;
    return true;
}

/// @brief Return a line's visual row count through the prefix cache.
/// @param editor Editor containing the line.
/// @param content_width Available text width in pixels.
/// @param line Zero-based source line.
/// @return Cached row count, a direct fallback calculation, or zero for invalid input.
static int codeeditor_cached_row_count_for_line(vg_codeeditor_t *editor,
                                                float content_width,
                                                int line) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    if (!codeeditor_layout_cache_ensure(editor, content_width))
        return codeeditor_visual_rows_for_line(editor, line, content_width);
    return editor->layout_cache_prefix_rows[line + 1] - editor->layout_cache_prefix_rows[line];
}

/// @brief Find the final source line contributing a visible row.
/// @param editor Editor whose fold/layout state is inspected.
/// @param content_width Available text width in pixels.
/// @return Last visible source-line index, or zero for an empty/invalid editor.
static int codeeditor_last_visible_line_from_cache(vg_codeeditor_t *editor, float content_width) {
    if (!editor || editor->line_count <= 0)
        return 0;
    if (!codeeditor_layout_cache_ensure(editor, content_width))
        return codeeditor_visible_anchor_line(editor, editor->line_count - 1);
    for (int line = editor->line_count - 1; line >= 0; line--) {
        if (editor->layout_cache_prefix_rows[line + 1] > editor->layout_cache_prefix_rows[line])
            return line;
    }
    return 0;
}

/// @brief Returns the total document height in pixels for the given @p content_width (sums all
/// visual rows).
/// @param editor Editor whose document is measured.
/// @param content_width Available text width in pixels.
/// @return Total visual-row height in pixels.
static float codeeditor_total_content_height_for_width(vg_codeeditor_t *editor,
                                                       float content_width) {
    if (!editor)
        return 0.0f;

    if (!editor->word_wrap && !codeeditor_has_hidden_lines(editor))
        return (float)editor->line_count * editor->line_height;

    if (codeeditor_layout_cache_ensure(editor, content_width))
        return editor->layout_cache_total_height;

    float total_rows = 0.0f;
    for (int i = 0; i < editor->line_count; i++) {
        codeeditor_perf_add(&editor->perf_stats.total_height_linear_scans, 1);
        total_rows += (float)codeeditor_visual_rows_for_line(editor, i, content_width);
    }
    return total_rows * editor->line_height;
}

/// @brief Computes the drawable content width without triggering document-height scans.
/// @param editor Editor supplying gutter and wrapping settings.
/// @param widget Widget bounds defining the viewport.
/// @return Non-negative text-content width in pixels.
static float codeeditor_content_draw_width(const vg_codeeditor_t *editor,
                                           const vg_widget_t *widget) {
    if (!editor || !widget)
        return 0.0f;

    float base_width = widget->width - editor->gutter_width;
    if (base_width < 0.0f)
        base_width = 0.0f;
    if (!editor->word_wrap)
        return base_width;

    float minimum_height = (float)editor->line_count * editor->line_height;
    float content_width =
        base_width - ((minimum_height > widget->height) ? CODEEDITOR_SCROLLBAR_WIDTH : 0.0f);
    return content_width > 0.0f ? content_width : 0.0f;
}

/// @brief Returns total document height using the resolved content draw width for the given widget
/// bounds.
/// @param editor Editor whose document is measured.
/// @param widget Widget bounds defining the viewport.
/// @return Total content height in pixels.
static float codeeditor_total_content_height(vg_codeeditor_t *editor, const vg_widget_t *widget) {
    return codeeditor_total_content_height_for_width(editor,
                                                     codeeditor_content_draw_width(editor, widget));
}

/// @brief Returns the total count of visual rows across all lines for @p content_width (used for
/// scroll math).
/// @param editor Editor whose document is measured.
/// @param content_width Available text width in pixels.
/// @return Total displayed row count.
static int codeeditor_total_visual_rows_for_width(vg_codeeditor_t *editor, float content_width) {
    if (!editor)
        return 0;

    if (!editor->word_wrap && !codeeditor_has_hidden_lines(editor))
        return editor->line_count;

    if (codeeditor_layout_cache_ensure(editor, content_width))
        return editor->layout_cache_total_visual_rows;

    int total_rows = 0;
    for (int i = 0; i < editor->line_count; i++) {
        codeeditor_perf_add(&editor->perf_stats.total_visual_row_linear_scans, 1);
        total_rows += codeeditor_visual_rows_for_line(editor, i, content_width);
    }
    return total_rows;
}

/// @brief Returns the maximum valid scroll_y value, with substantial empty
///        space below the last line so the user can scroll the document end up
///        to roughly the middle of the viewport (the scrollBeyondLastLine UX
///        used by VS Code, Sublime, Atom). Without this, the last few lines
///        are uncomfortable to read because they're pinned at the viewport's
///        bottom edge.
///
///        Padding is half the viewport, with a minimum of one line so single-
///        line viewports don't lose any scroll allowance.
/// @param editor Editor whose content height is measured.
/// @param widget Widget bounds defining the viewport.
/// @return Maximum non-negative vertical scroll offset.
static float codeeditor_max_scroll_y(vg_codeeditor_t *editor, const vg_widget_t *widget) {
    if (!editor || !widget)
        return 0.0f;
    float padding = widget->height * 0.5f;
    if (padding < editor->line_height)
        padding = editor->line_height;
    float max_scroll = codeeditor_total_content_height(editor, widget) - widget->height + padding;
    return max_scroll > 0.0f ? max_scroll : 0.0f;
}

/// @brief Visual column width of one line, expanding tabs to tab stops and
///        counting each UTF-8 codepoint as one column.
/// @param ln Source line to measure.
/// @param tab_w Tab-stop width; non-positive values use four.
/// @return Display-column width.
static int codeeditor_line_visual_cols(const vg_code_line_t *ln, int tab_w) {
    if (!ln || !ln->text)
        return 0;
    if (tab_w <= 0)
        tab_w = 4;
    int cols = 0;
    for (int k = 0; k < (int)ln->length; k++) {
        unsigned char c = (unsigned char)ln->text[k];
        if ((c & 0xC0) == 0x80)
            continue; // UTF-8 continuation byte
        if (c == '\t')
            cols += tab_w - (cols % tab_w);
        else
            cols += 1;
    }
    return cols;
}

/// @brief Widest line in the document, in visual columns. Cached and only
///        recomputed when the layout generation changes (i.e. after an edit).
/// @param editor Editor whose lines are measured.
/// @return Maximum visual column count.
static int codeeditor_longest_line_cols(vg_codeeditor_t *editor) {
    if (!editor)
        return 0;
    if (editor->longest_line_cols_generation == editor->layout_generation &&
        editor->layout_generation != 0)
        return editor->longest_line_cols_cache;
    int tab_w = editor->tab_width > 0 ? editor->tab_width : 4;
    int longest = 0;
    for (int i = 0; i < editor->line_count; i++) {
        int cols = codeeditor_line_visual_cols(&editor->lines[i], tab_w);
        if (cols > longest)
            longest = cols;
    }
    editor->longest_line_cols_cache = longest;
    editor->longest_line_cols_generation = editor->layout_generation;
    return longest;
}

/// @brief Maximum valid scroll_x: the widest line's pixel width (plus one column
///        of trailing pad) minus the visible text width. Zero in word-wrap mode.
/// @param editor Editor supplying content and character metrics.
/// @param widget Widget bounds defining the viewport.
/// @return Maximum non-negative horizontal scroll offset.
static float codeeditor_max_scroll_x(vg_codeeditor_t *editor, const vg_widget_t *widget) {
    if (!editor || !widget || editor->word_wrap || editor->char_width <= 0.0f)
        return 0.0f;
    float view_w = widget->width - editor->gutter_width - CODEEDITOR_SCROLLBAR_WIDTH;
    if (view_w < 0.0f)
        view_w = 0.0f;
    float content_w =
        (float)codeeditor_longest_line_cols(editor) * editor->char_width + editor->char_width;
    float max_scroll = content_w - view_w;
    return max_scroll > 0.0f ? max_scroll : 0.0f;
}

/// @brief Decomposes (line, col) into a wrap-row index within the line and a column within that
/// row.
/// @param editor Editor supplying wrap and line metrics.
/// @param content_width Available text width in pixels.
/// @param line Source line containing the position.
/// @param col Byte column within @p line.
/// @param[out] out_row_index Optional destination for the line-local wrap-row index.
/// @param[out] out_col_in_row Optional destination for the column within that row.
static void codeeditor_visual_offset_for_position(const vg_codeeditor_t *editor,
                                                  float content_width,
                                                  int line,
                                                  int col,
                                                  int *out_row_index,
                                                  int *out_col_in_row) {
    if (out_row_index)
        *out_row_index = 0;
    if (out_col_in_row)
        *out_col_in_row = 0;
    if (!editor || line < 0 || line >= editor->line_count)
        return;
    line = codeeditor_visible_anchor_line(editor, line);

    int chars_per_row = codeeditor_chars_per_row(editor, content_width);
    int row_index = 0;
    int col_in_row = col;
    if (chars_per_row > 0) {
        size_t len = editor->lines[line].length;
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

/// @brief Returns the absolute visual row index (across all lines) for document position (line,
/// col).
/// @param editor Editor supplying fold and wrap layout.
/// @param content_width Available text width in pixels.
/// @param line Source line of the position.
/// @param col Byte column of the position.
/// @return Absolute zero-based visual row.
static int codeeditor_visual_row_for_position(vg_codeeditor_t *editor,
                                              float content_width,
                                              int line,
                                              int col) {
    if (!editor)
        return 0;
    if (!editor->word_wrap && !codeeditor_has_hidden_lines(editor))
        return codeeditor_clamp_line_index(editor, line);
    if (line < 0)
        line = 0;
    if (line >= editor->line_count)
        line = editor->line_count - 1;
    line = codeeditor_visible_anchor_line(editor, line);

    if (codeeditor_layout_cache_ensure(editor, content_width)) {
        int visual_row = editor->layout_cache_prefix_rows[line];
        int wrapped_row = 0;
        codeeditor_visual_offset_for_position(editor, content_width, line, col, &wrapped_row, NULL);
        return visual_row + wrapped_row;
    }

    int visual_row = 0;
    for (int i = 0; i < line; i++) {
        codeeditor_perf_add(&editor->perf_stats.visual_row_linear_scans, 1);
        visual_row += codeeditor_visual_rows_for_line(editor, i, content_width);
    }

    int wrapped_row = 0;
    codeeditor_visual_offset_for_position(editor, content_width, line, col, &wrapped_row, NULL);
    visual_row += wrapped_row;
    return visual_row;
}

/// @brief Resolves an absolute @p visual_row index to the source line and wrap-row offset within
/// that line.
/// @param editor Editor supplying fold and wrap layout.
/// @param content_width Available text width in pixels.
/// @param visual_row Absolute visual-row index, clamped to available content.
/// @param[out] out_line Optional destination for the source line.
/// @param[out] out_row_in_line Optional destination for the wrap row within that line.
static void codeeditor_locate_visual_row(vg_codeeditor_t *editor,
                                         float content_width,
                                         int visual_row,
                                         int *out_line,
                                         int *out_row_in_line) {
    if (out_line)
        *out_line = 0;
    if (out_row_in_line)
        *out_row_in_line = 0;
    if (!editor || editor->line_count <= 0)
        return;

    if (visual_row < 0)
        visual_row = 0;

    if (!editor->word_wrap && !codeeditor_has_hidden_lines(editor)) {
        if (out_line)
            *out_line = codeeditor_clamp_line_index(editor, visual_row);
        if (out_row_in_line)
            *out_row_in_line = 0;
        return;
    }

    if (codeeditor_layout_cache_ensure(editor, content_width)) {
        int total_rows = editor->layout_cache_total_visual_rows;
        if (total_rows <= 0) {
            if (out_line)
                *out_line = 0;
            if (out_row_in_line)
                *out_row_in_line = 0;
            return;
        }
        if (visual_row >= total_rows)
            visual_row = total_rows - 1;

        int lo = 0;
        int hi = editor->line_count - 1;
        int found = codeeditor_last_visible_line_from_cache(editor, content_width);
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            int start = editor->layout_cache_prefix_rows[mid];
            int end = editor->layout_cache_prefix_rows[mid + 1];
            if (visual_row < start) {
                hi = mid - 1;
            } else if (visual_row >= end || end == start) {
                lo = mid + 1;
            } else {
                found = mid;
                break;
            }
        }
        if (out_line)
            *out_line = found;
        if (out_row_in_line)
            *out_row_in_line = visual_row - editor->layout_cache_prefix_rows[found];
        return;
    }

    int accumulated = 0;
    for (int line = 0; line < editor->line_count; line++) {
        codeeditor_perf_add(&editor->perf_stats.locate_visual_row_linear_scans, 1);
        int row_count = codeeditor_visual_rows_for_line(editor, line, content_width);
        if (row_count == 0)
            continue;
        if (visual_row < accumulated + row_count) {
            if (out_line)
                *out_line = line;
            if (out_row_in_line)
                *out_row_in_line = visual_row - accumulated;
            return;
        }
        accumulated += row_count;
    }

    if (out_line)
        *out_line = codeeditor_last_visible_line_from_cache(editor, content_width);
    if (out_row_in_line) {
        int last_rows = codeeditor_visual_rows_for_line(
            editor, codeeditor_last_visible_line_from_cache(editor, content_width), content_width);
        *out_row_in_line = last_rows > 0 ? last_rows - 1 : 0;
    }
}

/// @brief Clamps scroll_x to 0 in word-wrap mode and clamps scroll_y to [0, max_scroll_y].
/// @param editor Editor whose scroll offsets are constrained.
/// @param widget Widget bounds defining the available scroll range.
static void codeeditor_clamp_scroll(vg_codeeditor_t *editor, const vg_widget_t *widget);

/// @brief Enable or disable ligature shaping for code-editor text.
/// @details A changed setting invalidates painting and advances the base-widget revision.
/// @param editor Editor to configure.
/// @param enabled Whether supported font ligatures should be rendered.
void vg_codeeditor_set_ligatures_enabled(vg_codeeditor_t *editor, bool enabled) {
    if (!editor || editor->ligatures_enabled == enabled)
        return;
    editor->ligatures_enabled = enabled;
    editor->base.needs_paint = true;
    vg_widget_note_revision(&editor->base);
}

/// @brief Return whether ligature shaping is enabled for an editor.
/// @param editor Editor to inspect.
/// @return Stored setting, or `false` when @p editor is NULL.
bool vg_codeeditor_get_ligatures_enabled(const vg_codeeditor_t *editor) {
    return editor ? editor->ligatures_enabled : false;
}

/// @brief Advance wheel smooth-scroll easing by one frame.
/// @details Programmatic scrolls, caret reveals, and drags write offsets directly; easing only
/// runs while a wheel gesture is active and stops after both axes reach their targets.
/// @param editor Editor with an active smooth-scroll gesture.
/// @param delta_ms Elapsed frame time in milliseconds.
/// @return `true` while either axis remains in motion.
bool vg_codeeditor_smooth_tick(vg_codeeditor_t *editor, float delta_ms) {
    if (!editor || !editor->smooth_animating)
        return false;
    bool moving_x =
        vg_smooth_scroll_step(&editor->scroll_x, editor->smooth_target_x, delta_ms);
    bool moving_y =
        vg_smooth_scroll_step(&editor->scroll_y, editor->smooth_target_y, delta_ms);
    codeeditor_clamp_scroll(editor, &editor->base);
    editor->base.needs_paint = true;
    editor->smooth_animating = moving_x || moving_y;
    return editor->smooth_animating;
}

/// @brief Clamp horizontal and vertical offsets to the current viewport and document.
/// @details Horizontal scrolling is forced to zero under wrapping; otherwise both axes are bounded
/// by content-derived maximums.
/// @param editor Editor whose offsets are constrained.
/// @param widget Widget bounds defining the viewport.
static void codeeditor_clamp_scroll(vg_codeeditor_t *editor, const vg_widget_t *widget) {
    if (!editor || !widget)
        return;
    if (editor->word_wrap) {
        editor->scroll_x = 0.0f;
    } else {
        if (editor->scroll_x < 0.0f)
            editor->scroll_x = 0.0f;
        float max_scroll_x = codeeditor_max_scroll_x(editor, widget);
        if (editor->scroll_x > max_scroll_x)
            editor->scroll_x = max_scroll_x;
    }
    if (editor->scroll_y < 0.0f)
        editor->scroll_y = 0.0f;
    float max_scroll = codeeditor_max_scroll_y(editor, widget);
    if (editor->scroll_y > max_scroll)
        editor->scroll_y = max_scroll;
}

/// @brief Compute vertical scrollbar thumb geometry in widget-local coordinates.
/// @param editor Editor whose content and scroll offset determine the thumb.
/// @param widget Widget bounds defining the track.
/// @param[out] out_track_x Optional destination for the track's left edge.
/// @param[out] out_thumb_y Optional destination for the thumb's top edge.
/// @param[out] out_thumb_height Optional destination for the thumb height.
/// @param[out] out_max_scroll Optional destination for the document scroll range.
/// @param[out] out_thumb_travel Optional destination for the thumb travel range.
/// @return `true` when content exceeds the viewport and a scrollbar is required.
static bool codeeditor_get_scrollbar_metrics(vg_codeeditor_t *editor,
                                             const vg_widget_t *widget,
                                             float *out_track_x,
                                             float *out_thumb_y,
                                             float *out_thumb_height,
                                             float *out_max_scroll,
                                             float *out_thumb_travel) {
    if (out_track_x)
        *out_track_x = 0.0f;
    if (out_thumb_y)
        *out_thumb_y = 0.0f;
    if (out_thumb_height)
        *out_thumb_height = 0.0f;
    if (out_max_scroll)
        *out_max_scroll = 0.0f;
    if (out_thumb_travel)
        *out_thumb_travel = 0.0f;
    if (!editor || !widget || widget->height <= 0.0f)
        return false;

    float total_content_height = codeeditor_total_content_height(editor, widget);
    float visible_height = widget->height;
    if (total_content_height <= visible_height)
        return false;

    float thumb_ratio = visible_height / total_content_height;
    if (thumb_ratio > 1.0f)
        thumb_ratio = 1.0f;
    float thumb_height = visible_height * thumb_ratio;
    if (thumb_height < 20.0f)
        thumb_height = 20.0f;
    if (thumb_height > visible_height)
        thumb_height = visible_height;

    float max_scroll = total_content_height - visible_height;
    float thumb_travel = visible_height - thumb_height;
    float scroll_ratio = max_scroll > 0.0f ? editor->scroll_y / max_scroll : 0.0f;
    if (scroll_ratio < 0.0f)
        scroll_ratio = 0.0f;
    if (scroll_ratio > 1.0f)
        scroll_ratio = 1.0f;

    if (out_track_x)
        *out_track_x = widget->width - CODEEDITOR_SCROLLBAR_WIDTH;
    if (out_thumb_y)
        *out_thumb_y = scroll_ratio * thumb_travel;
    if (out_thumb_height)
        *out_thumb_height = thumb_height;
    if (out_max_scroll)
        *out_max_scroll = max_scroll;
    if (out_thumb_travel)
        *out_thumb_travel = thumb_travel;
    return true;
}

/// @brief Computes horizontal scrollbar geometry (all values widget-local).
/// @details Returns false when word wrap is on or the widest line fits without
///          horizontal scrolling. The thumb travels within the content area,
///          which begins at gutter_width, and the track sits along the bottom
///          edge (widget->height - CODEEDITOR_SCROLLBAR_WIDTH).
/// @param editor Editor whose longest line and horizontal offset determine the thumb.
/// @param widget Widget bounds defining the track.
/// @param[out] out_track_y Optional destination for the track's top edge.
/// @param[out] out_thumb_x Optional destination for the thumb's left edge.
/// @param[out] out_thumb_width Optional destination for the thumb width.
/// @param[out] out_max_scroll Optional destination for the content scroll range.
/// @param[out] out_thumb_travel Optional destination for the thumb travel range.
/// @return `true` when unwrapped content exceeds the viewport width.
static bool codeeditor_get_hscrollbar_metrics(vg_codeeditor_t *editor,
                                              const vg_widget_t *widget,
                                              float *out_track_y,
                                              float *out_thumb_x,
                                              float *out_thumb_width,
                                              float *out_max_scroll,
                                              float *out_thumb_travel) {
    if (out_track_y)
        *out_track_y = 0.0f;
    if (out_thumb_x)
        *out_thumb_x = 0.0f;
    if (out_thumb_width)
        *out_thumb_width = 0.0f;
    if (out_max_scroll)
        *out_max_scroll = 0.0f;
    if (out_thumb_travel)
        *out_thumb_travel = 0.0f;
    if (!editor || !widget || editor->word_wrap || editor->char_width <= 0.0f)
        return false;
    if (widget->width <= 0.0f || widget->height <= 0.0f)
        return false;

    float view_w = widget->width - editor->gutter_width - CODEEDITOR_SCROLLBAR_WIDTH;
    if (view_w <= 0.0f)
        return false;
    float content_w =
        (float)codeeditor_longest_line_cols(editor) * editor->char_width + editor->char_width;
    if (content_w <= view_w)
        return false;

    float thumb_ratio = view_w / content_w;
    if (thumb_ratio > 1.0f)
        thumb_ratio = 1.0f;
    float thumb_width = view_w * thumb_ratio;
    if (thumb_width < 20.0f)
        thumb_width = 20.0f;
    if (thumb_width > view_w)
        thumb_width = view_w;

    float max_scroll = content_w - view_w;
    float thumb_travel = view_w - thumb_width;
    float scroll_ratio = max_scroll > 0.0f ? editor->scroll_x / max_scroll : 0.0f;
    if (scroll_ratio < 0.0f)
        scroll_ratio = 0.0f;
    if (scroll_ratio > 1.0f)
        scroll_ratio = 1.0f;

    if (out_track_y)
        *out_track_y = widget->height - CODEEDITOR_SCROLLBAR_WIDTH;
    if (out_thumb_x)
        *out_thumb_x = editor->gutter_width + scroll_ratio * thumb_travel;
    if (out_thumb_width)
        *out_thumb_width = thumb_width;
    if (out_max_scroll)
        *out_max_scroll = max_scroll;
    if (out_thumb_travel)
        *out_thumb_travel = thumb_travel;
    return true;
}

/// @brief Converts a local-space click point (local_x, local_y) to a document (line, col) position.
/// @param editor Editor supplying scroll, wrap, fold, and character metrics.
/// @param widget Widget bounds defining the viewport.
/// @param local_x Pointer x coordinate relative to the widget.
/// @param local_y Pointer y coordinate relative to the widget.
/// @param[out] out_line Optional destination for the resolved source line.
/// @param[out] out_col Optional destination for the resolved byte column.
static void codeeditor_local_point_to_position(vg_codeeditor_t *editor,
                                               const vg_widget_t *widget,
                                               float local_x,
                                               float local_y,
                                               int *out_line,
                                               int *out_col) {
    if (out_line)
        *out_line = 0;
    if (out_col)
        *out_col = 0;
    if (!editor || !widget || editor->line_count <= 0)
        return;

    if (editor->word_wrap) {
        float content_width = codeeditor_content_draw_width(editor, widget);
        int chars_per_row = codeeditor_chars_per_row(editor, content_width);
        float doc_y = local_y + editor->scroll_y;
        int visual_row = editor->line_height > 0.0f ? (int)(doc_y / editor->line_height) : 0;
        int line = 0;
        int row_in_line = 0;
        codeeditor_locate_visual_row(editor, content_width, visual_row, &line, &row_in_line);

        float content_local_x = local_x - editor->gutter_width;
        if (content_local_x < 0.0f)
            content_local_x = 0.0f;
        int col_in_row =
            editor->char_width > 0.0f ? (int)(content_local_x / editor->char_width + 0.5f) : 0;
        if (col_in_row < 0)
            col_in_row = 0;
        int col = row_in_line * chars_per_row + col_in_row;
        col = codeeditor_char_col_to_byte_col(editor, line, col);
        if (out_line)
            *out_line = line;
        if (out_col)
            *out_col = col;
        return;
    }

    float content_local_x = local_x - editor->gutter_width + editor->scroll_x;
    float doc_y = local_y + editor->scroll_y;
    int visual_row = editor->line_height > 0.0f ? (int)(doc_y / editor->line_height) : 0;
    int line = 0;
    codeeditor_locate_visual_row(
        editor, codeeditor_content_draw_width(editor, widget), visual_row, &line, NULL);

    int col = editor->char_width > 0.0f ? (int)(content_local_x / editor->char_width + 0.5f) : 0;
    if (col < 0)
        col = 0;
    col = codeeditor_char_col_to_byte_col(editor, line, col);
    if (out_line)
        *out_line = line;
    if (out_col)
        *out_col = col;
}

/// @brief Moves the cursor up or down by @p visual_rows visual rows, preserving the column position
/// within a wrap row.
/// @param editor Editor whose primary cursor is moved.
/// @param widget Widget bounds defining wrap width.
/// @param visual_rows Signed visual-row displacement.
static void codeeditor_move_cursor_vertical(vg_codeeditor_t *editor,
                                            const vg_widget_t *widget,
                                            int visual_rows) {
    if (!editor || !widget || visual_rows == 0 || editor->line_count <= 0)
        return;

    float content_width = codeeditor_content_draw_width(editor, widget);
    int row_index = 0;
    int col_in_row = 0;
    codeeditor_visual_offset_for_position(
        editor, content_width, editor->cursor_line, editor->cursor_col, &row_index, &col_in_row);

    int visual_row = codeeditor_visual_row_for_position(
        editor, content_width, editor->cursor_line, editor->cursor_col);
    int target_visual_row = visual_row + visual_rows;
    int total_visual_rows = codeeditor_total_visual_rows_for_width(editor, content_width);
    if (total_visual_rows <= 0)
        return;
    if (target_visual_row < 0)
        target_visual_row = 0;
    if (target_visual_row >= total_visual_rows)
        target_visual_row = total_visual_rows - 1;

    int target_line = 0;
    int target_row_in_line = 0;
    codeeditor_locate_visual_row(
        editor, content_width, target_visual_row, &target_line, &target_row_in_line);

    int chars_per_row = codeeditor_chars_per_row(editor, content_width);
    int target_col = target_row_in_line * chars_per_row + col_in_row;
    int target_line_len = codeeditor_line_length_i32(editor, target_line);
    if (target_col > target_line_len)
        target_col = target_line_len;

    editor->cursor_line = target_line;
    editor->cursor_col = target_col;
}

/// @brief Adjusts scroll_y so the cursor's visual row is within the visible viewport.
/// @param editor Editor whose viewport should reveal its primary cursor.
static void ensure_cursor_visible(vg_codeeditor_t *editor) {
    if (!editor)
        return;

    float visible_height = editor->base.height;
    float content_width = codeeditor_content_draw_width(editor, &editor->base);
    int cursor_visual_row = codeeditor_visual_row_for_position(
        editor, content_width, editor->cursor_line, editor->cursor_col);
    float cursor_y = (float)cursor_visual_row * editor->line_height;

    // Scroll up if cursor is above visible area
    if (cursor_y < editor->scroll_y) {
        editor->scroll_y = cursor_y;
    }
    // Scroll down if cursor is below visible area
    else if (cursor_y + editor->line_height > editor->scroll_y + visible_height) {
        editor->scroll_y = cursor_y + editor->line_height - visible_height;
    }

    // Clamp scroll_y to valid range
    if (editor->scroll_y < 0)
        editor->scroll_y = 0;

    float max_scroll = codeeditor_max_scroll_y(editor, &editor->base);
    if (editor->scroll_y > max_scroll)
        editor->scroll_y = max_scroll;
}

/// @brief Recalculate gutter width, clamp cursors to visible lines, clamp scroll, and
///        mark the widget for re-layout and repaint.
///
/// @details Call this after any structural change (fold toggle, text replacement,
///          font change) that may affect layout metrics.
///
/// @param editor The code editor to refresh.
void vg_codeeditor_refresh_layout_state(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_bump_layout_generation(editor);
    codeeditor_refresh_fold_summary(editor);
    update_gutter_width(editor);
    codeeditor_adjust_hidden_cursors(editor);
    codeeditor_clamp_scroll(editor, &editor->base);
    editor->base.needs_layout = true;
    editor->base.needs_paint = true;
}
