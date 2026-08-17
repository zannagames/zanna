//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/gui/tests/test_vg_codeeditor_behavior.c
// Purpose: Behavioral unit tests for CodeEditor editing/interaction features
//          that do not require a real font or window: undo coalescing of typing
//          bursts (plan 02), edge autoscroll during a selection drag (plan 05),
//          and state-preserving full-buffer save normalization. Metrics are set
//          directly on the widget struct so the tests
//          run headless and deterministically.
// Key invariants:
//   - No font/backend dependency: char_width/line_height are set by hand.
//   - The deterministic typing clock is advanced via vg_codeeditor_tick(dt).
// Ownership/Lifetime:
//   - Each test creates and destroys its own editor.
// Links: src/lib/gui/src/widgets/vg_codeeditor_history.inc,
//        src/lib/gui/src/widgets/vg_codeeditor_input.inc
//
//===----------------------------------------------------------------------===//

#include "vg_event.h"
#include "vg_ide_widgets.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s\n", name);
        g_failures++;
    }
}

/// @brief Return the editor text as a heap string the caller must free.
static char *text_of(vg_codeeditor_t *e) {
    char *t = vg_codeeditor_get_text(e);
    return t ? t : NULL;
}

static int text_equals(vg_codeeditor_t *e, const char *expect) {
    char *t = text_of(e);
    int eq = t && strcmp(t, expect) == 0;
    if (!eq) {
        printf("    (got \"%s\" want \"%s\")\n", t ? t : "(null)", expect);
    }
    free(t);
    return eq;
}

/// @brief Type one character and advance the typing clock by `ms` milliseconds.
static void type_char(vg_codeeditor_t *e, const char *ch, float ms) {
    vg_codeeditor_insert_text(e, ch);
    if (ms > 0.0f)
        vg_codeeditor_tick(e, ms / 1000.0f);
}

// ---------------------------------------------------------------------------
// Plan 02: undo coalescing
// ---------------------------------------------------------------------------

static void test_coalesce_word_is_one_undo(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "");
    type_char(e, "h", 40);
    type_char(e, "e", 40);
    type_char(e, "l", 40);
    type_char(e, "l", 40);
    type_char(e, "o", 40);
    check("typed hello", text_equals(e, "hello"));
    vg_codeeditor_undo(e);
    check("single undo reverts the whole word", text_equals(e, ""));
    vg_widget_destroy(&e->base);
}

static void test_time_pause_breaks_unit(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "");
    type_char(e, "a", 40);
    type_char(e, "b", 40);
    type_char(e, "c", 40);
    // Long pause (>800ms) before the next burst.
    vg_codeeditor_tick(e, 1.0f);
    type_char(e, "d", 40);
    type_char(e, "e", 40);
    type_char(e, "f", 40);
    check("typed abcdef", text_equals(e, "abcdef"));
    vg_codeeditor_undo(e);
    check("first undo removes second burst", text_equals(e, "abc"));
    vg_codeeditor_undo(e);
    check("second undo removes first burst", text_equals(e, ""));
    vg_widget_destroy(&e->base);
}

static void test_whitespace_word_boundary(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "");
    const char *seq = "hello world";
    for (const char *p = seq; *p; p++) {
        char ch[2] = {*p, 0};
        type_char(e, ch, 40);
    }
    check("typed 'hello world'", text_equals(e, "hello world"));
    // Whitespace joins the previous word; the first non-space after it starts a
    // new unit. So "hello " and "world" are two units.
    vg_codeeditor_undo(e);
    check("first undo removes 'world'", text_equals(e, "hello "));
    vg_codeeditor_undo(e);
    check("second undo removes 'hello '", text_equals(e, ""));
    vg_widget_destroy(&e->base);
}

