//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file vg_codeeditor_editing.inc
/// @brief CodeEditor text mutation, cursor movement, selection, line-editing,
/// and indentation helpers.
///
/// @details This include fragment is consumed only by vg_codeeditor.c and keeps
/// editing operations separate from persistence, paint, input-dispatch, and
/// public API wrappers.
///
// File: src/lib/gui/src/widgets/vg_codeeditor_editing.inc
// Purpose: Selection normalization, cursor targeting, multi-cursor edit target application, and
// history recording glue.
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

//=============================================================================
// Selection Helper Functions
//=============================================================================

/// @brief Reads editor->selection and writes a canonically ordered start/end into the four
/// out-params.
/// @param editor Editor containing the primary selection.
/// @param[out] start_line Receives the earlier endpoint line.
/// @param[out] start_col Receives the earlier endpoint byte column.
/// @param[out] end_line Receives the later endpoint line.
/// @param[out] end_col Receives the later endpoint byte column.
static void normalize_selection(
    vg_codeeditor_t *editor, int *start_line, int *start_col, int *end_line, int *end_col) {
    *start_line = editor->selection.start_line;
    *start_col = editor->selection.start_col;
    *end_line = editor->selection.end_line;
    *end_col = editor->selection.end_col;

    // Normalize: start should be before end
    if (*start_line > *end_line || (*start_line == *end_line && *start_col > *end_col)) {
        int tmp = *start_line;
        *start_line = *end_line;
        *end_line = tmp;
        tmp = *start_col;
        *start_col = *end_col;
        *end_col = tmp;
    }
}

/// @brief Swaps the start/end pair in-place so that start always precedes end in document order.
/// @param[in,out] start_line First endpoint line.
/// @param[in,out] start_col First endpoint byte column.
/// @param[in,out] end_line Second endpoint line.
/// @param[in,out] end_col Second endpoint byte column.
static void normalize_selection_range(int *start_line,
                                      int *start_col,
                                      int *end_line,
                                      int *end_col) {
    if (*start_line > *end_line || (*start_line == *end_line && *start_col > *end_col)) {
        int tmp = *start_line;
        *start_line = *end_line;
        *end_line = tmp;
        tmp = *start_col;
        *start_col = *end_col;
        *end_col = tmp;
    }
}

/// @brief Extracts and returns a heap-allocated copy of the text in [start_line:start_col,
/// end_line:end_col).
/// @param editor Editor containing the requested text.
/// @param start_line First range line.
/// @param start_col First range byte column.
/// @param end_line Final range line.
/// @param end_col Exclusive final range byte column.
/// @param[out] out_len Optional destination for the copied byte count, excluding the terminator.
/// @return Caller-owned newline-joined text, or NULL on allocation failure.
static char *copy_text_range_len(vg_codeeditor_t *editor,
                                 int start_line,
                                 int start_col,
                                 int end_line,
                                 int end_col,
                                 size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!editor)
        return NULL;

    normalize_selection_range(&start_line, &start_col, &end_line, &end_col);

    size_t total_len = 0;
    for (int line = start_line; line <= end_line; line++) {
        int col_start = (line == start_line) ? start_col : 0;
        int col_end = (line == end_line) ? end_col : (int)editor->lines[line].length;
        total_len += (size_t)(col_end - col_start);
        if (line < end_line)
            total_len++;
    }

    char *result = malloc(total_len + 1);
    if (!result)
        return NULL;

    char *ptr = result;
    for (int line = start_line; line <= end_line; line++) {
        int col_start = (line == start_line) ? start_col : 0;
        int col_end = (line == end_line) ? end_col : (int)editor->lines[line].length;
        size_t len = (size_t)(col_end - col_start);
        if (len > 0) {
            memcpy(ptr, editor->lines[line].text + col_start, len);
            ptr += len;
        }
        if (line < end_line)
            *ptr++ = '\n';
    }
    *ptr = '\0';
    if (out_len)
        *out_len = total_len;
    return result;
}

/// @brief Extracts and returns a heap-allocated copy of the text in [start_line:start_col,
/// end_line:end_col).
/// @param editor Editor containing the requested text.
/// @param start_line First range line.
/// @param start_col First range byte column.
/// @param end_line Final range line.
/// @param end_col Exclusive final range byte column.
/// @return Caller-owned newline-joined text, or NULL on allocation failure.
static char *copy_text_range(
    vg_codeeditor_t *editor, int start_line, int start_col, int end_line, int end_col) {
    return copy_text_range_len(editor, start_line, start_col, end_line, end_col, NULL);
}

