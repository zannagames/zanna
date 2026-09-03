//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file vg_codeeditor_api.inc
/// @brief Public CodeEditor API functions for text, cursor, selection, scroll,
/// syntax, undo/redo, clipboard, font, dirty state, and performance stats.
///
/// @details This include fragment is consumed only by vg_codeeditor.c and
/// contains the exported API surface that forwards into the split internal
/// editor subsystems.
///
// File: src/lib/gui/src/widgets/vg_codeeditor_api.inc
// Purpose: Public CodeEditor API functions for text, cursor, selection, scroll, syntax, undo/redo,
// clipboard, font, dirty state, and performance stats.
//
// Key invariants:
//   - Included only by src/lib/gui/src/widgets/vg_codeeditor.c.
//   - Fragment order preserves the original static helper visibility and
//     vtable wiring from the monolithic source file.
//   - Public APIs share the core UTF-8 display-column and edit-journal helpers.
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
// CodeEditor API
//=============================================================================

/// @brief Advance the cursor blink timer; call once per frame while the editor is visible.
///
/// @details Only advances the timer when the editor has focus; no-op otherwise.
///
/// @param editor The code editor to tick.
/// @param dt     Elapsed time in seconds since the last frame.
void vg_codeeditor_tick(vg_codeeditor_t *editor, float dt) {
    if (!editor)
        return;

    // Edge autoscroll during a selection drag must run before the focus gate:
    // the drag holds input capture and should keep scrolling even if the blink
    // focus flag is not set. No-op unless a drag is in progress.
    codeeditor_drag_autoscroll_tick(editor, &editor->base, dt);

    // Advance the deterministic typing clock that drives undo coalescing. Using
    // accumulated frame dt (not a wall clock) keeps the gui library free of
    // platform time calls and lets tests control elapsed time via tick(dt).
    if (dt > 0.0f) {
        editor->typing_clock_ms += (uint64_t)(dt * 1000.0f);
    }

    if (!(editor->base.state & VG_STATE_FOCUSED))
        return;

    editor->cursor_blink_time += dt;
    if (editor->cursor_blink_time >= CURSOR_BLINK_RATE) {
        editor->cursor_blink_time -= CURSOR_BLINK_RATE;
        editor->cursor_visible = !editor->cursor_visible;
        editor->base.needs_paint = true;
    }
}

/// @brief Replace all editor content with the given byte span.
///
/// @details Splits text on newlines to populate the line array.  The cursor is
///          moved to (0,0), all selections and extra cursors are cleared, scroll
///          is reset to 0, and the modified flag is cleared.  NULL or empty text
///          results in a single empty line. Embedded NUL bytes are preserved in
///          the line buffers and returned by vg_codeeditor_get_text.
///
/// @param editor The code editor to update.
/// @param text   UTF-8 source bytes; may be NULL to clear.
/// @param len    Number of bytes to read from @p text.
void vg_codeeditor_set_text_bytes(vg_codeeditor_t *editor, const char *text, size_t len) {
    if (!editor)
        return;

    vg_code_line_t *new_lines = NULL;
    int new_capacity = 0;
    int new_count = 0;

    if (!text || len == 0) {
        if (!ensure_line_array_capacity(&new_lines, &new_capacity, 1) ||
            !init_line_text(&new_lines[0], "", 0)) {
            free_line_array(new_lines, new_count);
            return;
        }
        new_count = 1;
    } else {
        const char *start = text;
        const char *limit = text + len;
        while (start <= limit) {
            const void *found = memchr(start, '\n', (size_t)(limit - start));
            const char *end = found ? (const char *)found : limit;
            size_t line_len = (size_t)(end - start);

            if (!ensure_line_array_capacity(&new_lines, &new_capacity, new_count + 1) ||
                !init_line_text(&new_lines[new_count], start, line_len)) {
                free_line_array(new_lines, new_count);
                return;
            }
            new_count++;

            if (!found)
                break;
            start = end + 1;
        }

        if (new_count == 0) {
            if (!ensure_line_array_capacity(&new_lines, &new_capacity, 1) ||
                !init_line_text(&new_lines[0], "", 0)) {
                free_line_array(new_lines, new_count);
                return;
            }
            new_count = 1;
        }
    }

    if (!editor->document_buffer)
        editor->document_buffer = zanna_text_buffer_new();
    if (editor->document_buffer) {
        const char *buffer_text = text ? text : "";
        size_t buffer_len = text ? len : 0;
        if (!zanna_text_buffer_load_bytes(
                (zanna_text_buffer_t *)editor->document_buffer, buffer_text, buffer_len)) {
            free_line_array(new_lines, new_count);
            return;
        }
    }

    codeeditor_clear_highlight_spans(editor);
    codeeditor_free_inlay_hint_storage(editor);
    free_line_array(editor->lines, editor->line_count);
    editor->lines = new_lines;
    editor->line_capacity = new_capacity;
    editor->line_count = new_count;
    editor->cursor_line = 0;
    editor->cursor_col = 0;
    editor->has_selection = false;
    clear_extra_cursor_selections(editor);
    editor->extra_cursor_count = 0;
    editor->scroll_x = 0;
    editor->scroll_y = 0;
    editor->modified = false;
    editor->zia_block_comment_depth = 0;
    free(editor->fold_regions);
    editor->fold_regions = NULL;
    editor->fold_region_count = 0;
    editor->fold_region_cap = 0;
    editor->has_folded_lines = false;
    edit_history_clear(editor->history);

    codeeditor_bump_revision(editor);
    codeeditor_journal_mark_overflow(editor); // full text replace: consumer full-syncs
    vg_codeeditor_refresh_layout_state(editor);
    editor->base.needs_paint = true;
}

/// @brief Replace the complete document from a NUL-terminated UTF-8 string.
/// @details Delegates to @ref vg_codeeditor_set_text_bytes, resets cursor, selection, scroll,
/// folding, highlighting, history, and dirty state, and treats NULL as an empty document.
/// @param editor Code editor whose document should be replaced.
/// @param text NUL-terminated text copied into editor-owned storage, or NULL to clear.
void vg_codeeditor_set_text(vg_codeeditor_t *editor, const char *text) {
    vg_codeeditor_set_text_bytes(editor, text, text ? strlen(text) : 0);
}

