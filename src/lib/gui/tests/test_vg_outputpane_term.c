//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/gui/tests/test_vg_outputpane_term.c
// Purpose: Headless pinned-sequence-table tests for the OutputPane terminal
//          emulator (plan 11): scroll regions (DECSTBM/SU/SD/IL/DL), in-line
//          edits (ICH/DCH/ECH), tab stops (HT/HTS/TBC), DEC private modes
//          (?25/?2004/?1 + alternate screen), SGR reverse video, and DSR/DA
//          replies, streaming UTF-8, display-width cells, and bounded input.
// Key invariants:
//   - The parser core is exercised without a font, window, or PTY: sequences go
//     in through vg_outputpane_append and assertions read the public pane
//     struct (cells, cursor, margins, modes) and the queued input replies.
//   - Cursor-row cell state is flushed by cursor movement, so text snapshots
//     use an explicit save/home/restore round trip.
//   - Split UTF-8 scalars are retained without replacement; wide and joined
//     graphemes occupy terminal columns rather than raw-codepoint slots.
//   - Oversized bracketed paste keeps both delimiters and never exceeds the
//     public one-megabyte pending-input bound.
//
//===----------------------------------------------------------------------===//

#include "vg_event.h"
#include "vg_ide_widgets_panels.h"
#include "vgfx.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__);                              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/// @brief Feed a raw byte string through the terminal parser.
static void feed(vg_outputpane_t *pane, const char *bytes) {
    vg_outputpane_append(pane, bytes);
}

/// @brief Return the flushed text of logical line @p index ("" when absent).
/// @details Saves the cursor, moves home (which flushes the cursor row into its
///          line segments), snapshots via select-all, and restores the cursor.
///          Caller frees the returned buffer.
static char *snapshot(vg_outputpane_t *pane) {
    feed(pane,
         "\x1b"
         "7"
         "\x1b[1;1H");
    vg_outputpane_select_all(pane);
    char *text = vg_outputpane_get_selection(pane);
    feed(pane,
         "\x1b"
         "8");
    return text ? text : (char *)calloc(1, 1);
}

