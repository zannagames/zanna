//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/gui/src/widgets/vg_codeeditor_buffer.inc
// Purpose: Detachable per-document editor state (vg_editor_buffer_t): create,
//          destroy, swap in/out of a CodeEditor, and read-only accessors. Lets
//          many documents share one editor widget without losing undo history
//          or view state on every switch (plans 01/10/17).
// Key invariants:
//   - The swappable field set here mirrors vg_editor_buffer_t exactly.
//   - Swap moves ownership of arrays; it never copies line/history contents.
//   - View-only state (font, colors, layout caches, inlay/gutter overlays) is
//     invalidated/cleared on swap, never carried across documents.
// Links: src/lib/gui/include/vg_ide_widgets_editor.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements detachable code-editor document-state bundles.
/// @details Buffer swaps move owned line, history, selection, fold, token, and scroll storage
/// between a widget and a heap-allocated shell without copying document content. View-only caches
/// and overlays remain widget-local and are invalidated whenever the attached document changes.

// The document-state fields shared between vg_codeeditor_t and
// vg_editor_buffer_t. Keep in sync with the vg_editor_buffer_t declaration.
/// @brief Enumerate fields transferred between an editor and a detachable document buffer.
/// @param X Field-operation macro invoked once per transferred member.
#define EDBUF_FIELDS(X)                                                                             \
    X(lines) X(line_count) X(line_capacity) X(document_buffer) X(history) X(cursor_line)            \
        X(cursor_col) X(selection) X(has_selection) X(scroll_x) X(scroll_y) X(extra_cursors)        \
            X(extra_cursor_count) X(extra_cursor_cap) X(fold_regions) X(fold_region_count)          \
                X(fold_region_cap) X(has_folded_lines) X(semantic_tokens) X(semantic_token_count)   \
                    X(semantic_token_cap) X(semantic_tokens_sorted) X(highlight_spans)              \
                        X(highlight_span_count) X(highlight_span_cap) X(highlight_spans_sorted)     \
                            X(modified) X(revision) X(highlight_generation) X(typing_clock_ms)

/// @brief Move an editor's document state into a detachable buffer.
/// @details After field transfer, all editor-owned pointer/count groups are cleared so the widget
/// remains safe to destroy or populate with another buffer.
/// @param buf Empty destination buffer that assumes ownership of the document state.
/// @param ed Source editor left as an empty document shell.
static void editor_buffer_capture(vg_editor_buffer_t *buf, vg_codeeditor_t *ed) {
#define CAP(f) buf->f = ed->f;
    EDBUF_FIELDS(CAP)
#undef CAP
    // Null the moved-out owned pointers + counts so the editor is a valid empty
    // shell (safe to destroy or re-fill).
    ed->lines = NULL;
    ed->line_count = 0;
    ed->line_capacity = 0;
    ed->document_buffer = NULL;
    ed->history = NULL;
    ed->extra_cursors = NULL;
    ed->extra_cursor_count = 0;
    ed->extra_cursor_cap = 0;
    ed->fold_regions = NULL;
    ed->fold_region_count = 0;
    ed->fold_region_cap = 0;
    ed->semantic_tokens = NULL;
    ed->semantic_token_count = 0;
    ed->semantic_token_cap = 0;
    ed->highlight_spans = NULL;
    ed->highlight_span_count = 0;
    ed->highlight_span_cap = 0;
}

/// @brief Move a detachable buffer's document state into an editor.
/// @details Transferred owned pointers are cleared from @p buf, leaving only a shell that may be
/// freed without releasing the restored document.
/// @param ed Empty destination editor that assumes ownership of the document state.
/// @param buf Source buffer relinquishing all transferred storage.
static void editor_buffer_restore(vg_codeeditor_t *ed, vg_editor_buffer_t *buf) {
#define RES(f) ed->f = buf->f;
    EDBUF_FIELDS(RES)
#undef RES
    buf->lines = NULL;
    buf->document_buffer = NULL;
    buf->history = NULL;
    buf->extra_cursors = NULL;
    buf->fold_regions = NULL;
    buf->semantic_tokens = NULL;
    buf->highlight_spans = NULL;
}

/// @brief Create a detachable document buffer initialized from text.
/// @details A temporary editor constructs the complete line/history/document-buffer state, which
/// is then moved into the returned bundle. NULL or empty input creates one empty line.
/// @param text NUL-terminated UTF-8 document copied into the buffer, or NULL for empty content.
/// @return Newly allocated buffer owned by the caller, or NULL on allocation failure.
vg_editor_buffer_t *vg_editor_buffer_create(const char *text) {
    vg_codeeditor_t *tmp = vg_codeeditor_create(NULL);
    if (!tmp)
        return NULL;
    if (text && text[0])
        vg_codeeditor_set_text(tmp, text);
    vg_editor_buffer_t *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        vg_widget_destroy(&tmp->base);
        return NULL;
    }
    editor_buffer_capture(buf, tmp); // tmp left empty; buf owns the document state
    vg_widget_destroy(&tmp->base);
    return buf;
}

