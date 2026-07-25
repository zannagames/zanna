//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file vg_codeeditor_history.inc
/// @brief CodeEditor undo and redo history snapshot management.
///
/// @details This include fragment is consumed only by vg_codeeditor.c and
/// isolates the history ring helpers that record, replay, clear, and clone
/// editor text and cursor state.
///
// File: src/lib/gui/src/widgets/vg_codeeditor_history.inc
// Purpose: Undo and redo history storage, edit operation allocation, grouping, and stack navigation
// helpers.
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
// Undo/Redo History Management
//=============================================================================

#define HISTORY_INITIAL_CAPACITY 64
#define HISTORY_MAX_OPERATIONS 4096

/// @brief Allocates and zero-initializes a new edit history with HISTORY_INITIAL_CAPACITY operation
/// slots.
/// @return Newly allocated empty history, or NULL on allocation failure.
static vg_edit_history_t *edit_history_create(void) {
    vg_edit_history_t *history = calloc(1, sizeof(vg_edit_history_t));
    if (!history)
        return NULL;

    history->operations = calloc(HISTORY_INITIAL_CAPACITY, sizeof(vg_edit_op_t *));
    if (!history->operations) {
        free(history);
        return NULL;
    }

    history->capacity = HISTORY_INITIAL_CAPACITY;
    history->count = 0;
    history->current_index = 0;
    history->next_group_id = 1;
    history->is_grouping = false;
    history->current_group = 0;
    history->coalesce_enabled = true;
    history->coalesce_break = false;
    history->last_insert_time_ms = 0;

    return history;
}

// Consecutive single-codepoint inserts within this many milliseconds merge into
// one undo unit. 800ms mirrors the "typing burst" threshold used by mainstream
// editors — long enough for fast typing, short enough that a deliberate pause
// starts a new undo step.
#define CODEEDITOR_UNDO_COALESCE_MS 800
// Upper bound on a single coalesced unit's byte length. Bounds the realloc/append
// cost and keeps one Ctrl+Z from reverting an unreasonably large run.
#define CODEEDITOR_UNDO_COALESCE_MAX_BYTES 64

/// @brief Return the byte length of the single UTF-8 codepoint at the start of
///        @p s, or 0 if @p s is empty/NULL or is not exactly one codepoint.
/// @details Used to restrict undo coalescing to genuine single-character typing
///          (paste of a multi-char string never coalesces).
/// @param s NUL-terminated candidate string.
/// @return Sequence byte length from one through four, or zero unless @p s contains exactly one
/// structurally valid UTF-8 sequence.
static size_t codeeditor_single_codepoint_len(const char *s) {
    if (!s || !s[0])
        return 0;
    unsigned char c = (unsigned char)s[0];
    size_t len;
    if (c < 0x80)
        len = 1;
    else if ((c & 0xE0) == 0xC0)
        len = 2;
    else if ((c & 0xF0) == 0xE0)
        len = 3;
    else if ((c & 0xF8) == 0xF0)
        len = 4;
    else
        return 0; // invalid leading byte
    // Must be exactly that many bytes with no trailing content.
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
    }
    return s[len] == '\0' ? len : 0;
}

/// @brief Force the next recorded insert to begin a fresh undo unit.
/// @details Call from operations that should not extend the current typing run
///          (undo, redo, paste). Position/time/type continuity handles most
///          break cases implicitly; this is the belt-and-suspenders hook.
/// @param history History whose current coalescing run is broken.
static void edit_history_break_coalescing(vg_edit_history_t *history) {
    if (history)
        history->coalesce_break = true;
}

