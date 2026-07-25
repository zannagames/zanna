//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file vg_codeeditor_input.inc
/// @brief CodeEditor keyboard, mouse, wheel, selection gesture, popup, and
/// clipboard event handling.
///
/// @details This include fragment is consumed only by vg_codeeditor.c and keeps
/// event dispatch separate from text mutation primitives and the public API
/// layer that calls them.
///
// File: src/lib/gui/src/widgets/vg_codeeditor_input.inc
// Purpose: UTF-8 input encoding, indentation helpers, bracket-pair editing, backspace/delete
// handling, event dispatch, and focus hooks.
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

/// @brief Encodes @p cp as UTF-8 into @p buf (≥4 bytes); returns bytes written, or 0 for invalid
/// codepoints.
/// @param cp Unicode scalar value to encode.
/// @param[out] buf Destination with capacity for at least four bytes.
/// @return Encoded byte count, or zero for surrogates and out-of-range values.
static int encode_utf8(uint32_t cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        // Reject UTF-16 surrogates (U+D800–U+DFFF)
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0; // Out-of-range
}

/// @brief True if a character event should insert literal text instead of being treated as a
/// command shortcut.
/// @param event Character event whose modifier state is inspected.
/// @return `true` when no platform command modifier is active, including AltGr-style Ctrl+Alt.
static bool codeeditor_key_char_allows_text(const vg_event_t *event) {
    if (!event)
        return false;

    uint32_t mods = event->modifiers;
    bool has_super = (mods & VG_MOD_SUPER) != 0;
    bool has_ctrl = (mods & VG_MOD_CTRL) != 0;
    bool has_alt = (mods & VG_MOD_ALT) != 0;

    if (has_super)
        return false;
    if (has_ctrl && !has_alt)
        return false;
    if (has_alt && !has_ctrl)
        return false;
    return true;
}

/// @brief Return whether a Unicode scalar is accepted as editor text input.
/// @details C0/C1 controls, delete, surrogates, private-use scalars, and out-of-range values are
/// rejected before UTF-8 encoding.
/// @param cp Candidate Unicode scalar value.
/// @return `true` when the value is ordinary insertable text.
static bool codeeditor_codepoint_is_text(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F || cp > 0x10FFFF)
        return false;
    if (cp >= 0x80 && cp <= 0x9F)
        return false;
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return false;
    if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
        (cp >= 0x100000 && cp <= 0x10FFFD))
        return false;
    return true;
}

/// @brief Count the leading ASCII spaces and tabs in a line.
/// @param text Source line bytes.
/// @param len Number of readable bytes.
/// @return Leading indentation byte count.
static int codeeditor_leading_indent_len(const char *text, size_t len) {
    int i = 0;
    while ((size_t)i < len && (text[i] == ' ' || text[i] == '\t'))
        i++;
    return i;
}

/// @brief Find the last non-space, non-tab byte before a column.
/// @param text Source line bytes.
/// @param col Exclusive scan limit.
/// @return Byte index, or `-1` when the prefix is all indentation.
static int codeeditor_last_nonspace_before(const char *text, int col) {
    int i = col - 1;
    while (i >= 0 && (text[i] == ' ' || text[i] == '\t'))
        i--;
    return i;
}

/// @brief Return whether an opening delimiter increases automatic indentation.
/// @param ch Candidate byte.
/// @return `true` for an opening brace, parenthesis, or bracket.
static bool codeeditor_opener_adds_indent(char ch) {
    return ch == '{' || ch == '(' || ch == '[';
}

/// @brief Map an automatic-pair opener to its closing delimiter.
/// @param opener Candidate opening delimiter.
/// @return Matching closer, or NUL when unsupported.
static char codeeditor_closing_for_opener(char opener);