/// @brief Replace the complete document as one undoable state-preserving edit.
/// @details The ordinary SetText cold path intentionally resets document state. Save-time
///          normalization instead needs a regular edit: this routine temporarily removes extra
///          cursors from the edit target set, replaces the full primary range once, and restores
///          every cursor, selection, scroll offset, and still-valid fold. Prior history remains
///          intact and the replacement is appended as one undo unit.
/// @param editor Editor to update.
/// @param text Null-terminated UTF-8 replacement, or NULL for empty text.
/// @return True when applied or already equal; false on invalid/read-only/allocation failure.
bool vg_codeeditor_replace_all_text(vg_codeeditor_t *editor, const char *text) {
    if (!editor || editor->read_only)
        return false;
    const char *replacement = text ? text : "";
    char *current = vg_codeeditor_get_text(editor);
    if (!current)
        return false;
    bool unchanged = strcmp(current, replacement) == 0;
    free(current);
    if (unchanged)
        return true;

    const int saved_cursor_line = editor->cursor_line;
    const int saved_cursor_col = editor->cursor_col;
    const vg_selection_t saved_selection = editor->selection;
    const bool saved_has_selection = editor->has_selection;
    const float saved_scroll_x = editor->scroll_x;
    const float saved_scroll_y = editor->scroll_y;
    const int saved_extra_count = editor->extra_cursor_count;
    struct vg_extra_cursor *saved_extra = NULL;
    if (saved_extra_count > 0) {
        saved_extra = malloc((size_t)saved_extra_count * sizeof(*saved_extra));
        if (!saved_extra)
            return false;
        memcpy(
            saved_extra, editor->extra_cursors, (size_t)saved_extra_count * sizeof(*saved_extra));
    }

    vg_edit_target_t target;
    memset(&target, 0, sizeof(target));
    target.cursor_id = 0;
    target.cursor_line_before = editor->cursor_line;
    target.cursor_col_before = editor->cursor_col;
    target.start_line = 0;
    target.start_col = 0;
    target.end_line = editor->line_count > 0 ? editor->line_count - 1 : 0;
    target.end_col = editor->line_count > 0 ? (int)editor->lines[target.end_line].length : 0;

    uint64_t revision_before = editor->revision;
    editor->extra_cursor_count = 0;
    edit_history_break_coalescing(editor->history);
    apply_edit_targets(editor, &target, 1, replacement);

    editor->cursor_line = saved_cursor_line;
    editor->cursor_col = saved_cursor_col;
    clamp_editor_position(editor, &editor->cursor_line, &editor->cursor_col);
    editor->selection = saved_selection;
    clamp_editor_position(editor, &editor->selection.start_line, &editor->selection.start_col);
    clamp_editor_position(editor, &editor->selection.end_line, &editor->selection.end_col);
    editor->has_selection =
        saved_has_selection && compare_positions(editor->selection.start_line,
                                                 editor->selection.start_col,
                                                 editor->selection.end_line,
                                                 editor->selection.end_col) != 0;

    if (saved_extra_count > 0) {
        memcpy(
            editor->extra_cursors, saved_extra, (size_t)saved_extra_count * sizeof(*saved_extra));
        for (int i = 0; i < saved_extra_count; ++i) {
            struct vg_extra_cursor *cursor = &editor->extra_cursors[i];
            clamp_editor_position(editor, &cursor->line, &cursor->col);
            clamp_editor_position(
                editor, &cursor->selection.start_line, &cursor->selection.start_col);
            clamp_editor_position(editor, &cursor->selection.end_line, &cursor->selection.end_col);
            cursor->has_selection =
                cursor->has_selection && compare_positions(cursor->selection.start_line,
                                                           cursor->selection.start_col,
                                                           cursor->selection.end_line,
                                                           cursor->selection.end_col) != 0;
        }
    }
    editor->extra_cursor_count = saved_extra_count;
    free(saved_extra);

    int valid_folds = 0;
    editor->has_folded_lines = false;
    for (int i = 0; i < editor->fold_region_count; ++i) {
        struct vg_fold_region fold = editor->fold_regions[i];
        clamp_editor_line(editor, &fold.start_line);
        clamp_editor_line(editor, &fold.end_line);
        if (fold.end_line <= fold.start_line)
            continue;
        editor->fold_regions[valid_folds++] = fold;
        if (fold.folded)
            editor->has_folded_lines = true;
    }
    editor->fold_region_count = valid_folds;
    editor->scroll_x = saved_scroll_x;
    editor->scroll_y = saved_scroll_y;
    vg_codeeditor_refresh_layout_state(editor);
    editor->base.needs_paint = true;
    return editor->revision != revision_before;
}

// ── Incremental sync journal serialization (plan 08) ────────────────────────

/// @brief Minimal growable byte buffer for delta-JSON assembly.
/// @details Storage grows geometrically and @ref ok becomes false after the first allocation
/// failure so later append operations become harmless no-ops.
typedef struct {
    /// @brief Heap allocation containing bytes written so far.
    char *data;
    /// @brief Number of initialized content bytes.
    size_t len;
    /// @brief Allocated capacity in bytes.
    size_t cap;
    /// @brief Whether all growth and append operations have succeeded.
    bool ok;
} cej_buf_t;

/// @brief Ensure a delta-JSON buffer can accept additional bytes plus a terminator.
/// @param b Buffer to grow; a failed buffer is left unchanged.
/// @param extra Number of content bytes the caller intends to append.
static void cej_ensure(cej_buf_t *b, size_t extra) {
    if (!b->ok)
        return;
    if (b->len + extra + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 256;
        while (ncap < b->len + extra + 1)
            ncap *= 2;
        char *nd = realloc(b->data, ncap);
        if (!nd) {
            b->ok = false;
            return;
        }
        b->data = nd;
        b->cap = ncap;
    }
}

/// @brief Append a NUL-terminated byte string to a delta-JSON buffer.
/// @param b Destination buffer.
/// @param s Source string copied without its terminator.
static void cej_puts(cej_buf_t *b, const char *s) {
    size_t n = strlen(s);
    cej_ensure(b, n);
    if (b->ok) {
        memcpy(b->data + b->len, s, n);
        b->len += n;
    }
}

/// @brief Append one byte to a delta-JSON buffer.
/// @param b Destination buffer.
/// @param c Byte to append.
static void cej_putc(cej_buf_t *b, char c) {
    cej_ensure(b, 1);
    if (b->ok)
        b->data[b->len++] = c;
}

/// @brief Append a signed decimal integer to a delta-JSON buffer.
/// @param b Destination buffer.
/// @param v Value formatted in base ten.
static void cej_put_i64(cej_buf_t *b, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", v);
    cej_puts(b, tmp);
}