/// @brief Destroy a detached document buffer and all state it owns.
/// @param buf Buffer returned by @ref vg_editor_buffer_create or
/// @ref vg_codeeditor_swap_buffer; NULL is ignored.
void vg_editor_buffer_destroy(vg_editor_buffer_t *buf) {
    if (!buf)
        return;
    if (buf->lines) {
        for (int i = 0; i < buf->line_count; i++)
            free_line(&buf->lines[i]);
        free(buf->lines);
    }
    zanna_text_buffer_free((zanna_text_buffer_t *)buf->document_buffer);
    edit_history_destroy(buf->history);
    free(buf->highlight_spans);
    free(buf->semantic_tokens);
    free(buf->fold_regions);
    free(buf->extra_cursors);
    free(buf);
}

/// @brief Replace the document attached to an editor by moving in a detached buffer.
/// @details The function consumes and frees the @p incoming shell after moving its document state
/// into @p editor. The previous document is returned in a new detachable buffer. Document-specific
/// overlays and derived caches are cleared, transient interactions are reset, and the incremental
/// journal is marked for a required full synchronization.
/// @param editor Editor whose attached document is replaced.
/// @param incoming Buffer whose contents move into @p editor; consumed on success.
/// @return Caller-owned buffer containing the previous document, or NULL for invalid input or
/// allocation failure. On failure, @p incoming remains owned by the caller.
vg_editor_buffer_t *vg_codeeditor_swap_buffer(vg_codeeditor_t *editor,
                                              vg_editor_buffer_t *incoming) {
    if (!editor || !incoming)
        return NULL;
    vg_editor_buffer_t *prev = calloc(1, sizeof(*prev));
    if (!prev)
        return NULL;

    editor_buffer_capture(prev, editor);     // editor now empty; prev owns old state
    editor_buffer_restore(editor, incoming); // editor now owns incoming's state
    free(incoming);                          // arrays moved into editor; free the shell

    // Document-specific view overlays are NOT part of the buffer; clear them so
    // the previous document's hints/icons never bleed onto the new document
    // (the IDE re-applies them each frame for the active document).
    vg_codeeditor_clear_inlay_hints(editor);
    if (editor->gutter_icons) {
        for (int i = 0; i < editor->gutter_icon_count; i++)
            vg_icon_destroy(&editor->gutter_icons[i].image);
        free(editor->gutter_icons);
    }
    editor->gutter_icons = NULL;
    editor->gutter_icon_count = 0;
    editor->gutter_icon_cap = 0;

    // Invalidate all content/layout-derived view caches. The per-line syntax
    // color cache travels with the lines (its generation matches the swapped-in
    // highlight_generation), so already-highlighted lines are NOT re-lexed.
    codeeditor_bump_layout_generation(editor);
    editor->layout_cache_valid = false;
    editor->pair_match_cache_valid = false;
    editor->highlight_line_index_valid = false;
    editor->runtime_content_width_cache_valid = false;
    editor->longest_line_cols_generation = 0;

    // Reset transient interaction state.
    editor->scrollbar_dragging = false;
    editor->hscrollbar_dragging = false;
    editor->selection_dragging = false;
    editor->selection_drag_pending = false;
    editor->cursor_visible = true;
    editor->base.needs_paint = true;
    editor->base.needs_layout = true;

    // The sync journal is editor-resident, not part of the swapped buffer bundle,
    // so its deltas belong to the previous document. Force the next language pull
    // to full-sync the newly attached document.
    codeeditor_journal_mark_overflow(editor);
    return prev;
}

/// @brief Return a detached buffer's complete document text.
/// @details Line contents are joined with newline separators into a new NUL-terminated allocation.
/// @param buf Detached document buffer to read.
/// @return Caller-owned text, or NULL for invalid input or allocation failure.
char *vg_editor_buffer_get_text(vg_editor_buffer_t *buf) {
    if (!buf)
        return NULL;
    size_t total = 0;
    for (int i = 0; i < buf->line_count; i++)
        total += (size_t)buf->lines[i].length + 1u; // + separator/terminator budget
    char *out = malloc(total + 1u);
    if (!out)
        return NULL;
    size_t off = 0;
    for (int i = 0; i < buf->line_count; i++) {
        if (i > 0)
            out[off++] = '\n';
        if (buf->lines[i].length > 0) {
            memcpy(out + off, buf->lines[i].text, buf->lines[i].length);
            off += buf->lines[i].length;
        }
    }
    out[off] = '\0';
    return out;
}

/// @brief Return whether a detached document has unsaved modifications.
/// @param buf Buffer to inspect.
/// @return Stored modified flag, or `false` when @p buf is NULL.
bool vg_editor_buffer_is_modified(const vg_editor_buffer_t *buf) {
    return buf ? buf->modified : false;
}

/// @brief Mark a detached document as unmodified.
/// @param buf Buffer to update; NULL is ignored.
void vg_editor_buffer_clear_modified(vg_editor_buffer_t *buf) {
    if (buf)
        buf->modified = false;
}

/// @brief Return a detached document's monotonic content revision.
/// @param buf Buffer to inspect.
/// @return Stored revision, or zero when @p buf is NULL.
uint64_t vg_editor_buffer_get_revision(const vg_editor_buffer_t *buf) {
    return buf ? buf->revision : 0;
}