/// @brief Return true when @p ch is an ASCII identifier character.
/// @details Used by pair insertion heuristics to avoid inserting quotes in the
///          middle of words and to avoid adding paired punctuation immediately
///          before identifiers.
/// @param ch Candidate byte.
/// @return `true` for ASCII letters, digits, or underscore.
static bool codeeditor_is_identifier_byte(char ch) {
    unsigned char c = (unsigned char)ch;
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

#define CODEEDITOR_DRAG_START_THRESHOLD_PX 3.0f

/// @brief Assign the primary selection and place the caret at its end.
/// @details Coordinates are byte columns in the editor's internal UTF-8 line
///          buffers. Callers provide already-valid positions from hit testing
///          or boundary scanners; this helper normalizes only the selection
///          active flag and clears secondary cursor selections.
/// @param editor Editor whose primary selection should be changed.
/// @param start_line Start line of the range.
/// @param start_col Start byte column of the range.
/// @param end_line End line of the range.
/// @param end_col End byte column of the range; the caret lands here.
static void codeeditor_select_primary_range(
    vg_codeeditor_t *editor, int start_line, int start_col, int end_line, int end_col) {
    if (!editor)
        return;
    clamp_editor_position(editor, &start_line, &start_col);
    clamp_editor_position(editor, &end_line, &end_col);
    editor->selection.start_line = start_line;
    editor->selection.start_col = start_col;
    editor->selection.end_line = end_line;
    editor->selection.end_col = end_col;
    editor->cursor_line = end_line;
    editor->cursor_col = end_col;
    editor->has_selection = compare_positions(start_line, start_col, end_line, end_col) != 0;
    clear_extra_cursor_selections(editor);
    editor->cursor_visible = true;
    editor->base.needs_paint = true;
}

/// @brief Select the identifier, whitespace run, or single glyph under a hit-test position.
/// @details Double-clicking identifiers selects the full word. Double-clicking
///          whitespace selects the contiguous whitespace run; punctuation and
///          other bytes select one UTF-8 glyph. This mirrors common editor
///          behavior while keeping Zanna's identifier definition ASCII-based.
/// @param editor Editor to modify.
/// @param line_idx Zero-based line index from hit testing.
/// @param col Byte column from hit testing.
static void codeeditor_select_word_at(vg_codeeditor_t *editor, int line_idx, int col) {
    if (!editor || line_idx < 0 || line_idx >= editor->line_count)
        return;
    vg_code_line_t *line = &editor->lines[line_idx];
    int line_len = line->length > (size_t)INT_MAX ? INT_MAX : (int)line->length;
    if (line_len <= 0) {
        codeeditor_select_primary_range(editor, line_idx, 0, line_idx, 0);
        return;
    }
    if (col < 0)
        col = 0;
    if (col > line_len)
        col = line_len;

    int probe = col;
    if (probe == line_len)
        probe = codeeditor_prev_byte_boundary(editor, line_idx, line_len);
    if (probe > 0 && probe < line_len && !codeeditor_is_identifier_byte(line->text[probe]) &&
        codeeditor_is_identifier_byte(line->text[probe - 1])) {
        probe = codeeditor_prev_byte_boundary(editor, line_idx, probe);
    }
    if (probe < 0 || probe >= line_len)
        probe = 0;

    char target = line->text[probe];
    int start = probe;
    int end = codeeditor_next_byte_boundary(editor, line_idx, probe);
    if (codeeditor_is_identifier_byte(target)) {
        while (start > 0) {
            int prev = codeeditor_prev_byte_boundary(editor, line_idx, start);
            if (prev < 0 || !codeeditor_is_identifier_byte(line->text[prev]))
                break;
            start = prev;
        }
        while (end < line_len && codeeditor_is_identifier_byte(line->text[end])) {
            end = codeeditor_next_byte_boundary(editor, line_idx, end);
        }
    } else if (target == ' ' || target == '\t') {
        while (start > 0) {
            int prev = codeeditor_prev_byte_boundary(editor, line_idx, start);
            if (prev < 0 || (line->text[prev] != ' ' && line->text[prev] != '\t'))
                break;
            start = prev;
        }
        while (end < line_len && (line->text[end] == ' ' || line->text[end] == '\t')) {
            end = codeeditor_next_byte_boundary(editor, line_idx, end);
        }
    }
    codeeditor_select_primary_range(editor, line_idx, start, line_idx, end);
}

/// @brief Select the full logical line at @p line_idx.
/// @details Triple-clicking includes the trailing newline when one exists by
///          selecting from the clicked line's column zero to the next line's
///          column zero. The final line selects to its text length.
/// @param editor Editor to modify.
/// @param line_idx Zero-based line index to select.
static void codeeditor_select_line_at(vg_codeeditor_t *editor, int line_idx) {
    if (!editor || editor->line_count <= 0)
        return;
    if (line_idx < 0)
        line_idx = 0;
    if (line_idx >= editor->line_count)
        line_idx = editor->line_count - 1;
    if (line_idx + 1 < editor->line_count) {
        codeeditor_select_primary_range(editor, line_idx, 0, line_idx + 1, 0);
    } else {
        int end_col = (int)editor->lines[line_idx].length;
        codeeditor_select_primary_range(editor, line_idx, 0, line_idx, end_col);
    }
}

/// @brief Return the indentation text that should be added for one nesting level.
/// @details The returned text is stored in @p out and is always NUL-terminated.
///          It respects the editor's soft-tab setting and clamps extreme tab
///          widths to the same range as the rest of the editor input code.
/// @param editor Editor supplying tab width and soft-tab mode.
/// @param[out] out Destination byte buffer.
/// @param out_cap Capacity of @p out.
/// @param[out] out_len Optional destination for the produced byte count.
static void codeeditor_make_extra_indent(const vg_codeeditor_t *editor,
                                         char *out,
                                         size_t out_cap,
                                         int *out_len) {
    if (out_len)
        *out_len = 0;
    if (!out || out_cap == 0)
        return;
    out[0] = '\0';
    if (!editor)
        return;

    int extra_len = editor->use_spaces ? editor->tab_width : 1;
    if (extra_len < 1)
        extra_len = 1;
    if (extra_len > 16)
        extra_len = 16;

    if (editor->use_spaces) {
        size_t count = (size_t)extra_len;
        if (count >= out_cap)
            count = out_cap - 1;
        memset(out, ' ', count);
        out[count] = '\0';
        if (out_len)
            *out_len = (int)count;
    } else {
        out[0] = '\t';
        out[1 < out_cap ? 1 : 0] = '\0';
        if (out_len)
            *out_len = 1;
    }
}

/// @brief Return true when @p opener and @p closer form one of the editor's
///        automatic insertion pairs.
/// @param opener Candidate opening delimiter.
/// @param closer Candidate closing delimiter.
/// @return `true` when @p closer is the configured peer for @p opener.
static bool codeeditor_chars_are_pair(char opener, char closer) {
    return codeeditor_closing_for_opener(opener) == closer;
}

/// @brief Return true when the cursor is directly between an automatic pair.
/// @details This is the condition needed to split `{|}` into a three-line block
///          on Enter. The helper is intentionally byte-based because all pair
///          characters are single-byte ASCII.
/// @param editor Editor whose primary cursor and line are inspected.
/// @param[out] out_opener Optional destination for the opening delimiter.
/// @return `true` when the adjacent bytes form an automatic pair.
static bool codeeditor_cursor_between_pair(vg_codeeditor_t *editor, char *out_opener) {
    if (out_opener)
        *out_opener = '\0';
    if (!editor || editor->cursor_line < 0 || editor->cursor_line >= editor->line_count)
        return false;
    vg_code_line_t *line = &editor->lines[editor->cursor_line];
    int col = editor->cursor_col;
    if (col <= 0 || col >= (int)line->length)
        return false;
    char opener = line->text[col - 1];
    char closer = line->text[col];
    if (!codeeditor_chars_are_pair(opener, closer))
        return false;
    if (out_opener)
        *out_opener = opener;
    return true;
}

/// @brief Insert a split block when Enter is pressed between an automatic pair.
/// @details For `{|}` this inserts "\n<indent+level>\n<indent>" and restores the
///          cursor to the indented middle line. The underlying insertion remains
///          a single undo step; the latest history cursor is adjusted to the
///          user-visible final cursor position.
/// @param editor Editor receiving the split block.
/// @return `true` when the cursor was between an indenting pair and the block was inserted.
static bool codeeditor_try_split_pair_on_enter(vg_codeeditor_t *editor) {
    char opener = '\0';
    if (!codeeditor_cursor_between_pair(editor, &opener))
        return false;
    if (!codeeditor_opener_adds_indent(opener))
        return false;

    vg_code_line_t *line = &editor->lines[editor->cursor_line];
    int base_indent_len = codeeditor_leading_indent_len(line->text, line->length);
    char extra_indent[32];
    int extra_indent_len = 0;
    codeeditor_make_extra_indent(editor, extra_indent, sizeof(extra_indent), &extra_indent_len);

    size_t insert_len = 2u + (size_t)base_indent_len * 2u + (size_t)extra_indent_len;
    char *insert = malloc(insert_len + 1u);
    if (!insert)
        return false;

    char *p = insert;
    *p++ = '\n';
    if (base_indent_len > 0) {
        memcpy(p, line->text, (size_t)base_indent_len);
        p += base_indent_len;
    }
    if (extra_indent_len > 0) {
        memcpy(p, extra_indent, (size_t)extra_indent_len);
        p += extra_indent_len;
    }
    *p++ = '\n';
    if (base_indent_len > 0) {
        memcpy(p, line->text, (size_t)base_indent_len);
        p += base_indent_len;
    }
    *p = '\0';

    int cursor_line_after = editor->cursor_line + 1;
    int cursor_col_after = base_indent_len + extra_indent_len;
    vg_codeeditor_insert_text(editor, insert);
    editor->cursor_line = cursor_line_after;
    editor->cursor_col = cursor_col_after;
    edit_history_update_latest_cursor_after(
        editor->history, editor->cursor_line, editor->cursor_col);
    ensure_cursor_visible(editor);
    editor->cursor_visible = true;
    editor->base.needs_paint = true;
    free(insert);
    return true;
}

/// @brief Insert a newline with copied and optionally increased leading indentation.
/// @details Pair splitting takes precedence. Read-only edge cases, selections, multiple cursors,
/// or disabled auto-indent fall back to inserting a plain newline.
/// @param editor Editor receiving the newline.
static void codeeditor_insert_newline_with_indent(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    if (!editor->auto_indent || editor->has_selection || editor->extra_cursor_count > 0 ||
        editor->cursor_line < 0 || editor->cursor_line >= editor->line_count) {
        vg_codeeditor_insert_text(editor, "\n");
        return;
    }

    if (codeeditor_try_split_pair_on_enter(editor))
        return;

    vg_code_line_t *line = &editor->lines[editor->cursor_line];
    int line_len = line->length > (size_t)INT_MAX ? INT_MAX : (int)line->length;
    int col = editor->cursor_col;
    if (col < 0)
        col = 0;
    if (col > line_len)
        col = line_len;

    int indent_len = codeeditor_leading_indent_len(line->text, line->length);
    int extra_indent_len = 0;
    int last_nonspace = codeeditor_last_nonspace_before(line->text, col);
    char extra_indent[32];
    codeeditor_make_extra_indent(editor, extra_indent, sizeof(extra_indent), &extra_indent_len);
    if (last_nonspace < 0 || !codeeditor_opener_adds_indent(line->text[last_nonspace]))
        extra_indent_len = 0;

    size_t insert_len = 1u + (size_t)indent_len + (size_t)extra_indent_len;
    char *insert = malloc(insert_len + 1u);
    if (!insert) {
        vg_codeeditor_insert_text(editor, "\n");
        return;
    }

    char *p = insert;
    *p++ = '\n';
    if (indent_len > 0) {
        memcpy(p, line->text, (size_t)indent_len);
        p += indent_len;
    }
    if (extra_indent_len > 0) {
        memcpy(p, extra_indent, (size_t)extra_indent_len);
        p += extra_indent_len;
    }
    *p = '\0';

    vg_codeeditor_insert_text(editor, insert);
    free(insert);
}

/// @brief Move the primary caret one UTF-8 glyph left, crossing a line boundary if needed.
/// @param editor Editor whose primary caret should move.
static void codeeditor_move_cursor_left(vg_codeeditor_t *editor) {
    if (!editor || editor->line_count <= 0)
        return;
    if (editor->cursor_col > 0) {
        editor->cursor_col =
            codeeditor_prev_byte_boundary(editor, editor->cursor_line, editor->cursor_col);
    } else if (editor->cursor_line > 0) {
        editor->cursor_line--;
        editor->cursor_col = (int)editor->lines[editor->cursor_line].length;
    }
}

/// @brief Move the primary caret one UTF-8 glyph right, crossing a line boundary if needed.
/// @param editor Editor whose primary caret should move.
static void codeeditor_move_cursor_right(vg_codeeditor_t *editor) {
    if (!editor || editor->line_count <= 0)
        return;
    if (editor->cursor_col < (int)editor->lines[editor->cursor_line].length) {
        editor->cursor_col =
            codeeditor_next_byte_boundary(editor, editor->cursor_line, editor->cursor_col);
    } else if (editor->cursor_line < editor->line_count - 1) {
        editor->cursor_line++;
        editor->cursor_col = 0;
    }
}

/// @brief Classify a byte for word-wise editor movement.
/// @details The editor's word movement follows the same ASCII identifier
///          definition as double-click selection. Whitespace is treated as a
///          separable run; punctuation and non-ASCII glyph leading bytes form
///          compact punctuation runs.
/// @param ch Byte to classify.
/// @return 0 for whitespace, 1 for identifier bytes, 2 for punctuation/other.
static int codeeditor_word_class(char ch) {
    if (ch == ' ' || ch == '\t')
        return 0;
    return codeeditor_is_identifier_byte(ch) ? 1 : 2;
}

/// @brief Find the previous word boundary from a caret position.
/// @param editor Editor to inspect.
/// @param line In/out line index.
/// @param col In/out byte column.
static void codeeditor_previous_word_position(vg_codeeditor_t *editor, int *line, int *col) {
    if (!editor || !line || !col || editor->line_count <= 0)
        return;
    clamp_editor_position(editor, line, col);
    if (*col == 0) {
        if (*line > 0) {
            (*line)--;
            *col = (int)editor->lines[*line].length;
        }
        return;
    }

    vg_code_line_t *text_line = &editor->lines[*line];
    int pos = codeeditor_prev_byte_boundary(editor, *line, *col);
    while (pos > 0 && codeeditor_word_class(text_line->text[pos]) == 0)
        pos = codeeditor_prev_byte_boundary(editor, *line, pos);
    int klass = codeeditor_word_class(text_line->text[pos]);
    while (pos > 0) {
        int prev = codeeditor_prev_byte_boundary(editor, *line, pos);
        if (prev < 0 || codeeditor_word_class(text_line->text[prev]) != klass)
            break;
        pos = prev;
    }
    *col = pos;
}

/// @brief Find the next word boundary from a caret position.
/// @param editor Editor to inspect.
/// @param line In/out line index.
/// @param col In/out byte column.
static void codeeditor_next_word_position(vg_codeeditor_t *editor, int *line, int *col) {
    if (!editor || !line || !col || editor->line_count <= 0)
        return;
    clamp_editor_position(editor, line, col);
    int len = (int)editor->lines[*line].length;
    if (*col >= len) {
        if (*line < editor->line_count - 1) {
            (*line)++;
            *col = 0;
        }
        return;
    }

    vg_code_line_t *text_line = &editor->lines[*line];
    int pos = *col;
    int klass = codeeditor_word_class(text_line->text[pos]);
    while (pos < len && codeeditor_word_class(text_line->text[pos]) == klass)
        pos = codeeditor_next_byte_boundary(editor, *line, pos);
    while (pos < len && codeeditor_word_class(text_line->text[pos]) == 0)
        pos = codeeditor_next_byte_boundary(editor, *line, pos);
    *col = pos;
}

/// @brief Move the primary caret to the previous word boundary.
/// @param editor Editor whose primary caret should move.
static void codeeditor_move_cursor_word_left(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_previous_word_position(editor, &editor->cursor_line, &editor->cursor_col);
}

/// @brief Move the primary caret to the next word boundary.
/// @param editor Editor whose primary caret should move.
static void codeeditor_move_cursor_word_right(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_next_word_position(editor, &editor->cursor_line, &editor->cursor_col);
}

/// @brief Map an automatic-pair opener to its closing delimiter.
/// @param opener Candidate opening delimiter.
/// @return Matching closer, or NUL when unsupported.
static char codeeditor_closing_for_opener(char opener) {
    switch (opener) {
        case '(':
            return ')';
        case '[':
            return ']';
        case '{':
            return '}';
        case '"':
            return '"';
        case '\'':
            return '\'';
        default:
            return '\0';
    }
}

/// @brief Return whether a byte is a supported automatic-pair closer.
/// @param ch Candidate byte.
/// @return `true` for closing braces, brackets, parentheses, or either quote.
static bool codeeditor_is_pair_closer(char ch) {
    return ch == ')' || ch == ']' || ch == '}' || ch == '"' || ch == '\'';
}

/// @brief Test whether a byte range contains only ASCII spaces and tabs.
/// @param text Source bytes.
/// @param start First byte to inspect.
/// @param end Exclusive final byte.
/// @return `true` for a valid all-whitespace range.
static bool codeeditor_is_whitespace_span(const char *text, int start, int end) {
    if (!text || start < 0 || end < start)
        return false;
    for (int i = start; i < end; i++) {
        if (text[i] != ' ' && text[i] != '\t')
            return false;
    }
    return true;
}

/// @brief Reindent and insert a closing brace on an otherwise blank line.
/// @details The preferred indent matches the nearest unmatched opening brace; without one, a
/// single indentation level is removed. The entire line replacement is recorded through the
/// ordinary edit-target path.
/// @param editor Editor receiving the closing brace.
/// @return `true` when context permitted formatting and replacement was attempted.
static bool codeeditor_try_format_closing_brace(vg_codeeditor_t *editor) {
    if (!editor || !editor->auto_indent || editor->has_selection || editor->extra_cursor_count > 0)
        return false;
    if (editor->cursor_line < 0 || editor->cursor_line >= editor->line_count)
        return false;

    vg_code_line_t *line = &editor->lines[editor->cursor_line];
    int line_len = line->length > (size_t)INT_MAX ? INT_MAX : (int)line->length;
    int col = editor->cursor_col;
    if (col < 0 || col > line_len)
        return false;
    if (col == 0)
        return false;
    if (!codeeditor_is_whitespace_span(line->text, 0, col))
        return false;

    int replace_end = col;
    if (replace_end < line_len && line->text[replace_end] == '}')
        replace_end++;
    if (!codeeditor_is_whitespace_span(line->text, replace_end, line_len))
        return false;
    replace_end = line_len;

    int opener_line = -1;
    int opener_col = -1;
    int target_indent_len = 0;
    if (codeeditor_find_unmatched_opener_before(
            editor, editor->cursor_line, editor->cursor_col, '{', '}', &opener_line, &opener_col) &&
        opener_line >= 0 && opener_line < editor->line_count) {
        target_indent_len = codeeditor_leading_indent_len(editor->lines[opener_line].text,
                                                          editor->lines[opener_line].length);
    } else {
        target_indent_len = col;
        if (target_indent_len > 0 && line->text[target_indent_len - 1] == '\t') {
            target_indent_len--;
        } else {
            int spaces_to_remove = editor->tab_width;
            if (spaces_to_remove < 1)
                spaces_to_remove = 1;
            if (spaces_to_remove > 16)
                spaces_to_remove = 16;
            int removed = 0;
            while (target_indent_len > 0 && removed < spaces_to_remove &&
                   line->text[target_indent_len - 1] == ' ') {
                target_indent_len--;
                removed++;
            }
        }
    }
    if (target_indent_len < 0)
        target_indent_len = 0;
    if (target_indent_len > line_len)
        target_indent_len = line_len;

    size_t replacement_len = (size_t)target_indent_len + 1u;
    char *replacement = malloc(replacement_len + 1u);
    if (!replacement)
        return false;
    if (target_indent_len > 0)
        memcpy(replacement, line->text, (size_t)target_indent_len);
    replacement[target_indent_len] = '}';
    replacement[target_indent_len + 1] = '\0';

    vg_edit_target_t target = {0};
    int target_count = 0;
    add_edit_target(&target,
                    &target_count,
                    0,
                    editor->cursor_line,
                    editor->cursor_col,
                    editor->cursor_line,
                    0,
                    editor->cursor_line,
                    replace_end);
    apply_edit_targets(editor, &target, target_count, replacement);
    free(replacement);
    return true;
}

/// @brief Advance over an identical closer already present at the primary cursor.
/// @param editor Editor whose cursor may advance.
/// @param ch Typed closing delimiter.
/// @return `true` when the existing byte is skipped instead of inserted.
static bool codeeditor_try_skip_pair_closer(vg_codeeditor_t *editor, char ch) {
    if (!editor || editor->has_selection || editor->extra_cursor_count > 0)
        return false;
    if (!codeeditor_is_pair_closer(ch))
        return false;
    if (editor->cursor_line < 0 || editor->cursor_line >= editor->line_count)
        return false;
    vg_code_line_t *line = &editor->lines[editor->cursor_line];
    if (editor->cursor_col < 0 || editor->cursor_col >= (int)line->length)
        return false;
    if (line->text[editor->cursor_col] != ch)
        return false;
    editor->cursor_col++;
    editor->cursor_visible = true;
    editor->base.needs_paint = true;
    return true;
}

/// @brief Decide whether auto-pair insertion is appropriate at one caret.
/// @details The check intentionally stays conservative: it refuses to insert a
///          pair inside comments/strings, before identifiers, or for quotes in
///          the middle of identifier text. Returning false lets the caller fall
///          back to inserting the literal opener.
/// @param editor Editor containing the candidate caret context.
/// @param line_index Source line of the caret.
/// @param col Caret byte column.
/// @param opener Typed opening delimiter.
/// @return `true` when inserting both opener and closer is appropriate.
static bool codeeditor_should_insert_pair_at(vg_codeeditor_t *editor,
                                             int line_index,
                                             int col,
                                             char opener) {
    if (!editor || line_index < 0 || line_index >= editor->line_count)
        return false;
    char closer = codeeditor_closing_for_opener(opener);
    if (closer == '\0')
        return false;
    if (!codeeditor_position_is_code_context(editor, line_index, col))
        return false;

    vg_code_line_t *line = &editor->lines[line_index];
    char prev = col > 0 && col <= (int)line->length ? line->text[col - 1] : '\0';
    char next = col >= 0 && col < (int)line->length ? line->text[col] : '\0';
    if (next != '\0' && codeeditor_is_identifier_byte(next))
        return false;
    if ((opener == '"' || opener == '\'') && codeeditor_is_identifier_byte(prev))
        return false;
    return true;
}

/// @brief Return whether an edit target covers selected text rather than a caret.
/// @param target Edit target to inspect.
/// @return `true` when its normalized endpoints differ.
static bool codeeditor_edit_target_has_range(const vg_edit_target_t *target) {
    return target &&
           compare_positions(
               target->start_line, target->start_col, target->end_line, target->end_col) != 0;
}

/// @brief Build the replacement text for one pair operation.
/// @details Selection targets are wrapped as `<opener><selected><closer>`.
///          Caret targets insert either the opener/closer pair or the literal
///          opener depending on @p use_pair. The returned string is heap-owned.
/// @param editor Editor containing selected source text.
/// @param target Replacement range or caret.
/// @param opener Opening delimiter.
/// @param closer Closing delimiter.
/// @param use_pair Whether a caret target should receive both bytes.
/// @return Caller-owned replacement string, or NULL on allocation failure.
static char *codeeditor_pair_replacement_for_target(vg_codeeditor_t *editor,
                                                    const vg_edit_target_t *target,
                                                    char opener,
                                                    char closer,
                                                    bool use_pair) {
    if (!target)
        return NULL;
    if (codeeditor_edit_target_has_range(target)) {
        size_t selected_len = 0;
        char *selected = copy_text_range_len(editor,
                                             target->start_line,
                                             target->start_col,
                                             target->end_line,
                                             target->end_col,
                                             &selected_len);
        if (!selected)
            return NULL;
        if (selected_len > SIZE_MAX - 3u) {
            free(selected);
            return NULL;
        }
        char *replacement = malloc(selected_len + 3u);
        if (!replacement) {
            free(selected);
            return NULL;
        }
        replacement[0] = opener;
        memcpy(replacement + 1, selected, selected_len);
        replacement[selected_len + 1u] = closer;
        replacement[selected_len + 2u] = '\0';
        free(selected);
        return replacement;
    }

    char *replacement = malloc(use_pair ? 3u : 2u);
    if (!replacement)
        return NULL;
    replacement[0] = opener;
    if (use_pair) {
        replacement[1] = closer;
        replacement[2] = '\0';
    } else {
        replacement[1] = '\0';
    }
    return replacement;
}

/// @brief Insert or wrap an automatic pair for every active cursor.
/// @details Supports mixed selections and carets. At carets that pass the
///          context check, the cursor is restored between the inserted pair.
///          At selection targets, the selected text is wrapped and the cursor
///          lands after the closing delimiter.
/// @param editor Editor receiving the pair operation.
/// @param opener Typed opening delimiter.
/// @return `true` when at least one target warrants pair handling.
static bool codeeditor_try_insert_pair(vg_codeeditor_t *editor, char opener) {
    if (!editor)
        return false;
    char closer = codeeditor_closing_for_opener(opener);
    if (closer == '\0')
        return false;

    const int max_targets = 1 + editor->extra_cursor_count;
    vg_edit_target_t *targets = calloc((size_t)max_targets, sizeof(vg_edit_target_t));
    if (!targets) {
        free(targets);
        return false;
    }

    int target_count = 0;
    bool should_handle = false;
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
        should_handle = true;
    } else {
        bool use_pair = codeeditor_should_insert_pair_at(
            editor, editor->cursor_line, editor->cursor_col, opener);
        add_edit_target(targets,
                        &target_count,
                        0,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->cursor_line,
                        editor->cursor_col);
        should_handle = should_handle || use_pair;
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
            should_handle = true;
        } else {
            bool use_pair =
                codeeditor_should_insert_pair_at(editor, cursor->line, cursor->col, opener);
            add_edit_target(targets,
                            &target_count,
                            i + 1,
                            cursor->line,
                            cursor->col,
                            cursor->line,
                            cursor->col,
                            cursor->line,
                            cursor->col);
            should_handle = should_handle || use_pair;
        }
    }

    if (!should_handle || target_count <= 0) {
        free(targets);
        return false;
    }

    qsort(targets, (size_t)target_count, sizeof(*targets), edit_target_compare_desc);
    bool history_group_open = false;
    if (editor->history && target_count > 1) {
        edit_history_begin_group(editor->history);
        history_group_open = true;
    }
    for (int i = 0; i < target_count; i++) {
        bool has_range = codeeditor_edit_target_has_range(&targets[i]);
        bool use_pair =
            has_range || codeeditor_should_insert_pair_at(
                             editor, targets[i].start_line, targets[i].start_col, opener);
        char *replacement =
            codeeditor_pair_replacement_for_target(editor, &targets[i], opener, closer, use_pair);
        if (!replacement)
            continue;
        apply_edit_targets(editor, &targets[i], 1, replacement);
        if (!has_range && use_pair) {
            set_cursor_position_by_id(
                editor, targets[i].cursor_id, targets[i].start_line, targets[i].start_col + 1);
            edit_history_update_latest_cursor_after(
                editor->history, targets[i].start_line, targets[i].start_col + 1);
        }
        free(replacement);
    }
    if (history_group_open)
        edit_history_end_group(editor->history);

    ensure_cursor_visible(editor);
    editor->cursor_visible = true;
    editor->base.needs_paint = true;
    free(targets);
    return true;
}