/// @brief Append a string as a JSON literal with standard escaping.
/// @details Quotes, reverse solidus, common whitespace controls, and remaining control bytes are
/// escaped; other bytes are copied unchanged.
/// @param b Destination buffer.
/// @param s NUL-terminated source string.
static void cej_put_jstr(cej_buf_t *b, const char *s) {
    cej_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"')
            cej_puts(b, "\\\"");
        else if (c == '\\')
            cej_puts(b, "\\\\");
        else if (c == '\n')
            cej_puts(b, "\\n");
        else if (c == '\r')
            cej_puts(b, "\\r");
        else if (c == '\t')
            cej_puts(b, "\\t");
        else if (c < 0x20) {
            char u[8];
            snprintf(u, sizeof(u), "\\u%04x", c);
            cej_puts(b, u);
        } else
            cej_putc(b, (char)c);
    }
    cej_putc(b, '"');
}

/// @brief Duplicate a NUL-terminated status string.
/// @param s Source string.
/// @return Heap-allocated copy, or NULL on allocation failure.
static char *cej_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r)
        memcpy(r, s, n);
    return r;
}

/// @brief Serialize and consume forward document deltas newer than a revision.
/// @details Each JSON object describes a pre-edit byte range and its replacement text. Cold
/// mutations, a wrapped journal, or a detected revision gap return the owned literal `overflow`
/// and require the caller to perform a full synchronization. Successfully serialized or overflowed
/// entries are pruned from the journal.
/// @param editor Code editor whose incremental journal is consumed.
/// @param since_revision Last revision already applied by the consumer.
/// @return Caller-owned JSON array, caller-owned `overflow` string, or NULL for invalid input or
/// allocation failure.
static char *codeeditor_deltas_json(vg_codeeditor_t *editor,
                                    uint64_t since_revision,
                                    bool consume) {
    if (!editor)
        return NULL;
    // A cold mutation (undo/redo/SetText/buffer swap) or a ring wrap happened:
    // the caller cannot apply incrementally and must full-sync.
    if (editor->journal_overflowed) {
        if (consume) {
            codeeditor_journal_reset(editor);
            editor->journal_overflowed = false;
        }
        return cej_dup("overflow");
    }
    // Defensive gap check: retained deltas are contiguous in revision, so an
    // oldest revision beyond since_revision+1 means intervening deltas were lost.
    if (editor->journal_count > 0 && editor->journal_oldest_rev > since_revision + 1) {
        if (consume)
            codeeditor_journal_reset(editor);
        return cej_dup("overflow");
    }

    cej_buf_t b = {NULL, 0, 0, true};
    cej_putc(&b, '[');
    bool first = true;
    for (int i = 0; i < editor->journal_count; i++) {
        int idx = (editor->journal_head + i) % VG_EDIT_JOURNAL_CAP;
        const vg_edit_delta_t *d = &editor->journal[idx];
        if (d->revision <= since_revision)
            continue;
        if (!first)
            cej_putc(&b, ',');
        first = false;
        cej_puts(&b, "{\"r\":");
        cej_put_i64(&b, (long long)d->revision);
        cej_puts(&b, ",\"sl\":");
        cej_put_i64(&b, d->start_line);
        cej_puts(&b, ",\"sc\":");
        cej_put_i64(&b, d->start_col);
        cej_puts(&b, ",\"el\":");
        cej_put_i64(&b, d->end_line);
        cej_puts(&b, ",\"ec\":");
        cej_put_i64(&b, d->end_col);
        cej_puts(&b, ",\"t\":");
        cej_put_jstr(&b, d->text ? d->text : "");
        cej_putc(&b, '}');
    }
    cej_putc(&b, ']');

    // The caller now holds every delta up to the latest revision; drop them.
    if (consume)
        codeeditor_journal_reset(editor);

    if (!b.ok) {
        free(b.data);
        return NULL;
    }
    b.data[b.len] = '\0';
    return b.data;
}

char *vg_codeeditor_take_deltas_json(vg_codeeditor_t *editor, uint64_t since_revision) {
    return codeeditor_deltas_json(editor, since_revision, true);
}

char *vg_codeeditor_peek_deltas_json(vg_codeeditor_t *editor, uint64_t since_revision) {
    return codeeditor_deltas_json(editor, since_revision, false);
}

bool vg_codeeditor_apply_mirror_edit(vg_codeeditor_t *editor,
                                     int start_line,
                                     int start_col,
                                     int end_line,
                                     int end_col,
                                     const char *text) {
    if (!editor)
        return false;
    const bool was_read_only = editor->read_only;
    const bool was_modified = editor->modified;
    const uint64_t before = editor->revision;
    vg_edit_target_t target;
    memset(&target, 0, sizeof(target));
    target.cursor_id = 0;
    target.cursor_line_before = editor->cursor_line;
    target.cursor_col_before = editor->cursor_col;
    target.start_line = start_line;
    target.start_col = start_col;
    target.end_line = end_line;
    target.end_col = end_col;
    editor->read_only = false;
    apply_edit_targets(editor, &target, 1, text ? text : "");
    editor->read_only = was_read_only;
    editor->modified = was_modified;
    edit_history_clear(editor->history);
    codeeditor_journal_reset(editor);
    editor->journal_overflowed = false;
    return editor->revision != before;
}

size_t vg_codeeditor_display_column_for_byte(const vg_codeeditor_t *editor,
                                             int line,
                                             size_t byte_col) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    return codeeditor_display_columns_before_byte(
        &editor->lines[line], byte_col, editor->tab_width);
}

size_t vg_codeeditor_byte_for_display_column(const vg_codeeditor_t *editor,
                                             int line,
                                             size_t display_col) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    return codeeditor_byte_for_display_column(&editor->lines[line], display_col, editor->tab_width);
}

size_t vg_codeeditor_character_column_for_display(const vg_codeeditor_t *editor,
                                                  int line,
                                                  size_t display_col) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    const vg_code_line_t *source = &editor->lines[line];
    const size_t byte_limit = vg_codeeditor_byte_for_display_column(editor, line, display_col);
    size_t characters = 0;
    for (size_t pos = 0; pos < byte_limit;) {
        size_t span = codeeditor_utf8_span(source->text + pos);
        if (span == 0 || span > source->length - pos)
            span = 1;
        pos += span;
        characters++;
    }
    return characters;
}

size_t vg_codeeditor_line_display_columns(const vg_codeeditor_t *editor, int line) {
    if (!editor || line < 0 || line >= editor->line_count)
        return 0;
    return codeeditor_line_display_columns(&editor->lines[line], editor->tab_width);
}

/// @brief Remove every inlay hint and release its copied text.
/// @param editor Code editor to clear; NULL is ignored.
void vg_codeeditor_clear_inlay_hints(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_free_inlay_hint_storage(editor);
    editor->base.needs_paint = true;
}