static void test_undo_boundary_not_crossed(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "");
    type_char(e, "a", 40);
    type_char(e, "b", 40);
    type_char(e, "c", 40);
    vg_codeeditor_undo(e);
    check("undo clears first burst", text_equals(e, ""));
    // Typing after an undo must not merge into the (now redo-able) prior op.
    type_char(e, "x", 40);
    type_char(e, "y", 40);
    check("typed xy after undo", text_equals(e, "xy"));
    vg_codeeditor_undo(e);
    check("undo removes only the new burst", text_equals(e, ""));
    vg_widget_destroy(&e->base);
}

static void test_multichar_insert_not_coalesced(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "");
    type_char(e, "a", 40);
    type_char(e, "b", 40);
    // A multi-codepoint insert (like a paste) is its own undo unit.
    vg_codeeditor_insert_text(e, "XYZ");
    vg_codeeditor_tick(e, 0.04f);
    check("typed ab + inserted XYZ", text_equals(e, "abXYZ"));
    vg_codeeditor_undo(e);
    check("undo removes only the multi-char insert", text_equals(e, "ab"));
    vg_widget_destroy(&e->base);
}

// ---------------------------------------------------------------------------
// Plan 05: edge autoscroll during a selection drag
// ---------------------------------------------------------------------------

static void make_many_lines(vg_codeeditor_t *e, int n) {
    // Build "L0\nL1\n...\nL{n-1}".
    size_t cap = (size_t)n * 12 + 16;
    char *buf = malloc(cap);
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        off += (size_t)snprintf(buf + off, cap - off, "L%d\n", i);
    }
    vg_codeeditor_set_text(e, buf);
    free(buf);
}

static void test_drag_autoscroll_down(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    make_many_lines(e, 200);
    // Set metrics directly (no font).
    e->char_width = 8.0f;
    e->line_height = 10.0f;
    e->base.width = 400.0f;
    e->base.height = 100.0f; // 10 visible lines
    e->scroll_y = 0.0f;

    // Simulate an in-progress drag with the pointer held 40px below the bottom.
    e->selection_dragging = true;
    e->selection_anchor_line = 3;
    e->selection_anchor_col = 0;
    e->selection_last_drag_x = 50.0f;
    e->selection_last_drag_y = 140.0f;

    float prev = e->scroll_y;
    int increased = 0;
    for (int i = 0; i < 5; i++) {
        vg_codeeditor_tick(e, 0.1f);
        if (e->scroll_y > prev)
            increased++;
        prev = e->scroll_y;
    }
    check("held drag past bottom scrolls down repeatedly", increased >= 4);

    // Keep ticking; the scroll must converge to the editor's own maximum and
    // stop (no runaway past the content). The exact max depends on the editor's
    // content-height model, so assert convergence rather than a hardcoded value.
    for (int i = 0; i < 300; i++)
        vg_codeeditor_tick(e, 0.1f);
    float settled = e->scroll_y;
    for (int i = 0; i < 200; i++)
        vg_codeeditor_tick(e, 0.1f);
    check("autoscroll converges and stops (clamped, no runaway)", e->scroll_y == settled);
    check("autoscroll scrolled well into the document", settled > 1500.0f);
    vg_widget_destroy(&e->base);
}

static void test_no_autoscroll_when_not_dragging(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    make_many_lines(e, 200);
    e->char_width = 8.0f;
    e->line_height = 10.0f;
    e->base.width = 400.0f;
    e->base.height = 100.0f;
    e->scroll_y = 50.0f;
    e->selection_dragging = false;
    e->selection_last_drag_y = 140.0f; // past bottom, but no drag active
    for (int i = 0; i < 10; i++)
        vg_codeeditor_tick(e, 0.1f);
    check("no drag => scroll unchanged", e->scroll_y == 50.0f);
    vg_widget_destroy(&e->base);
}

// ---------------------------------------------------------------------------
// Plan 04: horizontal scrolling (wheel + Shift-wheel + clamp)
// ---------------------------------------------------------------------------