/// @brief Materialize the editor's native line array as a newline-joined byte string.
///
/// @details This helper is intentionally separate from vg_codeeditor_get_text so
///          internal mirror synchronization does not affect public performance
///          counters. Embedded NUL bytes stored in individual line buffers are
///          copied verbatim; @p out_len receives the byte count excluding the
///          terminating NUL byte.
///
/// @param editor Editor whose native line array should be joined.
/// @param out_len Optional output for the byte length excluding the terminator.
/// @return Caller-owned NUL-terminated buffer, or NULL on allocation failure.
static char *codeeditor_materialize_lines(const vg_codeeditor_t *editor, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!editor)
        return NULL;

    size_t total = 1;
    for (int i = 0; i < editor->line_count; i++) {
        size_t add = editor->lines[i].length;
        if (i < editor->line_count - 1) {
            if (add == SIZE_MAX)
                return NULL;
            add++;
        }
        if (add > SIZE_MAX - total)
            return NULL;
        total += add;
    }

    char *result = malloc(total);
    if (!result)
        return NULL;

    char *p = result;
    for (int i = 0; i < editor->line_count; i++) {
        memcpy(p, editor->lines[i].text, editor->lines[i].length);
        p += editor->lines[i].length;
        if (i < editor->line_count - 1)
            *p++ = '\n';
    }
    *p = '\0';
    if (out_len)
        *out_len = total > 0 ? total - 1 : 0;
    return result;
}

/// @brief Drop the optional shared text-buffer mirror after native line mutations.
/// @details The editor's line array is the authoritative storage for painting,
///          cursor movement, undo/redo, and full-text fallback queries.  Rebuilding
///          the mirror after every keystroke is O(document size); invalidating it
///          keeps edits proportional to the changed range while preserving correct
///          public text retrieval through the existing line materialization path.
/// @param editor Editor whose optional document_buffer mirror should be discarded.
static void codeeditor_discard_document_buffer_mirror(vg_codeeditor_t *editor) {
    if (!editor || !editor->document_buffer)
        return;
    zanna_text_buffer_free((zanna_text_buffer_t *)editor->document_buffer);
    editor->document_buffer = NULL;
}

/// @brief Clears the has_selection flag on all extra (non-primary) cursors.
/// @param editor Editor whose secondary cursor selections are cleared.
static void clear_extra_cursor_selections(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    for (int i = 0; i < editor->extra_cursor_count; i++)
        editor->extra_cursors[i].has_selection = false;
}

/// @brief Clamps @p *line to [0, line_count-1] without touching the column (used for selection
/// anchors).
/// @param editor Editor defining the valid line range.
/// @param[in,out] line Candidate line index updated in place.
static void clamp_editor_line(vg_codeeditor_t *editor, int *line) {
    if (!editor || !line || editor->line_count <= 0)
        return;
    if (*line < 0)
        *line = 0;
    if (*line >= editor->line_count)
        *line = editor->line_count - 1;
}

/// @brief Clamps @p *col to [0, lines[line].length] for an already-validated @p line index.
/// @param editor Editor containing the source line.
/// @param line Valid zero-based line index.
/// @param[in,out] col Candidate byte column updated in place.
static void clamp_editor_col(vg_codeeditor_t *editor, int line, int *col) {
    if (!editor || !col || editor->line_count <= 0)
        return;
    if (line < 0 || line >= editor->line_count)
        return;
    if (*col < 0)
        *col = 0;
    if (*col > (int)editor->lines[line].length)
        *col = (int)editor->lines[line].length;
}

/// @brief Clamps both line and col atomically; note that clamping the line may implicitly constrain
/// the col.
/// @param editor Editor defining the valid document range.
/// @param[in,out] line Candidate line index.
/// @param[in,out] col Candidate byte column.
static void clamp_editor_position(vg_codeeditor_t *editor, int *line, int *col) {
    if (!editor || !line || !col || editor->line_count <= 0)
        return;
    clamp_editor_line(editor, line);
    clamp_editor_col(editor, *line, col);
}

