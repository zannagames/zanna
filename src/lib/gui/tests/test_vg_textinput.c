//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/gui/tests/test_vg_textinput.c
// Purpose: Exercise grapheme-safe TextInput editing, bounded history, public
//          state edges, presentation modes, and atomic IME composition.
// Key invariants:
//   - Cursor/selection/edit operations never split an extended grapheme cluster.
//   - IME preedit never mutates committed text or history before commit.
// Ownership/Lifetime:
//   - Each test owns one unparented widget and destroys it before returning.
//   - Selected-text copies returned by the widget are freed by the test.
// Links: src/lib/gui/include/vg_widgets.h,
//        src/lib/gui/include/vg_grapheme.h,
//        src/lib/gui/src/widgets/vg_textinput.c
//
//===----------------------------------------------------------------------===//

#include "vg_event.h"
#include "vg_grapheme.h"
#include "vg_widgets.h"
#include "vgfx.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_E_ACUTE "e\xCC\x81"
#define TEXT_FAMILY                                                                                \
    "\xF0\x9F\x91\xA8"                                                                             \
    "\xE2\x80\x8D"                                                                                 \
    "\xF0\x9F\x91\xA9"                                                                             \
    "\xE2\x80\x8D"                                                                                 \
    "\xF0\x9F\x91\xA7"                                                                             \
    "\xE2\x80\x8D"                                                                                 \
    "\xF0\x9F\x91\xA6"
#define TEXT_FLAG_US                                                                               \
    "\xF0\x9F\x87\xBA"                                                                             \
    "\xF0\x9F\x87\xB8"
#define TEXT_HAN_TWO "\xE6\xBC\xA2\xE5\xAD\x97"

static int g_failures = 0;

/// @brief Report one failed Boolean assertion and return from the current test.
/// @param condition Expression result to require.
/// @param expression Source spelling of the expression for diagnostics.
/// @param line Source line containing the failed assertion.
static bool require_condition(bool condition, const char *expression, int line) {
    if (condition)
        return true;
    fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, line, expression);
    g_failures++;
    return false;
}

#define REQUIRE(expression)                                                                        \
    do {                                                                                           \
        if (!require_condition((expression), #expression, __LINE__))                               \
            return;                                                                                \
    } while (0)

/// @brief Send one key-down event directly through the TextInput vtable.
/// @param input Text input widget under test.
/// @param key Toolkit virtual key code.
/// @param modifiers Modifier mask, or VG_MOD_NONE.
/// @return Whether the widget handled the key.
static bool send_key(vg_textinput_t *input, vg_key_t key, uint32_t modifiers) {
    vg_event_t event = {0};
    event.type = VG_EVENT_KEY_DOWN;
    event.key.key = key;
    event.modifiers = modifiers;
    return input->base.vtable->handle_event(&input->base, &event);
}

/// @brief Verify cursor, selection, arrow, and deletion behavior on complex clusters.
static void test_grapheme_indices_and_keyboard_deletion(void) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    REQUIRE(input != NULL);
    const char text[] = TEXT_E_ACUTE TEXT_FAMILY TEXT_FLAG_US "Z";
    vg_textinput_set_text(input, text);
    REQUIRE(vg_textinput_get_cursor_grapheme(input) == 4);

    vg_textinput_set_cursor_grapheme(input, 1);
    REQUIRE(input->cursor_pos == 2);
    vg_textinput_select_graphemes(input, 1, 3);
    REQUIRE(vg_textinput_get_selection_start_grapheme(input) == 1);
    REQUIRE(vg_textinput_get_selection_end_grapheme(input) == 3);
    char *selected = vg_textinput_get_selection(input);
    REQUIRE(selected != NULL);
    REQUIRE(strcmp(selected, TEXT_FAMILY TEXT_FLAG_US) == 0);
    free(selected);

    vg_textinput_clear_selection(input);
    vg_textinput_set_cursor_grapheme(input, 4);
    REQUIRE(send_key(input, VG_KEY_LEFT, VG_MOD_NONE));
    REQUIRE(vg_textinput_get_cursor_grapheme(input) == 3);
    REQUIRE(send_key(input, VG_KEY_BACKSPACE, VG_MOD_NONE));
    REQUIRE(strcmp(vg_textinput_get_text(input), TEXT_E_ACUTE TEXT_FAMILY "Z") == 0);
    REQUIRE(vg_grapheme_count(input->text, input->text_len) == 3);
    REQUIRE(vg_textinput_undo(input));
    REQUIRE(strcmp(vg_textinput_get_text(input), text) == 0);

    vg_widget_destroy(&input->base);
}

/// @brief Verify max length, presentation flags, and single-line normalization.
static void test_limits_and_modes(void) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    REQUIRE(input != NULL);
    vg_textinput_set_max_length(input, 2);
    REQUIRE(vg_textinput_get_max_length(input) == 2);
    vg_textinput_set_text(input, TEXT_E_ACUTE TEXT_FAMILY TEXT_FLAG_US "Z");
    REQUIRE(strcmp(vg_textinput_get_text(input), TEXT_E_ACUTE TEXT_FAMILY) == 0);
    REQUIRE(!vg_textinput_insert_text(input, "X"));

    vg_textinput_select_graphemes(input, 1, 2);
    REQUIRE(vg_textinput_insert_text(input, "Q"));
    REQUIRE(strcmp(vg_textinput_get_text(input), TEXT_E_ACUTE "Q") == 0);
    vg_textinput_set_password(input, true);
    REQUIRE(vg_textinput_is_password(input));
    vg_textinput_set_read_only(input, true);
    REQUIRE(vg_textinput_is_read_only(input));
    REQUIRE(!vg_textinput_insert_text(input, "R"));
    vg_textinput_set_read_only(input, false);

    vg_textinput_set_max_length(input, 0);
    vg_textinput_set_multiline(input, true);
    vg_textinput_set_text(input, "one\r\ntwo\nthree");
    REQUIRE(vg_textinput_is_multiline(input));
    vg_textinput_set_multiline(input, false);
    REQUIRE(!vg_textinput_is_multiline(input));
    REQUIRE(strcmp(vg_textinput_get_text(input), "onetwothree") == 0);

    vg_widget_destroy(&input->base);
}