/// @brief Append a painted inlay hint at a document position.
/// @details The line must exist, the byte column is clamped to that line, and @p text is copied.
/// Appending an out-of-order position marks the collection for later sorting.
/// @param editor Code editor that owns the new hint.
/// @param line Zero-based source line.
/// @param col Byte column on @p line.
/// @param text Non-empty hint text copied internally.
/// @param color Packed hint text color.
void vg_codeeditor_add_inlay_hint(
    vg_codeeditor_t *editor, int line, int col, const char *text, uint32_t color) {
    if (!editor || !text || text[0] == '\0')
        return;
    if (line < 0 || line >= editor->line_count)
        return;
    if (col < 0)
        col = 0;
    if (col > (int)editor->lines[line].length)
        col = (int)editor->lines[line].length;
    if (editor->inlay_hint_count >= editor->inlay_hint_cap) {
        if (editor->inlay_hint_cap > INT_MAX / 2)
            return;
        int new_cap = editor->inlay_hint_cap ? editor->inlay_hint_cap * 2 : 8;
        void *p = realloc(editor->inlay_hints, (size_t)new_cap * sizeof(*editor->inlay_hints));
        if (!p)
            return;
        editor->inlay_hints = p;
        editor->inlay_hint_cap = new_cap;
    }
    char *copy = vg_strdup(text);
    if (!copy)
        return;
    if (editor->inlay_hint_count > 0) {
        const struct vg_inlay_hint *prev = &editor->inlay_hints[editor->inlay_hint_count - 1];
        if (prev->line > line || (prev->line == line && prev->col > col))
            editor->inlay_hints_sorted = false;
    }
    struct vg_inlay_hint *hint = &editor->inlay_hints[editor->inlay_hint_count++];
    hint->line = line;
    hint->col = col;
    hint->text = copy;
    hint->color = color;
    editor->base.needs_paint = true;
}

/// @brief Return the number of retained inlay hints.
/// @param editor Code editor to inspect.
/// @return Hint count, or zero when @p editor is NULL.
int vg_codeeditor_get_inlay_hint_count(const vg_codeeditor_t *editor) {
    return editor ? editor->inlay_hint_count : 0;
}

/// @brief Return the complete editor content as a heap-allocated newline-joined string.
///
/// @details The caller owns the returned buffer and must free it.
///
/// @param editor The code editor to query.
/// @return Heap-allocated null-terminated UTF-8 string, or NULL on allocation failure
///         or if editor is NULL.
char *vg_codeeditor_get_text(vg_codeeditor_t *editor) {
    if (!editor)
        return NULL;

    if (editor->document_buffer) {
        size_t len = 0;
        char *result =
            zanna_text_buffer_text_dup((const zanna_text_buffer_t *)editor->document_buffer, &len);
        if (result) {
            codeeditor_perf_add(&editor->perf_stats.full_text_copies, 1);
            codeeditor_perf_add(&editor->perf_stats.full_text_copy_bytes, len);
            return result;
        }
    }

    // Calculate total size, including one final NUL byte.
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
        if (i < editor->line_count - 1) {
            *p++ = '\n';
        }
    }
    *p = '\0';

    codeeditor_perf_add(&editor->perf_stats.full_text_copies, 1);
    codeeditor_perf_add(&editor->perf_stats.full_text_copy_bytes, total > 0 ? total - 1 : 0);
    return result;
}

/// @brief Return the editor's monotonic content revision.
/// @param editor Code editor to inspect.
/// @return Current revision, or zero when @p editor is NULL.
uint64_t vg_codeeditor_get_revision(vg_codeeditor_t *editor) {
    return editor ? editor->revision : 0;
}

/// @brief Return the selected text as a heap-allocated string.
///
/// @details The caller owns the returned buffer and must free it.  Returns NULL
///          if there is no active selection or if editor is NULL.
///
/// @param editor The code editor to query.
/// @return Heap-allocated null-terminated UTF-8 string of selected text, or NULL.
char *vg_codeeditor_get_selection(vg_codeeditor_t *editor) {
    if (!editor || !editor->has_selection)
        return NULL;

    int start_line, start_col, end_line, end_col;
    normalize_selection(editor, &start_line, &start_col, &end_line, &end_col);
    return copy_text_range(editor, start_line, start_col, end_line, end_col);
}

/// @brief Move the cursor to the specified line and column, clearing any selection.
///
/// @details line and col are clamped to valid positions.  Extra cursors and
///          selections are also cleared.
///
/// @param editor The code editor to update.
/// @param line   Zero-based target line; clamped to [0, line_count-1].
/// @param col    Zero-based target column; clamped to the line's length.
void vg_codeeditor_set_cursor(vg_codeeditor_t *editor, int line, int col) {
    if (!editor)
        return;

    clamp_editor_line(editor, &line);
    col = codeeditor_char_col_to_byte_col(editor, line, col);
    codeeditor_clamp_cursor_to_visible(editor, &line, &col);

    editor->cursor_line = line;
    editor->cursor_col = col;
    editor->has_selection = false;
    clear_extra_cursor_selections(editor);
    editor->base.needs_paint = true;
}

/// @brief Retrieve the current cursor line and column.
///
/// @param editor   The code editor to query.
/// @param out_line Output for the zero-based cursor line; may be NULL.
/// @param out_col  Output for the zero-based cursor column; may be NULL.
void vg_codeeditor_get_cursor(vg_codeeditor_t *editor, int *out_line, int *out_col) {
    if (!editor)
        return;
    if (out_line)
        *out_line = editor->cursor_line;
    if (out_col)
        *out_col = codeeditor_byte_col_to_char_col(editor, editor->cursor_line, editor->cursor_col);
}

/// @brief Programmatically set the selection range and position the cursor at the end.
///
/// @details All coordinates are clamped to valid positions.  The cursor is placed
///          at (end_line, end_col) after the call.
///
/// @param editor     The code editor to update.
/// @param start_line Zero-based selection start line.
/// @param start_col  Zero-based selection start column.
/// @param end_line   Zero-based selection end line; cursor lands here.
/// @param end_col    Zero-based selection end column; cursor lands here.
void vg_codeeditor_set_selection(
    vg_codeeditor_t *editor, int start_line, int start_col, int end_line, int end_col) {
    if (!editor)
        return;

    clamp_editor_line(editor, &start_line);
    clamp_editor_line(editor, &end_line);
    start_col = codeeditor_char_col_to_byte_col(editor, start_line, start_col);
    end_col = codeeditor_char_col_to_byte_col(editor, end_line, end_col);
    clamp_editor_position(editor, &start_line, &start_col);
    clamp_editor_position(editor, &end_line, &end_col);
    editor->selection.start_line = start_line;
    editor->selection.start_col = start_col;
    editor->selection.end_line = end_line;
    editor->selection.end_col = end_col;
    editor->cursor_line = end_line;
    editor->cursor_col = end_col;
    editor->has_selection = compare_positions(start_line, start_col, end_line, end_col) != 0;
    editor->base.needs_paint = true;
}