/// @brief Add a secondary cursor unless one already exists at the same position.
/// @param editor Editor whose cursor array may grow.
/// @param line Candidate source line, clamped to the document.
/// @param col Candidate byte column, clamped to the line.
static void codeeditor_add_extra_cursor_at(vg_codeeditor_t *editor, int line, int col) {
    if (!editor || editor->line_count <= 0)
        return;
    clamp_editor_position(editor, &line, &col);
    if (editor->cursor_line == line && editor->cursor_col == col)
        return;
    for (int i = 0; i < editor->extra_cursor_count; i++) {
        if (editor->extra_cursors[i].line == line && editor->extra_cursors[i].col == col)
            return;
    }
    if (editor->extra_cursor_count >= editor->extra_cursor_cap) {
        if (editor->extra_cursor_cap > INT_MAX / 2)
            return;
        int new_cap = editor->extra_cursor_cap ? editor->extra_cursor_cap * 2 : 4;
        void *p = realloc(editor->extra_cursors, (size_t)new_cap * sizeof(*editor->extra_cursors));
        if (!p)
            return;
        editor->extra_cursors = p;
        editor->extra_cursor_cap = new_cap;
    }
    struct vg_extra_cursor *cursor = &editor->extra_cursors[editor->extra_cursor_count++];
    cursor->line = line;
    cursor->col = col;
    memset(&cursor->selection, 0, sizeof(cursor->selection));
    cursor->has_selection = false;
}

/// @brief Clamps (line, col) to valid bounds and then snaps the line to its fold's visible anchor.
/// @param editor Editor defining document and fold bounds.
/// @param[in,out] line Candidate source line.
/// @param[in,out] col Candidate byte column.
static void codeeditor_clamp_cursor_to_visible(vg_codeeditor_t *editor, int *line, int *col) {
    if (!editor || !line || !col || editor->line_count <= 0)
        return;
    clamp_editor_position(editor, line, col);
    *line = codeeditor_visible_anchor_line(editor, *line);
    clamp_editor_col(editor, *line, col);
}

/// @brief Snaps all cursors (primary and extra) off hidden lines after a fold toggle or line
/// deletion.
/// @param editor Editor whose cursors are repaired.
static void codeeditor_adjust_hidden_cursors(vg_codeeditor_t *editor) {
    if (!editor || editor->line_count <= 0)
        return;

    codeeditor_clamp_cursor_to_visible(editor, &editor->cursor_line, &editor->cursor_col);
    for (int i = 0; i < editor->extra_cursor_count; i++) {
        codeeditor_clamp_cursor_to_visible(
            editor, &editor->extra_cursors[i].line, &editor->extra_cursors[i].col);
    }
}

/// @brief Returns -1, 0, or 1 for lhs < rhs, lhs == rhs, or lhs > rhs in document order.
/// @param lhs_line Left endpoint line.
/// @param lhs_col Left endpoint byte column.
/// @param rhs_line Right endpoint line.
/// @param rhs_col Right endpoint byte column.
/// @return Negative, zero, or positive according to document order.
static int compare_positions(int lhs_line, int lhs_col, int rhs_line, int rhs_col) {
    if (lhs_line != rhs_line)
        return (lhs_line < rhs_line) ? -1 : 1;
    if (lhs_col != rhs_col)
        return (lhs_col < rhs_col) ? -1 : 1;
    return 0;
}

/// @brief One normalized cursor-specific replacement range.
typedef struct vg_edit_target {
    /// @brief Zero for the primary cursor or one plus a secondary-cursor index.
    int cursor_id;
    /// @brief Cursor line captured before applying the edit.
    int cursor_line_before;
    /// @brief Cursor byte column captured before applying the edit.
    int cursor_col_before;
    /// @brief First line of the replacement range.
    int start_line;
    /// @brief First byte column of the replacement range.
    int start_col;
    /// @brief Final line of the replacement range.
    int end_line;
    /// @brief Exclusive final byte column of the replacement range.
    int end_col;
} vg_edit_target_t;

/// @brief qsort comparator that orders edit targets in descending document position (last target
/// first).
/// @param lhs Address of the first edit target.
/// @param rhs Address of the second edit target.
/// @return A negative, zero, or positive value in reverse document order.
static int edit_target_compare_desc(const void *lhs, const void *rhs) {
    const vg_edit_target_t *a = (const vg_edit_target_t *)lhs;
    const vg_edit_target_t *b = (const vg_edit_target_t *)rhs;
    int cmp = compare_positions(b->start_line, b->start_col, a->start_line, a->start_col);
    if (cmp != 0)
        return cmp;
    cmp = compare_positions(b->end_line, b->end_col, a->end_line, a->end_col);
    if (cmp != 0)
        return cmp;
    return a->cursor_id - b->cursor_id;
}