/// @brief Verify history availability, independent edges, and monotonic revision state.
static void test_history_and_independent_edges(void) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    REQUIRE(input != NULL);
    uint64_t initial_revision = vg_textinput_get_revision(input);
    REQUIRE(vg_textinput_insert_text(input, "a"));
    REQUIRE(vg_textinput_insert_text(input, "b"));
    REQUIRE(vg_textinput_can_undo(input));
    REQUIRE(!vg_textinput_can_redo(input));
    REQUIRE(vg_textinput_was_changed(input));
    REQUIRE(!vg_textinput_was_changed(input));
    REQUIRE(vg_textinput_get_revision(input) > initial_revision);

    REQUIRE(vg_textinput_undo(input));
    REQUIRE(strcmp(vg_textinput_get_text(input), "a") == 0);
    REQUIRE(vg_textinput_can_redo(input));
    REQUIRE(vg_textinput_redo(input));
    REQUIRE(strcmp(vg_textinput_get_text(input), "ab") == 0);
    REQUIRE(send_key(input, VG_KEY_ENTER, VG_MOD_NONE));
    REQUIRE(vg_textinput_was_submitted(input));
    REQUIRE(!vg_textinput_was_submitted(input));
    REQUIRE(vg_textinput_was_changed(input));

    vg_widget_destroy(&input->base);
}

/// @brief Verify one oversized edit cannot retain a snapshot beyond the aggregate history budget.
static void test_oversized_text_disables_snapshot_history(void) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    REQUIRE(input != NULL);

    const size_t oversized_len = 64u * 1024u;
    char *oversized = (char *)malloc(oversized_len + 1u);
    REQUIRE(oversized != NULL);
    memset(oversized, 'x', oversized_len);
    oversized[oversized_len] = '\0';

    vg_textinput_set_text(input, oversized);
    REQUIRE(input->text_len == oversized_len);
    REQUIRE(input->undo_count == 0);
    REQUIRE(!vg_textinput_can_undo(input));
    REQUIRE(!vg_textinput_can_redo(input));

    REQUIRE(vg_textinput_insert_text(input, "y"));
    REQUIRE(input->undo_count == 0);
    REQUIRE(!vg_textinput_can_undo(input));

    free(oversized);
    vg_widget_destroy(&input->base);
}

/// @brief Verify preedit update/cancel isolation and one-record composition commit.
static void test_atomic_ime_composition(void) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    REQUIRE(input != NULL);
    vg_textinput_set_text(input, "A" TEXT_FLAG_US "B");
    REQUIRE(vg_textinput_was_changed(input));
    uint64_t committed_revision = vg_textinput_get_revision(input);

    REQUIRE(vg_textinput_composition_start(input, 1, 1));
    REQUIRE(vg_textinput_composition_update(input, TEXT_E_ACUTE, 1, 0));
    REQUIRE(vg_textinput_is_composing(input));
    REQUIRE(strcmp(vg_textinput_get_composition_text(input), TEXT_E_ACUTE) == 0);
    REQUIRE(vg_textinput_get_composition_start(input) == 1);
    REQUIRE(vg_textinput_get_composition_length(input) == 1);
    REQUIRE(strcmp(vg_textinput_get_text(input), "A" TEXT_FLAG_US "B") == 0);
    REQUIRE(vg_textinput_get_revision(input) == committed_revision);
    REQUIRE(!vg_textinput_was_changed(input));

    REQUIRE(vg_textinput_composition_commit(input, TEXT_HAN_TWO));
    REQUIRE(!vg_textinput_is_composing(input));
    REQUIRE(strcmp(vg_textinput_get_text(input), "A" TEXT_HAN_TWO "B") == 0);
    REQUIRE(vg_textinput_was_changed(input));
    REQUIRE(vg_textinput_undo(input));
    REQUIRE(strcmp(vg_textinput_get_text(input), "A" TEXT_FLAG_US "B") == 0);
    REQUIRE(!vg_textinput_can_undo(input));

    vg_textinput_select_graphemes(input, 0, 1);
    size_t saved_start = input->selection_start;
    size_t saved_end = input->selection_end;
    REQUIRE(vg_textinput_composition_start(input, 2, 0));
    REQUIRE(vg_textinput_composition_update(input, "temporary", 4, 0));
    REQUIRE(vg_textinput_composition_cancel(input));
    REQUIRE(input->selection_start == saved_start);
    REQUIRE(input->selection_end == saved_end);
    REQUIRE(strcmp(vg_textinput_get_text(input), "A" TEXT_FLAG_US "B") == 0);

    vg_widget_destroy(&input->base);
}