/// @brief Insert text at each cursor, replacing any active selections.
///
/// @details In multi-cursor mode each cursor receives the same text, applied in
///          reverse document order to preserve position validity.  The operation
///          is recorded in the undo history.  No-op if editor is in read-only mode.
///
/// @param editor The code editor to update.
/// @param text   Null-terminated UTF-8 text to insert; may contain newlines.
void vg_codeeditor_insert_text(vg_codeeditor_t *editor, const char *text) {
    if (!editor || editor->read_only || !text || text[0] == '\0')
        return;

    const int max_targets = 1 + editor->extra_cursor_count;
    vg_edit_target_t *targets = calloc((size_t)max_targets, sizeof(vg_edit_target_t));
    if (!targets)
        return;

    int target_count = 0;
    if (editor->has_selection) {
        add_edit_target(targets,
                        &target_count,
                        0,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->selection.start_line,
                        editor->selection.start_col,
                        editor->selection.end_line,
                        editor->selection.end_col);
    } else {
        add_edit_target(targets,
                        &target_count,
                        0,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->cursor_line,
                        editor->cursor_col);
    }

    for (int i = 0; i < editor->extra_cursor_count; i++) {
        struct vg_extra_cursor *cursor = &editor->extra_cursors[i];
        if (cursor->has_selection) {
            add_edit_target(targets,
                            &target_count,
                            i + 1,
                            cursor->line,
                            cursor->col,
                            cursor->selection.start_line,
                            cursor->selection.start_col,
                            cursor->selection.end_line,
                            cursor->selection.end_col);
        } else {
            add_edit_target(targets,
                            &target_count,
                            i + 1,
                            cursor->line,
                            cursor->col,
                            cursor->line,
                            cursor->col,
                            cursor->line,
                            cursor->col);
        }
    }

    qsort(targets, (size_t)target_count, sizeof(*targets), edit_target_compare_desc);
    apply_edit_targets(editor, targets, target_count, text);
    free(targets);
}

/// @brief Delete the selected text at all active cursors, recording the operation.
///
/// @details If no selection exists the call is a no-op.  In multi-cursor mode,
///          all cursor selections are deleted in a single pass.
///
/// @param editor The code editor to update.
void vg_codeeditor_delete_selection(vg_codeeditor_t *editor) {
    if (!editor || editor->read_only)
        return;

    const int max_targets = 1 + editor->extra_cursor_count;
    vg_edit_target_t *targets = calloc((size_t)max_targets, sizeof(vg_edit_target_t));
    if (!targets)
        return;

    int target_count = 0;
    if (editor->has_selection) {
        add_edit_target(targets,
                        &target_count,
                        0,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->selection.start_line,
                        editor->selection.start_col,
                        editor->selection.end_line,
                        editor->selection.end_col);
    }

    for (int i = 0; i < editor->extra_cursor_count; i++) {
        struct vg_extra_cursor *cursor = &editor->extra_cursors[i];
        if (!cursor->has_selection)
            continue;
        add_edit_target(targets,
                        &target_count,
                        i + 1,
                        cursor->line,
                        cursor->col,
                        cursor->selection.start_line,
                        cursor->selection.start_col,
                        cursor->selection.end_line,
                        cursor->selection.end_col);
    }

    if (target_count > 0) {
        qsort(targets, (size_t)target_count, sizeof(*targets), edit_target_compare_desc);
        apply_edit_targets(editor, targets, target_count, NULL);
    }
    free(targets);
}

/// @brief Scroll the viewport so that the specified line is visible.
///
/// @details Scrolls the minimum distance needed: if the line is already visible
///          the scroll position is unchanged.  Line is clamped to [0, line_count-1].
///          Hidden (folded) lines are remapped to their visible anchor.
///
/// @param editor The code editor to scroll.
/// @param line   Zero-based line number to scroll into view.
void vg_codeeditor_scroll_to_line(vg_codeeditor_t *editor, int line) {
    if (!editor)
        return;

    if (line < 0)
        line = 0;
    if (line >= editor->line_count)
        line = editor->line_count - 1;
    line = codeeditor_visible_anchor_line(editor, line);

    float content_width = codeeditor_content_draw_width(editor, &editor->base);
    int target_visual_row = codeeditor_visual_row_for_position(editor, content_width, line, 0);
    float target_y = (float)target_visual_row * editor->line_height;
    float visible_height = editor->base.height;

    if (target_y < editor->scroll_y) {
        editor->scroll_y = target_y;
    } else if (target_y + editor->line_height > editor->scroll_y + visible_height) {
        editor->scroll_y = target_y + editor->line_height - visible_height;
    }

    codeeditor_clamp_scroll(editor, &editor->base);
    editor->base.needs_paint = true;
}

/// @brief Resolve the source line nearest the top of the viewport.
/// @details Converts the current vertical scroll position to a visual row, then maps that row
/// through wrapping and folding back to a source line.
/// @param editor Code editor to inspect.
/// @return Clamped zero-based source line, or zero when unavailable.
int vg_codeeditor_get_scroll_top_line(vg_codeeditor_t *editor) {
    if (!editor || editor->line_count <= 0 || editor->line_height <= 0.0f)
        return 0;

    int visual_row = (int)(editor->scroll_y / editor->line_height);
    int line = 0;
    codeeditor_locate_visual_row(
        editor, codeeditor_content_draw_width(editor, &editor->base), visual_row, &line, NULL);
    if (line < 0)
        return 0;
    if (line >= editor->line_count)
        return editor->line_count - 1;
    return line;
}

/// @brief Place a source line at the top of the viewport when possible.
/// @details The requested line is clamped, folded lines are mapped to their visible anchor, and
/// the corresponding visual-row offset is constrained to the document's valid scroll range.
/// @param editor Code editor to scroll.
/// @param line Zero-based source line.
void vg_codeeditor_set_scroll_top_line(vg_codeeditor_t *editor, int line) {
    if (!editor || editor->line_count <= 0)
        return;

    if (line < 0)
        line = 0;
    if (line >= editor->line_count)
        line = editor->line_count - 1;
    line = codeeditor_visible_anchor_line(editor, line);

    float content_width = codeeditor_content_draw_width(editor, &editor->base);
    int visual_row = codeeditor_visual_row_for_position(editor, content_width, line, 0);
    editor->scroll_y = (float)visual_row * editor->line_height;
    codeeditor_clamp_scroll(editor, &editor->base);
    editor->base.needs_paint = true;
}