/// @brief Insert raw bytes at the primary cursor without recording history.
/// @param editor Editor whose current line is modified.
/// @param bytes Source bytes.
/// @param n Number of bytes to insert.
VG_UNUSED static void insert_bytes(vg_codeeditor_t *editor, const char *bytes, size_t n) {
    if (!editor || !bytes || !n)
        return;
    codeeditor_clamp_cursor_to_visible(editor, &editor->cursor_line, &editor->cursor_col);
    vg_code_line_t *line = &editor->lines[editor->cursor_line];

    if (line->length > SIZE_MAX - 1 || n > SIZE_MAX - line->length - 1 ||
        line->length > (size_t)INT_MAX || n > (size_t)INT_MAX - line->length)
        return;
    if (!ensure_text_capacity(line, line->length + n + 1))
        return;

    memmove(line->text + editor->cursor_col + n,
            line->text + editor->cursor_col,
            line->length - editor->cursor_col + 1);

    memcpy(line->text + editor->cursor_col, bytes, n);
    line->length += n;
    editor->cursor_col += (int)n;
    editor->modified = true;
    line->modified = true;
    codeeditor_invalidate_line_highlight(editor, editor->cursor_line);
}

/// @brief Insert one raw byte at the primary cursor without recording history.
/// @param editor Editor whose current line is modified.
/// @param c Byte to insert.
VG_UNUSED static void insert_char(vg_codeeditor_t *editor, char c) {
    if (!editor)
        return;
    codeeditor_clamp_cursor_to_visible(editor, &editor->cursor_line, &editor->cursor_col);
    vg_code_line_t *line = &editor->lines[editor->cursor_line];

    if (line->length > SIZE_MAX - 2 || line->length >= (size_t)INT_MAX)
        return;
    if (!ensure_text_capacity(line, line->length + 2))
        return;

    // Make room for new char
    memmove(line->text + editor->cursor_col + 1,
            line->text + editor->cursor_col,
            line->length - editor->cursor_col + 1);

    line->text[editor->cursor_col] = c;
    line->length++;
    editor->cursor_col++;
    editor->modified = true;
    line->modified = true;
    codeeditor_invalidate_line_highlight(editor, editor->cursor_line);
}