/// @brief qsort comparator that orders edit targets in ascending document position (first target
/// first).
/// @param lhs Address of the first edit target.
/// @param rhs Address of the second edit target.
/// @return A negative, zero, or positive value in document order.
static int edit_target_compare_asc(const void *lhs, const void *rhs) {
    const vg_edit_target_t *a = (const vg_edit_target_t *)lhs;
    const vg_edit_target_t *b = (const vg_edit_target_t *)rhs;
    int cmp = compare_positions(a->start_line, a->start_col, b->start_line, b->start_col);
    if (cmp != 0)
        return cmp;
    cmp = compare_positions(a->end_line, a->end_col, b->end_line, b->end_col);
    if (cmp != 0)
        return cmp;
    return a->cursor_id - b->cursor_id;
}

/// @brief Updates the cursor at @p cursor_id (0 = primary, 1+ = extra) to (line, col).
/// @param editor Editor containing the cursor.
/// @param cursor_id Zero for primary or one plus the secondary-cursor index.
/// @param line New source line.
/// @param col New byte column.
static void set_cursor_position_by_id(vg_codeeditor_t *editor, int cursor_id, int line, int col) {
    if (cursor_id == 0) {
        editor->cursor_line = line;
        editor->cursor_col = col;
        return;
    }
    int extra_idx = cursor_id - 1;
    if (extra_idx < 0 || extra_idx >= editor->extra_cursor_count)
        return;
    editor->extra_cursors[extra_idx].line = line;
    editor->extra_cursors[extra_idx].col = col;
}

/// @brief Clears the selection flag for the cursor identified by @p cursor_id.
/// @param editor Editor containing the cursor.
/// @param cursor_id Zero for primary or one plus the secondary-cursor index.
static void clear_cursor_selection_by_id(vg_codeeditor_t *editor, int cursor_id) {
    if (cursor_id == 0) {
        editor->has_selection = false;
        return;
    }
    int extra_idx = cursor_id - 1;
    if (extra_idx < 0 || extra_idx >= editor->extra_cursor_count)
        return;
    editor->extra_cursors[extra_idx].has_selection = false;
}

/// @brief Appends a normalized, de-duplicated edit target to @p targets[]; no-op if an identical
/// range exists.
/// @param targets Caller-provided target array with room for one more entry.
/// @param[in,out] count Current target count incremented on append.
/// @param cursor_id Cursor owning the target.
/// @param cursor_line_before Pre-edit cursor line.
/// @param cursor_col_before Pre-edit cursor byte column.
/// @param start_line First replacement-range line.
/// @param start_col First replacement-range byte column.
/// @param end_line Final replacement-range line.
/// @param end_col Exclusive final replacement-range byte column.
static void add_edit_target(vg_edit_target_t *targets,
                            int *count,
                            int cursor_id,
                            int cursor_line_before,
                            int cursor_col_before,
                            int start_line,
                            int start_col,
                            int end_line,
                            int end_col) {
    normalize_selection_range(&start_line, &start_col, &end_line, &end_col);
    for (int i = 0; i < *count; i++) {
        if (targets[i].start_line == start_line && targets[i].start_col == start_col &&
            targets[i].end_line == end_line && targets[i].end_col == end_col)
            return;
    }

    targets[*count].cursor_id = cursor_id;
    targets[*count].cursor_line_before = cursor_line_before;
    targets[*count].cursor_col_before = cursor_col_before;
    targets[*count].start_line = start_line;
    targets[*count].start_col = start_col;
    targets[*count].end_line = end_line;
    targets[*count].end_col = end_col;
    (*count)++;
}