/// @brief Register a syntax highlighting callback invoked once per visible line during paint.
///
/// @details Setting a new callback invalidates all cached per-character colour data,
///          forcing a full highlight refresh on the next repaint.  Set callback to NULL
///          to disable syntax highlighting.
///
/// @param editor    The code editor to configure.
/// @param callback  Syntax highlighter called with (editor, line_idx, user_data).
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_codeeditor_set_syntax(vg_codeeditor_t *editor,
                              vg_syntax_callback_t callback,
                              void *user_data) {
    if (!editor)
        return;
    editor->syntax_highlighter = callback;
    editor->syntax_data = user_data;
    // Reset language-specific scanner state — block-comment depth from a prior
    // Zia document must not leak into a freshly-installed highlighter.
    editor->zia_block_comment_depth = 0;
    codeeditor_bump_highlight_generation(editor);
    // Invalidate cached colors so the new highlighter runs on the next paint.
    for (int i = 0; i < editor->line_count; i++) {
        if (editor->lines[i].colors) {
            free(editor->lines[i].colors);
            editor->lines[i].colors = NULL;
            editor->lines[i].colors_capacity = 0;
        }
        editor->lines[i].highlight_generation = 0;
        editor->lines[i].syntax_state_generation = 0;
    }
    editor->base.needs_paint = true;
}

/// @brief Low-level text insert at (line, col) without recording to history; handles embedded
/// newlines via line splits.
/// @param editor Code editor whose line storage is modified.
/// @param line Zero-based insertion line.
/// @param col Byte column clamped to the insertion line.
/// @param text NUL-terminated UTF-8 text, potentially containing newlines.
/// @return `true` when at least one accepted byte or newline is inserted.
static bool insert_text_at_internal(vg_codeeditor_t *editor, int line, int col, const char *text) {
    if (!editor || !text || line < 0 || line >= editor->line_count)
        return false;

    clamp_editor_position(editor, &line, &col);

    int cur_line = line;
    int cur_col = col;
    bool changed = false;

    for (const char *p = text; *p;) {
        if (*p == '\n') {
            if (!codeeditor_split_line_at(editor, cur_line, cur_col))
                return false;
            cur_line++;
            cur_col = 0;
            changed = true;
            p++;
            continue;
        }

        size_t span = codeeditor_utf8_span(p);
        if (span == 0) {
            unsigned char byte = (unsigned char)*p;
            if (byte < 32 && byte != '\t') {
                p++;
                continue;
            }
            span = 1;
        }

        if (!codeeditor_insert_bytes_at(editor, cur_line, &cur_col, p, span))
            return false;
        changed = true;
        p += span;
    }

    editor->cursor_line = cur_line;
    editor->cursor_col = cur_col;
    if (changed) {
        editor->modified = true;
        vg_codeeditor_refresh_layout_state(editor);
    }
    return true;
}

/// @brief Low-level range deletion without history recording; merges lines when the range spans
/// newlines.
/// @param editor Code editor whose line storage is modified.
/// @param start_line First range line.
/// @param start_col First range byte column.
/// @param end_line Last range line.
/// @param end_col Exclusive final range byte column.
static void delete_text_range_internal(
    vg_codeeditor_t *editor, int start_line, int start_col, int end_line, int end_col) {
    if (!editor || start_line < 0 || start_line >= editor->line_count)
        return;
    if (end_line < 0 || end_line >= editor->line_count)
        return;

    normalize_selection_range(&start_line, &start_col, &end_line, &end_col);
    clamp_editor_position(editor, &start_line, &start_col);
    clamp_editor_position(editor, &end_line, &end_col);
    normalize_selection_range(&start_line, &start_col, &end_line, &end_col);
    if (compare_positions(start_line, start_col, end_line, end_col) >= 0)
        return;

    if (start_line == end_line) {
        // Single line deletion
        vg_code_line_t *line = &editor->lines[start_line];
        if (start_col >= (int)line->length)
            return;
        if (end_col > (int)line->length)
            end_col = (int)line->length;

        memmove(line->text + start_col, line->text + end_col, line->length - end_col + 1);
        line->length -= (end_col - start_col);
        codeeditor_invalidate_line_highlight(editor, start_line);
    } else {
        // Multi-line deletion
        vg_code_line_t *first = &editor->lines[start_line];
        vg_code_line_t *last = &editor->lines[end_line];
        int old_line_count = editor->line_count;

        if (end_col > (int)last->length)
            end_col = (int)last->length;
        if (start_col > (int)first->length)
            start_col = (int)first->length;
        if ((size_t)start_col > SIZE_MAX - (last->length - (size_t)end_col))
            return;
        size_t new_len = start_col + (last->length - end_col);
        if (!ensure_text_capacity(first, new_len + 1))
            return;

        memcpy(first->text + start_col, last->text + end_col, last->length - end_col + 1);
        first->length = new_len;
        codeeditor_invalidate_line_highlight(editor, start_line);

        for (int i = start_line + 1; i <= end_line; i++) {
            free_line(&editor->lines[i]);
        }

        int lines_removed = end_line - start_line;
        if (end_line + 1 < editor->line_count) {
            memmove(&editor->lines[start_line + 1],
                    &editor->lines[end_line + 1],
                    (editor->line_count - end_line - 1) * sizeof(vg_code_line_t));
        }
        editor->line_count -= lines_removed;
        if (old_line_count > editor->line_count) {
            memset(&editor->lines[editor->line_count],
                   0,
                   (size_t)(old_line_count - editor->line_count) * sizeof(vg_code_line_t));
        }
        vg_codeeditor_refresh_layout_state(editor);
    }

    editor->cursor_line = start_line;
    editor->cursor_col = start_col;
    editor->modified = true;
    editor->lines[start_line].modified = true;
}

/// @brief Computes the document position reached after inserting @p text starting at (start_line,
/// start_col).
/// @param start_line Initial source line.
/// @param start_col Initial byte column.
/// @param text NUL-terminated text whose accepted bytes and newlines are traversed.
/// @param[out] out_line Optional destination for the resulting line.
/// @param[out] out_col Optional destination for the resulting byte column.
static void compute_text_end_position(
    int start_line, int start_col, const char *text, int *out_line, int *out_col) {
    int line = start_line;
    int col = start_col;
    if (col < 0)
        col = 0;

    if (text) {
        for (const char *p = text; *p;) {
            if (*p == '\n') {
                line++;
                col = 0;
                p++;
                continue;
            }

            size_t span = codeeditor_utf8_span(p);
            if (span == 0) {
                unsigned char byte = (unsigned char)*p;
                if (byte < 32 && byte != '\t') {
                    p++;
                    continue;
                }
                span = 1;
            }

            if (span > (size_t)(INT_MAX - col))
                col = INT_MAX;
            else
                col += (int)span;
            p += span;
        }
    }

    if (out_line)
        *out_line = line;
    if (out_col)
        *out_col = col;
}