/// @brief Inserts @p n bytes from @p bytes into @p line_idx at *col, advancing *col past the
/// inserted content.
/// @param editor Editor containing the destination line.
/// @param line_idx Zero-based destination line.
/// @param[in,out] col Byte column updated past the inserted bytes.
/// @param bytes Source bytes.
/// @param n Number of bytes to insert.
/// @return `true` when storage is updated.
static bool codeeditor_insert_bytes_at(
    vg_codeeditor_t *editor, int line_idx, int *col, const char *bytes, size_t n) {
    if (!editor || !bytes || !col || n == 0 || line_idx < 0 || line_idx >= editor->line_count)
        return false;

    vg_code_line_t *line = &editor->lines[line_idx];
    if (!ensure_line_text_exists(line))
        return false;
    if (*col < 0)
        *col = 0;
    int line_len = line->length > (size_t)INT_MAX ? INT_MAX : (int)line->length;
    if (*col > line_len)
        *col = line_len;

    if (line->length > SIZE_MAX - 1 || n > SIZE_MAX - line->length - 1 ||
        line->length > (size_t)INT_MAX || n > (size_t)INT_MAX - line->length)
        return false;
    if (n > (size_t)(INT_MAX - *col))
        return false;
    if (!ensure_text_capacity(line, line->length + n + 1))
        return false;

    memmove(line->text + *col + n, line->text + *col, line->length - (size_t)*col + 1);
    memcpy(line->text + *col, bytes, n);
    line->length += n;
    *col += (int)n;
    line->modified = true;
    codeeditor_invalidate_line_highlight(editor, line_idx);
    return true;
}

/// @brief Splits @p line_idx at @p col by inserting a new empty line and moving trailing text to
/// it.
/// @param editor Editor whose line array is modified.
/// @param line_idx Zero-based line to split.
/// @param col Byte column at which the new line begins.
/// @return `true` when the line array and both text buffers are updated.
static bool codeeditor_split_line_at(vg_codeeditor_t *editor, int line_idx, int col) {
    if (!editor || line_idx < 0 || line_idx >= editor->line_count)
        return false;
    if (editor->line_count == INT_MAX)
        return false;
    if (!ensure_line_capacity(editor, editor->line_count + 1))
        return false;

    vg_code_line_t *current = &editor->lines[line_idx];
    if (!ensure_line_text_exists(current))
        return false;
    if (col < 0)
        col = 0;
    int current_len = current->length > (size_t)INT_MAX ? INT_MAX : (int)current->length;
    if (col > current_len)
        col = current_len;

    size_t split_col = (size_t)col;
    size_t remaining = current->length - split_col;
    vg_code_line_t new_line;
    if (!init_line_text(&new_line, current->text + split_col, remaining))
        return false;

    if (line_idx + 1 < editor->line_count) {
        memmove(&editor->lines[line_idx + 2],
                &editor->lines[line_idx + 1],
                (size_t)(editor->line_count - line_idx - 1) * sizeof(vg_code_line_t));
    }
    editor->lines[line_idx + 1] = new_line;
    current = &editor->lines[line_idx];
    current->text[split_col] = '\0';
    current->length = split_col;
    current->modified = true;
    editor->lines[line_idx + 1].modified = true;
    codeeditor_invalidate_line_highlight(editor, line_idx);
    codeeditor_invalidate_line_highlight(editor, line_idx + 1);
    editor->line_count++;
    return true;
}

/// @brief Split the primary cursor's line and move the cursor to the new line.
/// @param editor Editor to modify.
VG_UNUSED static void insert_newline(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_clamp_cursor_to_visible(editor, &editor->cursor_line, &editor->cursor_col);
    if (!codeeditor_split_line_at(editor, editor->cursor_line, editor->cursor_col))
        return;

    editor->cursor_line++;
    editor->cursor_col = 0;
    editor->modified = true;

    vg_codeeditor_refresh_layout_state(editor);
}

/// @brief Delete one byte before the primary cursor or join with the previous line.
/// @param editor Editor to modify.
VG_UNUSED static void delete_char_backward(vg_codeeditor_t *editor) {
    if (!editor)
        return;
    codeeditor_clamp_cursor_to_visible(editor, &editor->cursor_line, &editor->cursor_col);
    if (editor->cursor_col > 0) {
        vg_code_line_t *line = &editor->lines[editor->cursor_line];
        memmove(line->text + editor->cursor_col - 1,
                line->text + editor->cursor_col,
                line->length - editor->cursor_col + 1);
        line->length--;
        editor->cursor_col--;
        editor->modified = true;
        line->modified = true;
        codeeditor_invalidate_line_highlight(editor, editor->cursor_line);
    } else if (editor->cursor_line > 0) {
        // Join with previous line
        vg_code_line_t *current = &editor->lines[editor->cursor_line];
        vg_code_line_t *prev = &editor->lines[editor->cursor_line - 1];
        int old_line_count = editor->line_count;

        size_t new_col = prev->length;

        if (prev->length > (size_t)INT_MAX || current->length > (size_t)INT_MAX - prev->length ||
            !ensure_text_capacity(prev, prev->length + current->length + 1))
            return;

        memcpy(prev->text + prev->length, current->text, current->length + 1);
        prev->length += current->length;

        // Free current line
        free_line(current);

        // Move lines up
        memmove(&editor->lines[editor->cursor_line],
                &editor->lines[editor->cursor_line + 1],
                (editor->line_count - editor->cursor_line - 1) * sizeof(vg_code_line_t));

        editor->line_count--;
        if (old_line_count > editor->line_count) {
            memset(&editor->lines[editor->line_count], 0, sizeof(vg_code_line_t));
        }
        editor->cursor_line--;
        editor->cursor_col = (int)new_col;
        editor->modified = true;
        prev->modified = true;
        codeeditor_invalidate_line_highlight(editor, editor->cursor_line);

        vg_codeeditor_refresh_layout_state(editor);
    }
}