static vg_event_t make_wheel(float dx, float dy, uint32_t mods) {
    vg_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = VG_EVENT_MOUSE_WHEEL;
    ev.wheel.delta_x = dx;
    ev.wheel.delta_y = dy;
    ev.modifiers = mods;
    return ev;
}

static vg_codeeditor_t *make_long_line_editor(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    char longline[600];
    for (int i = 0; i < 500; i++)
        longline[i] = 'x';
    longline[500] = 0;
    vg_codeeditor_set_text(e, longline);
    e->char_width = 8.0f;
    e->line_height = 12.0f;
    e->show_line_numbers = false;
    e->gutter_width = 0.0f;
    e->base.width = 400.0f;
    e->base.height = 120.0f;
    e->scroll_x = 0.0f;
    e->scroll_y = 0.0f;
    e->word_wrap = false;
    return e;
}

static void dispatch_n(vg_codeeditor_t *e, vg_event_t *ev, int n) {
    for (int i = 0; i < n; i++)
        e->base.vtable->handle_event(&e->base, ev);
    // Wheel input eases toward its target since Zanna Studio plan 05; settle
    // the animation so assertions observe the landed scroll position.
    int settle_guard = 0;
    while (vg_codeeditor_smooth_tick(e, 16.0f) && settle_guard++ < 1000) {
    }
}

static void test_horizontal_wheel_scroll(void) {
    vg_codeeditor_t *e = make_long_line_editor();
    // With scroll_x -= delta_x, a negative delta scrolls right (increases scroll_x).
    vg_event_t right = make_wheel(-4.0f, 0.0f, VG_MOD_NONE);
    dispatch_n(e, &right, 3);
    check("horizontal wheel moves scroll_x right", e->scroll_x > 0.0f);
    dispatch_n(e, &right, 400);
    float maxed = e->scroll_x;
    dispatch_n(e, &right, 50);
    check("horizontal scroll clamps (no runaway)", e->scroll_x == maxed);
    check("clamped horizontal scroll is substantial", maxed > 1000.0f);
    vg_event_t left = make_wheel(4.0f, 0.0f, VG_MOD_NONE);
    dispatch_n(e, &left, 500);
    check("horizontal wheel returns to 0", e->scroll_x == 0.0f);
    vg_widget_destroy(&e->base);
}

static void test_shift_wheel_is_horizontal(void) {
    vg_codeeditor_t *e = make_long_line_editor();
    // Shift + vertical wheel is remapped to horizontal.
    vg_event_t ev = make_wheel(0.0f, -4.0f, VG_MOD_SHIFT);
    dispatch_n(e, &ev, 3);
    check("shift+wheel scrolls horizontally", e->scroll_x > 0.0f);
    check("shift+wheel leaves vertical scroll untouched", e->scroll_y == 0.0f);
    vg_widget_destroy(&e->base);
}

static void test_wordwrap_disables_hscroll(void) {
    vg_codeeditor_t *e = make_long_line_editor();
    e->word_wrap = true;
    vg_event_t ev = make_wheel(-4.0f, 0.0f, VG_MOD_NONE);
    dispatch_n(e, &ev, 10);
    check("word wrap pins scroll_x at 0", e->scroll_x == 0.0f);
    vg_widget_destroy(&e->base);
}

// ---------------------------------------------------------------------------
// Plan 14: configurable wheel-speed multiplier
// ---------------------------------------------------------------------------

static void test_wheel_speed_multiplier(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    make_many_lines(e, 200);
    e->char_width = 8.0f;
    e->line_height = 12.0f;
    e->base.width = 400.0f;
    e->base.height = 120.0f;
    vg_event_t ev = make_wheel(0.0f, -1.0f, VG_MOD_NONE); // one unit down

    vg_set_wheel_speed(1.0f);
    e->scroll_y = 0.0f;
    dispatch_n(e, &ev, 1);
    float at1x = e->scroll_y;

    vg_set_wheel_speed(2.0f);
    e->scroll_y = 0.0f;
    dispatch_n(e, &ev, 1);
    float at2x = e->scroll_y;

    check("wheel moved at 1x speed", at1x > 0.0f);
    check("wheel speed 2x scrolls about twice as far", at2x > at1x * 1.9f && at2x < at1x * 2.1f);

    vg_set_wheel_speed(1.0f); // restore global for any later tests
    vg_widget_destroy(&e->base);
}