/// @brief Undo the most recent edit operation (or group of grouped operations).
///
/// @details Grouped operations sharing the same group_id are undone atomically.
///          Cursor position is restored to the pre-edit location.  No-op if the
///          history stack is empty.
///
/// @param editor The code editor to apply undo to.
void vg_codeeditor_undo(vg_codeeditor_t *editor) {
    if (!editor || !editor->history)
        return;

    edit_history_break_coalescing(editor->history);
    vg_edit_op_t *op = edit_history_pop_undo(editor->history);
    if (!op)
        return;

    // Handle grouped operations
    uint32_t group = op->group_id;
    do {
        switch (op->type) {
            case VG_EDIT_INSERT:
                compute_text_end_position(
                    op->start_line, op->start_col, op->new_text, &op->end_line, &op->end_col);
                // Undo insert = delete the inserted text
                delete_text_range_internal(
                    editor, op->start_line, op->start_col, op->end_line, op->end_col);
                break;

            case VG_EDIT_DELETE:
                // Undo delete = re-insert the deleted text
                insert_text_at_internal(editor, op->start_line, op->start_col, op->old_text);
                break;

            case VG_EDIT_REPLACE:
                compute_text_end_position(
                    op->start_line, op->start_col, op->new_text, &op->end_line, &op->end_col);
                // Undo replace = delete new text, insert old text
                delete_text_range_internal(
                    editor, op->start_line, op->start_col, op->end_line, op->end_col);
                insert_text_at_internal(editor, op->start_line, op->start_col, op->old_text);
                break;
        }

        // Restore cursor
        set_cursor_position_by_id(
            editor, op->cursor_id, op->cursor_line_before, op->cursor_col_before);

        // Check for more operations in same group
        if (group != 0) {
            vg_edit_op_t *next_op = edit_history_peek_undo(editor->history);
            if (next_op && next_op->group_id == group) {
                op = edit_history_pop_undo(editor->history);
            } else {
                break;
            }
        } else {
            break;
        }
    } while (op);

    editor->has_selection = false;
    clear_extra_cursor_selections(editor);
    codeeditor_discard_document_buffer_mirror(editor);
    codeeditor_bump_revision(editor);
    codeeditor_journal_mark_overflow(editor); // undo/redo replay: consumer full-syncs
    codeeditor_invalidate_all_highlights(editor);
    editor->base.needs_paint = true;
}

/// @brief Redo the most recently undone edit operation (or group).
///
/// @details Grouped operations are replayed atomically.  Cursor position is
///          restored to the post-edit location.  No-op if the redo stack is empty.
///
/// @param editor The code editor to apply redo to.
void vg_codeeditor_redo(vg_codeeditor_t *editor) {
    if (!editor || !editor->history)
        return;

    edit_history_break_coalescing(editor->history);
    vg_edit_op_t *op = edit_history_pop_redo(editor->history);
    if (!op)
        return;

    // Handle grouped operations
    uint32_t group = op->group_id;

    // Collect all ops in this group
    do {
        switch (op->type) {
            case VG_EDIT_INSERT:
                // Redo insert = insert the text again
                insert_text_at_internal(editor, op->start_line, op->start_col, op->new_text);
                break;

            case VG_EDIT_DELETE:
                compute_text_end_position(
                    op->start_line, op->start_col, op->old_text, &op->end_line, &op->end_col);
                // Redo delete = delete the text again
                delete_text_range_internal(
                    editor, op->start_line, op->start_col, op->end_line, op->end_col);
                break;

            case VG_EDIT_REPLACE:
                compute_text_end_position(
                    op->start_line, op->start_col, op->old_text, &op->end_line, &op->end_col);
                // Redo replace = delete old text, insert new text
                delete_text_range_internal(
                    editor, op->start_line, op->start_col, op->end_line, op->end_col);
                insert_text_at_internal(editor, op->start_line, op->start_col, op->new_text);
                break;
        }

        // Restore cursor
        set_cursor_position_by_id(
            editor, op->cursor_id, op->cursor_line_after, op->cursor_col_after);

        // Check for more operations in same group
        if (group != 0 && editor->history->current_index < editor->history->count) {
            vg_edit_op_t *next_op = editor->history->operations[editor->history->current_index];
            if (next_op && next_op->group_id == group) {
                op = edit_history_pop_redo(editor->history);
            } else {
                break;
            }
        } else {
            break;
        }
    } while (op);

    editor->has_selection = false;
    clear_extra_cursor_selections(editor);
    codeeditor_discard_document_buffer_mirror(editor);
    codeeditor_bump_revision(editor);
    codeeditor_journal_mark_overflow(editor); // undo/redo replay: consumer full-syncs
    codeeditor_invalidate_all_highlights(editor);
    editor->base.needs_paint = true;
}

/// @brief Copy the selected text to the system clipboard.
///
/// @details In multi-cursor mode, all cursor selections are concatenated with
///          newlines between each segment.  Returns false if there is no selection
///          or if clipboard access fails.
///
/// @param editor The code editor to copy from.
/// @return true if text was placed on the clipboard, false otherwise.
bool vg_codeeditor_copy(vg_codeeditor_t *editor) {
    if (!editor)
        return false;

    const int max_targets = 1 + editor->extra_cursor_count;
    vg_edit_target_t *targets = calloc((size_t)max_targets, sizeof(vg_edit_target_t));
    char **parts = NULL;
    size_t *part_lengths = NULL;
    char *clipboard_text = NULL;
    bool copied = false;

    if (!targets)
        return false;

    int target_count = 0;
    if (editor->has_selection) {
        add_edit_target(targets,
                        &target_count,
                        0,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->selection.start_line,
                        editor->selection.start_col,
                        editor->selection.end_line,
                        editor->selection.end_col);
    }
    for (int i = 0; i < editor->extra_cursor_count; i++) {
        struct vg_extra_cursor *cursor = &editor->extra_cursors[i];
        if (!cursor->has_selection)
            continue;
        add_edit_target(targets,
                        &target_count,
                        i + 1,
                        cursor->line,
                        cursor->col,
                        cursor->selection.start_line,
                        cursor->selection.start_col,
                        cursor->selection.end_line,
                        cursor->selection.end_col);
    }

    if (target_count <= 0)
        goto cleanup;

    qsort(targets, (size_t)target_count, sizeof(*targets), edit_target_compare_asc);

    parts = calloc((size_t)target_count, sizeof(char *));
    part_lengths = calloc((size_t)target_count, sizeof(size_t));
    if (!parts || !part_lengths)
        goto cleanup;

    size_t total_len = 0;
    for (int i = 0; i < target_count; i++) {
        parts[i] = copy_text_range_len(editor,
                                       targets[i].start_line,
                                       targets[i].start_col,
                                       targets[i].end_line,
                                       targets[i].end_col,
                                       &part_lengths[i]);
        if (!parts[i])
            goto cleanup;
        if (part_lengths[i] > SIZE_MAX - total_len)
            goto cleanup;
        total_len += part_lengths[i];
        if (i + 1 < target_count) {
            if (total_len == SIZE_MAX)
                goto cleanup;
            total_len++;
        }
    }

    if (total_len == SIZE_MAX)
        goto cleanup;
    clipboard_text = malloc(total_len + 1);
    if (!clipboard_text)
        goto cleanup;

    char *dst = clipboard_text;
    for (int i = 0; i < target_count; i++) {
        if (part_lengths[i] > 0) {
            memcpy(dst, parts[i], part_lengths[i]);
            dst += part_lengths[i];
        }
        if (i + 1 < target_count)
            *dst++ = '\n';
    }
    *dst = '\0';

    vgfx_clipboard_set_text(clipboard_text);
    copied = true;

cleanup:
    if (parts) {
        for (int i = 0; i < target_count; i++)
            free(parts[i]);
    }
    free(clipboard_text);
    free(part_lengths);
    free(parts);
    free(targets);
    return copied;
}