/// @brief Find the two-character delete range for Backspace between an automatic pair.
/// @details When the caret is between an automatic ASCII pair, the range spans
///          both delimiters so a single Backspace removes the pair together.
/// @param editor Editor containing the candidate pair.
/// @param line_index Source line of the caret.
/// @param col Caret byte column between the delimiters.
/// @param[out] out_start_col Optional destination for the opening-delimiter column.
/// @param[out] out_end_col Optional destination just after the closing delimiter.
/// @return `true` when adjacent bytes form a supported pair.
static bool codeeditor_backspace_pair_range(
    vg_codeeditor_t *editor, int line_index, int col, int *out_start_col, int *out_end_col) {
    if (out_start_col)
        *out_start_col = col;
    if (out_end_col)
        *out_end_col = col;
    if (!editor || line_index < 0 || line_index >= editor->line_count || col <= 0)
        return false;
    vg_code_line_t *line = &editor->lines[line_index];
    if (col >= (int)line->length)
        return false;
    int prev_col = codeeditor_prev_byte_boundary(editor, line_index, col);
    if (prev_col < 0 || prev_col >= col)
        return false;
    char opener = line->text[prev_col];
    char closer = line->text[col];
    if (!codeeditor_chars_are_pair(opener, closer))
        return false;
    if (out_start_col)
        *out_start_col = prev_col;
    if (out_end_col)
        *out_end_col = col + 1;
    return true;
}

/// @brief Performs a Backspace deletion across all cursors.
/// @details Deletes selections, removes paired delimiters when the caret is
///          between them, removes the previous UTF-8 codepoint otherwise, or
///          joins with the previous line at column zero.
/// @param editor Editor whose active cursor targets are deleted.
static void delete_backspace_targets(vg_codeeditor_t *editor) {
    if (!editor)
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
    } else if (editor->cursor_col > 0) {
        int delete_start = 0;
        int delete_end = 0;
        if (!codeeditor_backspace_pair_range(
                editor, editor->cursor_line, editor->cursor_col, &delete_start, &delete_end)) {
            delete_start =
                codeeditor_prev_byte_boundary(editor, editor->cursor_line, editor->cursor_col);
            delete_end = editor->cursor_col;
        }
        add_edit_target(targets,
                        &target_count,
                        0,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->cursor_line,
                        delete_start,
                        editor->cursor_line,
                        delete_end);
    } else if (editor->cursor_line > 0) {
        add_edit_target(targets,
                        &target_count,
                        0,
                        editor->cursor_line,
                        editor->cursor_col,
                        editor->cursor_line - 1,
                        (int)editor->lines[editor->cursor_line - 1].length,
                        editor->cursor_line,
                        0);
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
        } else if (cursor->col > 0) {
            int delete_start = 0;
            int delete_end = 0;
            if (!codeeditor_backspace_pair_range(
                    editor, cursor->line, cursor->col, &delete_start, &delete_end)) {
                delete_start = codeeditor_prev_byte_boundary(editor, cursor->line, cursor->col);
                delete_end = cursor->col;
            }
            add_edit_target(targets,
                            &target_count,
                            i + 1,
                            cursor->line,
                            cursor->col,
                            cursor->line,
                            delete_start,
                            cursor->line,
                            delete_end);
        } else if (cursor->line > 0) {
            add_edit_target(targets,
                            &target_count,
                            i + 1,
                            cursor->line,
                            cursor->col,
                            cursor->line - 1,
                            (int)editor->lines[cursor->line - 1].length,
                            cursor->line,
                            0);
        }
    }

    qsort(targets, (size_t)target_count, sizeof(*targets), edit_target_compare_desc);
    apply_edit_targets(editor, targets, target_count, NULL);
    free(targets);
}

/// @brief Append a forward-delete range for one cursor.
/// @details Selections are deleted as-is. Carets delete either one glyph, the
///          next word run, or the newline between the current and next line.
/// @param editor Editor that owns the cursor.
/// @param targets Target array to append to.
/// @param target_count In/out number of populated targets.
/// @param cursor_id 0 for primary cursor, 1+ for extra cursors.
/// @param cursor_line Cursor line before deletion.
/// @param cursor_col Cursor byte column before deletion.
/// @param has_selection Whether this cursor has a selection range.
/// @param selection Selection range to delete when @p has_selection is true.
/// @param by_word True to delete to the next word boundary instead of one glyph.
static void add_forward_delete_target(vg_codeeditor_t *editor,
                                      vg_edit_target_t *targets,
                                      int *target_count,
                                      int cursor_id,
                                      int cursor_line,
                                      int cursor_col,
                                      bool has_selection,
                                      vg_selection_t selection,
                                      bool by_word) {
    if (!editor || !targets || !target_count)
        return;
    if (has_selection) {
        add_edit_target(targets,
                        target_count,
                        cursor_id,
                        cursor_line,
                        cursor_col,
                        selection.start_line,
                        selection.start_col,
                        selection.end_line,
                        selection.end_col);
        return;
    }

    int end_line = cursor_line;
    int end_col = cursor_col;
    clamp_editor_position(editor, &end_line, &end_col);
    if (by_word) {
        codeeditor_next_word_position(editor, &end_line, &end_col);
    } else if (end_col < (int)editor->lines[end_line].length) {
        end_col = codeeditor_next_byte_boundary(editor, end_line, end_col);
    } else if (end_line < editor->line_count - 1) {
        end_line++;
        end_col = 0;
    }
    if (compare_positions(cursor_line, cursor_col, end_line, end_col) != 0) {
        add_edit_target(targets,
                        target_count,
                        cursor_id,
                        cursor_line,
                        cursor_col,
                        cursor_line,
                        cursor_col,
                        end_line,
                        end_col);
    }
}

/// @brief Delete forward across all active cursors.
/// @param editor Editor to mutate.
/// @param by_word True to delete to the next word boundary.
static void delete_forward_targets(vg_codeeditor_t *editor, bool by_word) {
    if (!editor)
        return;

    const int max_targets = 1 + editor->extra_cursor_count;
    vg_edit_target_t *targets = calloc((size_t)max_targets, sizeof(vg_edit_target_t));
    if (!targets)
        return;

    int target_count = 0;
    add_forward_delete_target(editor,
                              targets,
                              &target_count,
                              0,
                              editor->cursor_line,
                              editor->cursor_col,
                              editor->has_selection,
                              editor->selection,
                              by_word);
    for (int i = 0; i < editor->extra_cursor_count; i++) {
        struct vg_extra_cursor *cursor = &editor->extra_cursors[i];
        add_forward_delete_target(editor,
                                  targets,
                                  &target_count,
                                  i + 1,
                                  cursor->line,
                                  cursor->col,
                                  cursor->has_selection,
                                  cursor->selection,
                                  by_word);
    }

    qsort(targets, (size_t)target_count, sizeof(*targets), edit_target_compare_desc);
    apply_edit_targets(editor, targets, target_count, NULL);
    free(targets);
}

/// @brief Append a backward word-delete range for one cursor.
/// @param editor Editor that owns the cursor.
/// @param targets Target array to append to.
/// @param target_count In/out number of populated targets.
/// @param cursor_id 0 for primary cursor, 1+ for extra cursors.
/// @param cursor_line Cursor line before deletion.
/// @param cursor_col Cursor byte column before deletion.
/// @param has_selection Whether this cursor has a selection range.
/// @param selection Selection range to delete when @p has_selection is true.
static void add_backward_word_delete_target(vg_codeeditor_t *editor,
                                            vg_edit_target_t *targets,
                                            int *target_count,
                                            int cursor_id,
                                            int cursor_line,
                                            int cursor_col,
                                            bool has_selection,
                                            vg_selection_t selection) {
    if (!editor || !targets || !target_count)
        return;
    if (has_selection) {
        add_edit_target(targets,
                        target_count,
                        cursor_id,
                        cursor_line,
                        cursor_col,
                        selection.start_line,
                        selection.start_col,
                        selection.end_line,
                        selection.end_col);
        return;
    }
    int start_line = cursor_line;
    int start_col = cursor_col;
    codeeditor_previous_word_position(editor, &start_line, &start_col);
    if (compare_positions(start_line, start_col, cursor_line, cursor_col) != 0) {
        add_edit_target(targets,
                        target_count,
                        cursor_id,
                        cursor_line,
                        cursor_col,
                        start_line,
                        start_col,
                        cursor_line,
                        cursor_col);
    }
}

/// @brief Delete backward to the previous word boundary across all active cursors.
/// @param editor Editor to mutate.
static void delete_word_backward_targets(vg_codeeditor_t *editor) {
    if (!editor)
        return;

    const int max_targets = 1 + editor->extra_cursor_count;
    vg_edit_target_t *targets = calloc((size_t)max_targets, sizeof(vg_edit_target_t));
    if (!targets)
        return;

    int target_count = 0;
    add_backward_word_delete_target(editor,
                                    targets,
                                    &target_count,
                                    0,
                                    editor->cursor_line,
                                    editor->cursor_col,
                                    editor->has_selection,
                                    editor->selection);
    for (int i = 0; i < editor->extra_cursor_count; i++) {
        struct vg_extra_cursor *cursor = &editor->extra_cursors[i];
        add_backward_word_delete_target(editor,
                                        targets,
                                        &target_count,
                                        i + 1,
                                        cursor->line,
                                        cursor->col,
                                        cursor->has_selection,
                                        cursor->selection);
    }

    qsort(targets, (size_t)target_count, sizeof(*targets), edit_target_compare_desc);
    apply_edit_targets(editor, targets, target_count, NULL);
    free(targets);
}

/// @brief Choose the fixed selection anchor before keyboard navigation.
/// @details When extending an existing selection, the anchor is the endpoint
///          opposite the current caret. Without an existing selection, the
///          current caret becomes the anchor.
/// @param editor Editor whose primary selection is being extended.
/// @param out_line Receives the anchor line.
/// @param out_col Receives the anchor byte column.
static void codeeditor_keyboard_selection_anchor(vg_codeeditor_t *editor,
                                                 int *out_line,
                                                 int *out_col) {
    if (!editor || !out_line || !out_col)
        return;
    *out_line = editor->cursor_line;
    *out_col = editor->cursor_col;
    if (!editor->has_selection)
        return;
    if (compare_positions(editor->cursor_line,
                          editor->cursor_col,
                          editor->selection.start_line,
                          editor->selection.start_col) == 0) {
        *out_line = editor->selection.end_line;
        *out_col = editor->selection.end_col;
    } else {
        *out_line = editor->selection.start_line;
        *out_col = editor->selection.start_col;
    }
}