/// @brief Verify translated platform composition events drive one grapheme-safe edit.
/// @details Exercises the actual ZannaGFX-to-GUI payload bridge, current-selection replacement
///          sentinel, codepoint-to-grapheme preedit selection conversion, atomic commit/undo, and
///          cancellation restoration through the TextInput event vtable.
static void test_platform_ime_event_bridge(void) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    REQUIRE(input != NULL);
    vg_textinput_set_text(input, "A" TEXT_FLAG_US "B");
    REQUIRE(vg_textinput_was_changed(input));
    vg_textinput_select_graphemes(input, 1, 2);

    vgfx_event_t platform = {0};
    platform.type = VGFX_EVENT_COMPOSITION_START;
    platform.data.composition.replacement_start = -1;
    platform.data.composition.replacement_length = -1;
    vg_event_t event = vg_event_from_platform(&platform);
    REQUIRE(event.type == VG_EVENT_COMPOSITION_START);
    REQUIRE(input->base.vtable->handle_event(&input->base, &event));
    REQUIRE(vg_textinput_is_composing(input));
    REQUIRE(vg_textinput_get_composition_start(input) == 1);

    platform = (vgfx_event_t){0};
    platform.type = VGFX_EVENT_COMPOSITION_UPDATE;
    memcpy(platform.data.composition.text, TEXT_E_ACUTE, sizeof(TEXT_E_ACUTE) - 1u);
    platform.data.composition.text_length = sizeof(TEXT_E_ACUTE) - 1u;
    platform.data.composition.selection_start = 0;
    platform.data.composition.selection_length = 2;
    event = vg_event_from_platform(&platform);
    REQUIRE(event.type == VG_EVENT_COMPOSITION_UPDATE);
    REQUIRE(input->base.vtable->handle_event(&input->base, &event));
    REQUIRE(strcmp(vg_textinput_get_composition_text(input), TEXT_E_ACUTE) == 0);
    REQUIRE(input->composition_sel_start == 0);
    REQUIRE(input->composition_sel_length == 1);
    REQUIRE(strcmp(vg_textinput_get_text(input), "A" TEXT_FLAG_US "B") == 0);

    platform = (vgfx_event_t){0};
    platform.type = VGFX_EVENT_COMPOSITION_COMMIT;
    memcpy(platform.data.composition.text, TEXT_HAN_TWO, sizeof(TEXT_HAN_TWO) - 1u);
    platform.data.composition.text_length = sizeof(TEXT_HAN_TWO) - 1u;
    event = vg_event_from_platform(&platform);
    REQUIRE(input->base.vtable->handle_event(&input->base, &event));
    REQUIRE(strcmp(vg_textinput_get_text(input), "A" TEXT_HAN_TWO "B") == 0);
    REQUIRE(vg_textinput_undo(input));
    REQUIRE(strcmp(vg_textinput_get_text(input), "A" TEXT_FLAG_US "B") == 0);
    REQUIRE(!vg_textinput_can_undo(input));

    vg_textinput_set_cursor_grapheme(input, 3);
    platform = (vgfx_event_t){0};
    platform.type = VGFX_EVENT_COMPOSITION_UPDATE;
    memcpy(platform.data.composition.text, "temporary", sizeof("temporary"));
    platform.data.composition.text_length = sizeof("temporary") - 1u;
    platform.data.composition.replacement_start = -1;
    platform.data.composition.replacement_length = -1;
    event = vg_event_from_platform(&platform);
    REQUIRE(input->base.vtable->handle_event(&input->base, &event));
    REQUIRE(vg_textinput_is_composing(input));
    platform = (vgfx_event_t){0};
    platform.type = VGFX_EVENT_COMPOSITION_CANCEL;
    event = vg_event_from_platform(&platform);
    REQUIRE(input->base.vtable->handle_event(&input->base, &event));
    REQUIRE(!vg_textinput_is_composing(input));
    REQUIRE(vg_textinput_get_cursor_grapheme(input) == 3);

    vg_widget_destroy(&input->base);
}

/// @brief Execute all focused TextInput regressions.
/// @return EXIT_SUCCESS when every invariant passes.
int main(void) {
    test_grapheme_indices_and_keyboard_deletion();
    test_limits_and_modes();
    test_history_and_independent_edges();
    test_oversized_text_disables_snapshot_history();
    test_atomic_ime_composition();
    test_platform_ime_event_bridge();
    if (g_failures != 0) {
        fprintf(stderr, "%d TextInput test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    printf("TextInput grapheme/history/composition tests passed\n");
    return EXIT_SUCCESS;
}