/// @brief Copy the selected text to the clipboard and delete it from the editor.
///
/// @details No-op if the editor is read-only or there is no selection.
///
/// @param editor The code editor to cut from.
/// @return true if text was cut, false if read-only or nothing was selected.
bool vg_codeeditor_cut(vg_codeeditor_t *editor) {
    if (!editor || editor->read_only)
        return false;

    if (!vg_codeeditor_copy(editor))
        return false;

    vg_codeeditor_delete_selection(editor);
    editor->base.needs_paint = true;
    return true;
}

/// @brief Insert the current clipboard text at the cursor, replacing any selection.
///
/// @details No-op if the editor is read-only or the clipboard is empty.
///
/// @param editor The code editor to paste into.
/// @return true if text was pasted, false if read-only or clipboard empty.
bool vg_codeeditor_paste(vg_codeeditor_t *editor) {
    if (!editor || editor->read_only)
        return false;

    // A paste is its own undo unit even if it happens to be one character.
    edit_history_break_coalescing(editor->history);
    char *text = vgfx_clipboard_get_text();
    if (text) {
        vg_codeeditor_insert_text(editor, text);
        free(text);
        editor->base.needs_paint = true;
        return true;
    }
    return false;
}

/// @brief Select all text from the first character of line 0 to the end of the last line.
///
/// @param editor The code editor whose content will be fully selected.
void vg_codeeditor_select_all(vg_codeeditor_t *editor) {
    if (!editor || editor->line_count == 0)
        return;

    editor->selection.start_line = 0;
    editor->selection.start_col = 0;
    editor->selection.end_line = editor->line_count - 1;
    editor->selection.end_col = editor->lines[editor->line_count - 1].length;
    editor->has_selection = true;
    editor->cursor_line = editor->selection.end_line;
    editor->cursor_col = editor->selection.end_col;
    clear_extra_cursor_selections(editor);
    editor->base.needs_paint = true;
}

/// @brief Set the monospace font and size used to render all editor text.
///
/// @details Updates char_width (measured from 'M') and line_height from the font
///          metrics.  Triggers a full layout refresh.
///
/// @param editor The code editor to configure.
/// @param font   Monospace font to use; NULL leaves the font unchanged.
/// @param size   Font size in logical pixels; values ≤ 0 default to the theme's
///               normal text size.
void vg_codeeditor_set_font(vg_codeeditor_t *editor, vg_font_t *font, float size) {
    if (!editor)
        return;
    if (!font)
        return;

    editor->font = font;
    editor->font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;

    if (font) {
        // Calculate char width (assuming monospace)
        vg_text_metrics_t metrics;
        vg_font_measure_text(font, editor->font_size, "M", &metrics);
        editor->char_width = metrics.width;

        vg_font_metrics_t font_metrics;
        vg_font_get_metrics(font, editor->font_size, &font_metrics);
        editor->line_height = font_metrics.line_height;
    }

    vg_codeeditor_refresh_layout_state(editor);
}

/// @brief Return the number of lines in the editor (always ≥ 1).
///
/// @param editor The code editor to query.
/// @return Total line count, or 0 if editor is NULL.
int vg_codeeditor_get_line_count(vg_codeeditor_t *editor) {
    return editor ? editor->line_count : 0;
}

/// @brief Return true if the editor content has been modified since the last
///        vg_codeeditor_clear_modified call or vg_codeeditor_set_text call.
///
/// @param editor The code editor to query.
/// @return true if modified, false if clean or editor is NULL.
bool vg_codeeditor_is_modified(vg_codeeditor_t *editor) {
    return editor ? editor->modified : false;
}

/// @brief Clear the modified flag on the editor and all individual lines.
///
/// @details Typically called after saving the file to reset the dirty indicator.
///
/// @param editor The code editor to mark as clean.
void vg_codeeditor_clear_modified(vg_codeeditor_t *editor) {
    if (editor) {
        editor->modified = false;
        for (int i = 0; i < editor->line_count; i++) {
            editor->lines[i].modified = false;
        }
    }
}

/// @brief Reset all accumulated CodeEditor performance counters to zero.
///
/// @details Clears paint, layout, and helper counters collected in
///          vg_codeeditor_perf_stats_t without changing editor content or
///          invalidating layout. This is intended for tests and profiling
///          windows that need a fresh measurement interval.
///
/// @param editor The code editor whose counters should be reset; NULL is a no-op.
void vg_codeeditor_reset_perf_stats(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    memset(&editor->perf_stats, 0, sizeof(editor->perf_stats));
}

/// @brief Return a snapshot of accumulated CodeEditor performance counters.
///
/// @details The returned struct is a by-value copy, so callers can inspect it
///          without holding any editor lifetime guarantees. Passing NULL
///          returns an all-zero snapshot.
///
/// @param editor The code editor to inspect; may be NULL.
/// @return Current performance counter snapshot, or zeros for NULL.
vg_codeeditor_perf_stats_t vg_codeeditor_get_perf_stats(const vg_codeeditor_t *editor) {
    vg_codeeditor_perf_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (editor)
        stats = editor->perf_stats;
    return stats;
}