/// @brief Creates an edit operation and pushes it onto the undo history; no-op if history is NULL.
/// @details Eligible single-code-point primary inserts may coalesce into the preceding typing unit;
/// other edits are appended as distinct history operations.
/// @param editor Editor owning the history.
/// @param type Insert, delete, or replace operation type.
/// @param cursor_id Cursor that performed the edit.
/// @param start_line First edited line.
/// @param start_col First edited byte column.
/// @param end_line Final edited line.
/// @param end_col Final edited byte column.
/// @param old_text Replaced text copied into the operation, or NULL.
/// @param new_text Inserted text copied into the operation, or NULL.
/// @param cursor_line_before Cursor line before the edit.
/// @param cursor_col_before Cursor byte column before the edit.
/// @param cursor_line_after Cursor line after the edit.
/// @param cursor_col_after Cursor byte column after the edit.
static void record_edit_history(vg_codeeditor_t *editor,
                                vg_edit_op_type_t type,
                                int cursor_id,
                                int start_line,
                                int start_col,
                                int end_line,
                                int end_col,
                                const char *old_text,
                                const char *new_text,
                                int cursor_line_before,
                                int cursor_col_before,
                                int cursor_line_after,
                                int cursor_col_after) {
    if (!editor || !editor->history)
        return;
    vg_edit_op_t *op = create_edit_op(type,
                                      cursor_id,
                                      start_line,
                                      start_col,
                                      end_line,
                                      end_col,
                                      old_text,
                                      new_text,
                                      cursor_line_before,
                                      cursor_col_before,
                                      cursor_line_after,
                                      cursor_col_after);
    if (!op)
        return;
    // Merge single-character typing into the previous undo unit when it continues
    // an in-progress run; otherwise push a new op and (for a fresh coalescable
    // insert) anchor the next run.
    if (edit_history_try_coalesce_insert(editor->history, op, editor->typing_clock_ms)) {
        edit_op_destroy(op);
        return;
    }
    if (op->type == VG_EDIT_INSERT && op->cursor_id == 0 && !editor->history->is_grouping &&
        op->new_text && codeeditor_single_codepoint_len(op->new_text) != 0 &&
        op->new_text[0] != '\n') {
        editor->history->last_insert_time_ms = editor->typing_clock_ms;
        editor->history->coalesce_break = false;
    }
    edit_history_push(editor->history, op);
}