/// @brief Attempt to merge a freshly built single-codepoint insert @p op into the
///        previous history operation instead of pushing it as a new undo unit.
/// @param history History ring.
/// @param op      Newly created (not yet pushed) INSERT op.
/// @param now_ms  Current editor typing-clock time.
/// @return true when merged (the caller must destroy @p op and not push it).
/// @details Merges only when every continuity rule holds: coalescing enabled and
///          not broken; both prev and new op are primary-cursor, ungrouped
///          single-line INSERTs; the new op continues exactly at the previous
///          op's end; within the time window; under the size cap; and not across
///          a whitespace→word boundary. Merging appends the new text to the
///          previous op and advances its end/after-cursor, so undo/redo replay
///          is unchanged.
static bool edit_history_try_coalesce_insert(vg_edit_history_t *history,
                                             const vg_edit_op_t *op,
                                             uint64_t now_ms) {
    if (!history || !op)
        return false;
    if (!history->coalesce_enabled || history->coalesce_break)
        return false;
    if (history->is_grouping)
        return false;
    if (history->current_index != history->count) // never across an undo boundary
        return false;
    if (history->count == 0)
        return false;
    if (op->type != VG_EDIT_INSERT || op->cursor_id != 0 || op->group_id != 0)
        return false;
    if (!op->new_text)
        return false;
    if (codeeditor_single_codepoint_len(op->new_text) == 0) // single char only (not paste)
        return false;
    if (op->new_text[0] == '\n')
        return false;

    vg_edit_op_t *prev = history->operations[history->count - 1];
    if (!prev || prev->type != VG_EDIT_INSERT || prev->cursor_id != 0 || prev->group_id != 0)
        return false;
    if (!prev->new_text)
        return false;
    if (strchr(prev->new_text, '\n') != NULL) // Enter is a natural break point
        return false;
    // Insertion must continue exactly where the previous op ended.
    if (op->start_line != prev->end_line || op->start_col != prev->end_col)
        return false;
    // Time window.
    if (now_ms > history->last_insert_time_ms &&
        now_ms - history->last_insert_time_ms > CODEEDITOR_UNDO_COALESCE_MS)
        return false;
    // Size cap.
    size_t prev_len = strlen(prev->new_text);
    if (prev_len >= CODEEDITOR_UNDO_COALESCE_MAX_BYTES)
        return false;
    // Word-boundary rule: break when transitioning from whitespace to non-space.
    char prev_last = prev->new_text[prev_len - 1];
    char cur_first = op->new_text[0];
    bool prev_ws = (prev_last == ' ' || prev_last == '\t');
    bool cur_ws = (cur_first == ' ' || cur_first == '\t');
    if (prev_ws && !cur_ws)
        return false;

    // Merge: append the new codepoint to the previous op.
    size_t add_len = strlen(op->new_text);
    char *merged = realloc(prev->new_text, prev_len + add_len + 1);
    if (!merged)
        return false;
    memcpy(merged + prev_len, op->new_text, add_len + 1);
    prev->new_text = merged;
    prev->end_line = op->end_line;
    prev->end_col = op->end_col;
    prev->cursor_line_after = op->cursor_line_after;
    prev->cursor_col_after = op->cursor_col_after;
    history->last_insert_time_ms = now_ms;
    return true;
}

/// @brief Frees the old_text and new_text strings owned by @p op, then frees the op itself.
/// @param op Edit operation to destroy; NULL is ignored.
static void edit_op_destroy(vg_edit_op_t *op) {
    if (!op)
        return;
    free(op->old_text);
    free(op->new_text);
    free(op);
}

/// @brief Destroys all operations in @p history and frees the history struct itself.
/// @param history History to destroy; NULL is ignored.
static void edit_history_destroy(vg_edit_history_t *history) {
    if (!history)
        return;

    for (size_t i = 0; i < history->count; i++) {
        edit_op_destroy(history->operations[i]);
    }
    free(history->operations);
    free(history);
}

/// @brief Destroys all operations in @p history and resets count and current_index to 0 without
/// freeing the struct.
/// @param history History to empty while retaining its operation array.
VG_UNUSED
static void edit_history_clear(vg_edit_history_t *history) {
    if (!history)
        return;

    for (size_t i = 0; i < history->count; i++) {
        edit_op_destroy(history->operations[i]);
        history->operations[i] = NULL;
    }
    history->count = 0;
    history->current_index = 0;
    history->is_grouping = false;
    history->current_group = 0;
}

/// @brief Destroy the oldest operation and compact the remaining history.
/// @details Used to enforce HISTORY_MAX_OPERATIONS while preserving the newest
///          edits. The current undo cursor is shifted with the compacted array.
/// @param history Non-empty history whose oldest operation is removed.
static void edit_history_evict_oldest(vg_edit_history_t *history) {
    if (!history || history->count == 0)
        return;

    edit_op_destroy(history->operations[0]);
    if (history->count > 1) {
        memmove(history->operations,
                history->operations + 1,
                (history->count - 1) * sizeof(vg_edit_op_t *));
    }
    history->count--;
    history->operations[history->count] = NULL;
    if (history->current_index > 0)
        history->current_index--;
}

/// @brief Discards any redo tail, grows the operation array if needed, and appends @p op; sets its
/// group_id if grouping.
/// @param history History that assumes ownership of @p op.
/// @param op Newly allocated operation; destroyed internally if growth fails.
static void edit_history_push(vg_edit_history_t *history, vg_edit_op_t *op) {
    if (!history || !op)
        return;

    // Discard any redo operations
    for (size_t i = history->current_index; i < history->count; i++) {
        edit_op_destroy(history->operations[i]);
        history->operations[i] = NULL;
    }
    history->count = history->current_index;

    while (history->count >= HISTORY_MAX_OPERATIONS)
        edit_history_evict_oldest(history);

    // Grow capacity if needed
    if (history->count >= history->capacity) {
        size_t new_capacity = history->capacity * 2;
        if (new_capacity > HISTORY_MAX_OPERATIONS)
            new_capacity = HISTORY_MAX_OPERATIONS;
        vg_edit_op_t **new_ops =
            realloc(history->operations, new_capacity * sizeof(vg_edit_op_t *));
        if (!new_ops) {
            edit_op_destroy(op);
            return;
        }
        history->operations = new_ops;
        history->capacity = new_capacity;
    }

    // Set group ID if grouping
    if (history->is_grouping) {
        op->group_id = history->current_group;
    }

    history->operations[history->count++] = op;
    history->current_index = history->count;
}