// ---------------------------------------------------------------------------
// Plan 21: detachable editor buffers (swap document state)
// ---------------------------------------------------------------------------

static void test_buffer_create_and_text(void) {
    vg_editor_buffer_t *b = vg_editor_buffer_create("alpha\nbeta\ngamma");
    char *t = vg_editor_buffer_get_text(b);
    check("buffer_create round-trips text", t && strcmp(t, "alpha\nbeta\ngamma") == 0);
    free(t);
    check("fresh buffer is not modified", !vg_editor_buffer_is_modified(b));
    vg_editor_buffer_destroy(b);
}

static void test_buffer_swap_preserves_undo(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "");
    vg_codeeditor_insert_text(e, "hello"); // one undoable op; editor now "hello"
    check("editor shows hello", text_equals(e, "hello"));

    // Swap in a different document.
    vg_editor_buffer_t *bufB = vg_editor_buffer_create("world");
    vg_editor_buffer_t *prevA = vg_codeeditor_swap_buffer(e, bufB);
    check("after swap editor shows world", text_equals(e, "world"));

    // Edit B so it has its own state, then swap A back in. The swapped-in
    // buffer's cursor is at (0,0), so the insert prepends.
    vg_codeeditor_insert_text(e, "!");
    check("editor shows !world", text_equals(e, "!world"));
    vg_editor_buffer_t *prevB = vg_codeeditor_swap_buffer(e, prevA);
    check("after swapping back editor shows hello", text_equals(e, "hello"));

    // Undo history for A survived the round trip.
    vg_codeeditor_undo(e);
    check("undo reverts A's insert after swap round-trip", text_equals(e, ""));

    vg_editor_buffer_destroy(prevB);
    vg_widget_destroy(&e->base);
}

static void test_buffer_swap_preserves_cursor(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    make_many_lines(e, 100);
    vg_codeeditor_set_cursor(e, 7, 2);
    e->scroll_y = 55.0f;

    vg_editor_buffer_t *bufB = vg_editor_buffer_create("scratch");
    vg_editor_buffer_t *prevA = vg_codeeditor_swap_buffer(e, bufB);
    // Editor now shows B; cursor is B's.
    int line = 0, col = 0;
    vg_codeeditor_get_cursor(e, &line, &col);
    check("swapped-in buffer has its own cursor", line == 0);

    // Swap A back: cursor and scroll restored.
    vg_editor_buffer_t *prevB = vg_codeeditor_swap_buffer(e, prevA);
    vg_codeeditor_get_cursor(e, &line, &col);
    check("A's cursor line restored", line == 7);
    check("A's cursor col restored", col == 2);
    check("A's scroll restored", e->scroll_y == 55.0f);

    vg_editor_buffer_destroy(prevB);
    vg_widget_destroy(&e->base);
}

static void test_buffer_revision_independent(void) {
    // Editing one attached buffer must not disturb a detached one.
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "one");
    vg_editor_buffer_t *bufB = vg_editor_buffer_create("two");
    vg_editor_buffer_t *prevA = vg_codeeditor_swap_buffer(e, bufB);
    // Edit B heavily.
    for (int i = 0; i < 20; i++)
        vg_codeeditor_insert_text(e, "x");
    // A (detached) still reads "one".
    char *ta = vg_editor_buffer_get_text(prevA);
    check("detached buffer unaffected by edits to the attached one", ta && strcmp(ta, "one") == 0);
    free(ta);
    vg_editor_buffer_destroy(prevA);
    vg_widget_destroy(&e->base);
}