/// @brief True when line @p index of the snapshot equals @p expect.
static int line_equals(vg_outputpane_t *pane, int index, const char *expect) {
    char *text = snapshot(pane);
    int ok = 0;
    const char *p = text;
    for (int i = 0; i < index && p; i++) {
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    if (p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        // Trailing blanks are presentation-neutral; compare trimmed.
        while (len > 0 && p[len - 1] == ' ')
            len--;
        ok = strlen(expect) == len && strncmp(p, expect, len) == 0;
        if (!ok)
            fprintf(stderr, "  line %d was '%.*s' expected '%s'\n", index, (int)len, p, expect);
    } else if (expect[0] == '\0') {
        ok = 1;
    } else {
        fprintf(stderr, "  line %d missing, expected '%s'\n", index, expect);
    }
    free(text);
    return ok;
}

/// @brief Drain queued child-input bytes into @p buf (NUL-terminated).
static void drain_input(vg_outputpane_t *pane, char *buf, size_t cap) {
    buf[0] = '\0';
    size_t len = 0;
    char *bytes = vg_outputpane_take_input_bytes(pane, &len);
    if (bytes) {
        if (len >= cap)
            len = cap - 1;
        memcpy(buf, bytes, len);
        buf[len] = '\0';
        free(bytes);
    }
}

static int test_basic_grid_and_edits(void) {
    vg_outputpane_t *pane = vg_outputpane_create();
    CHECK(pane != NULL);
    vg_outputpane_set_terminal_mode(pane, true);

    // CUP + overwrite + EL0: "hello" then erase from column 3.
    feed(pane, "hello");
    CHECK(pane->cursor_col == 5);
    feed(pane, "\x1b[3G\x1b[K");
    CHECK(pane->cell_count == 2);
    CHECK(strcmp(pane->cells[0].utf8, "h") == 0);
    CHECK(strcmp(pane->cells[1].utf8, "e") == 0);

    // ICH: insert two blanks at column 1 of "abcdef" -> "  abcdef" shifted.
    feed(pane,
         "\x1b[2K\x1b[1G"
         "abcdef"
         "\x1b[3G\x1b[2@");
    CHECK(pane->cell_count == 8);
    CHECK(pane->cells[2].utf8[0] == '\0'); // blank cell
    CHECK(strcmp(pane->cells[4].utf8, "c") == 0);

    // DCH: delete the two blanks again -> "abcdef".
    feed(pane, "\x1b[2P");
    CHECK(pane->cell_count == 6);
    CHECK(strcmp(pane->cells[2].utf8, "c") == 0);

    // ECH: blank two cells in place (no shift).
    feed(pane, "\x1b[2X");
    CHECK(pane->cell_count == 6);
    CHECK(pane->cells[2].utf8[0] == '\0');
    CHECK(pane->cells[3].utf8[0] == '\0');
    CHECK(strcmp(pane->cells[4].utf8, "e") == 0);

    vg_outputpane_destroy(pane);
    return 0;
}

static int test_tabs(void) {
    vg_outputpane_t *pane = vg_outputpane_create();
    CHECK(pane != NULL);
    vg_outputpane_set_terminal_mode(pane, true);

    // Default ruler: every 8 columns; HT moves without writing glyphs.
    feed(pane, "\x1b[1G\t");
    CHECK(pane->cursor_col == 8);
    CHECK(pane->cell_count == 0); // pure cursor move, no blank glyphs written
    feed(pane, "\t");
    CHECK(pane->cursor_col == 16);

    // HTS at column 4 (cursor col index 3), then HT from home lands there.
    feed(pane, "\x1b[4G\x1bH\x1b[1G\t");
    CHECK(pane->cursor_col == 3);

    // TBC 0 clears that stop; HT falls through to the default 8.
    feed(pane, "\x1b[4G\x1b[0g\x1b[1G\t");
    CHECK(pane->cursor_col == 8);

    // TBC 3 clears everything; HT uses the no-stop fallback (> current col).
    feed(pane, "\x1b[3g\x1b[1G\t");
    CHECK(pane->cursor_col > 0);

    vg_outputpane_destroy(pane);
    return 0;
}

static int test_region_scrolling(void) {
    vg_outputpane_t *pane = vg_outputpane_create();
    CHECK(pane != NULL);
    vg_outputpane_set_terminal_mode(pane, true);

    // Full-screen program surface: alt screen with four addressed rows.
    feed(pane, "\x1b[?1049h");
    CHECK(pane->alternate_screen);
    feed(pane, "\x1b[1;1Hr1\x1b[2;1Hr2\x1b[3;1Hr3\x1b[4;1Hr4");

    // DECSTBM rows 1..3; the cursor homes to the origin.
    feed(pane, "\x1b[1;3r");
    CHECK(pane->term_scroll_top == 0);
    CHECK(pane->term_scroll_bottom == 2);
    CHECK(pane->cursor_col == 0);

    // LF at the bottom margin scrolls the region up; r4 stays pinned.
    feed(pane, "\x1b[3;1H\n");
    CHECK(line_equals(pane, 0, "r2"));
    CHECK(line_equals(pane, 1, "r3"));
    CHECK(line_equals(pane, 2, ""));
    CHECK(line_equals(pane, 3, "r4"));

    // RI at the top margin scrolls the region down.
    feed(pane, "\x1b[1;1H\x1bM");
    CHECK(line_equals(pane, 0, ""));
    CHECK(line_equals(pane, 1, "r2"));
    CHECK(line_equals(pane, 2, "r3"));
    CHECK(line_equals(pane, 3, "r4"));

    // DL at the top of the region deletes a row inside it only.
    feed(pane, "\x1b[1;1H\x1b[M");
    CHECK(line_equals(pane, 0, "r2"));
    CHECK(line_equals(pane, 1, "r3"));
    CHECK(line_equals(pane, 2, ""));
    CHECK(line_equals(pane, 3, "r4"));

    // IL inserts a blank row, pushing region content down.
    feed(pane, "\x1b[1;1H\x1b[L");
    CHECK(line_equals(pane, 0, ""));
    CHECK(line_equals(pane, 1, "r2"));
    CHECK(line_equals(pane, 2, "r3"));
    CHECK(line_equals(pane, 3, "r4"));

    // SU scrolls the region up explicitly.
    feed(pane, "\x1b[1S");
    CHECK(line_equals(pane, 0, "r2"));
    CHECK(line_equals(pane, 1, "r3"));
    CHECK(line_equals(pane, 2, ""));
    CHECK(line_equals(pane, 3, "r4"));

    // CSI r (no params) resets the margins.
    feed(pane, "\x1b[r");
    CHECK(pane->term_scroll_top == -1);
    CHECK(pane->term_scroll_bottom == -1);

    // Leaving the alt screen restores the primary buffer.
    feed(pane, "\x1b[?1049l");
    CHECK(!pane->alternate_screen);

    vg_outputpane_destroy(pane);
    return 0;
}

static int test_modes_and_replies(void) {
    vg_outputpane_t *pane = vg_outputpane_create();
    CHECK(pane != NULL);
    vg_outputpane_set_terminal_mode(pane, true);
    char buf[64];

    // DECSET/DECRST 25 toggles caret suppression.
    CHECK(!pane->term_cursor_hidden);
    feed(pane, "\x1b[?25l");
    CHECK(pane->term_cursor_hidden);
    feed(pane, "\x1b[?25h");
    CHECK(!pane->term_cursor_hidden);

    // DECSET 2004 arms bracketed paste; DECSET 1 arms application cursor keys.
    feed(pane, "\x1b[?2004h\x1b[?1h");
    CHECK(pane->bracketed_paste);
    CHECK(pane->app_cursor_keys);
    feed(pane, "\x1b[?2004l\x1b[?1l");
    CHECK(!pane->bracketed_paste);
    CHECK(!pane->app_cursor_keys);

    // SGR 7 writes swapped colors; SGR 0 restores.
    feed(pane, "\x1b[7mZ");
    CHECK(pane->cells[0].bg == pane->default_fg);
    CHECK(pane->cells[0].fg == pane->bg_color);
    feed(pane, "\x1b[0mZ");
    CHECK(pane->cells[1].fg == pane->default_fg);

    // DSR 6 reports the cursor position (1-based, screen-relative).
    feed(pane, "\x1b[2;5H\x1b[6n");
    drain_input(pane, buf, sizeof(buf));
    CHECK(strcmp(buf, "\x1b[2;5R") == 0);

    // DSR 5 reports OK; DA identifies the terminal.
    feed(pane, "\x1b[5n");
    drain_input(pane, buf, sizeof(buf));
    CHECK(strcmp(buf, "\x1b[0n") == 0);
    feed(pane, "\x1b[c");
    drain_input(pane, buf, sizeof(buf));
    CHECK(strcmp(buf, "\x1b[?1;2c") == 0);
    feed(pane, "\x1b[>c");
    drain_input(pane, buf, sizeof(buf));
    CHECK(strcmp(buf, "\x1b[>0;95;0c") == 0);

    // RIS resets margins, modes, and styling.
    feed(pane,
         "\x1b[1;3r\x1b[?25l\x1b[?2004h\x1b[7m\x1b"
         "c");
    CHECK(pane->term_scroll_top == -1);
    CHECK(!pane->term_cursor_hidden);
    CHECK(!pane->bracketed_paste);
    CHECK(!pane->ansi_reverse);

    vg_outputpane_destroy(pane);
    return 0;
}

static int test_scrollback_flow_unchanged(void) {
    vg_outputpane_t *pane = vg_outputpane_create();
    CHECK(pane != NULL);
    vg_outputpane_set_terminal_mode(pane, true);

    // Without margins, newline keeps growing the scrollback ring as before.
    size_t before = pane->line_count;
    feed(pane, "one\ntwo\nthree");
    CHECK(pane->line_count == before + 2);
    CHECK(line_equals(pane, 0, "one"));
    CHECK(line_equals(pane, 1, "two"));
    CHECK(line_equals(pane, 2, "three"));

    vg_outputpane_destroy(pane);
    return 0;
}

static int test_streaming_utf8_and_display_width(void) {
    vg_outputpane_t *pane = vg_outputpane_create();
    CHECK(pane != NULL);
    vg_outputpane_set_terminal_mode(pane, true);

    // A scalar split at an arbitrary process-read boundary is withheld until
    // its final continuation byte arrives.
    feed(pane, "\xE2\x82");
    CHECK(pane->utf8_carry_len == 2);
    CHECK(pane->cell_count == 0);
    feed(pane, "\xAC");
    CHECK(pane->utf8_carry_len == 0);
    CHECK(pane->cell_count == 1);
    CHECK(strcmp(pane->cells[0].utf8, "\xE2\x82\xAC") == 0);

    vg_outputpane_clear(pane);
    // CJK occupies two columns, a combining acute joins its base without
    // moving the cursor, and the following ASCII scalar lands at column 3.
    feed(pane,
         "\xE7\x95\x8C"
         "e\xCC\x81"
         "X");
    CHECK(pane->cursor_col == 4);
    CHECK(pane->cell_count == 4);
    CHECK(pane->cells[0].width == 2);
    CHECK(pane->cells[1].width == 0);
    CHECK(pane->cells[2].width == 1);
    CHECK(strcmp(pane->cells[2].utf8, "e\xCC\x81") == 0);
    CHECK(strcmp(pane->cells[3].utf8, "X") == 0);

    vg_outputpane_clear(pane);
    // ZWJ emoji and regional-indicator flags each form one two-column cell.
    feed(pane,
         "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
         "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6");
    CHECK(pane->cursor_col == 2);
    CHECK(pane->cell_count == 2);
    CHECK(pane->cells[0].width == 2);
    CHECK(pane->cells[1].width == 0);
    feed(pane, "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
    CHECK(pane->cursor_col == 4);
    CHECK(pane->cell_count == 4);
    CHECK(pane->cells[2].width == 2);
    CHECK(pane->cells[3].width == 0);

    vg_outputpane_destroy(pane);
    return 0;
}

static int test_bounded_bracketed_paste(void) {
    vg_outputpane_t *pane = vg_outputpane_create();
    CHECK(pane != NULL);
    vg_outputpane_set_terminal_mode(pane, true);
    feed(pane, "\x1b[?2004h");
    CHECK(pane->bracketed_paste);

    const size_t source_len = (size_t)VG_OUTPUTPANE_TERMINAL_INPUT_LIMIT + 64;
    char *source = malloc(source_len + 1);
    CHECK(source != NULL);
    memset(source, 'p', source_len);
    source[source_len] = '\0';
    vgfx_clipboard_set_text(source);

    vg_event_t paste = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_V, 0, VG_MOD_CTRL | VG_MOD_SHIFT);
    CHECK(pane->base.vtable->handle_event(&pane->base, &paste));
    CHECK(pane->pending_len == (size_t)VG_OUTPUTPANE_TERMINAL_INPUT_LIMIT);
    CHECK(pane->pending_input_dropped);

    size_t queued_len = 0;
    char *queued = vg_outputpane_take_input_bytes(pane, &queued_len);
    CHECK(queued != NULL);
    CHECK(queued_len == (size_t)VG_OUTPUTPANE_TERMINAL_INPUT_LIMIT);
    CHECK(memcmp(queued, "\x1b[200~", 6) == 0);
    CHECK(memcmp(queued + queued_len - 6, "\x1b[201~", 6) == 0);

    free(queued);
    free(source);
    vgfx_clipboard_set_text("");
    vg_outputpane_destroy(pane);
    return 0;
}

int main(void) {
    if (test_basic_grid_and_edits())
        return 1;
    if (test_tabs())
        return 1;
    if (test_region_scrolling())
        return 1;
    if (test_modes_and_replies())
        return 1;
    if (test_scrollback_flow_unchanged())
        return 1;
    if (test_streaming_utf8_and_display_width())
        return 1;
    if (test_bounded_bracketed_paste())
        return 1;
    printf("test_vg_outputpane_term: all checks passed\n");
    return 0;
}