/// @brief Finalize selection state after keyboard navigation.
/// @details For Shift-modified movement, this creates or extends the primary
///          selection from @p anchor_line/@p anchor_col to the new caret. For
///          unmodified movement it clears primary and extra cursor selections.
/// @param editor Editor to update.
/// @param selecting True when Shift is held for this navigation command.
/// @param anchor_line Fixed selection anchor line.
/// @param anchor_col Fixed selection anchor byte column.
static void codeeditor_finish_keyboard_navigation(vg_codeeditor_t *editor,
                                                  bool selecting,
                                                  int anchor_line,
                                                  int anchor_col) {
    if (!editor)
        return;
    if (selecting) {
        editor->selection.start_line = anchor_line;
        editor->selection.start_col = anchor_col;
        editor->selection.end_line = editor->cursor_line;
        editor->selection.end_col = editor->cursor_col;
        editor->has_selection =
            compare_positions(anchor_line, anchor_col, editor->cursor_line, editor->cursor_col) !=
            0;
        clear_extra_cursor_selections(editor);
    } else {
        editor->has_selection = false;
        clear_extra_cursor_selections(editor);
    }
}

/// @brief Extend the active pointer selection to the widget-local point (x, y).
/// @details Maps the point to a document position, sets the selection from the
///          drag anchor to that position, moves the cursor there, and marks the
///          widget for repaint. Shared by the MOUSE_MOVE handler (live pointer)
///          and the per-frame edge-autoscroll tick (last known pointer), so the
///          selection math lives in exactly one place.
/// @param editor Editor whose primary selection is extended.
/// @param widget Editor widget defining local-to-document geometry.
/// @param x Pointer x coordinate in widget-local space.
/// @param y Pointer y coordinate in widget-local space.
static void codeeditor_update_drag_selection(vg_codeeditor_t *editor,
                                             vg_widget_t *widget,
                                             float x,
                                             float y) {
    int line = 0;
    int col = 0;
    codeeditor_local_point_to_position(editor, widget, x, y, &line, &col);
    editor->selection.start_line = editor->selection_anchor_line;
    editor->selection.start_col = editor->selection_anchor_col;
    editor->selection.end_line = line;
    editor->selection.end_col = col;
    editor->cursor_line = line;
    editor->cursor_col = col;
    editor->has_selection = compare_positions(editor->selection.start_line,
                                              editor->selection.start_col,
                                              editor->selection.end_line,
                                              editor->selection.end_col) != 0;
    editor->cursor_visible = true;
    widget->needs_paint = true;
}

/// @brief Advance edge-autoscroll while a content-area selection drag is active.
/// @details When the pointer is held beyond the editor's top/bottom edge (or,
///          with word wrap off, the left/right content edge) the view keeps
///          scrolling even though no MOUSE_MOVE events arrive. Scroll speed is
///          proportional to how far past the edge the pointer sits, clamped to a
///          sane maximum. After scrolling, the selection is re-extended to the
///          last known pointer position so the highlighted range grows with the
///          view. ensure_cursor_visible() is intentionally NOT called here: it
///          would fight the proportional speed. codeeditor_clamp_scroll() bounds
///          the result. No-op unless a drag is in progress.
/// @param editor Code editor.
/// @param widget Editor widget (for viewport metrics).
/// @param dt     Seconds elapsed since the previous frame.
static void codeeditor_drag_autoscroll_tick(vg_codeeditor_t *editor,
                                            vg_widget_t *widget,
                                            float dt) {
    if (!editor || !widget || !editor->selection_dragging)
        return;
    if (dt <= 0.0f)
        return;

    float viewport_height = widget->height;
    float line_height = editor->line_height > 0.0f ? editor->line_height : 1.0f;
    float x = editor->selection_last_drag_x;
    float y = editor->selection_last_drag_y;
    bool scrolled = false;

    // Vertical edge autoscroll.
    float overshoot_y = 0.0f;
    if (y < 0.0f)
        overshoot_y = y; // negative: scroll up
    else if (y > viewport_height)
        overshoot_y = y - viewport_height; // positive: scroll down
    if (overshoot_y != 0.0f) {
        float mag = overshoot_y < 0.0f ? -overshoot_y : overshoot_y;
        float lines_per_sec = mag / line_height;
        if (lines_per_sec < 1.0f)
            lines_per_sec = 1.0f;
        if (lines_per_sec > 3.0f)
            lines_per_sec = 3.0f;
        lines_per_sec *= CODEEDITOR_AUTOSCROLL_MAX_LPS / 3.0f;
        float delta = (overshoot_y < 0.0f ? -1.0f : 1.0f) * lines_per_sec * line_height * dt;
        editor->selection_autoscroll_accum_y += delta;
        float whole = (float)(int)editor->selection_autoscroll_accum_y;
        if (whole != 0.0f) {
            editor->scroll_y += whole;
            editor->selection_autoscroll_accum_y -= whole;
            scrolled = true;
        }
    } else {
        editor->selection_autoscroll_accum_y = 0.0f;
    }

    // Horizontal edge autoscroll (skipped in word-wrap mode).
    if (!editor->word_wrap) {
        float char_width = editor->char_width > 0.0f ? editor->char_width : 1.0f;
        float left_edge = editor->gutter_width;
        float right_edge = widget->width - CODEEDITOR_SCROLLBAR_WIDTH;
        float overshoot_x = 0.0f;
        if (x < left_edge)
            overshoot_x = x - left_edge; // negative: scroll left
        else if (x > right_edge)
            overshoot_x = x - right_edge; // positive: scroll right
        if (overshoot_x != 0.0f) {
            float mag = overshoot_x < 0.0f ? -overshoot_x : overshoot_x;
            float cols_per_sec = mag / char_width;
            if (cols_per_sec < 1.0f)
                cols_per_sec = 1.0f;
            if (cols_per_sec > 3.0f)
                cols_per_sec = 3.0f;
            cols_per_sec *= CODEEDITOR_AUTOSCROLL_MAX_LPS / 3.0f;
            float delta = (overshoot_x < 0.0f ? -1.0f : 1.0f) * cols_per_sec * char_width * dt;
            editor->selection_autoscroll_accum_x += delta;
            float whole = (float)(int)editor->selection_autoscroll_accum_x;
            if (whole != 0.0f) {
                editor->scroll_x += whole;
                if (editor->scroll_x < 0.0f)
                    editor->scroll_x = 0.0f;
                editor->selection_autoscroll_accum_x -= whole;
                scrolled = true;
            }
        } else {
            editor->selection_autoscroll_accum_x = 0.0f;
        }
    }

    if (scrolled) {
        codeeditor_clamp_scroll(editor, widget);
        codeeditor_update_drag_selection(editor, widget, x, y);
    }
}