/// @brief Save normalization remains undoable without discarding editor state.
static void test_replace_all_text_preserves_state(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "one  \ntwo\nthree\nfour");
    vg_codeeditor_set_cursor(e, 2, 2);
    e->extra_cursors = calloc(1, sizeof(*e->extra_cursors));
    e->extra_cursor_cap = 1;
    e->extra_cursor_count = 1;
    e->extra_cursors[0].line = 3;
    e->extra_cursors[0].col = 3;
    e->extra_cursors[0].selection = (vg_selection_t){3, 1, 3, 3};
    e->extra_cursors[0].has_selection = true;
    e->fold_regions = calloc(1, sizeof(*e->fold_regions));
    e->fold_region_cap = 1;
    e->fold_region_count = 1;
    e->fold_regions[0] = (struct vg_fold_region){0, 1, true};
    e->has_folded_lines = true;
    e->scroll_x = 14.0f;
    e->scroll_y = 28.0f;

    check("state-preserving replacement succeeds",
          vg_codeeditor_replace_all_text(e, "one\ntwo\nthree\nfour\n"));
    check("replacement text applied", text_equals(e, "one\ntwo\nthree\nfour\n"));
    check("primary cursor retained", e->cursor_line == 2 && e->cursor_col == 2);
    check("extra cursor and selection retained",
          e->extra_cursor_count == 1 && e->extra_cursors[0].line == 3 &&
              e->extra_cursors[0].col == 3 && e->extra_cursors[0].has_selection);
    check("fold retained",
          e->fold_region_count == 1 && e->fold_regions[0].folded && e->has_folded_lines);
    check("scroll retained", e->scroll_x == 14.0f && e->scroll_y == 28.0f);
    vg_codeeditor_undo(e);
    check("normalization is one undo unit", text_equals(e, "one  \ntwo\nthree\nfour"));

    vg_widget_destroy(&e->base);
}

/// @brief Detached documents use the same state/history-preserving replacement.
static void test_detached_buffer_replace_all_text(void) {
    vg_codeeditor_t *e = vg_codeeditor_create(NULL);
    vg_codeeditor_set_text(e, "alpha  \nbeta");
    vg_codeeditor_set_cursor(e, 1, 2);
    vg_editor_buffer_t *detached =
        vg_codeeditor_swap_buffer(e, vg_editor_buffer_create("temporary"));
    check("detached replacement succeeds",
          vg_editor_buffer_replace_all_text(detached, "alpha\nbeta\n"));
    char *text = vg_editor_buffer_get_text(detached);
    check("detached replacement text applied", text && strcmp(text, "alpha\nbeta\n") == 0);
    free(text);
    vg_editor_buffer_t *temporary = vg_codeeditor_swap_buffer(e, detached);
    check("detached cursor retained", e->cursor_line == 1 && e->cursor_col == 2);
    vg_codeeditor_undo(e);
    check("detached normalization remains undoable", text_equals(e, "alpha  \nbeta"));
    vg_editor_buffer_destroy(temporary);
    vg_widget_destroy(&e->base);
}

int main(void) {
    printf("test_vg_codeeditor_behavior\n");
    test_buffer_create_and_text();
    test_buffer_swap_preserves_undo();
    test_buffer_swap_preserves_cursor();
    test_buffer_revision_independent();
    test_replace_all_text_preserves_state();
    test_detached_buffer_replace_all_text();
    test_coalesce_word_is_one_undo();
    test_time_pause_breaks_unit();
    test_whitespace_word_boundary();
    test_undo_boundary_not_crossed();
    test_multichar_insert_not_coalesced();
    test_drag_autoscroll_down();
    test_no_autoscroll_when_not_dragging();
    test_horizontal_wheel_scroll();
    test_shift_wheel_is_horizontal();
    test_wordwrap_disables_hscroll();
    test_wheel_speed_multiplier();
    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