/// @brief Applies @p replacement_text at each edit target in descending order, recording undo ops,
/// then updates cursor positions.
/// @details Target ranges are clamped and copied before mutation. Multi-cursor operations share a
/// history group and force full synchronization, while a single target emits a forward delta.
/// Syntax and layout caches are invalidated according to the changed text.
/// @param editor Editor whose document and cursors are modified.
/// @param targets Edit targets ordered from later to earlier document positions.
/// @param target_count Number of readable targets.
/// @param replacement_text Text inserted at every target, or NULL/empty for deletion.
static void apply_edit_targets(vg_codeeditor_t *editor,
                               vg_edit_target_t *targets,
                               int target_count,
                               const char *replacement_text) {
    if (!editor || target_count <= 0)
        return;

    const int cursor_count = 1 + editor->extra_cursor_count;
    int *new_lines = malloc((size_t)cursor_count * sizeof(int));
    int *new_cols = malloc((size_t)cursor_count * sizeof(int));
    vg_edit_target_t *clamped_targets = calloc((size_t)target_count, sizeof(vg_edit_target_t));
    char **old_texts = calloc((size_t)target_count, sizeof(char *));
    bool *has_old_texts = calloc((size_t)target_count, sizeof(bool));
    bool history_group_open = false;
    bool changed = false;
    bool syntax_global_dirty = false;
    if (!new_lines || !new_cols || !clamped_targets || !old_texts || !has_old_texts) {
        free(new_lines);
        free(new_cols);
        free(clamped_targets);
        free(old_texts);
        free(has_old_texts);
        return;
    }

    new_lines[0] = editor->cursor_line;
    new_cols[0] = editor->cursor_col;
    for (int i = 0; i < editor->extra_cursor_count; i++) {
        new_lines[i + 1] = editor->extra_cursors[i].line;
        new_cols[i + 1] = editor->extra_cursors[i].col;
    }

    for (int i = 0; i < target_count; i++) {
        clamped_targets[i] = targets[i];
        clamp_editor_position(
            editor, &clamped_targets[i].start_line, &clamped_targets[i].start_col);
        clamp_editor_position(editor, &clamped_targets[i].end_line, &clamped_targets[i].end_col);
        normalize_selection_range(&clamped_targets[i].start_line,
                                  &clamped_targets[i].start_col,
                                  &clamped_targets[i].end_line,
                                  &clamped_targets[i].end_col);
        has_old_texts[i] = compare_positions(clamped_targets[i].start_line,
                                             clamped_targets[i].start_col,
                                             clamped_targets[i].end_line,
                                             clamped_targets[i].end_col) != 0;
        if (has_old_texts[i]) {
            old_texts[i] = copy_text_range(editor,
                                           clamped_targets[i].start_line,
                                           clamped_targets[i].start_col,
                                           clamped_targets[i].end_line,
                                           clamped_targets[i].end_col);
            if (!old_texts[i])
                goto cleanup;
        }
    }

    if (editor->history && target_count > 1) {
        edit_history_begin_group(editor->history);
        history_group_open = true;
    }

    const uint64_t pending_rev = codeeditor_pending_revision(editor);
    for (int i = 0; i < target_count; i++) {
        const vg_edit_target_t *target = &clamped_targets[i];
        char *old_text = old_texts[i];
        int history_end_line = target->end_line;
        int history_end_col = target->end_col;
        const bool has_old_text = has_old_texts[i];
        syntax_global_dirty = syntax_global_dirty ||
                              codeeditor_text_may_affect_multiline_syntax(old_text) ||
                              codeeditor_text_may_affect_multiline_syntax(replacement_text);

        if (has_old_text) {
            delete_text_range_internal(
                editor, target->start_line, target->start_col, target->end_line, target->end_col);
            changed = true;
        } else {
            editor->cursor_line = target->start_line;
            editor->cursor_col = target->start_col;
        }

        if (replacement_text && replacement_text[0] != '\0') {
            if (!insert_text_at_internal(
                    editor, target->start_line, target->start_col, replacement_text)) {
                break;
            }
            changed = true;
        }

        new_lines[target->cursor_id] = editor->cursor_line;
        new_cols[target->cursor_id] = editor->cursor_col;
        clear_cursor_selection_by_id(editor, target->cursor_id);
        history_end_line = editor->cursor_line;
        history_end_col = editor->cursor_col;

        if (replacement_text && replacement_text[0] != '\0') {
            vg_edit_op_type_t type = has_old_text ? VG_EDIT_REPLACE : VG_EDIT_INSERT;
            record_edit_history(editor,
                                type,
                                target->cursor_id,
                                target->start_line,
                                target->start_col,
                                history_end_line,
                                history_end_col,
                                old_text,
                                replacement_text,
                                target->cursor_line_before,
                                target->cursor_col_before,
                                editor->cursor_line,
                                editor->cursor_col);
        } else if (has_old_text) {
            record_edit_history(editor,
                                VG_EDIT_DELETE,
                                target->cursor_id,
                                target->start_line,
                                target->start_col,
                                target->end_line,
                                target->end_col,
                                old_text,
                                NULL,
                                target->cursor_line_before,
                                target->cursor_col_before,
                                target->start_line,
                                target->start_col);
        }

        // Incremental sync journal (plan 08): capture the forward delta for the
        // single-cursor hot path — the pre-edit range [start..end) was replaced by
        // the replacement text (empty for a pure delete). Multi-cursor batches
        // fall back to an overflow (full re-sync) after the loop to avoid any
        // delta-ordering hazard.
        if (target_count == 1 &&
            ((replacement_text && replacement_text[0] != '\0') || has_old_text)) {
            codeeditor_journal_push(
                editor,
                target->start_line,
                target->start_col,
                target->end_line,
                target->end_col,
                (replacement_text && replacement_text[0] != '\0') ? replacement_text : "",
                pending_rev);
        }
    }

cleanup:
    if (history_group_open)
        edit_history_end_group(editor->history);

    for (int i = 0; i < cursor_count; i++)
        set_cursor_position_by_id(editor, i, new_lines[i], new_cols[i]);

    free(new_lines);
    free(new_cols);
    for (int i = 0; i < target_count; i++)
        free(old_texts[i]);
    free(clamped_targets);
    free(old_texts);
    free(has_old_texts);
    if (!changed)
        return;
    codeeditor_discard_document_buffer_mirror(editor);
    codeeditor_bump_revision(editor);
    if (target_count > 1)
        codeeditor_journal_mark_overflow(editor); // multi-cursor batch: consumer full-syncs
    if (syntax_global_dirty)
        codeeditor_invalidate_all_highlights(editor);
    editor->modified = true;
    vg_codeeditor_refresh_layout_state(editor);
    ensure_cursor_visible(editor);
    editor->base.needs_paint = true;
}