/// @brief VTable handle_event: routes mouse (click/drag/scroll/scrollbar) and keyboard events to
/// the appropriate editor action.
/// @param widget Code-editor base widget receiving the event.
/// @param event Pointer, wheel, text, or keyboard event to dispatch.
/// @return `true` when the editor consumes the event, otherwise `false`.
static bool codeeditor_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_codeeditor_t *editor = (vg_codeeditor_t *)widget;

    if (widget->state & VG_STATE_DISABLED) {
        return false;
    }

    switch (event->type) {
        case VG_EVENT_MOUSE_UP:
            if (editor->scrollbar_dragging) {
                editor->scrollbar_dragging = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                widget->needs_paint = true;
                return true;
            }
            if (editor->hscrollbar_dragging) {
                editor->hscrollbar_dragging = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                widget->needs_paint = true;
                return true;
            }
            if (editor->selection_dragging) {
                editor->selection_dragging = false;
                editor->selection_autoscroll_accum_y = 0.0f;
                editor->selection_autoscroll_accum_x = 0.0f;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                widget->needs_paint = true;
                return true;
            }
            if (editor->selection_drag_pending) {
                editor->selection_drag_pending = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                widget->needs_paint = true;
                return false;
            }
            break;

        case VG_EVENT_MOUSE_MOVE:
            if (editor->scrollbar_dragging) {
                float thumb_y = 0.0f;
                float thumb_height = 0.0f;
                float max_scroll = 0.0f;
                float thumb_travel = 0.0f;
                if (codeeditor_get_scrollbar_metrics(editor,
                                                     widget,
                                                     NULL,
                                                     &thumb_y,
                                                     &thumb_height,
                                                     &max_scroll,
                                                     &thumb_travel) &&
                    thumb_travel > 0.0f && max_scroll > 0.0f) {
                    float target_thumb_y = event->mouse.y - editor->scrollbar_drag_offset;
                    if (target_thumb_y < 0.0f)
                        target_thumb_y = 0.0f;
                    if (target_thumb_y > thumb_travel)
                        target_thumb_y = thumb_travel;
                    editor->scroll_y = (target_thumb_y / thumb_travel) * max_scroll;
                }
                codeeditor_clamp_scroll(editor, widget);
                widget->needs_paint = true;
                return true;
            }
            if (editor->hscrollbar_dragging) {
                float thumb_x = 0.0f;
                float thumb_width = 0.0f;
                float max_scroll_x = 0.0f;
                float thumb_travel = 0.0f;
                if (codeeditor_get_hscrollbar_metrics(editor,
                                                      widget,
                                                      NULL,
                                                      &thumb_x,
                                                      &thumb_width,
                                                      &max_scroll_x,
                                                      &thumb_travel) &&
                    thumb_travel > 0.0f && max_scroll_x > 0.0f) {
                    float target_thumb_x =
                        (event->mouse.x - editor->hscrollbar_drag_offset) - editor->gutter_width;
                    if (target_thumb_x < 0.0f)
                        target_thumb_x = 0.0f;
                    if (target_thumb_x > thumb_travel)
                        target_thumb_x = thumb_travel;
                    editor->scroll_x = (target_thumb_x / thumb_travel) * max_scroll_x;
                }
                codeeditor_clamp_scroll(editor, widget);
                widget->needs_paint = true;
                return true;
            }
            if (editor->selection_drag_pending) {
                float dx = event->mouse.x - editor->selection_drag_start_x;
                float dy = event->mouse.y - editor->selection_drag_start_y;
                float threshold_sq =
                    CODEEDITOR_DRAG_START_THRESHOLD_PX * CODEEDITOR_DRAG_START_THRESHOLD_PX;
                if (dx * dx + dy * dy < threshold_sq)
                    return true;
                editor->selection_drag_pending = false;
                editor->selection_dragging = true;
            }
            if (editor->selection_dragging) {
                editor->selection_last_drag_x = event->mouse.x;
                editor->selection_last_drag_y = event->mouse.y;
                codeeditor_update_drag_selection(editor, widget, event->mouse.x, event->mouse.y);
                ensure_cursor_visible(editor);
                return true;
            }
            break;

        case VG_EVENT_MOUSE_DOWN: {
            float scrollbar_width = CODEEDITOR_SCROLLBAR_WIDTH;

            // Check if click is on scrollbar
            float track_x = 0.0f;
            float thumb_y = 0.0f;
            float thumb_height = 0.0f;
            float max_scroll = 0.0f;
            float thumb_travel = 0.0f;
            if (codeeditor_get_scrollbar_metrics(editor,
                                                 widget,
                                                 &track_x,
                                                 &thumb_y,
                                                 &thumb_height,
                                                 &max_scroll,
                                                 &thumb_travel) &&
                event->mouse.x >= track_x && event->mouse.x < track_x + scrollbar_width &&
                event->mouse.y >= 0.0f && event->mouse.y < widget->height) {
                if (event->mouse.y >= thumb_y && event->mouse.y <= thumb_y + thumb_height) {
                    // Click on thumb — start drag
                    editor->scrollbar_dragging = true;
                    editor->scrollbar_drag_offset = event->mouse.y - thumb_y;
                    editor->scrollbar_drag_start_scroll = editor->scroll_y;
                    vg_widget_set_input_capture(widget);
                } else {
                    // Click on track — jump to position
                    float target_thumb_y = event->mouse.y - thumb_height * 0.5f;
                    if (target_thumb_y < 0.0f)
                        target_thumb_y = 0.0f;
                    if (target_thumb_y > thumb_travel)
                        target_thumb_y = thumb_travel;
                    editor->scroll_y =
                        thumb_travel > 0.0f ? (target_thumb_y / thumb_travel) * max_scroll : 0.0f;
                    codeeditor_clamp_scroll(editor, widget);
                }
                widget->needs_paint = true;
                return true;
            }

            // Horizontal scrollbar along the bottom edge (checked after the
            // vertical bar so the corner belongs to the vertical thumb).
            float htrack_y = 0.0f;
            float hthumb_x = 0.0f;
            float hthumb_width = 0.0f;
            float hmax_scroll = 0.0f;
            float hthumb_travel = 0.0f;
            if (codeeditor_get_hscrollbar_metrics(editor,
                                                  widget,
                                                  &htrack_y,
                                                  &hthumb_x,
                                                  &hthumb_width,
                                                  &hmax_scroll,
                                                  &hthumb_travel) &&
                event->mouse.y >= htrack_y && event->mouse.y < htrack_y + scrollbar_width &&
                event->mouse.x >= editor->gutter_width &&
                event->mouse.x < widget->width - scrollbar_width) {
                if (event->mouse.x >= hthumb_x && event->mouse.x <= hthumb_x + hthumb_width) {
                    // Click on thumb — start drag.
                    editor->hscrollbar_dragging = true;
                    editor->hscrollbar_drag_offset = event->mouse.x - hthumb_x;
                    editor->hscrollbar_drag_start_scroll = editor->scroll_x;
                    vg_widget_set_input_capture(widget);
                } else {
                    // Click on track — jump.
                    float target_thumb_x =
                        (event->mouse.x - editor->gutter_width) - hthumb_width * 0.5f;
                    if (target_thumb_x < 0.0f)
                        target_thumb_x = 0.0f;
                    if (target_thumb_x > hthumb_travel)
                        target_thumb_x = hthumb_travel;
                    editor->scroll_x = hthumb_travel > 0.0f
                                           ? (target_thumb_x / hthumb_travel) * hmax_scroll
                                           : 0.0f;
                    codeeditor_clamp_scroll(editor, widget);
                }
                widget->needs_paint = true;
                return true;
            }

            if (editor->gutter_width > 0.0f && event->mouse.x < editor->gutter_width) {
                int line = 0;
                codeeditor_local_point_to_position(
                    editor, widget, event->mouse.x, event->mouse.y, &line, NULL);
                float line_number_gutter_width = codeeditor_line_number_gutter_width(editor);
                float fold_gutter_width = codeeditor_fold_gutter_width(editor);

                if (editor->show_fold_gutter && fold_gutter_width > 0.0f &&
                    event->mouse.x >= line_number_gutter_width) {
                    struct vg_fold_region *region =
                        codeeditor_fold_region_starting_at_mut(editor, line);
                    if (region) {
                        region->folded = !region->folded;
                        vg_codeeditor_refresh_layout_state(editor);
                        return true;
                    }
                }

                int slot = -1;
                for (int i = 0; i < editor->gutter_icon_count; i++) {
                    if (editor->gutter_icons[i].line == line) {
                        slot = editor->gutter_icons[i].type;
                        break;
                    }
                }
                editor->gutter_clicked = true;
                editor->gutter_click_read = false;
                editor->gutter_clicked_line = line;
                editor->gutter_clicked_slot = slot;
                widget->needs_paint = true;
                return true;
            }

            int line = 0;
            int col = 0;
            codeeditor_local_point_to_position(
                editor, widget, event->mouse.x, event->mouse.y, &line, &col);
            uint32_t mods = event->modifiers;
            bool additive_click = (mods & (VG_MOD_CTRL | VG_MOD_SUPER)) != 0;
            bool range_click = (mods & VG_MOD_SHIFT) != 0;
            if (additive_click) {
                codeeditor_add_extra_cursor_at(editor, line, col);
                editor->cursor_visible = true;
                widget->needs_paint = true;
                return true;
            }
            if (range_click) {
                editor->selection.start_line = editor->cursor_line;
                editor->selection.start_col = editor->cursor_col;
                editor->selection.end_line = line;
                editor->selection.end_col = col;
                editor->cursor_line = line;
                editor->cursor_col = col;
                editor->has_selection = compare_positions(editor->selection.start_line,
                                                          editor->selection.start_col,
                                                          editor->selection.end_line,
                                                          editor->selection.end_col) != 0;
                clear_extra_cursor_selections(editor);
                editor->cursor_visible = true;
                widget->needs_paint = true;
                return true;
            }
            editor->cursor_line = line;
            editor->cursor_col = col;
            editor->has_selection = false;
            clear_extra_cursor_selections(editor);
            if (event->mouse.button == VG_MOUSE_LEFT) {
                editor->selection_drag_pending = true;
                editor->selection_dragging = false;
                editor->selection_drag_start_x = event->mouse.x;
                editor->selection_drag_start_y = event->mouse.y;
                editor->selection_anchor_line = line;
                editor->selection_anchor_col = col;
                vg_widget_set_input_capture(widget);
            }
            editor->cursor_visible = true;
            widget->needs_paint = true;
            return true;
        }

        case VG_EVENT_DOUBLE_CLICK: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            int line = 0;
            int col = 0;
            codeeditor_local_point_to_position(
                editor, widget, event->mouse.x, event->mouse.y, &line, &col);
            codeeditor_select_word_at(editor, line, col);
            ensure_cursor_visible(editor);
            widget->needs_paint = true;
            return true;
        }

        case VG_EVENT_TRIPLE_CLICK: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            int line = 0;
            int col = 0;
            codeeditor_local_point_to_position(
                editor, widget, event->mouse.x, event->mouse.y, &line, &col);
            (void)col;
            codeeditor_select_line_at(editor, line);
            ensure_cursor_visible(editor);
            widget->needs_paint = true;
            return true;
        }

        case VG_EVENT_KEY_DOWN: {
            // Check for modifier key shortcuts first
            uint32_t mods = event->modifiers;
            bool has_ctrl = (mods & VG_MOD_CTRL) != 0 || (mods & VG_MOD_SUPER) != 0; // Ctrl or Cmd
            bool has_shift = (mods & VG_MOD_SHIFT) != 0;

            // Clipboard and editing shortcuts (Ctrl/Cmd + key)
            if (has_ctrl) {
                switch (event->key.key) {
                    case VG_KEY_C: // Copy
                        vg_codeeditor_copy(editor);
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_X: // Cut
                        if (!editor->read_only)
                            vg_codeeditor_cut(editor);
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_V: // Paste
                        if (!editor->read_only)
                            vg_codeeditor_paste(editor);
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_Z: // Undo
                        if (!editor->read_only) {
                            if ((event->modifiers & VG_MOD_SHIFT) != 0)
                                vg_codeeditor_redo(editor);
                            else
                                vg_codeeditor_undo(editor);
                        }
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_Y: // Redo
                        if (!editor->read_only) {
                            vg_codeeditor_redo(editor);
                        }
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_A: // Select all
                        vg_codeeditor_select_all(editor);
                        widget->needs_paint = true;
                        return true;

                    default:
                        break;
                }
            }

            if (editor->read_only) {
                // Navigation only
                bool handled_key = true;
                bool navigation_key = true;
                int anchor_line = 0;
                int anchor_col = 0;
                codeeditor_keyboard_selection_anchor(editor, &anchor_line, &anchor_col);
                switch (event->key.key) {
                    case VG_KEY_UP:
                        codeeditor_move_cursor_vertical(editor, widget, -1);
                        break;
                    case VG_KEY_DOWN:
                        codeeditor_move_cursor_vertical(editor, widget, 1);
                        break;
                    case VG_KEY_LEFT:
                        if (has_ctrl)
                            codeeditor_move_cursor_word_left(editor);
                        else
                            codeeditor_move_cursor_left(editor);
                        break;
                    case VG_KEY_RIGHT:
                        if (has_ctrl)
                            codeeditor_move_cursor_word_right(editor);
                        else
                            codeeditor_move_cursor_right(editor);
                        break;
                    case VG_KEY_HOME:
                        if (has_ctrl) {
                            editor->cursor_line = 0;
                            editor->cursor_col = 0;
                        } else if (editor->cursor_line >= 0 &&
                                   editor->cursor_line < editor->line_count) {
                            vg_code_line_t *line = &editor->lines[editor->cursor_line];
                            int first_text_col =
                                codeeditor_leading_indent_len(line->text, line->length);
                            editor->cursor_col =
                                editor->cursor_col == first_text_col ? 0 : first_text_col;
                        }
                        break;
                    case VG_KEY_END:
                        if (has_ctrl) {
                            editor->cursor_line =
                                editor->line_count > 0 ? editor->line_count - 1 : 0;
                            editor->cursor_col =
                                editor->line_count > 0
                                    ? (int)editor->lines[editor->cursor_line].length
                                    : 0;
                        } else if (editor->cursor_line >= 0 &&
                                   editor->cursor_line < editor->line_count) {
                            vg_code_line_t *line = &editor->lines[editor->cursor_line];
                            int end_col = (int)line->length;
                            int last_text_col = end_col;
                            while (last_text_col > 0 && (line->text[last_text_col - 1] == ' ' ||
                                                         line->text[last_text_col - 1] == '\t'))
                                last_text_col--;
                            editor->cursor_col =
                                editor->cursor_col == last_text_col ? end_col : last_text_col;
                        }
                        break;
                    case VG_KEY_PAGE_UP: {
                        int visible_lines = (int)(widget->height / editor->line_height);
                        if (visible_lines < 1)
                            visible_lines = 1;
                        codeeditor_move_cursor_vertical(editor, widget, -visible_lines);
                        break;
                    }
                    case VG_KEY_PAGE_DOWN: {
                        int visible_lines = (int)(widget->height / editor->line_height);
                        if (visible_lines < 1)
                            visible_lines = 1;
                        codeeditor_move_cursor_vertical(editor, widget, visible_lines);
                        break;
                    }
                    default:
                        handled_key = false;
                        navigation_key = false;
                        break;
                }
                if (!handled_key)
                    return false;
                if (navigation_key)
                    codeeditor_finish_keyboard_navigation(
                        editor, has_shift, anchor_line, anchor_col);
                ensure_cursor_visible(editor);
                widget->needs_paint = true;
                return true;
            }

            bool handled_key = true;
            bool navigation_key = true;
            int anchor_line = 0;
            int anchor_col = 0;
            codeeditor_keyboard_selection_anchor(editor, &anchor_line, &anchor_col);
            switch (event->key.key) {
                case VG_KEY_UP:
                    codeeditor_move_cursor_vertical(editor, widget, -1);
                    break;
                case VG_KEY_DOWN:
                    codeeditor_move_cursor_vertical(editor, widget, 1);
                    break;
                case VG_KEY_LEFT:
                    if (has_ctrl)
                        codeeditor_move_cursor_word_left(editor);
                    else
                        codeeditor_move_cursor_left(editor);
                    break;
                case VG_KEY_RIGHT:
                    if (has_ctrl)
                        codeeditor_move_cursor_word_right(editor);
                    else
                        codeeditor_move_cursor_right(editor);
                    break;
                case VG_KEY_HOME:
                    if (has_ctrl) {
                        editor->cursor_line = 0;
                        editor->cursor_col = 0;
                    } else if (editor->cursor_line >= 0 &&
                               editor->cursor_line < editor->line_count) {
                        vg_code_line_t *line = &editor->lines[editor->cursor_line];
                        int first_text_col =
                            codeeditor_leading_indent_len(line->text, line->length);
                        editor->cursor_col =
                            editor->cursor_col == first_text_col ? 0 : first_text_col;
                    } else {
                        editor->cursor_col = 0;
                    }
                    break;
                case VG_KEY_END:
                    if (has_ctrl) {
                        editor->cursor_line = editor->line_count > 0 ? editor->line_count - 1 : 0;
                        editor->cursor_col = editor->line_count > 0
                                                 ? (int)editor->lines[editor->cursor_line].length
                                                 : 0;
                    } else if (editor->cursor_line >= 0 &&
                               editor->cursor_line < editor->line_count) {
                        vg_code_line_t *line = &editor->lines[editor->cursor_line];
                        int end_col = (int)line->length;
                        int last_text_col = end_col;
                        while (last_text_col > 0 && (line->text[last_text_col - 1] == ' ' ||
                                                     line->text[last_text_col - 1] == '\t'))
                            last_text_col--;
                        editor->cursor_col =
                            editor->cursor_col == last_text_col ? end_col : last_text_col;
                    }
                    break;
                case VG_KEY_PAGE_UP: {
                    int visible_lines = (int)(widget->height / editor->line_height);
                    if (visible_lines < 1)
                        visible_lines = 1;
                    codeeditor_move_cursor_vertical(editor, widget, -visible_lines);
                    break;
                }
                case VG_KEY_PAGE_DOWN: {
                    int visible_lines = (int)(widget->height / editor->line_height);
                    if (visible_lines < 1)
                        visible_lines = 1;
                    codeeditor_move_cursor_vertical(editor, widget, visible_lines);
                    break;
                }
                case VG_KEY_BACKSPACE:
                    navigation_key = false;
                    if (has_ctrl)
                        delete_word_backward_targets(editor);
                    else
                        delete_backspace_targets(editor);
                    break;
                case VG_KEY_DELETE:
                    navigation_key = false;
                    delete_forward_targets(editor, has_ctrl);
                    break;
                case VG_KEY_ENTER:
                    navigation_key = false;
                    codeeditor_insert_newline_with_indent(editor);
                    break;
                case VG_KEY_TAB:
                    navigation_key = false;
                    if (editor->use_spaces) {
                        char spaces[32];
                        int space_count = editor->tab_width;
                        if (space_count < 0)
                            space_count = 0;
                        if (space_count >= (int)sizeof(spaces))
                            space_count = (int)sizeof(spaces) - 1;
                        memset(spaces, ' ', (size_t)space_count);
                        spaces[space_count] = '\0';
                        vg_codeeditor_insert_text(editor, spaces);
                    } else {
                        vg_codeeditor_insert_text(editor, "\t");
                    }
                    break;
                default:
                    handled_key = false;
                    navigation_key = false;
                    break;
            }

            if (!handled_key)
                return false;

            // Ensure cursor stays visible after movement
            ensure_cursor_visible(editor);

            editor->cursor_visible = true;
            if (navigation_key)
                codeeditor_finish_keyboard_navigation(editor, has_shift, anchor_line, anchor_col);
            else
                clear_extra_cursor_selections(editor);
            widget->needs_paint = true;
            return true;
        }

        case VG_EVENT_KEY_CHAR: {
            if (!codeeditor_key_char_allows_text(event))
                return false;
            uint32_t cp = event->key.codepoint;
            bool printable = codeeditor_codepoint_is_text(cp);
            if (!editor->read_only && printable) {
                if (cp < 0x80) {
                    char text[2] = {(char)cp, '\0'};
                    if (text[0] == '}' && codeeditor_try_format_closing_brace(editor)) {
                        ensure_cursor_visible(editor);
                        widget->needs_paint = true;
                        return true;
                    }
                    if (codeeditor_try_skip_pair_closer(editor, text[0])) {
                        ensure_cursor_visible(editor);
                        widget->needs_paint = true;
                        return true;
                    }
                    if (codeeditor_try_insert_pair(editor, text[0])) {
                        ensure_cursor_visible(editor);
                        widget->needs_paint = true;
                        return true;
                    }
                    vg_codeeditor_insert_text(editor, text);
                } else {
                    char utf8[5] = {0};
                    int n = encode_utf8(cp, utf8);
                    if (n > 0) {
                        utf8[n] = '\0';
                        vg_codeeditor_insert_text(editor, utf8);
                    }
                }
                ensure_cursor_visible(editor);
                widget->needs_paint = true;
            }
            return true;
        }

        case VG_EVENT_MOUSE_WHEEL: {
            float wheel_dy = event->wheel.delta_y;
            float wheel_dx = event->wheel.delta_x;
            // Shift + vertical wheel scrolls horizontally (mice without an
            // x-wheel). Trackpads deliver delta_x directly.
            if ((event->modifiers & VG_MOD_SHIFT) != 0 && wheel_dx == 0.0f) {
                wheel_dx = wheel_dy;
                wheel_dy = 0.0f;
            }
            float wheel_speed = vg_get_wheel_speed();
            float step_y = wheel_dy * editor->line_height * CODEEDITOR_MOUSE_WHEEL_LINES * wheel_speed;
            float step_x = editor->word_wrap
                               ? 0.0f
                               : wheel_dx * editor->char_width * CODEEDITOR_MOUSE_WHEEL_LINES *
                                     wheel_speed;
            if (vg_smooth_scroll_effective()) {
                // Accumulate into an eased target; vg_codeeditor_smooth_tick
                // advances scroll toward it each frame.
                float keep_x = editor->scroll_x;
                float keep_y = editor->scroll_y;
                editor->scroll_x =
                    (editor->smooth_animating ? editor->smooth_target_x : editor->scroll_x) -
                    step_x;
                editor->scroll_y =
                    (editor->smooth_animating ? editor->smooth_target_y : editor->scroll_y) -
                    step_y;
                codeeditor_clamp_scroll(editor, widget);
                editor->smooth_target_x = editor->scroll_x;
                editor->smooth_target_y = editor->scroll_y;
                editor->scroll_x = keep_x;
                editor->scroll_y = keep_y;
                editor->smooth_animating = editor->smooth_target_x != editor->scroll_x ||
                                           editor->smooth_target_y != editor->scroll_y;
                widget->needs_paint = true;
                return true;
            }
            if (wheel_dy != 0.0f)
                editor->scroll_y -= step_y;
            if (wheel_dx != 0.0f && !editor->word_wrap)
                editor->scroll_x -= step_x;
            codeeditor_clamp_scroll(editor, widget);
            widget->needs_paint = true;
            return true;
        }

        default:
            break;
    }

    return false;
}

/// @brief Return whether the code editor may participate in keyboard focus traversal.
/// @param widget Code-editor base widget to inspect.
/// @return `true` when enabled and visible.
static bool codeeditor_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief Update cursor blink state after a focus transition.
/// @param widget Code-editor base widget whose state changed.
/// @param gained Whether the editor gained focus.
static void codeeditor_on_focus(vg_widget_t *widget, bool gained) {
    vg_codeeditor_t *editor = (vg_codeeditor_t *)widget;
    if (gained) {
        editor->cursor_visible = true;
        editor->cursor_blink_time = 0;
        widget->needs_paint = true;
    }
}