/// @brief Decrements current_index and returns the operation to be undone, or NULL if at the
/// beginning.
/// @param history History whose undo cursor moves backward.
/// @return Borrowed operation to reverse, or NULL when no undo is available.
static vg_edit_op_t *edit_history_pop_undo(vg_edit_history_t *history) {
    if (!history || history->current_index == 0)
        return NULL;
    history->current_index--;
    return history->operations[history->current_index];
}

/// @brief Returns the operation at current_index-1 without advancing the index, or NULL if empty.
/// @param history History to inspect.
/// @return Borrowed latest undo operation, or NULL.
static vg_edit_op_t *edit_history_peek_undo(vg_edit_history_t *history) {
    if (!history || history->current_index == 0)
        return NULL;
    return history->operations[history->current_index - 1];
}

/// @brief Returns the operation at current_index and advances it, enabling a redo step; NULL if at
/// end.
/// @param history History whose undo cursor moves forward.
/// @return Borrowed operation to replay, or NULL when no redo is available.
static vg_edit_op_t *edit_history_pop_redo(vg_edit_history_t *history) {
    if (!history || history->current_index >= history->count)
        return NULL;
    vg_edit_op_t *op = history->operations[history->current_index];
    history->current_index++;
    return op;
}

/// @brief Begins an edit group so that subsequent pushed operations share a group_id for atomic
/// undo.
/// @param history History entering grouped-recording mode.
VG_UNUSED
static void edit_history_begin_group(vg_edit_history_t *history) {
    if (!history)
        return;
    history->is_grouping = true;
    history->current_group = history->next_group_id++;
}

/// @brief Ends the current edit group, clearing is_grouping and current_group on @p history.
/// @param history History leaving grouped-recording mode.
VG_UNUSED
static void edit_history_end_group(vg_edit_history_t *history) {
    if (!history)
        return;
    history->is_grouping = false;
    history->current_group = 0;
}

/// @brief Rewrite the cursor-after position of the latest undo operation.
/// @details Used by paired-character insertion: the edit still owns the full
///          inserted range, but redo should leave the cursor between the pair.
/// @param history History whose latest ungrouped operation is updated.
/// @param line Desired post-redo cursor line.
/// @param col Desired post-redo cursor byte column.
static void edit_history_update_latest_cursor_after(vg_edit_history_t *history, int line, int col) {
    if (!history || history->current_index == 0 || history->current_index != history->count)
        return;
    vg_edit_op_t *op = history->operations[history->current_index - 1];
    if (!op || op->group_id != 0)
        return;
    op->cursor_line_after = line;
    op->cursor_col_after = col;
}

/// @brief Allocates a new vg_edit_op_t, strdup'ing old_text and new_text, with group_id initialised
/// to 0.
/// @param type Operation kind.
/// @param cursor_id Cursor that produced the operation.
/// @param start_line First edited line.
/// @param start_col First edited byte column.
/// @param end_line Final edited line.
/// @param end_col Final edited byte column.
/// @param old_text Replaced text copied into the operation, or NULL.
/// @param new_text Inserted text copied into the operation, or NULL.
/// @param cursor_line_before Cursor line before the operation.
/// @param cursor_col_before Cursor byte column before the operation.
/// @param cursor_line_after Cursor line after the operation.
/// @param cursor_col_after Cursor byte column after the operation.
/// @return Newly allocated owned operation, or NULL on allocation failure.
static vg_edit_op_t *create_edit_op(vg_edit_op_type_t type,
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
    vg_edit_op_t *op = calloc(1, sizeof(vg_edit_op_t));
    if (!op)
        return NULL;

    op->type = type;
    op->cursor_id = cursor_id;
    op->start_line = start_line;
    op->start_col = start_col;
    op->end_line = end_line;
    op->end_col = end_col;
    op->old_text = old_text ? vg_strdup(old_text) : NULL;
    op->new_text = new_text ? vg_strdup(new_text) : NULL;
    if ((old_text && !op->old_text) || (new_text && !op->new_text)) {
        edit_op_destroy(op);
        return NULL;
    }
    op->cursor_line_before = cursor_line_before;
    op->cursor_col_before = cursor_col_before;
    op->cursor_line_after = cursor_line_after;
    op->cursor_col_after = cursor_col_after;
    op->group_id = 0;

    return op;
}
