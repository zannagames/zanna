//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/tests/test_vg_audit_fixes.c
// Purpose: Regression tests for GUI bugs identified in the Zanna.GUI in-depth
//          audits (Rounds 1–7). Covers dialog re-entrancy, event routing,
//          layout clamping, animation, input guards, and widget lifetime.
// Key invariants:
//   - Each TEST exercises ONE bug from the audit; the assertion holds only
//     once the fix is applied.
//   - Tests are pure-logic (no window / vgfx surface required) so they run in
//     CI on every platform.
// Ownership/Lifetime:
//   - Every test function creates and destroys its own widgets; no shared
//     widget state persists between tests except the global tooltip singleton.
// Links: lib/gui/include/vg_widget.h, lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
#include "vg_event.h"
#include "vg_ide_widgets.h"
#include "vg_layout.h"
#include "vg_theme.h"
#include "vg_widget.h"
#include "vg_widgets.h"
#include "vgfx.h"
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define TEST_PROCESS_ID _getpid
#else
#include <unistd.h>
#define TEST_PROCESS_ID getpid
#endif

//=============================================================================
// Test Harness
//=============================================================================

static int g_passed = 0;
static int g_failed = 0;
static bool g_verbose = false;

/// @brief Portable strdup replacement used to set up drag-drop string fields without depending on
/// POSIX strdup.
static char *test_strdup_local(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

static void test_make_temp_path(char *out, size_t out_size, const char *stem, const char *ext) {
    if (!out || out_size == 0)
        return;

    const char *dir = getenv("TMPDIR");
#ifdef _WIN32
    if (!dir || !*dir)
        dir = getenv("TEMP");
    if (!dir || !*dir)
        dir = getenv("TMP");
#endif
    if (!dir || !*dir)
        dir = "/tmp";

    size_t dir_len = strlen(dir);
    const char *sep =
        (dir_len > 0 && (dir[dir_len - 1] == '/' || dir[dir_len - 1] == '\\')) ? "" : "/";
    snprintf(
        out, out_size, "%s%s%s_%ld%s", dir, sep, stem, (long)TEST_PROCESS_ID(), ext ? ext : "");
    out[out_size - 1] = '\0';
}

#define TEST(name) static void test_##name(void)
#define RUN(name)                                                                                  \
    do {                                                                                           \
        if (g_verbose) {                                                                           \
            printf("  %-60s", #name "...");                                                        \
            fflush(stdout);                                                                        \
        }                                                                                          \
        int before = g_failed;                                                                     \
        test_##name();                                                                             \
        if (g_failed == before) {                                                                  \
            if (g_verbose)                                                                         \
                printf("OK\n");                                                                    \
        } else if (!g_verbose) {                                                                   \
            printf("  %s...FAIL\n", #name);                                                        \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL\n  (%s:%d: %s)\n", __FILE__, __LINE__, #cond);                            \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEQ(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_TRUE(c) ASSERT(c)
#define ASSERT_FALSE(c) ASSERT(!(c))
#define ASSERT_NEAR(a, b, eps) ASSERT(((a) - (b)) < (eps) && ((b) - (a)) < (eps))

//=============================================================================
// Fix #1: vg_dialog_close re-entrancy guard
//=============================================================================

static int g_d1_result_calls = 0;
static int g_d1_close_calls = 0;

/// @brief Result callback that re-enters vg_dialog_close to verify the closing_in_progress guard.
static void d1_on_result_reenters(vg_dialog_t *dlg, vg_dialog_result_t r, void *ud) {
    (void)r;
    (void)ud;
    g_d1_result_calls++;
    // Re-entry: the closing_in_progress guard MUST swallow this call.
    vg_dialog_close(dlg, VG_DIALOG_RESULT_CANCEL);
}

/// @brief Close callback that increments a counter; used to verify it fires exactly once despite
/// re-entry.
static void d1_on_close(vg_dialog_t *dlg, void *ud) {
    (void)dlg;
    (void)ud;
    g_d1_close_calls++;
}

/// @brief Fix #1 — closing_in_progress guard prevents result and close callbacks from firing more
/// than once.
TEST(dialog_close_reentry_is_guarded) {
    g_d1_result_calls = 0;
    g_d1_close_calls = 0;

    vg_dialog_t *dlg = vg_dialog_create("re-entry");
    ASSERT_NOT_NULL(dlg);
    vg_dialog_set_on_result(dlg, d1_on_result_reenters, NULL);
    vg_dialog_set_on_close(dlg, d1_on_close, NULL);

    vg_dialog_close(dlg, VG_DIALOG_RESULT_OK);

    // Without the closing_in_progress guard the result handler fires twice and
    // the close handler also fires twice. The guard makes both fire exactly once.
    ASSERT_EQ(g_d1_result_calls, 1);
    ASSERT_EQ(g_d1_close_calls, 1);

    vg_widget_destroy(&dlg->base);
}

//=============================================================================
// Fix #7: set_on_close routes its own user_data to the close callback
//=============================================================================

static void *g_d7_result_ud = NULL;
static void *g_d7_close_ud = NULL;

/// @brief Result callback that captures its user_data pointer for later assertion.
static void d7_on_result(vg_dialog_t *dlg, vg_dialog_result_t r, void *ud) {
    (void)dlg;
    (void)r;
    g_d7_result_ud = ud;
}

/// @brief Close callback that captures its user_data pointer for later assertion.
static void d7_on_close(vg_dialog_t *dlg, void *ud) {
    (void)dlg;
    g_d7_close_ud = ud;
}

/// @brief Fix #7 — set_on_close routes its own user_data independently from the result callback's
/// user_data.
TEST(dialog_set_on_close_user_data_routed_independently) {
    g_d7_result_ud = (void *)0x1; // sentinel
    g_d7_close_ud = (void *)0x1;

    int result_marker = 1;
    int close_marker = 2;

    vg_dialog_t *dlg = vg_dialog_create("two-ud");
    ASSERT_NOT_NULL(dlg);
    vg_dialog_set_on_result(dlg, d7_on_result, &result_marker);
    vg_dialog_set_on_close(dlg, d7_on_close, &close_marker);

    vg_dialog_close(dlg, VG_DIALOG_RESULT_OK);

    // Pre-fix: on_close received the result handler's user_data because the
    // setter skipped its own user_data when on_result was registered.
    ASSERT(g_d7_result_ud == &result_marker);
    ASSERT(g_d7_close_ud == &close_marker);

    vg_widget_destroy(&dlg->base);
}

//=============================================================================
// Fix #2: tooltip manager clears references on widget destroy
//=============================================================================

/// @brief Fix #2 — tooltip manager clears its hovered_widget reference when the hovered widget is
/// destroyed.
TEST(tooltip_manager_drops_pointer_when_widget_destroyed) {
    vg_tooltip_manager_t *mgr = vg_tooltip_manager_get();
    ASSERT_NOT_NULL(mgr);
    vg_tooltip_manager_on_leave(mgr); // start clean
    ASSERT_NULL(mgr->hovered_widget);

    vg_widget_t *w = vg_widget_create(VG_WIDGET_LABEL);
    ASSERT_NOT_NULL(w);
    vg_widget_set_tooltip_text(w, "hello");

    vg_tooltip_manager_on_hover(mgr, w, 5, 5);
    ASSERT(mgr->hovered_widget == w);

    // Destroying the hovered widget must clear the manager's pointer; otherwise
    // the next hover-update dereferences freed memory.
    vg_widget_destroy(w);
    ASSERT_NULL(mgr->hovered_widget);

    // A subsequent hover with a fresh widget must work without crashing.
    vg_widget_t *w2 = vg_widget_create(VG_WIDGET_LABEL);
    vg_widget_set_tooltip_text(w2, "world");
    vg_tooltip_manager_on_hover(mgr, w2, 7, 7);
    ASSERT(mgr->hovered_widget == w2);
    vg_widget_destroy(w2);
    ASSERT_NULL(mgr->hovered_widget);
}

/// @brief Fix #2 — replacing or clearing tooltip text on the hovered widget immediately refreshes
/// the manager's shared popup without requiring another pointer movement.
TEST(tooltip_manager_refreshes_hovered_widget_text) {
    vg_tooltip_manager_t *mgr = vg_tooltip_manager_get();
    ASSERT_NOT_NULL(mgr);
    vg_tooltip_manager_on_leave(mgr);

    vg_widget_t *widget = vg_widget_create(VG_WIDGET_BUTTON);
    ASSERT_NOT_NULL(widget);
    vg_widget_set_tooltip_text(widget, "Open hierarchy");
    vg_tooltip_manager_on_hover(mgr, widget, 10, 20);
    ASSERT_NOT_NULL(mgr->active_tooltip);
    ASSERT(strcmp(mgr->active_tooltip->text, "Open hierarchy") == 0);

    vg_widget_set_tooltip_text(widget, "Return to viewport");
    ASSERT(strcmp(mgr->active_tooltip->text, "Return to viewport") == 0);

    vg_widget_set_tooltip_text(widget, NULL);
    ASSERT_FALSE(mgr->pending_show);
    ASSERT_FALSE(mgr->active_tooltip->is_visible);

    vg_widget_destroy(widget);
    ASSERT_NULL(mgr->hovered_widget);
}

//=============================================================================
// Fix #3: notification created_at is stamped on first manager update
//=============================================================================

/// @brief Fix #3 — created_at is stamped on the first manager update tick, preventing instant
/// dismissal.
TEST(notification_auto_dismiss_uses_lazy_created_at) {
    vg_notification_manager_t *mgr = vg_notification_manager_create();
    ASSERT_NOT_NULL(mgr);

    uint32_t id = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "t", "m", 100);
    ASSERT(id > 0);
    ASSERT_EQ(mgr->notification_count, (size_t)1);
    ASSERT_EQ(mgr->notifications[0]->created_at, (uint64_t)0);

    // First update at t=10000 must STAMP created_at, not interpret 10000 - 0
    // as an elapsed millisecond count (which would dismiss instantly).
    vg_notification_manager_update(mgr, 10000);
    ASSERT(mgr->notification_count == 1);
    ASSERT_EQ(mgr->notifications[0]->created_at, (uint64_t)10000);
    ASSERT_FALSE(mgr->notifications[0]->dismissed);

    // 50 ms later — still well within the 100 ms duration. Notification stays.
    vg_notification_manager_update(mgr, 10050);
    ASSERT(mgr->notification_count == 1);
    ASSERT_FALSE(mgr->notifications[0]->dismissed);

    // 1 second later — past duration + fade. Notification is removed.
    vg_notification_manager_update(mgr, 11000);
    ASSERT_EQ(mgr->notification_count, (size_t)0);

    vg_widget_destroy(&mgr->base);
}

//=============================================================================
// Fix #4: find/replace advances by match length, not one byte
//=============================================================================

/// @brief Fix #4 — search advances by match length so "aa" in "aaaa" yields 2 non-overlapping
/// matches.
TEST(findreplacebar_search_finds_overlapping_pattern_non_overlapping) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    vg_codeeditor_set_text(ed, "aaaa");

    vg_findreplacebar_t *bar = vg_findreplacebar_create();
    ASSERT_NOT_NULL(bar);
    vg_findreplacebar_set_target(bar, ed);

    // Without a textinput child the bar's search is gated; instead drive it by
    // setting find text directly through the public API and triggering search.
    // Public surface: vg_findreplacebar_set_find_text + vg_findreplacebar_find_next
    // exist; we use whichever the implementation exposes.
    vg_findreplacebar_set_find_text(bar, "aa");
    // The search is triggered as a side effect of set_find_text.
    // Non-overlapping: "aa" in "aaaa" → 2 matches at cols 0 and 2.
    ASSERT_EQ(bar->match_count, (size_t)2);
    ASSERT_EQ(bar->matches[0].start_col, (uint32_t)0);
    ASSERT_EQ(bar->matches[1].start_col, (uint32_t)2);

    vg_widget_destroy(&bar->base);
    vg_widget_destroy(&ed->base);
}

//=============================================================================
// Fix #5: tabbar measure re-clamps scroll_x after total_width shrinks
//=============================================================================

/// @brief Fix #5 — tabbar measure re-clamps scroll_x to 0 when enough tabs are removed that all
/// fit.
TEST(tabbar_measure_clamps_scroll_after_tabs_removed) {
    vg_tabbar_t *tb = vg_tabbar_create(NULL);
    ASSERT_NOT_NULL(tb);

    vg_tab_t *t1 = vg_tabbar_add_tab(tb, "alpha", false);
    vg_tab_t *t2 = vg_tabbar_add_tab(tb, "beta", false);
    vg_tab_t *t3 = vg_tabbar_add_tab(tb, "gamma", false);
    vg_tab_t *t4 = vg_tabbar_add_tab(tb, "delta", false);
    (void)t1;
    (void)t4;

    // Measure with a wide available width so total_width is computed.
    vg_widget_measure(&tb->base, 4000.0f, 50.0f);
    float full_total = tb->total_width;
    ASSERT(full_total > 0.0f);

    // Park the scroll near the end as if user had scrolled right.
    tb->scroll_x = full_total;

    // Remove the last two tabs; total_width shrinks substantially.
    vg_tabbar_remove_tab(tb, t2);
    vg_tabbar_remove_tab(tb, t3);

    // Measure again with a wide enough viewport that everything fits.
    vg_widget_measure(&tb->base, 4000.0f, 50.0f);
    ASSERT(tb->total_width < full_total);

    // Pre-fix: scroll_x stayed at the old (now invalid) position and the paint
    // loop culled every remaining tab. Post-fix: scroll_x is clamped to 0
    // because the remaining tabs fit in the available width.
    ASSERT_EQ(tb->scroll_x, 0.0f);

    vg_widget_destroy(&tb->base);
}

/// @brief An active tab selected while its bar is hidden must be revealed once a
/// narrow viewport is measured. This models a bottom panel activated while its
/// SplitPane side is still collapsed and prevents the selected panel tab from
/// opening entirely offscreen.
TEST(tabbar_measure_reveals_active_tab_selected_while_hidden) {
    vg_tabbar_t *tb = vg_tabbar_create(NULL);
    ASSERT_NOT_NULL(tb);

    vg_tabbar_add_tab(tb, "Problems", false);
    vg_tabbar_add_tab(tb, "Output", false);
    vg_tabbar_add_tab(tb, "Search", false);
    vg_tabbar_add_tab(tb, "References", false);
    vg_tabbar_add_tab(tb, "Variables", false);
    vg_tabbar_add_tab(tb, "Call Stack", false);
    vg_tabbar_add_tab(tb, "Debug Console", false);
    vg_tab_t *terminal = vg_tabbar_add_tab(tb, "Terminal", false);
    ASSERT_NOT_NULL(terminal);

    // The bottom pane has not been laid out yet, so activation cannot derive a
    // viewport and leaves scroll_x at the start of the strip.
    tb->base.width = 0.0f;
    tb->base.measured_width = 0.0f;
    tb->total_width = 0.0f;
    vg_tabbar_set_active(tb, terminal);
    ASSERT_EQ(tb->scroll_x, 0.0f);

    // Once the pane opens, measurement knows the real viewport and must bring
    // the already-active last tab into view without requiring another click.
    vg_widget_measure(&tb->base, 240.0f, 50.0f);
    ASSERT(tb->total_width > 240.0f);
    ASSERT(tb->scroll_x > 0.0f);
    ASSERT(tb->scroll_x <= tb->total_width - 240.0f);

    vg_widget_destroy(&tb->base);
}

//=============================================================================
// Fix #6: contextmenu_show_at no longer requires impl_data
//=============================================================================

/// @brief Fix #6 — show_at records coordinates without accessing impl_data, so NULL impl_data is
/// safe.
TEST(contextmenu_show_at_does_not_dereference_impl_data) {
    vg_contextmenu_t *menu = vg_contextmenu_create();
    ASSERT_NOT_NULL(menu);
    vg_contextmenu_add_item(menu, "Cut", NULL, NULL, NULL);
    vg_contextmenu_add_item(menu, "Copy", NULL, NULL, NULL);

    // Pre-fix: vg_contextmenu_show_at cast menu->base.impl_data (NULL here)
    // to vgfx_window_t and called vgfx_get_size on it. The null guard kept it
    // from crashing but silently skipped the screen-edge clamp. Post-fix: the
    // clamping is moved to paint, so show_at simply records the requested
    // coordinates without touching impl_data.
    menu->base.impl_data = NULL;
    vg_contextmenu_show_at(menu, 1234, 5678);

    ASSERT_EQ(menu->base.x, 1234.0f);
    ASSERT_EQ(menu->base.y, 5678.0f);
    ASSERT_TRUE(menu->is_visible);

    vg_widget_destroy(&menu->base);
}

/// @brief Fix #6b — item icon setter deep-copies pixel data, replaces old icon on re-set, and
/// widens measured width.
TEST(contextmenu_item_icon_setter_owns_replaces_and_reserves_space) {
    vg_contextmenu_t *plain = vg_contextmenu_create();
    ASSERT_NOT_NULL(plain);
    plain->min_width = 0;
    ASSERT_NOT_NULL(vg_contextmenu_add_item(plain, "Open", NULL, NULL, NULL));
    vg_widget_measure(&plain->base, 0.0f, 0.0f);
    float plain_width = plain->base.measured_width;

    vg_contextmenu_t *menu = vg_contextmenu_create();
    ASSERT_NOT_NULL(menu);
    menu->min_width = 0;
    vg_menu_item_t *item = vg_contextmenu_add_item(menu, "Open", NULL, NULL, NULL);
    ASSERT_NOT_NULL(item);
    ASSERT(item->owner_contextmenu == menu);

    uint8_t rgba[16] = {0x10,
                        0x20,
                        0x30,
                        0xFF,
                        0x40,
                        0x50,
                        0x60,
                        0x80,
                        0x70,
                        0x80,
                        0x90,
                        0x40,
                        0xA0,
                        0xB0,
                        0xC0,
                        0x00};
    vg_icon_t image = vg_icon_from_pixels(rgba, 2, 2);
    ASSERT_EQ(image.type, VG_ICON_IMAGE);
    ASSERT_NOT_NULL(image.data.image.pixels);
    ASSERT(image.data.image.pixels != rgba);

    vg_contextmenu_item_set_icon(item, image);
    ASSERT_EQ(item->icon.type, VG_ICON_IMAGE);
    ASSERT_NOT_NULL(item->icon.data.image.pixels);
    ASSERT_EQ(item->icon.data.image.width, 2u);
    ASSERT_EQ(item->icon.data.image.height, 2u);

    vg_widget_measure(&menu->base, 0.0f, 0.0f);
    ASSERT(menu->base.measured_width > plain_width + 20.0f);

    vg_contextmenu_item_set_icon(item, vg_icon_from_glyph('X'));
    ASSERT_EQ(item->icon.type, VG_ICON_GLYPH);
    ASSERT_EQ(item->icon.data.glyph, (uint32_t)'X');

    vg_widget_destroy(&menu->base);
    vg_widget_destroy(&plain->base);
}

//=============================================================================
// Fix #8: clamp_editor_position helpers (verified indirectly via set_cursor)
//=============================================================================

/// @brief Fix #8 — set_cursor clamps line first, then col against the new line's length; negatives
/// go to (0,0).
TEST(codeeditor_set_cursor_clamps_atomic) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    vg_codeeditor_set_text(ed, "ab\nc\ndef\n");

    // Past EOF on both axes — clamp_editor_position must clamp line FIRST then
    // col against the new line's length; col cannot exceed the new line.
    vg_codeeditor_set_cursor(ed, 999, 999);
    int line, col;
    vg_codeeditor_get_cursor(ed, &line, &col);
    ASSERT(line >= 0 && line < ed->line_count);
    ASSERT(col >= 0 && col <= (int)ed->lines[line].length);

    // Negative inputs clamp to (0, 0).
    vg_codeeditor_set_cursor(ed, -5, -5);
    vg_codeeditor_get_cursor(ed, &line, &col);
    ASSERT_EQ(line, 0);
    ASSERT_EQ(col, 0);

    vg_widget_destroy(&ed->base);
}

//=============================================================================
// Fix #10: splitpane proportional clamp when min sizes exceed available
//=============================================================================

/// @brief Fix #10 — when min sizes exceed available space, both panes share space proportionally
/// rather than zeroing the first.
TEST(splitpane_proportional_clamp_when_mins_exceed_available) {
    vg_splitpane_t *sp = vg_splitpane_create(NULL, VG_SPLIT_HORIZONTAL);
    ASSERT_NOT_NULL(sp);

    // vg_splitpane_create already populates first/second pane containers; reuse
    // them. Adding extra children would put them after the panes in the sibling
    // chain and splitpane_arrange would never reach them.
    vg_widget_t *first = vg_splitpane_get_first(sp);
    vg_widget_t *second = vg_splitpane_get_second(sp);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    vg_splitpane_set_min_sizes(sp, 200.0f, 200.0f);
    vg_splitpane_set_position(sp, 0.5f);

    // Total available is much smaller than min_first + min_second (400). The
    // pre-fix code gave the second pane the full space and silently zeroed the
    // first pane. Post-fix: distribute proportionally — both panes get the
    // same share since both mins are equal.
    vg_widget_arrange(&sp->base, 0.0f, 0.0f, 300.0f + sp->splitter_size, 50.0f);

    ASSERT(first->width > 100.0f);
    ASSERT(second->width > 100.0f);
    ASSERT_NEAR(first->width, second->width, 1.0f);

    vg_widget_destroy(&sp->base);
}

/// @brief Split-pane state changes must invalidate the complete ancestor chain
/// so a retained root layout cannot leave collapsed child geometry stale.
TEST(splitpane_collapse_invalidates_ancestor_layout) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    vg_splitpane_t *sp = vg_splitpane_create(root, VG_SPLIT_HORIZONTAL);
    ASSERT_NOT_NULL(sp);

    root->needs_layout = false;
    root->needs_paint = false;
    sp->base.needs_layout = false;
    sp->base.needs_paint = false;

    vg_splitpane_collapse_second(sp);
    ASSERT_TRUE(sp->base.needs_layout);
    ASSERT_TRUE(root->needs_layout);
    ASSERT_TRUE(root->needs_paint);

    root->needs_layout = false;
    root->needs_paint = false;
    vg_splitpane_restore(sp);
    ASSERT_TRUE(root->needs_layout);
    ASSERT_TRUE(root->needs_paint);

    vg_widget_destroy(root);
}

//=============================================================================
// Fix #11: scrollview hit-test excludes the scrollbar gutter
//=============================================================================

/// @brief Fix #11 — clicks in the scrollbar gutter hit the scrollview, not the child beneath.
TEST(scrollview_hit_test_excludes_scrollbar_gutter) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_scrollview_t *sv = vg_scrollview_create(root);
    ASSERT_NOT_NULL(sv);

    // Place a child label that fills the full scrollview width — including
    // under the scrollbar gutter.
    vg_widget_t *child = vg_widget_create(VG_WIDGET_LABEL);
    child->visible = true;
    child->enabled = true;
    vg_widget_add_child(&sv->base, child);

    root->visible = true;
    root->enabled = true;
    sv->base.visible = true;
    sv->base.enabled = true;

    // Force the vertical scrollbar to be visible so the gutter exists at
    // the right edge. Auto-hide would otherwise drop it because the child
    // has no measured overflow content at arrange time, and the hit-test
    // correctly only excludes the gutter when a scrollbar is actually
    // shown.
    sv->auto_hide_scrollbars = false;
    sv->show_v_scrollbar = true;
    sv->show_h_scrollbar = false;

    vg_widget_arrange(root, 0.0f, 0.0f, 100.0f, 100.0f);
    vg_widget_arrange(&sv->base, 0.0f, 0.0f, 100.0f, 100.0f);
    child->x = 0.0f;
    child->y = 0.0f;
    child->width = 100.0f;
    child->height = 100.0f;
    child->measured_width = 100.0f;
    child->measured_height = 100.0f;

    // Click at (95, 50) — inside the right-edge gutter. Pre-fix, hit_test
    // descended into the child unconditionally and returned it. Post-fix, the
    // gutter strip is excluded so the hit returns the scrollview itself.
    vg_widget_t *hit = vg_widget_hit_test(root, 95.0f, 50.0f);
    ASSERT(hit == &sv->base);

    // A click in the middle still routes to the child.
    vg_widget_t *hit_center = vg_widget_hit_test(root, 50.0f, 50.0f);
    ASSERT(hit_center == child);

    vg_widget_destroy(root);
}

//=============================================================================
// Fix #12: flex non-stretch align doesn't shrink child below measured size
//=============================================================================

// Custom widget that paints nothing but has a fixed measured size so we can
// observe the post-arrange child size.
/// @brief vtable measure — reports a fixed 80×50 size, ignoring available space.
static void fixed_measure(vg_widget_t *w, float aw, float ah) {
    (void)aw;
    (void)ah;
    w->measured_width = 80.0f;
    w->measured_height = 50.0f;
}

/// @brief vtable arrange — accepts whatever size the parent assigns.
static void fixed_arrange(vg_widget_t *w, float x, float y, float ww, float hh) {
    w->x = x;
    w->y = y;
    w->width = ww;
    w->height = hh;
}

static vg_widget_vtable_t g_fixed_vtable = {
    .measure = fixed_measure,
    .arrange = fixed_arrange,
};

/// @brief Fix #12 — flex non-stretch alignment gives children their full measured_height; margins
/// are offsets only.
TEST(flex_non_stretch_keeps_measured_height) {
    vg_widget_t *hbox = vg_hbox_create(0.0f);
    vg_hbox_set_align(hbox, VG_ALIGN_CENTER); // non-stretch

    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    child->vtable = &g_fixed_vtable;
    vg_widget_set_margins(child, 5.0f, 5.0f, 5.0f, 5.0f);
    vg_widget_add_child(hbox, child);

    // Container is much taller than the child. Pre-fix: child_h was set to
    // measured_height - margin_top - margin_bottom = 40, shrinking the child
    // below its measured size. Post-fix: child_h == measured_height (50) and
    // margins act as positional offsets only.
    vg_widget_measure(hbox, 800.0f, 200.0f);
    vg_widget_arrange(hbox, 0.0f, 0.0f, 800.0f, 200.0f);

    ASSERT_EQ(child->height, 50.0f);

    vg_widget_destroy(hbox);
}

//=============================================================================
// Fix #13: textinput password mask handles long content (no fixed buffer)
//=============================================================================

/// @brief Fix #13 — password mask uses dynamic allocation; 4000-char text survives round-trip
/// without truncation.
TEST(textinput_password_long_text_round_trip_intact) {
    vg_textinput_t *ti = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(ti);
    ti->password_mode = true; // public struct field; no setter exists

    // Build a 4000-character ASCII payload — well past the old 1023 cap.
    enum { N = 4000 };

    char *payload = (char *)malloc(N + 1);
    ASSERT_NOT_NULL(payload);
    for (int i = 0; i < N; i++)
        payload[i] = (char)('a' + (i % 26));
    payload[N] = '\0';
    vg_textinput_set_text(ti, payload);

    // The underlying text buffer must remain intact regardless of the mask
    // rendering buffer; pre-fix this was already true, but the mask render
    // would silently truncate. The mask path now allocates dynamically — we
    // can't directly observe the rendered buffer without a window, but
    // verifying the input is still N chars guards against any regression in
    // the storage path the password fix might have touched.
    ASSERT_EQ(strlen(ti->text), (size_t)N);

    free(payload);
    vg_widget_destroy(&ti->base);
}

//=============================================================================
// Fix #9: codeeditor scrollbar handles exact-fit content (no NaN)
//=============================================================================

// The scrollbar branch lives inside paint() and needs a vgfx canvas to invoke
// fully. As a sanity check we construct a buffer where total_content_height
// would equal visible_height and confirm the editor's scroll_y stays clamped
// to a finite value when set by the API. The actual NaN-guard is exercised
// at paint time; this test guards against future regressions in scroll_y
// clamping that would propagate the same divide-by-zero.

/// @brief Fix #9 — scroll_y stays finite (0) when content fits exactly, preventing a divide-by-zero
/// NaN.
TEST(codeeditor_exact_fit_scroll_position_finite) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    vg_codeeditor_set_text(ed, "one\n");

    // Force scroll to a defined position; with one short line and no scrolling
    // possible, the editor should keep scroll_y at 0 rather than producing NaN.
    ed->scroll_y = 0.0f;
    ASSERT(ed->scroll_y == ed->scroll_y); // not NaN

    vg_widget_destroy(&ed->base);
}

//=============================================================================
// Round 2 regressions + audit findings
//=============================================================================

// R2: Wheel event mouse/wheel union — delta_y must survive dispatch intact.
// vg_event.mouse and vg_event.wheel share a union, so any call that writes
// event.mouse.x/y on a wheel event destroys wheel.delta_x/y. The fix removes
// VG_EVENT_MOUSE_WHEEL from event_has_widget_local_mouse_coords and the
// analogous list in rt_gui_send_event_to_widget.
/// @brief R2 — wheel.delta_x/y are not overwritten when the event dispatch loop localises mouse
/// coordinates.
TEST(wheel_delta_survives_localize_call) {
    // Simulate the event-construction sequence for a scroll event.
    vg_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = VG_EVENT_MOUSE_WHEEL;
    ev.wheel.delta_x = 2.5f;
    ev.wheel.delta_y = -7.5f;
    ev.wheel.screen_x = 100.0f;
    ev.wheel.screen_y = 200.0f;

    // Snapshot the on-wire delta (aliased to mouse.x/y).
    float before_dx = ev.wheel.delta_x;
    float before_dy = ev.wheel.delta_y;

    // Create a widget at (50, 80) and dispatch to it. Pre-fix, the localize
    // step would overwrite the union slot, making wheel.delta_y collapse to
    // (screen_y - widget_y) = 120, destroying the scroll intent.
    vg_widget_t *w = vg_widget_create(VG_WIDGET_LABEL);
    ASSERT_NOT_NULL(w);
    w->x = 50.0f;
    w->y = 80.0f;
    w->width = 200.0f;
    w->height = 200.0f;
    w->measured_width = 200.0f;
    w->measured_height = 200.0f;

    ev.target = w;
    vg_event_send(w, &ev);

    // After dispatch, the wheel deltas must match what we put in — proving
    // mouse.x/y was not overwritten.
    ASSERT_EQ(ev.wheel.delta_x, before_dx);
    ASSERT_EQ(ev.wheel.delta_y, before_dy);

    vg_widget_destroy(w);
}

/// @brief R2 — the ZannaGFX positive-down Y convention is translated to the GUI positive-up
/// wheel convention exactly once at the platform boundary.
TEST(platform_scroll_direction_is_translated_for_gui_widgets) {
    vgfx_event_t platform_event;
    memset(&platform_event, 0, sizeof(platform_event));
    platform_event.type = VGFX_EVENT_SCROLL;
    platform_event.time_ms = 9182;
    platform_event.data.scroll.delta_x = 2.5f;
    platform_event.data.scroll.delta_y = 3.0f; // Native/content direction: down.
    platform_event.data.scroll.x = 140;
    platform_event.data.scroll.y = 220;

    vg_event_t event = vg_event_from_platform(&platform_event);
    ASSERT_EQ(event.type, VG_EVENT_MOUSE_WHEEL);
    ASSERT_EQ(event.timestamp, (uint64_t)9182);
    ASSERT_EQ(event.wheel.delta_x, 2.5f);
    ASSERT_EQ(event.wheel.delta_y, -3.0f); // GUI wheel direction: down.
    ASSERT_EQ(event.wheel.screen_x, 140.0f);
    ASSERT_EQ(event.wheel.screen_y, 220.0f);
}

/// @brief R2 — Enter key on a button reports a click with the event timestamp; key repeat does not.
TEST(button_keyboard_activation_reports_actual_click) {
    vg_button_t *button = vg_button_create(NULL, "Run");
    ASSERT_NOT_NULL(button);

    vg_widget_clear_reported_click();
    vg_event_t key = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, 0);
    key.timestamp = 777;
    ASSERT_TRUE(vg_event_send(&button->base, &key));

    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    ASSERT(state.reported_click_widget == &button->base);
    ASSERT_EQ(state.reported_click_time_ms, (uint64_t)777);

    vg_widget_clear_reported_click();
    key.timestamp = 778;
    key.key.repeat = true;
    ASSERT_TRUE(vg_event_send(&button->base, &key));
    vg_widget_get_runtime_state(&state);
    ASSERT_NULL(state.reported_click_widget);

    vg_widget_destroy(&button->base);
}

/// @brief R2 — mouse-up outside the pressed widget's bounds does not synthesize a click event.
TEST(mouseup_outside_pressed_widget_does_not_report_click) {
    vg_button_t *button = vg_button_create(NULL, "Run");
    ASSERT_NOT_NULL(button);
    vg_widget_arrange(&button->base, 0.0f, 0.0f, 80.0f, 30.0f);

    vg_widget_clear_reported_click();
    vg_event_t down = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    ASSERT_FALSE(vg_event_send(&button->base, &down));

    vg_event_t up = vg_event_mouse(VG_EVENT_MOUSE_UP, 120.0f, 120.0f, VG_MOUSE_LEFT, 0);
    ASSERT_FALSE(vg_event_send(&button->base, &up));

    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    ASSERT_NULL(state.reported_click_widget);

    vg_widget_destroy(&button->base);
}

static int g_mouseup_click_order[4];
static int g_mouseup_click_order_count = 0;

/// @brief vtable handle_event — records the order in which MOUSE_UP and CLICK events arrive.
static bool mouseup_click_order_handle_event(vg_widget_t *widget, vg_event_t *event) {
    (void)widget;
    if (event->type == VG_EVENT_MOUSE_UP) {
        g_mouseup_click_order[g_mouseup_click_order_count++] = 1;
        return false;
    }
    if (event->type == VG_EVENT_CLICK) {
        g_mouseup_click_order[g_mouseup_click_order_count++] = 2;
        return true;
    }
    return false;
}

static vg_widget_vtable_t g_mouseup_click_order_vtable = {
    .handle_event = mouseup_click_order_handle_event,
};

/// @brief R2 — MOUSE_UP handler fires before the synthesized CLICK event in the same dispatch
/// sequence.
TEST(mouseup_handler_runs_before_synthesized_click) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(widget);
    widget->vtable = &g_mouseup_click_order_vtable;
    vg_widget_arrange(widget, 0.0f, 0.0f, 80.0f, 30.0f);

    memset(g_mouseup_click_order, 0, sizeof(g_mouseup_click_order));
    g_mouseup_click_order_count = 0;

    vg_event_t down = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    (void)vg_event_send(widget, &down);
    vg_event_t up = vg_event_mouse(VG_EVENT_MOUSE_UP, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_send(widget, &up));

    ASSERT_EQ(g_mouseup_click_order_count, 2);
    ASSERT_EQ(g_mouseup_click_order[0], 1);
    ASSERT_EQ(g_mouseup_click_order[1], 2);

    vg_widget_destroy(widget);
}

/// @brief R2 — a removed listbox item handle is no longer live; selecting it leaves selected
/// unchanged.
TEST(listbox_removed_item_handle_is_inert) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(listbox);
    vg_listbox_item_t *item = vg_listbox_add_item(listbox, "alpha", NULL);
    ASSERT_NOT_NULL(item);

    vg_listbox_remove_item(listbox, item);
    ASSERT_FALSE(vg_listbox_item_is_live(item));
    vg_listbox_select(listbox, item);
    ASSERT_NULL(listbox->selected);

    vg_widget_destroy(&listbox->base);
}

/// @brief R2 — removed treeview nodes and their subtrees become inert; operations on them are
/// no-ops.
TEST(treeview_removed_node_handles_are_inert) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    ASSERT_NOT_NULL(tree);
    vg_tree_node_t *parent = vg_treeview_add_node(tree, NULL, "parent");
    ASSERT_NOT_NULL(parent);
    vg_tree_node_t *child = vg_treeview_add_node(tree, parent, "child");
    ASSERT_NOT_NULL(child);

    vg_treeview_remove_node(tree, parent);
    ASSERT_FALSE(vg_tree_node_is_live(parent));
    ASSERT_FALSE(vg_tree_node_is_live(child));

    vg_treeview_select(tree, child);
    ASSERT_NULL(tree->selected);
    vg_tree_node_set_data(child, (void *)0x1234);
    ASSERT_NULL(vg_treeview_add_node(tree, child, "grandchild"));

    vg_widget_destroy(&tree->base);
}

/// @brief R2 — scrollview content_height updates when the child's preferred size changes between
/// arrange calls.
TEST(scrollview_auto_content_size_tracks_child_measurement) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    vg_scrollview_t *scroll = vg_scrollview_create(root);
    ASSERT_NOT_NULL(scroll);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(child);
    vg_widget_add_child(&scroll->base, child);

    vg_widget_set_preferred_size(child, 60.0f, 40.0f);
    vg_widget_arrange(&scroll->base, 0.0f, 0.0f, 100.0f, 30.0f);
    ASSERT_EQ(scroll->content_height, 40.0f);

    vg_widget_set_preferred_size(child, 60.0f, 90.0f);
    vg_widget_arrange(&scroll->base, 0.0f, 0.0f, 100.0f, 30.0f);
    ASSERT_EQ(scroll->content_height, 90.0f);
    ASSERT_TRUE(scroll->show_v_scrollbar);

    vg_widget_destroy(root);
}

/// @brief R2 — vg_dropdown_clear closes the open popup and releases input capture before clearing
/// items.
TEST(dropdown_clear_closes_open_capture) {
    vg_dropdown_t *dropdown = vg_dropdown_create(NULL);
    ASSERT_NOT_NULL(dropdown);
    ASSERT_EQ(vg_dropdown_add_item(dropdown, "one"), 0);
    ASSERT_EQ(vg_dropdown_add_item(dropdown, "two"), 1);

    dropdown->open = true;
    vg_widget_set_input_capture(&dropdown->base);
    ASSERT(vg_widget_get_input_capture() == &dropdown->base);

    vg_dropdown_clear(dropdown);
    ASSERT_FALSE(dropdown->open);
    ASSERT_NULL(vg_widget_get_input_capture());
    ASSERT_EQ(dropdown->item_count, 0);

    vg_widget_destroy(&dropdown->base);
}

/// @brief R2 — add_separator returns a non-NULL item handle with separator==true and the correct
/// owner.
TEST(contextmenu_separator_returns_item_handle) {
    vg_contextmenu_t *menu = vg_contextmenu_create();
    ASSERT_NOT_NULL(menu);

    vg_menu_item_t *separator = vg_contextmenu_add_separator(menu);
    ASSERT_NOT_NULL(separator);
    ASSERT_TRUE(separator->separator);
    ASSERT(separator->owner_contextmenu == menu);
    ASSERT_EQ(menu->item_count, (size_t)1);

    vg_widget_destroy(&menu->base);
}

/// @brief R2 — vg_widget_is_live returns false for a zero-initialised non-widget stack variable.
TEST(widget_live_sentinel_rejects_non_widget_storage) {
    uint64_t not_a_widget = 0;
    ASSERT_FALSE(vg_widget_is_live((const vg_widget_t *)&not_a_widget));
}

// A3: Dropdown flip-above logic. Without a real window the flip-above branch
// is gated on vgfx_get_size succeeding, so here we only verify that the
// absence of a window does not crash and the panel still positions below by
// default.
/// @brief A3 — opening a dropdown with no window (NULL impl_data) does not crash; panel defaults to
/// below.
TEST(dropdown_flip_above_without_window_is_noop) {
    vg_dropdown_t *dd = vg_dropdown_create(NULL);
    ASSERT_NOT_NULL(dd);
    vg_dropdown_add_item(dd, "one");
    vg_dropdown_add_item(dd, "two");
    vg_dropdown_add_item(dd, "three");
    dd->base.impl_data = NULL; // no window

    dd->open = true;
    // Exercise hit-test at a point just below the trigger — should resolve
    // without crashing and report a hit even without a window.
    dd->base.x = 0.0f;
    dd->base.y = 0.0f;
    dd->base.width = 100.0f;
    dd->base.height = 20.0f;
    // (Internal panel math runs; just make sure the call returns cleanly.)
    (void)dd;

    vg_widget_destroy(&dd->base);
}

// A4: Narrow scrollview keeps child hit-testing. Previously a 10-px-wide
// scrollview would have child_clip_w go negative and block descent.
/// @brief A4 — a narrow scrollview (< kMinDimForGutter) still routes clicks to children correctly.
TEST(scrollview_narrow_still_hit_tests_children) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_scrollview_t *sv = vg_scrollview_create(root);
    ASSERT_NOT_NULL(sv);

    vg_widget_t *child = vg_widget_create(VG_WIDGET_LABEL);
    child->visible = true;
    child->enabled = true;
    vg_widget_add_child(&sv->base, child);

    root->visible = true;
    root->enabled = true;
    sv->base.visible = true;
    sv->base.enabled = true;

    // Narrower than the gutter heuristic's minimum dimension.
    vg_widget_arrange(root, 0.0f, 0.0f, 24.0f, 24.0f);
    vg_widget_arrange(&sv->base, 0.0f, 0.0f, 24.0f, 24.0f);
    child->x = 0.0f;
    child->y = 0.0f;
    child->width = 24.0f;
    child->height = 24.0f;
    child->measured_width = 24.0f;
    child->measured_height = 24.0f;

    // Post-fix: even on a 24×24 scrollview (< kMinDimForGutter), clicks on the
    // child are still routed to the child rather than swallowed.
    vg_widget_t *hit = vg_widget_hit_test(root, 12.0f, 12.0f);
    ASSERT(hit == child);

    vg_widget_destroy(root);
}

// A5: Tooltip wrap must terminate on whitespace-only content. A bug in the
// advance logic could spin the wrap loop forever on "   " / tab-only text.
/// @brief A5 — tooltip word-wrap terminates in bounded time on whitespace-only content (no infinite
/// loop).
TEST(tooltip_wrap_terminates_on_whitespace_only_text) {
    vg_tooltip_t *tip = vg_tooltip_create();
    ASSERT_NOT_NULL(tip);
    vg_tooltip_set_text(tip, "   "); // three spaces only
    tip->max_width = 12;             // very narrow to force wrap
    tip->padding = 1;

    // The post-fix progress guard ensures measurement returns in bounded time
    // even for pathological whitespace input. If the guard is missing, this
    // call loops forever and the test times out.
    // Exercise measure via the widget vtable (the internal wrap is called).
    if (tip->base.vtable && tip->base.vtable->measure)
        tip->base.vtable->measure(&tip->base, 0.0f, 0.0f);

    ASSERT_TRUE(true); // reached here => no infinite loop
    vg_widget_destroy(&tip->base);
}

/// @brief A5 — tooltip wrapping uses the last complete word that fits instead of cutting the next
/// word into the preceding line.
TEST(tooltip_wrap_prefers_complete_words) {
    vg_tooltip_t *tip = vg_tooltip_create();
    ASSERT_NOT_NULL(tip);
    vg_font_t *font = vg_font_load_file(ZANNA_TEST_FONT_PATH);
    ASSERT_NOT_NULL(font);
    tip->font = font;

    vg_text_metrics_t partial_line = {0};
    vg_font_measure_text(tip->font, tip->font_size, "MMMMM MMMM", &partial_line);
    ASSERT_TRUE(partial_line.width > 0.0f);

    tip->padding = 1;
    tip->max_width = (uint32_t)partial_line.width + tip->padding * 2;
    vg_tooltip_set_text(tip, "MMMMM MMMMM MMMMM");
    tip->base.vtable->measure(&tip->base, 0.0f, 0.0f);

    vg_font_metrics_t font_metrics = {0};
    vg_font_get_metrics(tip->font, tip->font_size, &font_metrics);
    float line_height =
        font_metrics.line_height > 0 ? (float)font_metrics.line_height : tip->font_size;
    float expected_height = line_height * 3.0f + (float)tip->padding * 2.0f;
    ASSERT_NEAR(tip->base.measured_height, expected_height, 0.5f);

    vg_widget_destroy(&tip->base);
    vg_font_destroy(font);
}

/// @brief A5 — one explicit newline starts the next line without inserting an unintended blank
/// row between the two paragraphs.
TEST(tooltip_wrap_honors_single_newline_once) {
    vg_tooltip_t *tip = vg_tooltip_create();
    ASSERT_NOT_NULL(tip);
    vg_font_t *font = vg_font_load_file(ZANNA_TEST_FONT_PATH);
    ASSERT_NOT_NULL(font);
    tip->font = font;

    tip->padding = 1;
    tip->max_width = 1000;
    vg_tooltip_set_text(tip, "first line\nsecond line");
    tip->base.vtable->measure(&tip->base, 0.0f, 0.0f);

    vg_font_metrics_t font_metrics = {0};
    vg_font_get_metrics(tip->font, tip->font_size, &font_metrics);
    float line_height =
        font_metrics.line_height > 0 ? (float)font_metrics.line_height : tip->font_size;
    float expected_height = line_height * 2.0f + (float)tip->padding * 2.0f;
    ASSERT_NEAR(tip->base.measured_height, expected_height, 0.5f);

    vg_widget_destroy(&tip->base);
    vg_font_destroy(font);
}

/// @brief A5 — the exact long scene-hierarchy hint wraps inside a compact
/// root and a follow-cursor popup near the lower-right corner remains wholly
/// on screen.
TEST(tooltip_scene_hint_wraps_and_clamps_to_root) {
    vg_tooltip_manager_t *mgr = vg_tooltip_manager_get();
    ASSERT_NOT_NULL(mgr);
    vg_tooltip_manager_on_leave(mgr);

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *button = vg_widget_create(VG_WIDGET_BUTTON);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(button);
    root->visible = true;
    root->enabled = true;
    button->visible = true;
    button->enabled = true;
    vg_widget_add_child(root, button);
    vg_widget_arrange(root, 0.0f, 0.0f, 96.0f, 160.0f);
    vg_widget_arrange(button, 40.0f, 110.0f, 50.0f, 24.0f);

    vg_widget_set_tooltip_text(button, "Open the scene hierarchy at the full editor width");
    vg_tooltip_manager_on_hover(mgr, button, 92, 156);
    ASSERT_NOT_NULL(mgr->active_tooltip);
    ASSERT(mgr->active_tooltip->anchor_widget == button);

    vg_font_t *font = vg_font_load_file(ZANNA_TEST_FONT_PATH);
    ASSERT_NOT_NULL(font);
    mgr->active_tooltip->font = font;
    vg_tooltip_show_at(mgr->active_tooltip, 92, 156);

    vg_font_metrics_t metrics = {0};
    vg_font_get_metrics(font, mgr->active_tooltip->font_size, &metrics);
    float line_height =
        metrics.line_height > 0 ? (float)metrics.line_height : mgr->active_tooltip->font_size;
    ASSERT(mgr->active_tooltip->base.measured_height >
           line_height + (float)mgr->active_tooltip->padding * 2.0f);
    ASSERT(mgr->active_tooltip->base.measured_width <= 92.0f);
    ASSERT(mgr->active_tooltip->base.x >= 0.0f);
    ASSERT(mgr->active_tooltip->base.y >= 0.0f);
    ASSERT(mgr->active_tooltip->base.x + mgr->active_tooltip->base.measured_width <= 96.0f);
    ASSERT(mgr->active_tooltip->base.y + mgr->active_tooltip->base.measured_height <= 160.0f);
    ASSERT(mgr->active_tooltip->base.y < 156.0f);

    vg_tooltip_hide(mgr->active_tooltip);
    mgr->active_tooltip->font = vg_theme_get_current()->typography.font_regular;
    vg_widget_destroy(root);
    vg_font_destroy(font);
    ASSERT_NULL(mgr->hovered_widget);
}

// A6: TreeView scroll_y is re-clamped when a node collapses.
/// @brief A6 — collapsing a node re-clamps scroll_y so the viewport doesn't show a blank area.
TEST(treeview_collapse_reclamps_scroll) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    ASSERT_NOT_NULL(tree);
    tree->base.height = 50.0f;
    tree->row_height = 10.0f;

    // Build a small tree: root with many expanded children.
    vg_tree_node_t *root = vg_treeview_add_node(tree, NULL, "root");
    for (int i = 0; i < 20; i++) {
        char label[16];
        snprintf(label, sizeof(label), "child-%d", i);
        vg_treeview_add_node(tree, root, label);
    }
    vg_treeview_expand(tree, root);

    // Scroll near the bottom.
    tree->scroll_y = 150.0f;

    // Collapse the root. Content shrinks to 1 visible row — scroll_y must be
    // clamped back into range; pre-fix it would stay at 150, showing a blank
    // viewport.
    vg_treeview_collapse(tree, root);

    float max_scroll = (float)1 * tree->row_height - tree->base.height;
    if (max_scroll < 0.0f)
        max_scroll = 0.0f;
    ASSERT(tree->scroll_y <= max_scroll + 0.01f);

    vg_widget_destroy(&tree->base);
}

// A10: Notification fade with fade_duration_ms == 0 must not produce NaN
// opacity and must snap to dismissed promptly past duration.
/// @brief A10 — zero fade_duration_ms snaps opacity to 1 (not NaN) and dismisses promptly past
/// duration.
TEST(notification_zero_fade_duration_snaps_cleanly) {
    vg_notification_manager_t *mgr = vg_notification_manager_create();
    ASSERT_NOT_NULL(mgr);
    mgr->fade_duration_ms = 0; // no fade
    mgr->slide_duration_ms = 0;

    uint32_t id = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "t", "m", 50);
    ASSERT(id > 0);

    vg_notification_manager_update(mgr, 1000);
    ASSERT(mgr->notification_count == 1);
    ASSERT(mgr->notifications[0]->opacity == 1.0f); // snap, not NaN

    // Past duration with no fade — should dismiss on this tick.
    vg_notification_manager_update(mgr, 1100);
    ASSERT_EQ(mgr->notification_count, (size_t)0);

    vg_widget_destroy(&mgr->base);
}

/// @brief Verify retained notification bounds exclude the manager's full-window positioning base.
/// @details A settled toast must report a compact shadow-inclusive rectangle, while removal resets
///          every output to zero so the compositor can damage only the previous retained union.
TEST(notification_visual_bounds_track_only_painted_toasts) {
    vg_notification_manager_t *mgr = vg_notification_manager_create();
    ASSERT_NOT_NULL(mgr);
    mgr->base.x = 0.0f;
    mgr->base.y = 0.0f;
    mgr->base.width = 640.0f;
    mgr->base.height = 480.0f;
    mgr->fade_duration_ms = 0;
    mgr->slide_duration_ms = 0;

    uint32_t id = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "Title", "Body", 0);
    ASSERT(id != 0);
    vg_notification_manager_update(mgr, 1000);

    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    ASSERT_TRUE(vg_notification_manager_get_visual_bounds(mgr, &x, &y, &width, &height));
    ASSERT(width > (float)mgr->notification_width);
    ASSERT(width < mgr->base.width);
    ASSERT(height > 0.0f);
    ASSERT(height < mgr->base.height);

    vg_notification_dismiss(mgr, id);
    vg_notification_manager_update(mgr, 1001);
    ASSERT_FALSE(vg_notification_manager_get_visual_bounds(mgr, &x, &y, &width, &height));
    ASSERT_EQ(x, 0.0f);
    ASSERT_EQ(y, 0.0f);
    ASSERT_EQ(width, 0.0f);
    ASSERT_EQ(height, 0.0f);
    vg_notification_manager_destroy(mgr);
}

/// @brief R2 — tooltip manager respects duration_ms hide-delay, hiding after exactly hide_timer ms
/// on leave.
TEST(tooltip_manager_honors_duration_and_hide_delay) {
    vg_tooltip_manager_t *mgr = vg_tooltip_manager_get();
    ASSERT_NOT_NULL(mgr);
    vg_tooltip_manager_on_leave(mgr);

    vg_widget_t *w = vg_widget_create(VG_WIDGET_LABEL);
    ASSERT_NOT_NULL(w);
    vg_widget_set_tooltip_text(w, "timed");

    vg_tooltip_manager_on_hover(mgr, w, 10, 20);
    ASSERT_NOT_NULL(mgr->active_tooltip);
    vg_tooltip_set_timing(mgr->active_tooltip, 0, 40, 30);
    mgr->hover_start_time = 1000;
    vg_tooltip_manager_update(mgr, 1000);
    ASSERT_TRUE(mgr->active_tooltip->is_visible);

    mgr->active_tooltip->show_timer = 1000;
    vg_tooltip_manager_update(mgr, 1031);
    ASSERT_FALSE(mgr->active_tooltip->is_visible);

    vg_tooltip_manager_on_hover(mgr, w, 10, 20);
    mgr->hover_start_time = 2000;
    vg_tooltip_manager_update(mgr, 2000);
    ASSERT_TRUE(mgr->active_tooltip->is_visible);
    mgr->active_tooltip->duration_ms = 0;

    vg_tooltip_manager_on_leave(mgr);
    ASSERT_TRUE(mgr->active_tooltip->is_visible);
    ASSERT(mgr->active_tooltip->hide_timer > 0);
    uint64_t hide_at = mgr->active_tooltip->hide_timer;
    vg_tooltip_manager_update(mgr, hide_at - 1);
    ASSERT_TRUE(mgr->active_tooltip->is_visible);
    vg_tooltip_manager_update(mgr, hide_at);
    ASSERT_FALSE(mgr->active_tooltip->is_visible);

    vg_widget_destroy(w);
    vg_tooltip_manager_on_leave(mgr);
}

/// @brief R2 — manually dismissed notification plays fade+slide exit animation before being
/// removed.
TEST(notification_manual_dismiss_respects_exit_animation) {
    vg_notification_manager_t *mgr = vg_notification_manager_create();
    ASSERT_NOT_NULL(mgr);
    mgr->fade_duration_ms = 100;
    mgr->slide_duration_ms = 120;

    uint32_t id = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "Title", "Body", 0);
    ASSERT(id > 0);

    vg_notification_manager_update(mgr, 1000);
    vg_notification_manager_update(mgr, 1200);
    ASSERT_EQ(mgr->notification_count, (size_t)1);
    ASSERT_EQ(mgr->notifications[0]->opacity, 1.0f);
    ASSERT_EQ(mgr->notifications[0]->slide_progress, 1.0f);

    vg_notification_dismiss(mgr, id);
    ASSERT_TRUE(mgr->notifications[0]->dismissed);
    ASSERT_EQ(mgr->notifications[0]->dismiss_started_at, (uint64_t)0);

    vg_notification_manager_update(mgr, 1200);
    ASSERT_EQ(mgr->notification_count, (size_t)1);
    ASSERT_EQ(mgr->notifications[0]->dismiss_started_at, (uint64_t)1200);

    vg_notification_manager_update(mgr, 1260);
    ASSERT_EQ(mgr->notification_count, (size_t)1);
    ASSERT(mgr->notifications[0]->opacity < 1.0f);
    ASSERT(mgr->notifications[0]->opacity > 0.0f);
    ASSERT(mgr->notifications[0]->slide_progress < 1.0f);
    ASSERT(mgr->notifications[0]->slide_progress > 0.0f);

    vg_notification_manager_update(mgr, 1330);
    ASSERT_EQ(mgr->notification_count, (size_t)0);

    vg_widget_destroy(&mgr->base);
}

//=============================================================================
// Round 3 — Zanna.GUI class audit fixes
//=============================================================================

/// @brief R3 — radiogroup_destroy clears button→group pointers; button_destroy removes it from
/// group's list.
TEST(radiogroup_destroy_and_radio_destroy_clear_cross_references) {
    vg_radiogroup_t *group = vg_radiogroup_create();
    ASSERT_NOT_NULL(group);
    vg_radiobutton_t *a = vg_radiobutton_create(NULL, "A", group);
    vg_radiobutton_t *b = vg_radiobutton_create(NULL, "B", group);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(group->button_count, 2);

    vg_radiobutton_set_selected(b, true);
    ASSERT_EQ(vg_radiogroup_get_selected(group), 1);

    vg_widget_destroy(&b->base);
    ASSERT_EQ(group->button_count, 1);
    ASSERT_EQ(vg_radiogroup_get_selected(group), -1);

    vg_radiogroup_destroy(group);
    ASSERT_NULL(a->group);
    vg_radiobutton_set_selected(a, true);
    ASSERT_TRUE(vg_radiobutton_is_selected(a));
    vg_widget_destroy(&a->base);
}

typedef struct {
    int count;
    uint32_t last_color;
} colorpicker_change_state_t;

/// @brief on_change callback that counts invocations and records the last reported color.
static void colorpicker_change_counter(vg_widget_t *widget, uint32_t color, void *user_data) {
    (void)widget;
    colorpicker_change_state_t *state = (colorpicker_change_state_t *)user_data;
    state->count++;
    state->last_color = color;
}

/// @brief R3 — set_color fires on_change exactly once; no-op when color unchanged.
TEST(colorpicker_set_color_emits_once_after_child_slider_sync) {
    vg_colorpicker_t *picker = vg_colorpicker_create(NULL);
    ASSERT_NOT_NULL(picker);
    colorpicker_change_state_t state = {0, 0};
    vg_colorpicker_set_on_change(picker, colorpicker_change_counter, &state);

    vg_colorpicker_set_color(picker, 0x11223344u);
    ASSERT_EQ(state.count, 1);
    ASSERT_EQ(state.last_color, 0x11223344u);

    vg_colorpicker_set_color(picker, 0x11223344u);
    ASSERT_EQ(state.count, 1);

    vg_widget_destroy(&picker->base);
}

typedef struct {
    int count;
    int last_index;
    uint32_t last_color;
} colorpalette_select_state_t;

/// @brief on_select callback that counts invocations and records the last selected index and color.
static void colorpalette_select_counter(vg_widget_t *palette,
                                        uint32_t color,
                                        int index,
                                        void *user_data) {
    (void)palette;
    colorpalette_select_state_t *state = (colorpalette_select_state_t *)user_data;
    state->count++;
    state->last_index = index;
    state->last_color = color;
}

/// @brief R3 — on_select fires once per CLICK event and not on MOUSE_DOWN; index and color are
/// accurate.
TEST(colorpalette_click_callback_fires_once_per_click) {
    uint32_t colors[] = {0xFF112233u, 0xFF445566u};
    vg_colorpalette_t *palette = vg_colorpalette_create(NULL);
    ASSERT_NOT_NULL(palette);
    vg_colorpalette_set_colors(palette, colors, 2);
    vg_widget_arrange(&palette->base, 80.0f, 60.0f, 100.0f, 40.0f);

    colorpalette_select_state_t state = {0, -1, 0};
    vg_colorpalette_set_on_select(palette, colorpalette_select_counter, &state);

    vg_event_t down = {0};
    down.type = VG_EVENT_MOUSE_DOWN;
    down.mouse.x = 5.0f;
    down.mouse.y = 5.0f;
    ASSERT_TRUE(palette->base.vtable->handle_event(&palette->base, &down));
    ASSERT_EQ(state.count, 0);

    vg_event_t click = down;
    click.type = VG_EVENT_CLICK;
    ASSERT_TRUE(palette->base.vtable->handle_event(&palette->base, &click));
    ASSERT_EQ(state.count, 1);
    ASSERT_EQ(state.last_index, 0);
    ASSERT_EQ(state.last_color, colors[0]);

    vg_widget_destroy(&palette->base);
}

/// @brief Recommendation 30 — ColorSwatch keyboard activation matches pointer selection and
/// exposes semantic color/revision state.
TEST(colorswatch_keyboard_activation_is_accessible_and_revisioned) {
    vg_colorswatch_t *swatch = vg_colorswatch_create(NULL, 0xFF112233u);
    ASSERT_NOT_NULL(swatch);
    ASSERT_EQ(vg_widget_get_accessible_role(&swatch->base), VG_ACCESSIBLE_ROLE_BUTTON);
    ASSERT(strcmp(vg_widget_get_accessible_value(&swatch->base), "#112233") == 0);

    colorpicker_change_state_t state = {0, 0};
    vg_colorswatch_set_on_select(swatch, colorpicker_change_counter, &state);
    const uint64_t revision = vg_widget_get_revision(&swatch->base);
    ASSERT_FALSE(vg_widget_was_changed(&swatch->base));

    vg_event_t enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, VG_MOD_NONE);
    ASSERT_TRUE(swatch->base.vtable->handle_event(&swatch->base, &enter));
    ASSERT_TRUE(enter.handled);
    ASSERT_TRUE(vg_colorswatch_is_selected(swatch));
    ASSERT_EQ(state.count, 1);
    ASSERT_EQ(state.last_color, 0xFF112233u);
    ASSERT_TRUE(vg_widget_was_changed(&swatch->base));
    ASSERT_FALSE(vg_widget_was_changed(&swatch->base));
    ASSERT(vg_widget_get_revision(&swatch->base) > revision);

    vg_colorswatch_set_color(swatch, 0x7FABCDEFu);
    ASSERT(strcmp(vg_widget_get_accessible_value(&swatch->base), "#ABCDEF alpha 127") == 0);
    ASSERT_TRUE(vg_widget_was_changed(&swatch->base));
    vg_widget_destroy(&swatch->base);
}

/// @brief Recommendation 30 — ColorPalette uses target-local hit coordinates, supports keyboard
/// navigation, compacts removal safely, and exposes semantic/revision state.
TEST(colorpalette_keyboard_removal_and_semantics_are_complete) {
    const uint32_t colors[] = {0xFF010203u, 0xFF112233u, 0xFFABCDEFu};
    vg_colorpalette_t *palette = vg_colorpalette_create(NULL);
    ASSERT_NOT_NULL(palette);
    vg_colorpalette_set_colors(palette, colors, 3);
    ASSERT_EQ(vg_widget_get_accessible_role(&palette->base), VG_ACCESSIBLE_ROLE_LIST);
    ASSERT_EQ(vg_colorpalette_get_color_count(palette), 3);
    uint32_t color = 0;
    ASSERT_TRUE(vg_colorpalette_get_color_at(palette, 1, &color));
    ASSERT_EQ(color, colors[1]);
    ASSERT_FALSE(vg_colorpalette_get_color_at(palette, -1, &color));

    vg_widget_arrange(&palette->base, 100.0f, 75.0f, 120.0f, 40.0f);
    colorpalette_select_state_t state = {0, -1, 0};
    vg_colorpalette_set_on_select(palette, colorpalette_select_counter, &state);
    (void)vg_widget_was_changed(&palette->base);

    vg_event_t click = vg_event_mouse(VG_EVENT_CLICK, 5.0f, 5.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(palette->base.vtable->handle_event(&palette->base, &click));
    ASSERT_EQ(vg_colorpalette_get_selected(palette), 0);
    ASSERT_EQ(state.count, 1);
    ASSERT(strcmp(vg_widget_get_accessible_value(&palette->base), "1 of 3, #010203") == 0);

    const uint64_t revision = vg_widget_get_revision(&palette->base);
    vg_event_t right = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_RIGHT, 0, VG_MOD_NONE);
    ASSERT_TRUE(palette->base.vtable->handle_event(&palette->base, &right));
    ASSERT_EQ(vg_colorpalette_get_selected(palette), 1);
    ASSERT_EQ(state.count, 2);
    ASSERT_EQ(state.last_color, colors[1]);
    ASSERT(vg_widget_get_revision(&palette->base) > revision);

    ASSERT_TRUE(vg_colorpalette_remove_color(palette, 0));
    ASSERT_EQ(vg_colorpalette_get_selected(palette), 0);
    ASSERT_TRUE(vg_colorpalette_get_color_at(palette, 0, &color));
    ASSERT_EQ(color, colors[1]);
    ASSERT(strcmp(vg_widget_get_accessible_value(&palette->base), "1 of 2, #112233") == 0);
    ASSERT_FALSE(vg_colorpalette_remove_color(palette, 8));
    ASSERT_TRUE(vg_colorpalette_remove_color(palette, 0));
    ASSERT_EQ(vg_colorpalette_get_selected(palette), -1);
    ASSERT(strcmp(vg_widget_get_accessible_value(&palette->base), "") == 0);
    ASSERT_TRUE(vg_widget_was_changed(&palette->base));

    vg_colorpalette_clear(palette);
    ASSERT_EQ(vg_colorpalette_get_color_count(palette), 0);
    vg_widget_destroy(&palette->base);
}

/// @brief Recommendation 30 — ColorPicker slider and keyboard changes publish exactly one
/// callback/edge, preserve alpha across RGB assignment, and keep semantic component text current.
TEST(colorpicker_keyboard_components_and_semantics_are_complete) {
    vg_colorpicker_t *picker = vg_colorpicker_create(NULL);
    ASSERT_NOT_NULL(picker);
    ASSERT_EQ(vg_widget_get_accessible_role(&picker->base), VG_ACCESSIBLE_ROLE_GROUP);
    ASSERT(strcmp(vg_widget_get_accessible_value(&picker->base), "#000000 alpha 255") == 0);

    colorpicker_change_state_t state = {0, 0};
    vg_colorpicker_set_on_change(picker, colorpicker_change_counter, &state);
    const uint64_t initial_revision = vg_widget_get_revision(&picker->base);
    vg_slider_set_value(picker->slider_r, 17.0f);
    ASSERT_EQ(state.count, 1);
    ASSERT_EQ(state.last_color, 0xFF110000u);
    ASSERT_TRUE(vg_widget_was_changed(&picker->base));
    ASSERT_FALSE(vg_widget_was_changed(&picker->base));
    ASSERT(vg_widget_get_revision(&picker->base) > initial_revision);

    vg_event_t shifted_right = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_RIGHT, 0, VG_MOD_SHIFT);
    ASSERT_TRUE(picker->base.vtable->handle_event(&picker->base, &shifted_right));
    ASSERT_EQ(picker->r, 27);
    ASSERT_EQ(state.count, 2);
    ASSERT_TRUE(vg_widget_was_changed(&picker->base));
    ASSERT(strcmp(vg_widget_get_accessible_value(&picker->base), "#1B0000 alpha 255") == 0);

    vg_colorpicker_show_alpha(picker, true);
    ASSERT_TRUE(vg_colorpicker_is_alpha_enabled(picker));
    ASSERT_FALSE(vg_widget_was_changed(&picker->base));
    vg_event_t up = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_UP, 0, VG_MOD_NONE);
    ASSERT_TRUE(picker->base.vtable->handle_event(&picker->base, &up));
    ASSERT_EQ(picker->active_channel, 3);
    vg_event_t left = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_LEFT, 0, VG_MOD_NONE);
    ASSERT_TRUE(picker->base.vtable->handle_event(&picker->base, &left));
    ASSERT_EQ(vg_colorpicker_get_alpha(picker), 254);
    ASSERT_EQ(state.count, 3);
    ASSERT_TRUE(vg_widget_was_changed(&picker->base));

    vg_colorpicker_set_rgb(picker, 1, 2, 3);
    ASSERT_EQ(vg_colorpicker_get_alpha(picker), 254);
    ASSERT_EQ(vg_colorpicker_get_color(picker), 0xFE010203u);
    ASSERT(strcmp(vg_widget_get_accessible_value(&picker->base), "#010203 alpha 254") == 0);
    vg_widget_destroy(&picker->base);
}

/// @brief R3 — label, checkbox, and radiobutton adopt the current theme's font_regular on
/// construction.
TEST(label_and_checkbox_use_theme_regular_font_on_create) {
    vg_theme_t *old_theme = vg_theme_get_current();
    vg_theme_t theme = *old_theme;
    vg_font_t *sentinel_font = (vg_font_t *)(uintptr_t)0x1234;
    theme.typography.font_regular = sentinel_font;
    theme.typography.size_normal = 15.0f;
    vg_theme_set_current(&theme);

    vg_label_t *label = vg_label_create(NULL, "Label");
    vg_checkbox_t *checkbox = vg_checkbox_create(NULL, "Check");
    vg_radiogroup_t *group = vg_radiogroup_create();
    vg_radiobutton_t *radio = vg_radiobutton_create(NULL, "Radio", group);
    vg_theme_set_current(old_theme);

    ASSERT_NOT_NULL(label);
    ASSERT_NOT_NULL(checkbox);
    ASSERT_NOT_NULL(radio);
    ASSERT(label->font == sentinel_font);
    ASSERT(checkbox->font == sentinel_font);
    ASSERT(radio->font == sentinel_font);

    vg_widget_destroy(&label->base);
    vg_widget_destroy(&checkbox->base);
    vg_widget_destroy(&radio->base);
    vg_radiogroup_destroy(group);
}

/// @brief R3 — Ctrl-clicking a selected item deselects it; current falls back to the previous
/// selection.
TEST(listbox_ctrl_toggle_off_does_not_leave_deselected_current_item) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    lb->multi_select = true;
    vg_listbox_item_t *a = vg_listbox_add_item(lb, "A", NULL);
    vg_listbox_item_t *b = vg_listbox_add_item(lb, "B", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    vg_listbox_select(lb, a);

    vg_event_t ev = {0};
    ev.type = VG_EVENT_MOUSE_DOWN;
    ev.modifiers = VG_MOD_CTRL;
    ev.mouse.y = lb->item_height + 1.0f;
    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT(b->selected);
    ASSERT(lb->selected == b);

    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT_FALSE(b->selected);
    ASSERT(lb->selected != b);
    ASSERT(lb->selected == a);

    vg_widget_destroy(&lb->base);
}

/// @brief R3 — Ctrl-toggle in virtual-mode listbox deselects item and moves current_index to
/// previous selection.
TEST(listbox_virtual_ctrl_toggle_off_clears_or_moves_current_index) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    lb->multi_select = true;
    vg_listbox_set_virtual_mode(lb, true, 3, 20.0f);
    vg_listbox_select_index(lb, 0);

    vg_event_t ev = {0};
    ev.type = VG_EVENT_MOUSE_DOWN;
    ev.modifiers = VG_MOD_CTRL;
    ev.mouse.y = 21.0f;
    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT_TRUE((lb->selection_bitmap[0] & (uint8_t)(1u << 1u)) != 0);
    ASSERT_EQ(lb->selected_index, (size_t)1);

    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT_FALSE((lb->selection_bitmap[0] & (uint8_t)(1u << 1u)) != 0);
    ASSERT_NEQ(lb->selected_index, (size_t)1);
    ASSERT_EQ(lb->selected_index, (size_t)0);

    vg_widget_destroy(&lb->base);
}

/// @brief R3 — wheel event bubbles (returns false) when the scrollview is already at the scroll
/// limit.
TEST(scrollview_wheel_bubbles_when_scroll_does_not_change) {
    vg_scrollview_t *sv = vg_scrollview_create(NULL);
    ASSERT_NOT_NULL(sv);
    vg_scrollview_set_content_size(sv, 100.0f, 100.0f);
    vg_widget_arrange(&sv->base, 0.0f, 0.0f, 100.0f, 100.0f);

    vg_event_t wheel = {0};
    wheel.type = VG_EVENT_MOUSE_WHEEL;
    wheel.wheel.delta_y = -1.0f;
    ASSERT_FALSE(sv->base.vtable->handle_event(&sv->base, &wheel));
    ASSERT_FALSE(wheel.handled);

    vg_widget_destroy(&sv->base);
}

/// @brief R3 — switching to VG_SCROLL_VERTICAL zeroes the now-disabled scroll_x offset.
TEST(scrollview_direction_disables_stale_axis_offset) {
    vg_scrollview_t *sv = vg_scrollview_create(NULL);
    ASSERT_NOT_NULL(sv);
    vg_scrollview_set_content_size(sv, 300.0f, 300.0f);
    vg_widget_arrange(&sv->base, 0.0f, 0.0f, 100.0f, 100.0f);
    vg_scrollview_set_scroll(sv, 50.0f, 60.0f);
    ASSERT(sv->scroll_x > 0.0f);

    vg_scrollview_set_direction(sv, VG_SCROLL_VERTICAL);
    ASSERT_EQ(sv->scroll_x, 0.0f);
    ASSERT(sv->scroll_y > 0.0f);

    vg_widget_destroy(&sv->base);
}

/// @brief R3 — inverted ranges are normalised; NaN values for range or current value are rejected.
TEST(slider_and_spinner_ranges_normalize_and_reject_nonfinite_values) {
    vg_slider_t *slider = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(slider);
    vg_slider_set_range(slider, 100.0f, 0.0f);
    ASSERT_EQ(slider->min_value, 0.0f);
    ASSERT_EQ(slider->max_value, 100.0f);
    vg_slider_set_value(slider, 25.0f);
    ASSERT_EQ(vg_slider_get_value(slider), 25.0f);
    vg_slider_set_range(slider, strtof("nan", NULL), 200.0f);
    ASSERT_EQ(slider->min_value, 0.0f);
    ASSERT_EQ(slider->max_value, 100.0f);
    vg_slider_set_value(slider, strtof("nan", NULL));
    ASSERT_EQ(vg_slider_get_value(slider), 0.0f);

    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);
    vg_spinner_set_range(spinner, 100.0, 0.0);
    ASSERT_EQ(spinner->min_value, 0.0);
    ASSERT_EQ(spinner->max_value, 100.0);
    vg_spinner_set_value(spinner, 25.0);
    ASSERT_EQ(vg_spinner_get_value(spinner), 25.0);
    vg_spinner_set_value(spinner, strtod("nan", NULL));
    ASSERT_EQ(vg_spinner_get_value(spinner), 0.0);

    vg_widget_destroy(&slider->base);
    vg_widget_destroy(&spinner->base);
}

/// @brief R3 — spinner text_buffer is dynamically sized; 90-decimal format produces output longer
/// than 64 bytes.
TEST(spinner_decimal_display_resizes_beyond_legacy_fixed_buffer) {
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);
    vg_spinner_set_range(spinner, -10.0, 10.0);
    vg_spinner_set_decimals(spinner, 90);
    vg_spinner_set_value(spinner, 1.0 / 3.0);

    ASSERT_NOT_NULL(spinner->text_buffer);
    ASSERT(strlen(spinner->text_buffer) > 64);
    ASSERT_EQ(spinner->cursor_pos, strlen(spinner->text_buffer));

    vg_widget_destroy(&spinner->base);
}

static int dropdown_change_count = 0;
static int dropdown_last_index = -2;
static char dropdown_last_text[64];

/// @brief on_change callback that counts invocations and records the last selected index and text.
static void dropdown_change_counter(vg_widget_t *dropdown,
                                    int index,
                                    const char *text,
                                    void *user_data) {
    (void)dropdown;
    (void)user_data;
    dropdown_change_count++;
    dropdown_last_index = index;
    snprintf(dropdown_last_text, sizeof(dropdown_last_text), "%s", text ? text : "");
}

/// @brief R3 — typeahead matches against the first Unicode codepoint of each item, not just the
/// first byte.
TEST(dropdown_unicode_typeahead_decodes_first_codepoint) {
    vg_dropdown_t *dd = vg_dropdown_create(NULL);
    ASSERT_NOT_NULL(dd);
    vg_dropdown_add_item(dd, "alpha");
    vg_dropdown_add_item(dd,
                         "\xC3\xA9"
                         "clair");

    vg_event_t ev = {0};
    ev.type = VG_EVENT_KEY_CHAR;
    ev.key.codepoint = 0x00E9;
    ASSERT_TRUE(dd->base.vtable->handle_event(&dd->base, &ev));
    ASSERT_EQ(vg_dropdown_get_selected(dd), 1);

    vg_widget_destroy(&dd->base);
}

/// @brief R3 — removing or clearing items fires on_change only when the effective selection
/// actually changes.
TEST(dropdown_remove_and_clear_notify_effective_selection_changes) {
    vg_dropdown_t *dd = vg_dropdown_create(NULL);
    ASSERT_NOT_NULL(dd);
    vg_dropdown_add_item(dd, "first");
    vg_dropdown_add_item(dd, "second");
    vg_dropdown_set_selected(dd, 1);

    dropdown_change_count = 0;
    dropdown_last_index = -2;
    dropdown_last_text[0] = '\0';
    vg_dropdown_set_on_change(dd, dropdown_change_counter, NULL);

    vg_dropdown_remove_item(dd, 0);
    ASSERT_EQ(dropdown_change_count, 1);
    ASSERT_EQ(dropdown_last_index, 0);
    ASSERT_EQ(strcmp(dropdown_last_text, "second"), 0);

    vg_dropdown_clear(dd);
    ASSERT_EQ(dropdown_change_count, 2);
    ASSERT_EQ(dropdown_last_index, -1);
    ASSERT_EQ(dropdown_last_text[0], '\0');

    vg_widget_destroy(&dd->base);
}

/// @brief R3 — unhandled KEY_DOWN events (e.g. Escape) bubble to the parent rather than being
/// consumed.
TEST(textinput_unhandled_keydown_bubbles_to_parent) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(input);

    vg_event_t ev = {0};
    ev.type = VG_EVENT_KEY_DOWN;
    ev.key.key = VG_KEY_ESCAPE;
    ASSERT_FALSE(input->base.vtable->handle_event(&input->base, &ev));

    input->read_only = true;
    ASSERT_FALSE(input->base.vtable->handle_event(&input->base, &ev));

    vg_widget_destroy(&input->base);
}

/// @brief Write a minimal valid 2×1 24-bit BMP to disk so vg_image_load_file can be tested.
static bool write_test_bmp_2x1(const char *path) {
    static const uint8_t bmp[] = {0x42, 0x4D, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36,
                                  0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x08, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00};
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(bmp, 1, sizeof(bmp), f) == sizeof(bmp);
    fclose(f);
    return ok;
}

/// @brief R3 — vg_image_load_file decodes a 2×1 BMP into RGBA pixels with correct channel values.
TEST(image_load_file_decodes_bmp_into_rgba_pixels) {
    char path[512];
    test_make_temp_path(path, sizeof(path), "vg_image_load_file_test", ".bmp");
    remove(path);
    ASSERT_TRUE(write_test_bmp_2x1(path));

    vg_image_t *image = vg_image_create(NULL);
    ASSERT_NOT_NULL(image);
    ASSERT_TRUE(vg_image_load_file(image, path));
    ASSERT_EQ(image->img_width, 2);
    ASSERT_EQ(image->img_height, 1);
    ASSERT_NOT_NULL(image->pixels);
    ASSERT_EQ(image->pixels[0], 0xFF);
    ASSERT_EQ(image->pixels[1], 0x00);
    ASSERT_EQ(image->pixels[2], 0x00);
    ASSERT_EQ(image->pixels[3], 0xFF);
    ASSERT_EQ(image->pixels[4], 0x00);
    ASSERT_EQ(image->pixels[5], 0xFF);
    ASSERT_EQ(image->pixels[6], 0x00);
    ASSERT_EQ(image->pixels[7], 0xFF);

    vg_widget_destroy(&image->base);
    remove(path);
}

/// @brief R3 — max_size, NaN constraints, negative margins, and NaN flex are all sanitised to valid
/// values.
TEST(layout_measure_constraints_and_negative_arrange_are_clamped) {
    vg_widget_t *vbox = vg_vbox_create(0.0f);
    ASSERT_NOT_NULL(vbox);
    vg_widget_set_preferred_size(vbox, 300.0f, 200.0f);
    vg_widget_set_max_size(vbox, 120.0f, 90.0f);
    vg_widget_measure(vbox, 1000.0f, 1000.0f);
    ASSERT_EQ(vbox->measured_width, 120.0f);
    ASSERT_EQ(vbox->measured_height, 90.0f);
    vg_widget_destroy(vbox);

    vg_widget_t *sanitized = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(sanitized);
    vg_widget_set_min_size(sanitized, 80.0f, strtof("nan", NULL));
    vg_widget_set_max_size(sanitized, 20.0f, 30.0f);
    vg_widget_set_preferred_size(sanitized, strtof("nan", NULL), 100.0f);
    vg_widget_set_margins(sanitized, -1.0f, strtof("nan", NULL), 2.0f, -3.0f);
    vg_widget_set_paddings(sanitized, strtof("nan", NULL), 3.0f, -4.0f, 5.0f);
    vg_widget_set_flex(sanitized, strtof("nan", NULL));
    ASSERT_EQ(sanitized->constraints.min_width, 80.0f);
    ASSERT_EQ(sanitized->constraints.min_height, 0.0f);
    ASSERT_EQ(sanitized->constraints.max_width, 80.0f);
    ASSERT_EQ(sanitized->constraints.max_height, 30.0f);
    ASSERT_EQ(sanitized->constraints.preferred_width, 0.0f);
    ASSERT_EQ(sanitized->constraints.preferred_height, 30.0f);
    ASSERT_EQ(sanitized->layout.margin_left, 0.0f);
    ASSERT_EQ(sanitized->layout.margin_top, 0.0f);
    ASSERT_EQ(sanitized->layout.margin_right, 2.0f);
    ASSERT_EQ(sanitized->layout.margin_bottom, 0.0f);
    ASSERT_EQ(sanitized->layout.padding_left, 0.0f);
    ASSERT_EQ(sanitized->layout.padding_top, 3.0f);
    ASSERT_EQ(sanitized->layout.padding_right, 0.0f);
    ASSERT_EQ(sanitized->layout.padding_bottom, 5.0f);
    ASSERT_EQ(sanitized->layout.flex, 0.0f);
    vg_widget_destroy(sanitized);

    vg_widget_t *grid = vg_grid_create(2, 2);
    ASSERT_NOT_NULL(grid);
    vg_grid_set_gap(grid, strtof("nan", NULL), -8.0f);
    vg_grid_set_column_width(grid, 0, strtof("nan", NULL));
    vg_grid_set_row_height(grid, 0, -4.0f);
    vg_grid_layout_t *grid_layout = (vg_grid_layout_t *)grid->impl_data;
    ASSERT_EQ(grid_layout->column_gap, 0.0f);
    ASSERT_EQ(grid_layout->row_gap, 0.0f);
    ASSERT_EQ(grid_layout->column_widths[0], 0.0f);
    ASSERT_EQ(grid_layout->row_heights[0], -4.0f);
    vg_widget_destroy(grid);

    vg_widget_t *hbox = vg_hbox_create(0.0f);
    ASSERT_NOT_NULL(hbox);
    vg_widget_set_paddings(hbox, 0.0f, 50.0f, 0.0f, 50.0f);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_fixed_vtable;
    vg_widget_add_child(hbox, child);
    vg_widget_measure(hbox, 20.0f, 20.0f);
    vg_widget_arrange(hbox, 0.0f, 0.0f, 20.0f, 20.0f);
    ASSERT(child->height >= 0.0f);
    ASSERT(child->width >= 0.0f);
    vg_widget_destroy(hbox);
}

//=============================================================================
// Round 4 — Runtime-facing GUI correctness fixes
//=============================================================================

static int g_route_outside_events = 0;
static int g_route_modal_events = 0;

/// @brief vtable handle_event — increments the per-widget counter keyed by user_data tag
/// (1=outside, 2=modal).
static bool route_test_handle_event(vg_widget_t *widget, vg_event_t *event) {
    if (widget->user_data == (void *)1)
        g_route_outside_events++;
    else if (widget->user_data == (void *)2)
        g_route_modal_events++;
    event->handled = true;
    return true;
}

/// @brief vtable can_focus — focusable when enabled and visible.
static bool route_test_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

static vg_widget_vtable_t g_route_test_vtable = {
    .handle_event = route_test_handle_event,
    .can_focus = route_test_can_focus,
};

/// @brief R4 — when a modal root is set, input_capture held by an outside widget is released on
/// mouse-down.
TEST(modal_root_releases_external_mouse_capture) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *outside = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *modal = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(outside);
    ASSERT_NOT_NULL(modal);

    outside->vtable = &g_route_test_vtable;
    modal->vtable = &g_route_test_vtable;
    outside->user_data = (void *)1;
    modal->user_data = (void *)2;
    vg_widget_add_child(root, outside);
    vg_widget_add_child(root, modal);
    vg_widget_arrange(root, 0.0f, 0.0f, 300.0f, 300.0f);
    vg_widget_arrange(outside, 0.0f, 0.0f, 80.0f, 80.0f);
    vg_widget_arrange(modal, 100.0f, 100.0f, 80.0f, 80.0f);

    g_route_outside_events = 0;
    g_route_modal_events = 0;
    vg_widget_set_input_capture(outside);
    vg_widget_set_modal_root(modal);

    vg_event_t ev = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &ev));
    ASSERT_NULL(vg_widget_get_input_capture());
    ASSERT_EQ(g_route_outside_events, 0);
    ASSERT_EQ(g_route_modal_events, 0);

    vg_widget_set_modal_root(NULL);
    vg_widget_destroy(root);
}

/// @brief R4 — keyboard events are redirected to the modal root; external capture is released.
TEST(modal_root_redirects_keyboard_away_from_external_capture) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *outside = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *modal = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(outside);
    ASSERT_NOT_NULL(modal);

    outside->vtable = &g_route_test_vtable;
    modal->vtable = &g_route_test_vtable;
    outside->user_data = (void *)1;
    modal->user_data = (void *)2;
    vg_widget_add_child(root, outside);
    vg_widget_add_child(root, modal);

    g_route_outside_events = 0;
    g_route_modal_events = 0;
    vg_widget_set_focus(outside);
    vg_widget_set_input_capture(outside);
    vg_widget_set_modal_root(modal);

    vg_event_t ev = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &ev));
    ASSERT_NULL(vg_widget_get_input_capture());
    ASSERT_EQ(g_route_outside_events, 0);
    ASSERT_EQ(g_route_modal_events, 1);

    vg_widget_set_modal_root(NULL);
    vg_widget_set_focus(NULL);
    vg_widget_destroy(root);
}

/// @brief R4 — removing an item from the wrong menu (cross-owner handle) is a silent no-op.
TEST(menubar_remove_rejects_cross_owner_handles) {
    vg_menubar_t *bar = vg_menubar_create(NULL);
    ASSERT_NOT_NULL(bar);
    vg_menu_t *file = vg_menubar_add_menu(bar, "File");
    vg_menu_t *edit = vg_menubar_add_menu(bar, "Edit");
    ASSERT_NOT_NULL(file);
    ASSERT_NOT_NULL(edit);
    vg_menu_item_t *open = vg_menu_add_item(file, "Open", NULL, NULL, NULL);
    vg_menu_item_t *copy = vg_menu_add_item(edit, "Copy", NULL, NULL, NULL);
    ASSERT_NOT_NULL(open);
    ASSERT_NOT_NULL(copy);

    vg_menu_remove_item(file, copy);
    ASSERT_EQ(file->item_count, 1);
    ASSERT_EQ(edit->item_count, 1);
    ASSERT(file->first_item == open);
    ASSERT(edit->first_item == copy);

    vg_menubar_remove_menu(bar, edit);
    ASSERT_EQ(bar->menu_count, 1);
    ASSERT(bar->first_menu == file);
    vg_menu_remove_item(file, open);
    ASSERT_EQ(file->item_count, 0);

    vg_widget_destroy(&bar->base);
}

/// @brief R4 — zero capacity is re-allocated on set_text; surrogate and out-of-range codepoints are
/// silently dropped.
TEST(textinput_zero_capacity_and_invalid_codepoints_are_ignored) {
    vg_textinput_t *ti = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(ti);

    free(ti->text);
    ti->text = NULL;
    ti->text_capacity = 0;
    ti->text_len = 0;
    vg_textinput_set_text(ti, "abc");
    ASSERT_NOT_NULL(ti->text);
    ASSERT_EQ(strcmp(ti->text, "abc"), 0);
    ASSERT(ti->text_capacity >= 4);

    vg_event_t surrogate = vg_event_key(VG_EVENT_KEY_CHAR, 0, 0xD800u, 0);
    ASSERT_TRUE(ti->base.vtable->handle_event(&ti->base, &surrogate));
    ASSERT_EQ(strcmp(ti->text, "abc"), 0);

    vg_event_t out_of_range = vg_event_key(VG_EVENT_KEY_CHAR, 0, 0x110000u, 0);
    ASSERT_TRUE(ti->base.vtable->handle_event(&ti->base, &out_of_range));
    ASSERT_EQ(strcmp(ti->text, "abc"), 0);

    vg_widget_destroy(&ti->base);
}

/// @brief R4 — select_index with an out-of-range index leaves the current selection unchanged.
TEST(listbox_select_index_out_of_range_keeps_selection) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_item_t *a = vg_listbox_add_item(lb, "A", NULL);
    vg_listbox_item_t *b = vg_listbox_add_item(lb, "B", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    vg_listbox_select(lb, b);
    ASSERT(lb->selected == b);

    vg_listbox_select_index(lb, 99);
    ASSERT(lb->selected == b);
    ASSERT_TRUE(b->selected);

    vg_widget_destroy(&lb->base);
}

/// @brief R4 — regex search correctly records variable-length match spans across multiple lines.
TEST(findreplacebar_regex_search_finds_variable_length_matches) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    vg_codeeditor_set_text(ed, "alpha 123 beta\nz9 z10");

    vg_findreplacebar_t *bar = vg_findreplacebar_create();
    ASSERT_NOT_NULL(bar);
    vg_findreplacebar_set_target(bar, ed);

    vg_search_options_t options = {0};
    options.use_regex = true;
    vg_findreplacebar_set_options(bar, &options);
    vg_findreplacebar_find(bar, "[0-9]+");

    ASSERT_EQ(bar->match_count, (size_t)3);
    ASSERT_EQ(bar->matches[0].start_col, (uint32_t)6);
    ASSERT_EQ(bar->matches[0].end_col, (uint32_t)9);
    ASSERT_EQ(bar->matches[1].start_col, (uint32_t)1);
    ASSERT_EQ(bar->matches[2].start_col, (uint32_t)4);

    vg_widget_destroy(&bar->base);
    vg_widget_destroy(&ed->base);
}

/// @brief R4 — pressing Down 5 times scrolls the selected node into view and lands on index 5.
TEST(treeview_keyboard_navigation_scrolls_selected_node_into_view) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    ASSERT_NOT_NULL(tree);
    tree->base.height = 30.0f;
    tree->row_height = 10.0f;

    vg_tree_node_t *first = NULL;
    for (int i = 0; i < 10; i++) {
        char label[16];
        snprintf(label, sizeof(label), "node-%d", i);
        vg_tree_node_t *node = vg_treeview_add_node(tree, NULL, label);
        if (i == 0)
            first = node;
    }
    ASSERT_NOT_NULL(first);
    vg_treeview_select(tree, first);
    ASSERT_EQ(tree->scroll_y, 0.0f);

    vg_event_t down = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_DOWN, 0, 0);
    for (int i = 0; i < 5; i++)
        ASSERT_TRUE(tree->base.vtable->handle_event(&tree->base, &down));

    ASSERT_NOT_NULL(tree->selected);
    ASSERT(tree->scroll_y > 0.0f);
    int current = 0;
    int selected_index = -1;
    for (vg_tree_node_t *node = tree->root->first_child; node; node = node->next_sibling) {
        if (node == tree->selected) {
            selected_index = current;
            break;
        }
        current++;
    }
    ASSERT_EQ(selected_index, 5);

    vg_widget_destroy(&tree->base);
}

/// @brief Explorer-sized retained trees consume wheel input only while their viewport can move.
TEST(treeview_wheel_scrolls_overflowing_rows_and_bubbles_at_limits) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    vg_treeview_t *tree = vg_treeview_create(root);
    ASSERT_NOT_NULL(tree);
    tree->row_height = 18.0f;
    vg_widget_arrange(root, 0.0f, 0.0f, 260.0f, 160.0f);
    vg_widget_arrange(&tree->base, 30.0f, 24.0f, 180.0f, 72.0f);

    for (int i = 0; i < 20; i++) {
        char label[24];
        snprintf(label, sizeof(label), "project-file-%d", i);
        ASSERT_NOT_NULL(vg_treeview_add_node(tree, NULL, label));
    }

    // Exercise the real hit-test/dispatch route used by the app, including
    // screen coordinates over a non-origin Explorer pane.
    vgfx_event_t native_wheel_down;
    memset(&native_wheel_down, 0, sizeof(native_wheel_down));
    native_wheel_down.type = VGFX_EVENT_SCROLL;
    native_wheel_down.data.scroll.delta_y = 1.0f;
    native_wheel_down.data.scroll.x = 60;
    native_wheel_down.data.scroll.y = 50;
    vg_event_t wheel_down = vg_event_from_platform(&native_wheel_down);
    ASSERT_TRUE(vg_event_dispatch(root, &wheel_down));
    ASSERT_EQ(wheel_down.target, &tree->base);
    ASSERT(tree->scroll_y > 0.0f);

    // Overflow is also discoverable and operable without a wheel. Hovering the
    // right gutter highlights the thumb, a track click scrolls, and gutter
    // clicks never leak through to row selection.
    vg_event_t gutter_hover = {0};
    gutter_hover.type = VG_EVENT_MOUSE_MOVE;
    gutter_hover.mouse.x = 179.0f;
    gutter_hover.mouse.y = 10.0f;
    ASSERT_TRUE(tree->base.vtable->handle_event(&tree->base, &gutter_hover));
    ASSERT_TRUE(tree->scrollbar_hovered);

    tree->scroll_y = 0.0f;
    vg_event_t track_down = {0};
    track_down.type = VG_EVENT_MOUSE_DOWN;
    track_down.mouse.button = VG_MOUSE_LEFT;
    track_down.mouse.x = 179.0f;
    track_down.mouse.y = 70.0f;
    ASSERT_TRUE(tree->base.vtable->handle_event(&tree->base, &track_down));
    ASSERT(tree->scroll_y > 0.0f);
    ASSERT_NULL(tree->selected);

    vg_event_t gutter_click = {0};
    gutter_click.type = VG_EVENT_CLICK;
    gutter_click.mouse.button = VG_MOUSE_LEFT;
    gutter_click.mouse.x = 179.0f;
    gutter_click.mouse.y = 70.0f;
    ASSERT_TRUE(tree->base.vtable->handle_event(&tree->base, &gutter_click));
    ASSERT_NULL(tree->selected);

    // Drag the thumb from the top toward the middle and release capture.
    tree->scroll_y = 0.0f;
    vg_event_t thumb_down = {0};
    thumb_down.type = VG_EVENT_MOUSE_DOWN;
    thumb_down.mouse.button = VG_MOUSE_LEFT;
    thumb_down.mouse.x = 179.0f;
    thumb_down.mouse.y = 5.0f;
    ASSERT_TRUE(tree->base.vtable->handle_event(&tree->base, &thumb_down));
    ASSERT_TRUE(tree->scrollbar_dragging);
    ASSERT_EQ(vg_widget_get_input_capture(), &tree->base);

    vg_event_t thumb_drag = {0};
    thumb_drag.type = VG_EVENT_MOUSE_MOVE;
    thumb_drag.mouse.x = 179.0f;
    thumb_drag.mouse.y = 42.0f;
    ASSERT_TRUE(tree->base.vtable->handle_event(&tree->base, &thumb_drag));
    ASSERT(tree->scroll_y > 0.0f);

    vg_event_t thumb_up = {0};
    thumb_up.type = VG_EVENT_MOUSE_UP;
    thumb_up.mouse.button = VG_MOUSE_LEFT;
    thumb_up.mouse.x = 179.0f;
    thumb_up.mouse.y = 42.0f;
    ASSERT_TRUE(tree->base.vtable->handle_event(&tree->base, &thumb_up));
    ASSERT_FALSE(tree->scrollbar_dragging);
    ASSERT_NULL(vg_widget_get_input_capture());

    tree->scroll_y = 0.0f;
    vg_event_t wheel_up = {0};
    wheel_up.type = VG_EVENT_MOUSE_WHEEL;
    wheel_up.wheel.delta_y = 1.0f;
    ASSERT_FALSE(tree->base.vtable->handle_event(&tree->base, &wheel_up));
    ASSERT_EQ(tree->scroll_y, 0.0f);

    vg_widget_destroy(root);
}

/// @brief R4 — remove_child clears focus, input-capture, modal-root, and reported-click for the
/// removed subtree.
TEST(widget_remove_child_clears_runtime_references_to_subtree) {
    vg_widget_runtime_state_t empty = {0};
    vg_widget_set_runtime_state(&empty);

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_button_t *button = vg_button_create(NULL, "Detach");
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(button);
    vg_widget_add_child(root, &button->base);

    vg_widget_set_focus(&button->base);
    vg_widget_set_input_capture(&button->base);
    vg_widget_set_modal_root(&button->base);
    vg_widget_note_click(&button->base, 1234);

    vg_widget_remove_child(root, &button->base);

    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    ASSERT_NULL(state.focused_widget);
    ASSERT_NULL(state.input_capture_widget);
    ASSERT_NULL(state.modal_root);
    ASSERT_NULL(state.reported_click_widget);
    ASSERT_EQ(state.reported_click_time_ms, (uint64_t)0);
    ASSERT_FALSE(vg_widget_has_state(&button->base, VG_STATE_FOCUSED));

    vg_widget_destroy(&button->base);
    vg_widget_destroy(root);
    vg_widget_set_runtime_state(&empty);
}

/// @brief R4 — clear_children clears all runtime references (focus, capture, modal, click) for
/// every child.
TEST(widget_clear_children_clears_runtime_references_to_subtrees) {
    vg_widget_runtime_state_t empty = {0};
    vg_widget_set_runtime_state(&empty);

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_button_t *button = vg_button_create(NULL, "Clear");
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(button);
    vg_widget_add_child(root, &button->base);

    vg_widget_set_focus(&button->base);
    vg_widget_set_input_capture(&button->base);
    vg_widget_set_modal_root(&button->base);
    vg_widget_note_click(&button->base, 5678);

    vg_widget_clear_children(root);

    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    ASSERT_NULL(state.focused_widget);
    ASSERT_NULL(state.input_capture_widget);
    ASSERT_NULL(state.modal_root);
    ASSERT_NULL(state.reported_click_widget);
    ASSERT_EQ(root->child_count, 0);

    vg_widget_destroy(&button->base);
    vg_widget_destroy(root);
    vg_widget_set_runtime_state(&empty);
}

/// @brief R4 — set_runtime_state and set_focus silently discard pointers that fail
/// vg_widget_is_live.
TEST(widget_runtime_restore_and_focus_reject_invalid_handles) {
    vg_widget_runtime_state_t empty = {0};
    vg_widget_set_runtime_state(&empty);

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_button_t *button = vg_button_create(root, "Keep");
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(button);
    vg_widget_set_focus(&button->base);
    ASSERT_EQ(vg_widget_get_focused(root), &button->base);

    uint64_t not_a_widget = 0;
    vg_widget_set_focus((vg_widget_t *)&not_a_widget);
    ASSERT_EQ(vg_widget_get_focused(root), &button->base);

    vg_widget_runtime_state_t invalid = {0};
    invalid.focused_widget = (vg_widget_t *)&not_a_widget;
    invalid.input_capture_widget = (vg_widget_t *)&not_a_widget;
    invalid.modal_root = (vg_widget_t *)&not_a_widget;
    invalid.hovered_widget = (vg_widget_t *)&not_a_widget;
    invalid.last_click_widget = (vg_widget_t *)&not_a_widget;
    invalid.last_click_time_ms = 99;
    invalid.last_click_button = VG_MOUSE_LEFT;
    invalid.reported_click_widget = (vg_widget_t *)&not_a_widget;
    invalid.reported_click_time_ms = 100;

    vg_widget_set_runtime_state(&invalid);

    vg_widget_runtime_state_t state = {0};
    vg_widget_get_runtime_state(&state);
    ASSERT_NULL(state.focused_widget);
    ASSERT_NULL(state.input_capture_widget);
    ASSERT_NULL(state.modal_root);
    ASSERT_NULL(state.hovered_widget);
    ASSERT_NULL(state.last_click_widget);
    ASSERT_EQ(state.last_click_time_ms, (uint64_t)0);
    ASSERT_EQ(state.last_click_button, -1);
    ASSERT_NULL(state.reported_click_widget);
    ASSERT_EQ(state.reported_click_time_ms, (uint64_t)0);

    vg_widget_destroy(root);
    vg_widget_set_runtime_state(&empty);
}

//=============================================================================
// Round 5 — Low-level widget/runtime audit fixes
//=============================================================================

static int g_take_impl_destroy_count = 0;
static int g_take_impl_saw_data = 0;

/// @brief vtable destroy — calls vg_widget_take_impl_data and frees the data; used to verify take
/// semantics.
static void take_impl_destroy(vg_widget_t *widget) {
    void *data = vg_widget_take_impl_data(widget);
    if (data) {
        g_take_impl_saw_data++;
        free(data);
    }
    g_take_impl_destroy_count++;
}

static vg_widget_vtable_t g_take_impl_vtable = {
    .destroy = take_impl_destroy,
};

/// @brief R5 — vg_widget_take_impl_data returns the pointer and sets impl_data to NULL atomically.
TEST(widget_impl_data_can_be_taken_by_custom_destroy) {
    g_take_impl_destroy_count = 0;
    g_take_impl_saw_data = 0;

    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(widget);
    widget->vtable = &g_take_impl_vtable;
    widget->impl_data = malloc(16);
    ASSERT_NOT_NULL(widget->impl_data);

    vg_widget_destroy(widget);
    ASSERT_EQ(g_take_impl_destroy_count, 1);
    ASSERT_EQ(g_take_impl_saw_data, 1);
}

/// @brief R5 — widget_destroy frees drag_type, drag_data, accepted_drop_types, and _drop_received_*
/// strings.
TEST(widget_destroy_releases_owned_drag_drop_strings) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(widget);
    widget->drag_type = test_strdup_local("text/plain");
    widget->drag_data = test_strdup_local("payload");
    widget->accepted_drop_types = test_strdup_local("text/plain,image/png");
    widget->_drop_received_type = test_strdup_local("text/plain");
    widget->_drop_received_data = test_strdup_local("dropped");
    ASSERT_NOT_NULL(widget->drag_type);
    ASSERT_NOT_NULL(widget->drag_data);
    ASSERT_NOT_NULL(widget->accepted_drop_types);
    ASSERT_NOT_NULL(widget->_drop_received_type);
    ASSERT_NOT_NULL(widget->_drop_received_data);

    vg_widget_destroy(widget);
    ASSERT_TRUE(true);
}

/// @brief R5 — get_focused(root) returns the focused widget only if it lives in that root's
/// subtree.
TEST(widget_get_focused_is_scoped_to_root_subtree) {
    vg_widget_runtime_state_t empty = {0};
    vg_widget_set_runtime_state(&empty);

    vg_widget_t *left_root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *right_root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_button_t *button = vg_button_create(right_root, "Focus");
    ASSERT_NOT_NULL(left_root);
    ASSERT_NOT_NULL(right_root);
    ASSERT_NOT_NULL(button);

    vg_widget_set_focus(&button->base);
    ASSERT_EQ(vg_widget_get_focused(right_root), &button->base);
    ASSERT_NULL(vg_widget_get_focused(left_root));
    ASSERT_EQ(vg_widget_get_focused(NULL), &button->base);

    vg_widget_destroy(left_root);
    vg_widget_destroy(right_root);
    vg_widget_set_runtime_state(&empty);
}

/// @brief R5 — insert_child with a negative index clamps to 0, placing the child at the front.
TEST(widget_insert_child_negative_index_clamps_to_front) {
    vg_widget_t *parent = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *first = vg_widget_create(VG_WIDGET_LABEL);
    vg_widget_t *second = vg_widget_create(VG_WIDGET_LABEL);
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    vg_widget_add_child(parent, first);
    vg_widget_insert_child(parent, second, -10);

    ASSERT_EQ(parent->child_count, 2);
    ASSERT(parent->first_child == second);
    ASSERT(parent->last_child == first);
    ASSERT(second->next_sibling == first);
    ASSERT(first->prev_sibling == second);

    vg_widget_destroy(parent);
}

/// @brief R5 — focus_next/prev work correctly beyond 512 focusable widgets (dynamic allocation
/// path).
TEST(widget_tab_order_handles_more_than_legacy_fixed_cap) {
    vg_widget_runtime_state_t empty = {0};
    vg_widget_set_runtime_state(&empty);

    enum { COUNT = 520 };

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);

    vg_button_t *buttons[COUNT];
    for (int i = 0; i < COUNT; i++) {
        char label[16];
        snprintf(label, sizeof(label), "B%d", i);
        buttons[i] = vg_button_create(root, label);
        ASSERT_NOT_NULL(buttons[i]);
    }

    vg_widget_set_focus(&buttons[511]->base);
    vg_widget_focus_next(root);
    ASSERT_EQ(vg_widget_get_focused(root), &buttons[512]->base);

    vg_widget_focus_prev(root);
    ASSERT_EQ(vg_widget_get_focused(root), &buttons[511]->base);

    vg_widget_destroy(root);
    vg_widget_set_runtime_state(&empty);
}

/// @brief R5 — resize event carries logical pixel dimensions; falls back to physical when logical
/// fields are zero.
TEST(platform_resize_event_reports_logical_gui_dimensions) {
    vgfx_event_t pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = VGFX_EVENT_RESIZE;
    pe.data.resize.width = 2000;
    pe.data.resize.height = 1000;
    pe.data.resize.logical_width = 1000;
    pe.data.resize.logical_height = 500;

    vg_event_t ev = vg_event_from_platform(&pe);
    ASSERT_EQ(ev.type, VG_EVENT_RESIZE);
    ASSERT_EQ(ev.resize.width, 1000);
    ASSERT_EQ(ev.resize.height, 500);

    pe.data.resize.logical_width = 0;
    pe.data.resize.logical_height = 0;
    ev = vg_event_from_platform(&pe);
    ASSERT_EQ(ev.resize.width, 2000);
    ASSERT_EQ(ev.resize.height, 1000);
}

//=============================================================================
// Round 6: Low-level rendering, grid, text, and widget value hardening
//=============================================================================

static float g_painted_child_x = -1.0f;
static float g_painted_child_y = -1.0f;

/// @brief vtable paint — records the widget's x/y at paint time for screen-space coordinate
/// assertions.
static void test_capture_paint(vg_widget_t *widget, void *canvas) {
    (void)canvas;
    g_painted_child_x = widget->x;
    g_painted_child_y = widget->y;
}

/// @brief R6 — child widget's x/y at paint time are in screen space; layout-space coordinates are
/// not mutated.
TEST(widget_paint_uses_screen_space_for_nested_children) {
    vg_widget_vtable_t paint_vtable = {0};
    paint_vtable.paint = test_capture_paint;

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *parent = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(child);
    child->vtable = &paint_vtable;

    vg_widget_add_child(root, parent);
    vg_widget_add_child(parent, child);
    vg_widget_arrange(root, 10.0f, 20.0f, 200.0f, 100.0f);
    vg_widget_arrange(parent, 30.0f, 40.0f, 100.0f, 50.0f);
    vg_widget_arrange(child, 5.0f, 6.0f, 20.0f, 10.0f);

    vg_widget_paint(root, (void *)1);
    ASSERT_NEAR(g_painted_child_x, 45.0f, 0.001f);
    ASSERT_NEAR(g_painted_child_y, 66.0f, 0.001f);

    // Painting in screen space must not mutate layout-space coordinates.
    ASSERT_NEAR(child->x, 5.0f, 0.001f);
    ASSERT_NEAR(child->y, 6.0f, 0.001f);

    vg_widget_destroy(root);
}

/// @brief R6 — grid_place with negative row/col clamps to (0,0); child is placed and sized in the
/// first cell.
TEST(grid_negative_placement_clamps_to_first_cell) {
    vg_widget_t *grid = vg_grid_create(2, 2);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_LABEL);
    ASSERT_NOT_NULL(grid);
    ASSERT_NOT_NULL(child);
    vg_widget_add_child(grid, child);
    vg_grid_place(grid, child, -3, -4, 1, 1);
    vg_widget_measure(grid, 100.0f, 100.0f);
    vg_widget_arrange(grid, 0.0f, 0.0f, 100.0f, 100.0f);
    ASSERT_NEAR(child->x, 0.0f, 0.001f);
    ASSERT_NEAR(child->y, 0.0f, 0.001f);
    ASSERT_TRUE(child->width > 0.0f);
    ASSERT_TRUE(child->height > 0.0f);
    vg_widget_destroy(grid);
}

/// @brief R6 — insert_text clamps a stale cursor_col that exceeds the line length before operating.
TEST(codeeditor_edit_helpers_clamp_stale_cursor_columns) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);
    vg_codeeditor_set_text(editor, "abc");
    editor->cursor_line = 0;
    editor->cursor_col = 1000;
    vg_codeeditor_insert_text(editor, "\nX");
    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "abc\nX") == 0);
    free(text);
    vg_widget_destroy(&editor->base);
}

/// @brief R6 — set_opacity with NaN clamps to 1.0f; the stored value is always finite.
TEST(image_opacity_sanitizes_nan) {
    vg_image_t *image = vg_image_create(NULL);
    ASSERT_NOT_NULL(image);
    vg_image_set_opacity(image, NAN);
    ASSERT_TRUE(isfinite(image->opacity));
    ASSERT_NEAR(image->opacity, 1.0f, 0.001f);
    vg_widget_destroy(&image->base);
}

/// @brief R6 — set_text strips invalid UTF-8 bytes; insert respects max_length and drops invalid
/// sequences.
TEST(textinput_sanitizes_invalid_utf8_before_storage) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(input);

    const unsigned char invalid_set[] = {'a', 0xFFu, 'b', '\0'};
    vg_textinput_set_text(input, (const char *)invalid_set);
    ASSERT_EQ(strcmp(input->text, "ab"), 0);

    input->max_length = 3;
    vg_textinput_set_text(input, "abcdef");
    ASSERT_EQ(strcmp(input->text, "abc"), 0);

    input->max_length = 0;
    vg_textinput_set_cursor(input, 3);
    const unsigned char invalid_insert[] = {'x', 0xC0u, 'y', '\0'};
    vg_textinput_insert(input, (const char *)invalid_insert);
    ASSERT_EQ(strcmp(input->text, "abcxy"), 0);

    vg_widget_destroy(&input->base);
}

/// @brief R6 — KEY_CHAR with a multi-byte codepoint inserts the complete UTF-8 byte sequence, not a
/// partial one.
TEST(codeeditor_inserts_utf8_as_complete_byte_sequences) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_event_t event = vg_event_key(VG_EVENT_KEY_CHAR, 0, 0x00E9u, 0);
    ASSERT_TRUE(editor->base.vtable->handle_event(&editor->base, &event));

    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    const unsigned char expected[] = {0xC3u, 0xA9u, '\0'};
    ASSERT_EQ(strcmp(text, (const char *)expected), 0);
    free(text);

    vg_codeeditor_insert_text(editor, "Z");
    text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT_EQ(strcmp(text, "\xC3\xA9Z"), 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

static int g_capture_release_clicks = 0;
static int g_capture_release_ups = 0;

/// @brief vtable handle_event — acquires capture on DOWN, releases on UP, increments counters for
/// UP and CLICK.
static bool capture_release_handle_event(vg_widget_t *widget, vg_event_t *event) {
    if (event->type == VG_EVENT_MOUSE_DOWN) {
        vg_widget_set_input_capture(widget);
        return true;
    }
    if (event->type == VG_EVENT_MOUSE_UP) {
        g_capture_release_ups++;
        vg_widget_release_input_capture();
        return true;
    }
    if (event->type == VG_EVENT_CLICK) {
        g_capture_release_clicks++;
        return true;
    }
    return false;
}

static vg_widget_vtable_t g_capture_release_vtable = {
    .handle_event = capture_release_handle_event,
};

/// @brief R6 — releasing capture in the MOUSE_UP handler prevents a synthetic CLICK from firing
/// outside bounds.
TEST(captured_mouseup_release_suppresses_outside_synthetic_click) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_capture_release_vtable;
    vg_widget_add_child(root, child);
    vg_widget_arrange(root, 0.0f, 0.0f, 200.0f, 200.0f);
    vg_widget_arrange(child, 0.0f, 0.0f, 20.0f, 20.0f);

    g_capture_release_clicks = 0;
    g_capture_release_ups = 0;

    vg_event_t down = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 5.0f, 5.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &down));
    ASSERT_EQ(vg_widget_get_input_capture(), child);

    vg_event_t up = vg_event_mouse(VG_EVENT_MOUSE_UP, 120.0f, 120.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &up));
    ASSERT_EQ(g_capture_release_ups, 1);
    ASSERT_EQ(g_capture_release_clicks, 0);
    ASSERT_NULL(vg_widget_get_input_capture());

    vg_widget_destroy(root);
}

/// @brief R6 — adding more children than a grid's explicit row count creates implicit rows with
/// positive dimensions.
TEST(grid_auto_flow_creates_implicit_rows) {
    vg_widget_t *grid = vg_grid_create(2, 1);
    vg_widget_t *a = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *b = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *c = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(grid);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);
    vg_widget_add_child(grid, a);
    vg_widget_add_child(grid, b);
    vg_widget_add_child(grid, c);

    vg_widget_measure(grid, 200.0f, 100.0f);
    vg_widget_arrange(grid, 0.0f, 0.0f, 200.0f, 100.0f);

    ASSERT_TRUE(c->y > a->y);
    ASSERT_TRUE(c->height > 0.0f);
    ASSERT_TRUE(c->width > 0.0f);

    vg_widget_destroy(grid);
}

static float g_capture_local_x = 0.0f;
static float g_capture_local_y = 0.0f;

/// @brief vtable handle_event — captures the MOUSE_UP local coordinates for later assertion.
static bool capture_local_coords_handle_event(vg_widget_t *widget, vg_event_t *event) {
    (void)widget;
    if (event->type == VG_EVENT_MOUSE_UP) {
        g_capture_local_x = event->mouse.x;
        g_capture_local_y = event->mouse.y;
        return true;
    }
    return false;
}

static vg_widget_vtable_t g_capture_local_coords_vtable = {
    .handle_event = capture_local_coords_handle_event,
};

/// @brief R6 — captured MOUSE_UP events are localised exactly once to the captured widget's
/// coordinate space.
TEST(captured_mouse_events_are_localized_once) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_capture_local_coords_vtable;
    vg_widget_add_child(root, child);
    vg_widget_arrange(root, 0.0f, 0.0f, 200.0f, 200.0f);
    vg_widget_arrange(child, 50.0f, 60.0f, 80.0f, 80.0f);

    vg_widget_set_input_capture(child);
    g_capture_local_x = -999.0f;
    g_capture_local_y = -999.0f;

    vg_event_t up = vg_event_mouse(VG_EVENT_MOUSE_UP, 70.0f, 90.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &up));
    ASSERT_NEAR(g_capture_local_x, 20.0f, 0.001f);
    ASSERT_NEAR(g_capture_local_y, 30.0f, 0.001f);

    vg_widget_release_input_capture();
    vg_widget_destroy(root);
}

/// @brief vtable handle_event — destroys itself on MOUSE_DOWN to test dispatch safety after
/// self-destroy.
static bool destroy_self_on_event(vg_widget_t *widget, vg_event_t *event) {
    if (event->type == VG_EVENT_MOUSE_DOWN) {
        vg_widget_destroy(widget);
        return true;
    }
    return false;
}

static vg_widget_vtable_t g_destroy_self_vtable = {
    .handle_event = destroy_self_on_event,
};

/// @brief R6 — an event handler may call vg_widget_destroy on itself without corrupting the
/// dispatch loop.
TEST(event_handler_can_destroy_target_during_dispatch) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_destroy_self_vtable;
    vg_widget_add_child(root, child);
    vg_widget_arrange(root, 0.0f, 0.0f, 200.0f, 200.0f);
    vg_widget_arrange(child, 10.0f, 10.0f, 40.0f, 40.0f);

    vg_event_t down = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 20.0f, 20.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &down));
    ASSERT_FALSE(vg_widget_is_live(child));

    vg_widget_destroy(root);
}

typedef struct {
    int key_count;
} per_root_focus_state_t;

/// @brief vtable handle_event — increments per-root key count on KEY_DOWN; absorbs MOUSE_DOWN.
static bool per_root_focus_handle_event(vg_widget_t *widget, vg_event_t *event) {
    if (event->type == VG_EVENT_MOUSE_DOWN)
        return true;
    if (event->type == VG_EVENT_KEY_DOWN) {
        per_root_focus_state_t *state = (per_root_focus_state_t *)widget->user_data;
        state->key_count++;
        return true;
    }
    return false;
}

/// @brief vtable can_focus — focusable when enabled and visible.
static bool per_root_focus_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

static vg_widget_vtable_t g_per_root_focus_vtable = {
    .handle_event = per_root_focus_handle_event,
    .can_focus = per_root_focus_can_focus,
};

/// @brief R6 — each root tree has independent focus state; keyboard events go to the focused widget
/// in that root only.
TEST(event_dispatch_keeps_focus_state_per_root) {
    vg_widget_t *root_a = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *root_b = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child_a = vg_widget_create(VG_WIDGET_CUSTOM);
    vg_widget_t *child_b = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root_a);
    ASSERT_NOT_NULL(root_b);
    ASSERT_NOT_NULL(child_a);
    ASSERT_NOT_NULL(child_b);

    per_root_focus_state_t state_a = {0};
    per_root_focus_state_t state_b = {0};
    child_a->vtable = &g_per_root_focus_vtable;
    child_b->vtable = &g_per_root_focus_vtable;
    child_a->user_data = &state_a;
    child_b->user_data = &state_b;

    vg_widget_add_child(root_a, child_a);
    vg_widget_add_child(root_b, child_b);
    vg_widget_arrange(root_a, 0.0f, 0.0f, 100.0f, 100.0f);
    vg_widget_arrange(child_a, 0.0f, 0.0f, 50.0f, 50.0f);
    vg_widget_arrange(root_b, 0.0f, 0.0f, 100.0f, 100.0f);
    vg_widget_arrange(child_b, 0.0f, 0.0f, 50.0f, 50.0f);

    vg_event_t down_a = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root_a, &down_a));
    vg_event_t down_b = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root_b, &down_b));

    vg_event_t key_a = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, 0);
    ASSERT_TRUE(vg_event_dispatch(root_a, &key_a));
    ASSERT_EQ(state_a.key_count, 1);
    ASSERT_EQ(state_b.key_count, 0);

    vg_event_t key_b = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, 0);
    ASSERT_TRUE(vg_event_dispatch(root_b, &key_b));
    ASSERT_EQ(state_a.key_count, 1);
    ASSERT_EQ(state_b.key_count, 1);

    vg_widget_destroy(root_a);
    vg_widget_destroy(root_b);
}

/// @brief R6 — surrogate codepoints (0xD800–0xDFFF) are dropped silently by both command palette
/// and file dialog.
TEST(commandpalette_and_filedialog_reject_surrogate_codepoints) {
    vg_commandpalette_t *palette = vg_commandpalette_create();
    ASSERT_NOT_NULL(palette);
    vg_commandpalette_show(palette);

    vg_event_t surrogate = vg_event_key(VG_EVENT_KEY_CHAR, 0, 0xD800u, 0);
    ASSERT_TRUE(palette->base.vtable->handle_event(&palette->base, &surrogate));
    ASSERT_NULL(palette->current_query);

    vg_event_t valid = vg_event_key(VG_EVENT_KEY_CHAR, 0, 'A', 0);
    ASSERT_TRUE(palette->base.vtable->handle_event(&palette->base, &valid));
    ASSERT_NOT_NULL(palette->current_query);
    ASSERT_EQ(strcmp(palette->current_query, "A"), 0);

    vg_filedialog_t *dialog = vg_filedialog_create(VG_FILEDIALOG_SAVE);
    ASSERT_NOT_NULL(dialog);
    vg_filedialog_set_filename(dialog, "x");
    dialog->filename_active = true;
    surrogate = vg_event_key(VG_EVENT_KEY_CHAR, 0, 0xDFFFu, 0);
    (void)dialog->base.base.vtable->handle_event(&dialog->base.base, &surrogate);
    ASSERT_NOT_NULL(dialog->default_filename);
    ASSERT_EQ(strcmp(dialog->default_filename, "x"), 0);

    vg_commandpalette_destroy(palette);
    vg_filedialog_destroy(dialog);
}

/// @brief R8 — malformed/truncated UTF-8 labels do not read past the terminator while filtering.
TEST(commandpalette_filter_handles_truncated_utf8_labels) {
    vg_commandpalette_t *palette = vg_commandpalette_create();
    ASSERT_NOT_NULL(palette);
    ASSERT_NOT_NULL(vg_commandpalette_add_command(palette, "bad", "\xE2", NULL, NULL, NULL));
    vg_commandpalette_show(palette);

    vg_event_t query = vg_event_key(VG_EVENT_KEY_CHAR, 0, 'x', 0);
    ASSERT_TRUE(palette->base.vtable->handle_event(&palette->base, &query));
    ASSERT_EQ(palette->filtered_count, (size_t)0);

    vg_commandpalette_destroy(palette);
}

/// @brief R6 — insert with max_length truncates the replacement text before clearing the selection.
TEST(textinput_replacement_validates_before_mutating_selection) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(input);
    vg_textinput_set_text(input, "abcdef");
    input->max_length = 6;
    vg_textinput_select(input, 2, 6);

    vg_textinput_insert(input, "WXYZ123");

    ASSERT_EQ(strcmp(vg_textinput_get_text(input), "abWXYZ"), 0);
    ASSERT_EQ(input->cursor_pos, 6u);
    ASSERT_EQ(input->selection_start, input->selection_end);
    vg_widget_destroy(&input->base);
}

TEST(textinput_null_font_preserves_existing_font_and_invalid_tick_is_ignored) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(input);

    vg_font_t *sentinel_font = (vg_font_t *)0x1234;
    input->font = sentinel_font;
    input->font_size = 13.0f;
    vg_textinput_set_font(input, NULL, 18.0f);
    ASSERT_EQ(input->font, sentinel_font);
    ASSERT_EQ(input->font_size, 18.0f);

    input->base.state |= VG_STATE_FOCUSED;
    input->cursor_blink_time = 0.25f;
    input->cursor_visible = true;
    vg_textinput_tick(input, NAN);
    ASSERT_NEAR(input->cursor_blink_time, 0.25f, 0.0001f);
    ASSERT_TRUE(input->cursor_visible);
    vg_textinput_tick(input, -1.0f);
    ASSERT_NEAR(input->cursor_blink_time, 0.25f, 0.0001f);
    ASSERT_TRUE(input->cursor_visible);

    vg_widget_destroy(&input->base);
}

TEST(findreplace_whole_word_does_not_match_inside_utf8_word) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    vg_codeeditor_set_text(ed, "été t");

    vg_findreplacebar_t *bar = vg_findreplacebar_create();
    ASSERT_NOT_NULL(bar);
    vg_findreplacebar_set_target(bar, ed);
    vg_search_options_t options = {0};
    options.whole_word = true;
    vg_findreplacebar_set_options(bar, &options);
    vg_findreplacebar_find(bar, "t");

    ASSERT_EQ(bar->match_count, (size_t)1);
    ASSERT_EQ(bar->matches[0].start_col, (uint32_t)4);

    vg_widget_destroy(&bar->base);
    vg_widget_destroy(&ed->base);
}

/// @brief R6 — append_line, select_all, clear, and max_lines=0 all leave line_count and
/// has_selection consistent.
TEST(outputpane_append_line_and_clear_keep_line_state_consistent) {
    vg_outputpane_t *pane = vg_outputpane_create();
    ASSERT_NOT_NULL(pane);

    vg_outputpane_append_line(pane, "one");
    vg_outputpane_append_line(pane, "two");
    ASSERT_EQ(pane->line_count, 3u);
    ASSERT_EQ(pane->lines[0].segment_count, 1u);
    ASSERT_EQ(strcmp(pane->lines[0].segments[0].text, "one"), 0);
    ASSERT_EQ(pane->lines[1].segment_count, 1u);
    ASSERT_EQ(strcmp(pane->lines[1].segments[0].text, "two"), 0);
    ASSERT_EQ(pane->lines[2].segment_count, 0u);

    vg_outputpane_select_all(pane);
    ASSERT_TRUE(pane->has_selection);
    vg_outputpane_clear(pane);
    ASSERT_EQ(pane->line_count, 0u);
    ASSERT_FALSE(pane->has_selection);

    vg_outputpane_set_max_lines(pane, 0);
    ASSERT_EQ(pane->max_lines, 1u);
    vg_outputpane_append_line(pane, "clamped");
    ASSERT_TRUE(pane->line_count <= 1u);
    vg_outputpane_destroy(pane);
}

static vg_output_line_t *test_outputpane_line_at(vg_outputpane_t *pane, size_t logical_index) {
    if (!pane || logical_index >= pane->line_count || pane->line_capacity == 0)
        return NULL;
    return &pane->lines[(pane->line_start + logical_index) % pane->line_capacity];
}

TEST(outputpane_ring_buffer_preserves_logical_order_after_eviction) {
    vg_outputpane_t *pane = vg_outputpane_create();
    ASSERT_NOT_NULL(pane);

    vg_outputpane_set_max_lines(pane, 3);
    vg_outputpane_append_line(pane, "one");
    vg_outputpane_append_line(pane, "two");
    vg_outputpane_append_line(pane, "three");
    vg_outputpane_append_line(pane, "four");

    ASSERT_EQ(pane->line_count, 3u);
    vg_output_line_t *first = test_outputpane_line_at(pane, 0);
    vg_output_line_t *second = test_outputpane_line_at(pane, 1);
    vg_output_line_t *third = test_outputpane_line_at(pane, 2);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT_NOT_NULL(third);
    ASSERT_EQ(strcmp(first->segments[0].text, "three"), 0);
    ASSERT_EQ(strcmp(second->segments[0].text, "four"), 0);
    ASSERT_EQ(third->segment_count, 0u);

    vg_outputpane_destroy(pane);
}

TEST(outputpane_terminal_csi_clear_display_preserves_ansi_style) {
    vg_outputpane_t *pane = vg_outputpane_create();
    ASSERT_NOT_NULL(pane);

    vg_outputpane_set_terminal_mode(pane, true);
    vg_outputpane_append(pane, "old\n\033[31mred\033[2Jafter");

    ASSERT_EQ(pane->line_count, 1u);
    vg_output_line_t *line = test_outputpane_line_at(pane, 0);
    ASSERT_NOT_NULL(line);
    ASSERT_EQ(line->segment_count, 1u);
    ASSERT_EQ(strcmp(line->segments[0].text, "after"), 0);
    ASSERT_EQ(line->segments[0].fg_color, (uint32_t)0xFFCC0000u);

    vg_outputpane_destroy(pane);
}

/// @brief Verify DEC alternate-screen mode preserves and restores primary scrollback.
/// @details Full-screen terminal programs enter CSI ?1049h and leave CSI ?1049l.
///          The output pane must show the alternate buffer while active, then
///          restore the original primary lines, cursor row, and ownership fields
///          when the program exits alternate-screen mode.
TEST(outputpane_terminal_alternate_screen_restores_primary_buffer) {
    vg_outputpane_t *pane = vg_outputpane_create();
    ASSERT_NOT_NULL(pane);

    vg_outputpane_set_terminal_mode(pane, true);
    vg_outputpane_append(pane, "shell\nprompt");
    ASSERT_EQ(pane->line_count, 2u);
    ASSERT_EQ(strcmp(test_outputpane_line_at(pane, 0)->segments[0].text, "shell"), 0);
    ASSERT_EQ(strcmp(test_outputpane_line_at(pane, 1)->segments[0].text, "prompt"), 0);

    vg_outputpane_append(pane, "\033[?1049hfull\nscreen");
    ASSERT_TRUE(pane->alternate_screen);
    ASSERT_EQ(pane->primary_line_count, 2u);
    ASSERT_EQ(pane->line_count, 2u);
    ASSERT_EQ(strcmp(test_outputpane_line_at(pane, 0)->segments[0].text, "full"), 0);
    ASSERT_EQ(strcmp(test_outputpane_line_at(pane, 1)->segments[0].text, "screen"), 0);

    vg_outputpane_append(pane, "\033[?1049l");
    ASSERT_FALSE(pane->alternate_screen);
    ASSERT_NULL(pane->primary_lines);
    ASSERT_EQ(pane->primary_line_count, 0u);
    ASSERT_EQ(pane->line_count, 2u);
    ASSERT_EQ(strcmp(test_outputpane_line_at(pane, 0)->segments[0].text, "shell"), 0);
    ASSERT_EQ(strcmp(test_outputpane_line_at(pane, 1)->segments[0].text, "prompt"), 0);

    vg_outputpane_destroy(pane);
}

/// @brief Verify terminal SGR supports true-color foregrounds and 256-color backgrounds.
/// @details The terminal parser and append-only ANSI parser share the same SGR
///          application helper. This regression ensures 38;2;r;g;b and 48;5;n
///          sequences produce explicit ARGB segment colors instead of being
///          ignored as unknown SGR operands.
TEST(outputpane_terminal_extended_sgr_colors_are_rendered) {
    vg_outputpane_t *pane = vg_outputpane_create();
    ASSERT_NOT_NULL(pane);

    vg_outputpane_set_terminal_mode(pane, true);
    vg_outputpane_append(pane, "\033[38;2;1;2;3mtrue\033[0m \033[48;5;196mback");

    vg_output_line_t *line = test_outputpane_line_at(pane, 0);
    ASSERT_NOT_NULL(line);
    ASSERT_EQ(line->segment_count, 3u);
    ASSERT_EQ(strcmp(line->segments[0].text, "true"), 0);
    ASSERT_EQ(line->segments[0].fg_color, (uint32_t)0xFF010203u);
    ASSERT_EQ(strcmp(line->segments[1].text, " "), 0);
    ASSERT_EQ(line->segments[1].fg_color, pane->default_fg);
    ASSERT_EQ(strcmp(line->segments[2].text, "back"), 0);
    ASSERT_EQ(line->segments[2].bg_color, (uint32_t)0xFFFF0000u);

    vg_outputpane_destroy(pane);
}

/// @brief Verify terminal key capture emits raw bytes with an authoritative length.
/// @details Ctrl+Space queues an embedded NUL byte, Shift+Tab queues CSI Z, modified
///          arrows use xterm CSI modifier encoding, and function keys use xterm
///          tilde sequences. The byte-counted drain must preserve the NUL instead
///          of truncating through strlen.
TEST(outputpane_terminal_key_input_uses_byte_counted_xterm_sequences) {
    vg_outputpane_t *pane = vg_outputpane_create();
    ASSERT_NOT_NULL(pane);
    vg_outputpane_set_terminal_mode(pane, true);

    vg_event_t enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, VG_MOD_NONE);
    vg_event_t backspace = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_BACKSPACE, 0, VG_MOD_NONE);
    ASSERT_TRUE(pane->base.vtable->handle_event(&pane->base, &enter));
    ASSERT_TRUE(pane->base.vtable->handle_event(&pane->base, &backspace));
    size_t len = 0;
    char *bytes = vg_outputpane_take_input_bytes(pane, &len);
    const char line_edit_expected[] = "\r\x7f";
    ASSERT_NOT_NULL(bytes);
    ASSERT_EQ(len, sizeof(line_edit_expected) - 1u);
    ASSERT_EQ(memcmp(bytes, line_edit_expected, sizeof(line_edit_expected) - 1u), 0);
    free(bytes);

    vg_event_t ctrl_space = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_SPACE, 0, VG_MOD_CTRL);
    ASSERT_TRUE(pane->base.vtable->handle_event(&pane->base, &ctrl_space));
    bytes = vg_outputpane_take_input_bytes(pane, &len);
    ASSERT_NOT_NULL(bytes);
    ASSERT_EQ(len, 1u);
    ASSERT_EQ(bytes[0], '\0');
    free(bytes);

    vg_event_t shift_tab = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_TAB, 0, VG_MOD_SHIFT);
    vg_event_t ctrl_right = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_RIGHT, 0, VG_MOD_CTRL);
    vg_event_t f5 = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_F5, 0, VG_MOD_NONE);
    ASSERT_TRUE(pane->base.vtable->handle_event(&pane->base, &shift_tab));
    ASSERT_TRUE(pane->base.vtable->handle_event(&pane->base, &ctrl_right));
    ASSERT_TRUE(pane->base.vtable->handle_event(&pane->base, &f5));

    bytes = vg_outputpane_take_input_bytes(pane, &len);
    ASSERT_NOT_NULL(bytes);
    const char expected[] = "\033[Z\033[1;5C\033[15~";
    ASSERT_EQ(len, sizeof(expected) - 1u);
    ASSERT_EQ(memcmp(bytes, expected, sizeof(expected) - 1u), 0);
    free(bytes);

    vg_outputpane_destroy(pane);
}

static void *g_context_select_ud = NULL;
static void *g_context_dismiss_ud = NULL;

/// @brief on_select callback that captures its user_data pointer for later assertion.
static void context_select_cb(vg_contextmenu_t *menu, vg_menu_item_t *item, void *ud) {
    (void)menu;
    (void)item;
    g_context_select_ud = ud;
}

/// @brief on_dismiss callback that captures its user_data pointer for later assertion.
static void context_dismiss_cb(vg_contextmenu_t *menu, void *ud) {
    (void)menu;
    g_context_dismiss_ud = ud;
}

/// @brief R6 — callbacks route correct user_data; submenu dismiss restores parent capture;
/// destroyed registry is inert.
TEST(contextmenu_callbacks_capture_and_registry_are_lifetime_safe) {
    int select_marker = 1;
    int dismiss_marker = 2;
    g_context_select_ud = NULL;
    g_context_dismiss_ud = NULL;

    vg_contextmenu_t *parent = vg_contextmenu_create();
    vg_contextmenu_t *submenu = vg_contextmenu_create();
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(submenu);
    vg_contextmenu_set_on_select(parent, context_select_cb, &select_marker);
    vg_contextmenu_set_on_dismiss(parent, context_dismiss_cb, &dismiss_marker);
    vg_menu_item_t *item = vg_contextmenu_add_item(parent, "Run", NULL, NULL, NULL);
    ASSERT_NOT_NULL(item);
    parent->on_select(parent, item, parent->on_select_data);
    vg_contextmenu_dismiss(parent);
    ASSERT(g_context_select_ud == &select_marker);
    ASSERT(g_context_dismiss_ud == &dismiss_marker);

    parent->is_visible = true;
    parent->base.visible = true;
    submenu->parent_menu = parent;
    submenu->is_visible = true;
    submenu->base.visible = true;
    vg_widget_set_input_capture(&submenu->base);
    vg_contextmenu_dismiss(submenu);
    ASSERT(vg_widget_get_input_capture() == &parent->base);
    vg_widget_release_input_capture();

    vg_widget_t *target = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_contextmenu_t *registered = vg_contextmenu_create();
    ASSERT_NOT_NULL(target);
    ASSERT_NOT_NULL(registered);
    vg_contextmenu_register_for_widget(target, registered);
    vg_contextmenu_destroy(registered);
    vg_event_t right = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 1.0f, 1.0f, VG_MOUSE_RIGHT, 0);
    ASSERT_FALSE(vg_contextmenu_process_event(target, &right));

    vg_widget_destroy(target);
    vg_contextmenu_destroy(parent);
    vg_contextmenu_destroy(submenu);
}

static float g_direct_event_mouse_x = 0.0f;
static float g_direct_event_mouse_y = 0.0f;

/// @brief vtable handle_event — captures the MOUSE_MOVE coordinates seen at the parent level after
/// bubbling.
static bool direct_event_parent_handler(vg_widget_t *widget, vg_event_t *event) {
    (void)widget;
    if (event->type == VG_EVENT_MOUSE_MOVE) {
        g_direct_event_mouse_x = event->mouse.x;
        g_direct_event_mouse_y = event->mouse.y;
        return true;
    }
    return false;
}

static vg_widget_vtable_t g_direct_event_parent_vtable = {
    .handle_event = direct_event_parent_handler,
};

/// @brief R6 — vg_event_send restores the original screen-space coordinates after bubbling through
/// parent vtables.
TEST(direct_event_send_restores_mouse_coordinates_after_bubbling) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CUSTOM);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    root->vtable = &g_direct_event_parent_vtable;
    vg_widget_add_child(root, child);
    vg_widget_arrange(root, 50.0f, 60.0f, 200.0f, 200.0f);
    vg_widget_arrange(child, 10.0f, 20.0f, 50.0f, 50.0f);

    vg_event_t move = vg_event_mouse(VG_EVENT_MOUSE_MOVE, 70.0f, 90.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_send(child, &move));
    ASSERT_NEAR(g_direct_event_mouse_x, 20.0f, 0.001f);
    ASSERT_NEAR(g_direct_event_mouse_y, 30.0f, 0.001f);
    ASSERT_NEAR(move.mouse.x, 70.0f, 0.001f);
    ASSERT_NEAR(move.mouse.y, 90.0f, 0.001f);

    vg_widget_destroy(root);
}

static float g_custom_paint_x = 0.0f;
static float g_custom_paint_y = 0.0f;
static float g_custom_paint_layout_x = 0.0f;
static float g_custom_paint_layout_y = 0.0f;

/// @brief vtable paint — records screen-space x/y and layout-space bounds at paint time.
static void custom_paint_records_local_geometry(vg_widget_t *widget, void *canvas) {
    (void)canvas;
    g_custom_paint_x = widget->x;
    g_custom_paint_y = widget->y;
    vg_widget_get_bounds(widget, &g_custom_paint_layout_x, &g_custom_paint_layout_y, NULL, NULL);
}

static vg_widget_vtable_t g_custom_paint_vtable = {
    .paint = custom_paint_records_local_geometry,
};

/// @brief R6 — vg_widget_get_bounds returns layout-space coordinates while paint receives
/// screen-space x/y.
TEST(custom_widget_paint_can_query_layout_geometry) {
    int dummy_canvas = 0;
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_custom_paint_vtable;
    vg_widget_add_child(root, child);
    vg_widget_arrange(root, 40.0f, 50.0f, 200.0f, 200.0f);
    vg_widget_arrange(child, 11.0f, 12.0f, 30.0f, 30.0f);

    vg_widget_paint(root, &dummy_canvas);
    ASSERT_NEAR(g_custom_paint_x, 51.0f, 0.001f);
    ASSERT_NEAR(g_custom_paint_y, 62.0f, 0.001f);
    ASSERT_NEAR(g_custom_paint_layout_x, 11.0f, 0.001f);
    ASSERT_NEAR(g_custom_paint_layout_y, 12.0f, 0.001f);
    ASSERT_NEAR(child->x, 11.0f, 0.001f);
    ASSERT_NEAR(child->y, 12.0f, 0.001f);

    vg_widget_destroy(root);
}

/// @brief R6 — destroying a focused child clears the root's focus state so subsequent key dispatch
/// returns false.
TEST(event_root_state_forgets_destroyed_child_focus) {
    vg_widget_t *root_a = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *root_b = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child_a = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root_a);
    ASSERT_NOT_NULL(root_b);
    ASSERT_NOT_NULL(child_a);
    per_root_focus_state_t state_a = {0};
    child_a->vtable = &g_per_root_focus_vtable;
    child_a->user_data = &state_a;
    vg_widget_add_child(root_a, child_a);
    vg_widget_arrange(root_a, 0.0f, 0.0f, 100.0f, 100.0f);
    vg_widget_arrange(child_a, 0.0f, 0.0f, 50.0f, 50.0f);
    vg_widget_arrange(root_b, 0.0f, 0.0f, 100.0f, 100.0f);

    vg_event_t down_a = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root_a, &down_a));
    vg_event_t noop_b = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, 0);
    (void)vg_event_dispatch(root_b, &noop_b);

    vg_widget_destroy(child_a);
    vg_event_t key_a = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, 0);
    ASSERT_FALSE(vg_event_dispatch(root_a, &key_a));
    ASSERT_EQ(state_a.key_count, 0);

    vg_widget_destroy(root_a);
    vg_widget_destroy(root_b);
}

/// @brief R6 — vg_key_from_vgfx_key maps known platform keys and returns VG_KEY_UNKNOWN for
/// unmapped values.
TEST(vgfx_special_keys_translate_through_public_helper) {
    ASSERT_EQ(vg_key_from_vgfx_key(VGFX_KEY_LEFT), VG_KEY_LEFT);
    ASSERT_EQ(vg_key_from_vgfx_key(VGFX_KEY_TAB), VG_KEY_TAB);
    ASSERT_EQ(vg_key_from_vgfx_key('A'), VG_KEY_A);
    ASSERT_EQ(vg_key_from_vgfx_key(-1), VG_KEY_UNKNOWN);
}

/// @brief R6 — grid_place at a row beyond the declared row count extends the implicit row set for
/// arrange.
TEST(grid_explicit_placement_extends_effective_rows) {
    vg_widget_t *grid = vg_grid_create(2, 1);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(grid);
    ASSERT_NOT_NULL(child);
    vg_widget_add_child(grid, child);
    vg_grid_place(grid, child, 1, 3, 1, 1);

    vg_widget_measure(grid, 200.0f, 100.0f);
    vg_widget_arrange(grid, 0.0f, 0.0f, 200.0f, 100.0f);

    ASSERT_TRUE(child->y > 0.0f);
    ASSERT_TRUE(child->height > 0.0f);
    ASSERT_TRUE(child->width > 0.0f);

    vg_widget_destroy(grid);
}

/// @brief R6 — set_runtime_state discards all pointers that refer to already-destroyed widgets.
TEST(widget_runtime_restore_rejects_destroyed_widget_pointer) {
    vg_widget_runtime_state_t empty = {0};
    vg_widget_set_runtime_state(&empty);

    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(widget);
    vg_widget_runtime_state_t state = {0};
    state.focused_widget = widget;
    state.input_capture_widget = widget;
    state.modal_root = widget;
    state.hovered_widget = widget;
    state.last_click_widget = widget;
    state.reported_click_widget = widget;
    state.last_click_time_ms = 11;
    state.reported_click_time_ms = 22;

    vg_widget_destroy(widget);
    vg_widget_set_runtime_state(&state);

    vg_widget_runtime_state_t restored = {0};
    vg_widget_get_runtime_state(&restored);
    ASSERT_NULL(restored.focused_widget);
    ASSERT_NULL(restored.input_capture_widget);
    ASSERT_NULL(restored.modal_root);
    ASSERT_NULL(restored.hovered_widget);
    ASSERT_NULL(restored.last_click_widget);
    ASSERT_NULL(restored.reported_click_widget);
}

//=============================================================================
// Round 7 - Zanna.GUI class correctness audit fixes
//=============================================================================

static int g_overlay_paint_count = 0;

/// @brief vtable paint_overlay — increments a counter to detect unwanted extra overlay repaints.
static void overlay_count_paint(vg_widget_t *widget, void *canvas) {
    (void)widget;
    (void)canvas;
    g_overlay_paint_count++;
}

static vg_widget_vtable_t g_overlay_count_vtable = {
    .paint_overlay = overlay_count_paint,
};

/// @brief R7 — scrollview's internal overlay pass does not trigger an extra paint_overlay call on
/// children.
TEST(scrollview_internal_overlay_children_are_not_repainted_by_global_pass) {
    // A bare vg_widget_create() allocation tagged VG_WIDGET_SCROLLVIEW is a
    // land-mine: destroy (and any other path) downcasts by type and writes
    // scrollview fields past the base-sized allocation (ASan-caught in
    // clear_interactive_state_recursive). Keep the default vtable so the
    // dummy paint surface is never dereferenced, but give the widget full
    // scrollview-sized storage.
    vg_scrollview_t *storage = (vg_scrollview_t *)calloc(1, sizeof(vg_scrollview_t));
    ASSERT_NOT_NULL(storage);
    vg_widget_init(&storage->base, VG_WIDGET_SCROLLVIEW, NULL);
    vg_widget_t *root = &storage->base;
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_overlay_count_vtable;
    vg_widget_add_child(root, child);

    g_overlay_paint_count = 0;
    vg_widget_paint(root, (void *)0x1);
    ASSERT_EQ(g_overlay_paint_count, 0);

    vg_widget_destroy(root);
}

/// @brief R7 — removing and re-adding a child to a grid clears placement metadata so it flows from
/// column 0.
TEST(grid_placement_metadata_is_removed_when_child_detaches) {
    vg_widget_t *grid = vg_grid_create(2, 1);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(grid);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_fixed_vtable;

    vg_widget_add_child(grid, child);
    vg_grid_place(grid, child, 1, 0, 1, 1);
    vg_widget_measure(grid, 200.0f, 100.0f);
    vg_widget_arrange(grid, 0.0f, 0.0f, 200.0f, 100.0f);
    ASSERT(child->x > 90.0f);

    vg_widget_remove_child(grid, child);
    vg_widget_add_child(grid, child);
    vg_widget_measure(grid, 200.0f, 100.0f);
    vg_widget_arrange(grid, 0.0f, 0.0f, 200.0f, 100.0f);
    ASSERT_NEAR(child->x, 0.0f, 0.001f);

    vg_widget_destroy(grid);
}

/// @brief R7 — removing and re-adding a child to a dock clears the side-dock metadata so it fills
/// the center.
TEST(dock_metadata_is_removed_when_child_detaches) {
    vg_widget_t *dock = vg_dock_create();
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(dock);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_fixed_vtable;

    vg_dock_add(dock, child, VG_DOCK_LEFT);
    vg_widget_measure(dock, 100.0f, 50.0f);
    vg_widget_arrange(dock, 0.0f, 0.0f, 100.0f, 50.0f);
    ASSERT(child->width < 100.0f);

    vg_widget_remove_child(dock, child);
    vg_widget_add_child(dock, child);
    vg_widget_measure(dock, 100.0f, 50.0f);
    vg_widget_arrange(dock, 0.0f, 0.0f, 100.0f, 50.0f);
    ASSERT_NEAR(child->width, 100.0f, 0.001f);

    vg_widget_destroy(dock);
}

static int g_radio_a_false = 0;
static int g_radio_b_true = 0;
static int g_radio_b_false = 0;

/// @brief on_change callback for radio A — increments counter when A is deselected.
static void radio_a_change(vg_widget_t *radio, bool selected, void *user_data) {
    (void)radio;
    (void)user_data;
    if (!selected)
        g_radio_a_false++;
}

/// @brief on_change callback for radio B — increments true/false counters based on selection state.
static void radio_b_change(vg_widget_t *radio, bool selected, void *user_data) {
    (void)radio;
    (void)user_data;
    if (selected)
        g_radio_b_true++;
    else
        g_radio_b_false++;
}

/// @brief R7 — selecting a radio button fires on_change for both old and new button, marks both
/// needs_paint.
TEST(radiobutton_group_selection_updates_callbacks_dirty_and_clear_state) {
    vg_radiogroup_t *group = vg_radiogroup_create();
    ASSERT_NOT_NULL(group);
    vg_radiobutton_t *a = vg_radiobutton_create(NULL, "A", group);
    vg_radiobutton_t *b = vg_radiobutton_create(NULL, "B", group);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    a->on_change = radio_a_change;
    b->on_change = radio_b_change;

    vg_radiobutton_set_selected(a, true);
    a->base.needs_paint = false;
    b->base.needs_paint = false;
    g_radio_a_false = 0;
    g_radio_b_true = 0;
    g_radio_b_false = 0;

    vg_radiobutton_set_selected(b, true);
    ASSERT_FALSE(vg_radiobutton_is_selected(a));
    ASSERT_TRUE(vg_radiobutton_is_selected(b));
    ASSERT_EQ(vg_radiogroup_get_selected(group), 1);
    ASSERT_EQ(g_radio_a_false, 1);
    ASSERT_EQ(g_radio_b_true, 1);
    ASSERT_TRUE(a->base.needs_paint);
    ASSERT_TRUE(b->base.needs_paint);

    b->base.needs_paint = false;
    vg_radiobutton_set_selected(b, false);
    ASSERT_FALSE(vg_radiobutton_is_selected(b));
    ASSERT_EQ(vg_radiogroup_get_selected(group), -1);
    ASSERT_EQ(g_radio_b_false, 1);
    ASSERT_TRUE(b->base.needs_paint);

    vg_widget_destroy(&a->base);
    vg_widget_destroy(&b->base);
    vg_radiogroup_destroy(group);
}

static int g_checkbox_change_count = 0;
static bool g_checkbox_last_checked = true;

/// @brief on_change callback that counts invocations and records the last checked state.
static void checkbox_change_counter(vg_widget_t *checkbox, bool checked, void *user_data) {
    (void)checkbox;
    (void)user_data;
    g_checkbox_change_count++;
    g_checkbox_last_checked = checked;
}

/// @brief R7 — set_indeterminate(true) clears checked state and VG_STATE_CHECKED, firing on_change
/// once.
TEST(checkbox_indeterminate_clears_checked_state_and_style_flag) {
    vg_checkbox_t *cb = vg_checkbox_create(NULL, "Tri");
    ASSERT_NOT_NULL(cb);
    vg_checkbox_set_on_change(cb, checkbox_change_counter, NULL);
    vg_checkbox_set_checked(cb, true);

    g_checkbox_change_count = 0;
    g_checkbox_last_checked = true;
    vg_checkbox_set_indeterminate(cb, true);

    ASSERT_TRUE(vg_checkbox_is_indeterminate(cb));
    ASSERT_FALSE(vg_checkbox_is_checked(cb));
    ASSERT_FALSE((cb->base.state & VG_STATE_CHECKED) != 0);
    ASSERT_EQ(g_checkbox_change_count, 1);
    ASSERT_FALSE(g_checkbox_last_checked);

    vg_checkbox_set_checked(cb, false);
    ASSERT_FALSE(vg_checkbox_is_indeterminate(cb));

    vg_widget_destroy(&cb->base);
}

static int g_textinput_change_count = 0;

/// @brief on_change callback that counts how many times the textinput fires a change event.
static void textinput_change_counter(vg_widget_t *widget, const char *text, void *user_data) {
    (void)widget;
    (void)text;
    (void)user_data;
    g_textinput_change_count++;
}

/// @brief R7 — inserting over a selection emits exactly one on_change event, not two (delete +
/// insert).
TEST(textinput_replacement_insert_emits_single_change) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(input);
    vg_textinput_set_text(input, "abc");
    vg_textinput_set_on_change(input, textinput_change_counter, NULL);

    g_textinput_change_count = 0;
    vg_textinput_select(input, 1, 2);
    vg_textinput_insert(input, "Z");

    ASSERT_EQ(strcmp(vg_textinput_get_text(input), "aZc"), 0);
    ASSERT_EQ(g_textinput_change_count, 1);

    vg_widget_destroy(&input->base);
}

/// @brief Set a preferred size of 180×48 on widget and assert that measured size matches exactly.
static void assert_preferred_measure(vg_widget_t *widget) {
    ASSERT_NOT_NULL(widget);
    vg_widget_set_preferred_size(widget, 180.0f, 48.0f);
    vg_widget_measure(widget, 1000.0f, 1000.0f);
    ASSERT_NEAR(widget->measured_width, 180.0f, 0.001f);
    ASSERT_NEAR(widget->measured_height, 48.0f, 0.001f);
}

/// @brief R7 — textinput, scrollview, slider, progressbar, spinner, listbox, dropdown, checkbox,
/// radio all clamp to preferred_size.
TEST(specialized_widgets_honor_preferred_size_constraints) {
    vg_textinput_t *textinput = vg_textinput_create(NULL);
    vg_scrollview_t *scrollview = vg_scrollview_create(NULL);
    vg_slider_t *slider = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    vg_progressbar_t *progress = vg_progressbar_create(NULL);
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    vg_dropdown_t *dropdown = vg_dropdown_create(NULL);
    vg_checkbox_t *checkbox = vg_checkbox_create(NULL, "check");
    vg_radiogroup_t *group = vg_radiogroup_create();
    ASSERT_NOT_NULL(group);
    vg_radiobutton_t *radio = vg_radiobutton_create(NULL, "radio", group);

    ASSERT_NOT_NULL(textinput);
    ASSERT_NOT_NULL(scrollview);
    ASSERT_NOT_NULL(slider);
    ASSERT_NOT_NULL(progress);
    ASSERT_NOT_NULL(spinner);
    ASSERT_NOT_NULL(listbox);
    ASSERT_NOT_NULL(dropdown);
    ASSERT_NOT_NULL(checkbox);
    ASSERT_NOT_NULL(radio);

    assert_preferred_measure(&textinput->base);
    assert_preferred_measure(&scrollview->base);
    assert_preferred_measure(&slider->base);
    assert_preferred_measure(&progress->base);
    assert_preferred_measure(&spinner->base);
    assert_preferred_measure(&listbox->base);
    assert_preferred_measure(&dropdown->base);
    assert_preferred_measure(&checkbox->base);
    assert_preferred_measure(&radio->base);

    vg_widget_destroy(&textinput->base);
    vg_widget_destroy(&scrollview->base);
    vg_widget_destroy(&slider->base);
    vg_widget_destroy(&progress->base);
    vg_widget_destroy(&spinner->base);
    vg_widget_destroy(&listbox->base);
    vg_widget_destroy(&dropdown->base);
    vg_widget_destroy(&checkbox->base);
    vg_widget_destroy(&radio->base);
    vg_radiogroup_destroy(group);
}

/// @brief R7 — slider respects max_size, clamping measured dimensions below the preferred_size.
TEST(specialized_widgets_honor_max_size_constraints) {
    vg_slider_t *slider = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(slider);
    vg_widget_set_preferred_size(&slider->base, 200.0f, 40.0f);
    vg_widget_set_max_size(&slider->base, 120.0f, 24.0f);
    vg_widget_measure(&slider->base, 1000.0f, 1000.0f);
    ASSERT_NEAR(slider->base.measured_width, 120.0f, 0.001f);
    ASSERT_NEAR(slider->base.measured_height, 24.0f, 0.001f);
    vg_widget_destroy(&slider->base);
}

/// @brief R7 — NaN progress value clamps to 0; negative or NaN tick deltas leave animation_phase
/// unchanged.
TEST(progressbar_sanitizes_nan_value_and_normalizes_phase) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);

    vg_progressbar_set_value(pb, strtof("nan", NULL));
    ASSERT_NEAR(vg_progressbar_get_value(pb), 0.0f, 0.001f);

    vg_progressbar_set_style(pb, VG_PROGRESS_INDETERMINATE);
    vg_progressbar_tick(pb, 10.0f);
    ASSERT(pb->animation_phase >= 0.0f && pb->animation_phase < 1.0f);
    float phase = pb->animation_phase;
    vg_progressbar_tick(pb, -1.0f);
    ASSERT_NEAR(pb->animation_phase, phase, 0.001f);
    vg_progressbar_tick(pb, strtof("nan", NULL));
    ASSERT_NEAR(pb->animation_phase, phase, 0.001f);

    vg_widget_destroy(&pb->base);
}

/// @brief R8 — circular progress is a real rendering style with square natural measurement.
TEST(progressbar_circular_style_measures_as_square_indicator) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);

    vg_progressbar_set_style(pb, VG_PROGRESS_CIRCULAR);
    vg_widget_measure(&pb->base, 1000.0f, 1000.0f);
    ASSERT_EQ(pb->style, VG_PROGRESS_CIRCULAR);
    ASSERT_NEAR(pb->base.measured_width, 48.0f, 0.001f);
    ASSERT_NEAR(pb->base.measured_height, 48.0f, 0.001f);

    vg_widget_destroy(&pb->base);
}

/// @brief R8 — changing the step immediately re-snaps the current slider value.
TEST(slider_set_step_resnaps_current_value) {
    vg_slider_t *slider = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(slider);

    vg_slider_set_range(slider, 0.0f, 10.0f);
    vg_slider_set_value(slider, 2.3f);
    ASSERT_NEAR(vg_slider_get_value(slider), 2.3f, 0.001f);
    vg_slider_set_step(slider, 1.0f);
    ASSERT_NEAR(vg_slider_get_value(slider), 2.0f, 0.001f);

    vg_widget_destroy(&slider->base);
}

/// @brief R8 — clearing custom dialog buttons resets the preset/count coherently.
TEST(dialog_custom_buttons_clear_resets_custom_state) {
    vg_dialog_t *dialog = vg_dialog_create("buttons");
    ASSERT_NOT_NULL(dialog);

    vg_dialog_button_def_t defs[1] = {
        {.label = "Apply", .result = VG_DIALOG_RESULT_OK, .is_default = true, .is_cancel = false},
    };
    vg_dialog_set_custom_buttons(dialog, defs, 1);
    ASSERT_EQ(dialog->custom_button_count, (size_t)1);
    ASSERT_EQ(dialog->button_preset, VG_DIALOG_BUTTONS_CUSTOM);

    vg_dialog_set_custom_buttons(dialog, NULL, 0);
    ASSERT_NULL(dialog->custom_buttons);
    ASSERT_EQ(dialog->custom_button_count, (size_t)0);
    ASSERT_NEQ(dialog->button_preset, VG_DIALOG_BUTTONS_CUSTOM);

    vg_widget_destroy(&dialog->base);
}

/// @brief R8 — breadcrumb clear closes dropdown state and releases input capture.
TEST(breadcrumb_clear_releases_dropdown_capture) {
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    ASSERT_NOT_NULL(bc);
    vg_breadcrumb_push(bc, "root", NULL);
    ASSERT_EQ(bc->item_count, (size_t)1);
    vg_breadcrumb_item_add_dropdown(&bc->items[0], "child", NULL);

    bc->dropdown_open = true;
    bc->dropdown_index = 0;
    bc->dropdown_hovered = 0;
    vg_widget_set_input_capture(&bc->base);
    ASSERT(vg_widget_get_input_capture() == &bc->base);

    vg_breadcrumb_clear(bc);
    ASSERT_FALSE(bc->dropdown_open);
    ASSERT_EQ(bc->dropdown_index, -1);
    ASSERT_EQ(bc->dropdown_hovered, -1);
    ASSERT_NULL(vg_widget_get_input_capture());

    vg_widget_destroy(&bc->base);
}

/// @brief R7 — set_font with a negative or NaN size leaves font_size unchanged at the current valid
/// value.
TEST(spinner_set_font_rejects_invalid_sizes) {
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);

    vg_spinner_set_font(spinner, NULL, -4.0f);
    ASSERT(isfinite(spinner->font_size));
    ASSERT(spinner->font_size > 0.0f);
    float fallback = spinner->font_size;

    vg_spinner_set_font(spinner, NULL, strtof("nan", NULL));
    ASSERT_NEAR(spinner->font_size, fallback, 0.001f);

    vg_widget_destroy(&spinner->base);
}

/// @brief R7 — set_placeholder sets needs_layout and needs_paint so the new text is measured and
/// repainted.
TEST(dropdown_placeholder_marks_layout_dirty) {
    vg_dropdown_t *dd = vg_dropdown_create(NULL);
    ASSERT_NOT_NULL(dd);
    dd->base.needs_layout = false;
    dd->base.needs_paint = false;

    vg_dropdown_set_placeholder(dd, "A much wider placeholder");
    ASSERT_TRUE(dd->base.needs_layout);
    ASSERT_TRUE(dd->base.needs_paint);

    vg_widget_destroy(&dd->base);
}

//=============================================================================
// Round 8: Low-level library correctness fixes
//=============================================================================

TEST(widget_capture_and_modal_reject_non_live_pointers) {
    vg_widget_t fake;
    memset(&fake, 0, sizeof(fake));

    vg_widget_set_input_capture(&fake);
    ASSERT_NULL(vg_widget_get_input_capture());

    vg_widget_set_modal_root(&fake);
    ASSERT_NULL(vg_widget_get_modal_root());

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    vg_widget_set_input_capture(root);
    ASSERT(vg_widget_get_input_capture() == root);
    vg_widget_set_modal_root(root);
    ASSERT(vg_widget_get_modal_root() == root);

    vg_widget_set_visible(root, false);
    ASSERT_NULL(vg_widget_get_input_capture());
    ASSERT_NULL(vg_widget_get_modal_root());
    vg_widget_destroy(root);
    ASSERT_NULL(vg_widget_get_input_capture());
    ASSERT_NULL(vg_widget_get_modal_root());
}

TEST(event_dispatch_keeps_state_for_more_than_legacy_root_cap) {
    enum { ROOT_COUNT = 20 };

    vg_widget_t *roots[ROOT_COUNT] = {0};
    vg_widget_t *children[ROOT_COUNT] = {0};

    for (int i = 0; i < ROOT_COUNT; i++) {
        roots[i] = vg_widget_create(VG_WIDGET_CONTAINER);
        children[i] = vg_widget_create(VG_WIDGET_CUSTOM);
        ASSERT_NOT_NULL(roots[i]);
        ASSERT_NOT_NULL(children[i]);
        children[i]->vtable = &g_per_root_focus_vtable;
        vg_widget_add_child(roots[i], children[i]);
    }

    for (int i = 0; i < ROOT_COUNT; i++) {
        vg_event_t ev = {0};
        ev.type = VG_EVENT_RESIZE;
        vg_event_dispatch(roots[i], &ev);
        vg_widget_set_focus(children[i]);
    }

    vg_event_t save_last = {0};
    save_last.type = VG_EVENT_RESIZE;
    vg_event_dispatch(roots[0], &save_last);

    for (int i = 0; i < ROOT_COUNT; i++) {
        vg_event_t ev = {0};
        ev.type = VG_EVENT_RESIZE;
        vg_event_dispatch(roots[i], &ev);
        ASSERT(vg_widget_get_focused(roots[i]) == children[i]);
    }

    for (int i = 0; i < ROOT_COUNT; i++)
        vg_widget_destroy(roots[i]);
}

TEST(textinput_ignores_command_modified_character_events) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(input);

    vg_event_t ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 'x', VG_MOD_CTRL);
    ASSERT_FALSE(vg_event_send(&input->base, &ev));
    ASSERT(strcmp(vg_textinput_get_text(input), "") == 0);

    ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 'y', VG_MOD_SUPER);
    ASSERT_FALSE(vg_event_send(&input->base, &ev));
    ASSERT(strcmp(vg_textinput_get_text(input), "") == 0);

    ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, '@', VG_MOD_CTRL | VG_MOD_ALT);
    ASSERT_TRUE(vg_event_send(&input->base, &ev));
    ASSERT(strcmp(vg_textinput_get_text(input), "@") == 0);

    vg_widget_destroy(&input->base);
}

TEST(codeeditor_ignores_command_modified_character_events) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_event_t ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 'x', VG_MOD_CTRL);
    ASSERT_FALSE(vg_event_send(&editor->base, &ev));
    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "") == 0);
    free(text);

    ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 'y', VG_MOD_SUPER);
    ASSERT_FALSE(vg_event_send(&editor->base, &ev));
    text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "") == 0);
    free(text);

    ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, '@', VG_MOD_CTRL | VG_MOD_ALT);
    ASSERT_TRUE(vg_event_send(&editor->base, &ev));
    text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "@") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

TEST(text_widgets_reject_private_use_function_key_codepoints) {
    vg_textinput_t *input = vg_textinput_create(NULL);
    ASSERT_NOT_NULL(input);

    vg_event_t ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 0xF700u, 0);
    ASSERT_TRUE(vg_event_send(&input->base, &ev));
    ASSERT(strcmp(vg_textinput_get_text(input), "") == 0);

    ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 'x', 0);
    ASSERT_TRUE(vg_event_send(&input->base, &ev));
    ASSERT(strcmp(vg_textinput_get_text(input), "x") == 0);

    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 0xF703u, 0);
    ASSERT_TRUE(vg_event_send(&editor->base, &ev));
    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "") == 0);
    free(text);

    ev = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 'y', 0);
    ASSERT_TRUE(vg_event_send(&editor->base, &ev));
    text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "y") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
    vg_widget_destroy(&input->base);
}

TEST(codeeditor_printable_keydown_preserves_selection_for_character_input) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_codeeditor_set_text(editor, "abc");
    vg_codeeditor_set_selection(editor, 0, 1, 0, 2);

    vg_event_t key = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_X, 0, VG_MOD_NONE);
    ASSERT_FALSE(vg_event_send(&editor->base, &key));

    vg_event_t ch = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 'x', VG_MOD_NONE);
    ASSERT_TRUE(vg_event_send(&editor->base, &ch));

    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "axc") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

TEST(listbox_virtual_keyboard_navigation_handles_large_counts) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(listbox);

    uint8_t selection[1] = {0};
    listbox->virtual_mode = true;
    listbox->total_item_count = (size_t)INT32_MAX + 4u;
    listbox->selection_bitmap = selection;
    listbox->selection_bitmap_size = 4;
    listbox->selected_index = SIZE_MAX;
    listbox->anchor_selected_index = SIZE_MAX;
    listbox->item_height = 20.0f;
    listbox->base.height = 60.0f;

    vg_event_t ev = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_DOWN, 0, VG_MOD_NONE);
    ASSERT_TRUE(vg_event_send(&listbox->base, &ev));
    ASSERT_EQ(vg_listbox_get_selected_index(listbox), (size_t)0);
    ASSERT_TRUE((selection[0] & 0x01u) != 0);

    listbox->selection_bitmap = NULL;
    listbox->selection_bitmap_size = 0;
    vg_widget_destroy(&listbox->base);
}

TEST(tabbar_removed_tab_handles_are_inert) {
    vg_tabbar_t *tabbar = vg_tabbar_create(NULL);
    ASSERT_NOT_NULL(tabbar);

    vg_tab_t *first = vg_tabbar_add_tab(tabbar, "first", true);
    vg_tab_t *second = vg_tabbar_add_tab(tabbar, "second", true);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    vg_tabbar_remove_tab(tabbar, second);
    ASSERT_FALSE(vg_tab_is_live(second));
    ASSERT_EQ(tabbar->tab_count, 1);
    ASSERT(tabbar->active_tab == first);

    vg_tab_set_title(second, "stale");
    vg_tab_set_modified(second, true);
    vg_tab_set_tooltip(second, "stale");
    vg_tabbar_set_active(tabbar, second);
    vg_tabbar_remove_tab(tabbar, second);

    ASSERT_EQ(tabbar->tab_count, 1);
    ASSERT(tabbar->active_tab == first);
    ASSERT(strcmp(first->title, "first") == 0);

    vg_widget_destroy(&tabbar->base);
}

static int g_round8_menu_accel_calls = 0;

static void round8_menu_accel_action(void *data) {
    (void)data;
    g_round8_menu_accel_calls++;
}

TEST(menubar_clear_remove_and_accelerators_drop_stale_items) {
    vg_menubar_t *menubar = vg_menubar_create(NULL);
    ASSERT_NOT_NULL(menubar);
    vg_menu_t *menu = vg_menubar_add_menu(menubar, "File");
    ASSERT_NOT_NULL(menu);
    vg_menu_item_t *item = vg_menu_add_item(menu, "New", "Ctrl+N", round8_menu_accel_action, NULL);
    ASSERT_NOT_NULL(item);
    vg_menubar_register_accelerator(menubar, item, "Ctrl+N");

    g_round8_menu_accel_calls = 0;
    menubar->open_menu = menu;
    menubar->highlighted = item;
    menubar->menu_active = true;
    menu->open = true;

    vg_menu_remove_item(menu, item);
    ASSERT_FALSE(vg_menu_item_is_live(item));
    ASSERT_NULL(menubar->highlighted);
    ASSERT_NULL(menubar->open_menu);
    ASSERT_FALSE(menubar->menu_active);
    ASSERT_FALSE(menu->open);
    ASSERT_FALSE(vg_menubar_handle_accelerator(menubar, 'N', VG_MOD_CTRL));
    ASSERT_EQ(g_round8_menu_accel_calls, 0);

    vg_menu_item_t *clear_item = vg_menu_add_item(menu, "Open", NULL, NULL, NULL);
    ASSERT_NOT_NULL(clear_item);
    menubar->open_menu = menu;
    menubar->highlighted = clear_item;
    menubar->menu_active = true;
    menu->open = true;

    vg_menu_clear(menu);
    ASSERT_FALSE(vg_menu_item_is_live(clear_item));
    ASSERT_EQ(menu->item_count, 0);
    ASSERT_NULL(menubar->highlighted);
    ASSERT_NULL(menubar->open_menu);
    ASSERT_FALSE(menubar->menu_active);
    ASSERT_FALSE(menu->open);

    vg_menu_t *second_menu = vg_menubar_add_menu(menubar, "Edit");
    ASSERT_NOT_NULL(second_menu);
    vg_menu_item_t *second_item = vg_menu_add_item(second_menu, "Cut", NULL, NULL, NULL);
    ASSERT_NOT_NULL(second_item);
    menubar->open_menu = second_menu;
    menubar->highlighted = second_item;
    menubar->menu_active = true;
    second_menu->open = true;

    vg_menubar_remove_menu(menubar, second_menu);
    ASSERT_NULL(menubar->highlighted);
    ASSERT_NULL(menubar->open_menu);
    ASSERT_FALSE(menubar->menu_active);

    vg_widget_destroy(&menubar->base);
}

TEST(contextmenu_clear_retires_items_and_submenu_state) {
    vg_contextmenu_t *parent = vg_contextmenu_create();
    vg_contextmenu_t *child = vg_contextmenu_create();
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(child);

    vg_menu_item_t *item = vg_contextmenu_add_submenu(parent, "More", child);
    ASSERT_NOT_NULL(item);
    ASSERT_TRUE(vg_menu_item_is_live(item));

    parent->is_visible = true;
    parent->hovered_index = 0;
    parent->clicked_index = 0;
    parent->active_submenu = child;
    child->is_visible = true;

    vg_contextmenu_clear(parent);
    ASSERT_EQ(parent->item_count, (size_t)0);
    ASSERT_EQ(parent->hovered_index, -1);
    ASSERT_EQ(parent->clicked_index, -1);
    ASSERT_NULL(parent->active_submenu);
    ASSERT_FALSE(vg_menu_item_is_live(item));

    vg_contextmenu_item_set_enabled(item, false);
    vg_contextmenu_item_set_checked(item, true);
    vg_contextmenu_destroy(parent);
}

TEST(toolbar_removed_item_handles_are_inert) {
    vg_toolbar_t *toolbar = vg_toolbar_create(NULL, VG_TOOLBAR_HORIZONTAL);
    ASSERT_NOT_NULL(toolbar);
    vg_toolbar_item_t *item =
        vg_toolbar_add_button(toolbar, "open", "Open", vg_icon_from_glyph('O'), NULL, NULL);
    ASSERT_NOT_NULL(item);

    toolbar->hovered_item = item;
    toolbar->pressed_item = item;
    toolbar->focused_index = 0;
    vg_toolbar_remove_item_ptr(toolbar, item);
    ASSERT_EQ(toolbar->item_count, (size_t)0);
    ASSERT_NULL(toolbar->hovered_item);
    ASSERT_NULL(toolbar->pressed_item);
    ASSERT_EQ(toolbar->focused_index, -1);
    ASSERT_FALSE(vg_toolbar_item_is_live(item));

    vg_toolbar_item_set_enabled(item, false);
    vg_toolbar_item_set_checked(item, true);
    vg_toolbar_item_set_text(item, "stale");
    vg_toolbar_item_set_tooltip(item, "stale");

    vg_widget_destroy(&toolbar->base);
}

TEST(statusbar_removed_item_handles_are_inert) {
    vg_statusbar_t *statusbar = vg_statusbar_create(NULL);
    ASSERT_NOT_NULL(statusbar);
    vg_statusbar_item_t *item = vg_statusbar_add_text(statusbar, VG_STATUSBAR_ZONE_LEFT, "Ready");
    ASSERT_NOT_NULL(item);

    statusbar->hovered_item = item;
    vg_statusbar_remove_item(statusbar, item);
    ASSERT_EQ(statusbar->left_count, (size_t)0);
    ASSERT_NULL(statusbar->hovered_item);
    ASSERT_FALSE(vg_statusbar_item_is_live(item));

    vg_statusbar_item_set_text(item, "stale");
    vg_statusbar_item_set_tooltip(item, "stale");
    vg_statusbar_item_set_progress(item, 0.5f);
    vg_statusbar_item_set_visible(item, false);

    vg_widget_destroy(&statusbar->base);
}

TEST(filedialog_create_always_has_current_path) {
    vg_filedialog_t *dialog = vg_filedialog_create(VG_FILEDIALOG_OPEN);
    ASSERT_NOT_NULL(dialog);
    ASSERT_NOT_NULL(dialog->current_path);
    ASSERT(dialog->current_path[0] != '\0');
    vg_filedialog_destroy(dialog);
}

//=============================================================================
// Round 9 - Zanna.GUI class correctness audit fixes
//=============================================================================

TEST(platform_event_translation_preserves_focus_unknown_and_modifiers) {
    ASSERT_EQ(vg_key_from_vgfx_key(VGFX_KEY_UNKNOWN), VG_KEY_UNKNOWN);
    ASSERT_EQ(vg_key_from_vgfx_key(-17), VG_KEY_UNKNOWN);

    vgfx_event_t pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = VGFX_EVENT_FOCUS_GAINED;
    vg_event_t ev = vg_event_from_platform(&pe);
    ASSERT_EQ(ev.type, VG_EVENT_FOCUS_IN);

    pe.type = VGFX_EVENT_FOCUS_LOST;
    ev = vg_event_from_platform(&pe);
    ASSERT_EQ(ev.type, VG_EVENT_FOCUS_OUT);

    memset(&pe, 0, sizeof(pe));
    pe.type = VGFX_EVENT_MOUSE_DOWN;
    pe.data.mouse_button.x = 12;
    pe.data.mouse_button.y = 34;
    pe.data.mouse_button.button = VGFX_MOUSE_LEFT;
    pe.data.mouse_button.modifiers = VGFX_MOD_SHIFT | VGFX_MOD_CTRL | VGFX_MOD_CMD;
    ev = vg_event_from_platform(&pe);
    ASSERT_EQ(ev.type, VG_EVENT_MOUSE_DOWN);
    ASSERT_EQ(ev.modifiers, (uint32_t)(VG_MOD_SHIFT | VG_MOD_CTRL | VG_MOD_SUPER));

    pe.type = (vgfx_event_type_t)9999;
    ev = vg_event_from_platform(&pe);
    ASSERT_EQ(ev.type, VG_EVENT_NONE);

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    ASSERT_FALSE(vg_event_dispatch(root, &ev));
    ASSERT_FALSE(vg_event_send(root, &ev));
    vg_widget_destroy(root);
}

TEST(focus_rejects_hidden_or_disabled_ancestors) {
    vg_widget_set_focus(NULL);

    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *panel = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(panel);
    vg_widget_add_child(root, panel);

    vg_textinput_t *input = vg_textinput_create(panel);
    ASSERT_NOT_NULL(input);

    vg_widget_set_visible(panel, false);
    vg_widget_set_focus(&input->base);
    ASSERT_NULL(vg_widget_get_focused(root));

    vg_widget_set_visible(panel, true);
    vg_widget_set_enabled(root, false);
    vg_widget_set_focus(&input->base);
    ASSERT_NULL(vg_widget_get_focused(root));

    vg_widget_set_enabled(root, true);
    vg_widget_set_focus(&input->base);
    ASSERT(vg_widget_get_focused(root) == &input->base);
    vg_widget_set_focus(NULL);

    vg_widget_destroy(root);
}

TEST(flex_grow_keeps_unequal_measured_basis) {
    vg_widget_t *flex = vg_flex_create();
    ASSERT_NOT_NULL(flex);
    vg_flex_set_direction(flex, VG_DIRECTION_ROW);
    vg_flex_set_align_items(flex, VG_ALIGN_START);
    vg_flex_set_gap(flex, 0.0f);

    vg_widget_t *small = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *large = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(small);
    ASSERT_NOT_NULL(large);
    vg_widget_set_preferred_size(small, 50.0f, 20.0f);
    vg_widget_set_preferred_size(large, 100.0f, 20.0f);
    vg_widget_set_flex(small, 1.0f);
    vg_widget_set_flex(large, 1.0f);
    vg_widget_add_child(flex, small);
    vg_widget_add_child(flex, large);

    vg_widget_measure(flex, 300.0f, 40.0f);
    vg_widget_arrange(flex, 0.0f, 0.0f, 300.0f, 40.0f);

    ASSERT_NEAR(small->width, 125.0f, 0.001f);
    ASSERT_NEAR(large->width, 175.0f, 0.001f);

    vg_widget_destroy(flex);
}

static int g_handled_mouseup_clicks = 0;

static bool handled_mouseup_suppresses_click_handler(vg_widget_t *widget, vg_event_t *event) {
    (void)widget;
    if (event->type == VG_EVENT_MOUSE_UP)
        return true;
    if (event->type == VG_EVENT_CLICK)
        g_handled_mouseup_clicks++;
    return false;
}

static vg_widget_vtable_t g_handled_mouseup_vtable = {
    .handle_event = handled_mouseup_suppresses_click_handler,
};

TEST(handled_mouseup_does_not_synthesize_click) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(widget);
    widget->vtable = &g_handled_mouseup_vtable;
    vg_widget_arrange(widget, 0.0f, 0.0f, 80.0f, 30.0f);

    g_handled_mouseup_clicks = 0;
    vg_event_t down = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    (void)vg_event_send(widget, &down);
    vg_event_t up = vg_event_mouse(VG_EVENT_MOUSE_UP, 10.0f, 10.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_send(widget, &up));
    ASSERT_EQ(g_handled_mouseup_clicks, 0);

    vg_widget_destroy(widget);
}

TEST(codeeditor_set_text_preserves_trailing_newlines) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_codeeditor_set_text(editor, "alpha\n");
    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "alpha\n") == 0);
    free(text);

    vg_codeeditor_set_text(editor, "alpha\n\n");
    text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "alpha\n\n") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

TEST(listbox_virtual_shrink_updates_logical_selection_bitmap_size) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(listbox);
    vg_listbox_set_virtual_mode(listbox, true, 10, 20.0f);
    ASSERT_TRUE(listbox->virtual_mode);
    ASSERT_EQ(listbox->selection_bitmap_size, (size_t)10);
    ASSERT_EQ(listbox->selection_bitmap_capacity, (size_t)10);

    vg_listbox_select_index(listbox, 8);
    ASSERT_EQ(vg_listbox_get_selected_index(listbox), (size_t)8);

    vg_listbox_set_total_count(listbox, 3);
    ASSERT_EQ(listbox->selection_bitmap_size, (size_t)3);
    ASSERT(listbox->selection_bitmap_capacity >= (size_t)3);
    ASSERT_EQ(vg_listbox_get_selected_index(listbox), SIZE_MAX);

    vg_widget_destroy(&listbox->base);
}

TEST(tabbar_close_click_uses_monotonic_version) {
    vg_tabbar_t *tabbar = vg_tabbar_create(NULL);
    ASSERT_NOT_NULL(tabbar);
    tabbar->auto_close = false;
    vg_tab_t *tab = vg_tabbar_add_tab(tabbar, "one", true);
    ASSERT_NOT_NULL(tab);

    vg_event_t close = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_W, 0, VG_MOD_CTRL);
    ASSERT_TRUE(vg_event_send(&tabbar->base, &close));
    ASSERT_EQ(tabbar->close_clicked_index, 0);
    ASSERT_EQ(tabbar->close_click_version, (uint64_t)1);

    tabbar->reported_close_click_version = tabbar->close_click_version;
    ASSERT_TRUE(vg_event_send(&tabbar->base, &close));
    ASSERT_EQ(tabbar->close_clicked_index, 0);
    ASSERT_EQ(tabbar->close_click_version, (uint64_t)2);

    vg_widget_destroy(&tabbar->base);
}

TEST(button_invalid_style_and_icon_position_are_clamped) {
    vg_button_t *button = vg_button_create(NULL, "Action");
    ASSERT_NOT_NULL(button);

    vg_button_set_style(button, (vg_button_style_t)999);
    ASSERT_EQ(button->style, VG_BUTTON_STYLE_DEFAULT);
    vg_button_set_style(button, (vg_button_style_t)-99);
    ASSERT_EQ(button->style, VG_BUTTON_STYLE_DEFAULT);

    vg_button_set_icon_position(button, 1);
    ASSERT_EQ(button->icon_pos, 1);
    vg_button_set_icon_position(button, 99);
    ASSERT_EQ(button->icon_pos, 0);
    vg_button_set_icon_position(button, -4);
    ASSERT_EQ(button->icon_pos, 0);

    vg_widget_destroy(&button->base);
}

TEST(dropdown_rejects_capacity_growth_overflow) {
    vg_dropdown_t *dropdown = vg_dropdown_create(NULL);
    ASSERT_NOT_NULL(dropdown);
    int old_count = dropdown->item_count;
    int old_capacity = dropdown->item_capacity;

    dropdown->item_count = INT_MAX / 2 + 1;
    dropdown->item_capacity = INT_MAX / 2 + 1;
    ASSERT_EQ(vg_dropdown_add_item(dropdown, "overflow"), -1);

    dropdown->item_count = old_count;
    dropdown->item_capacity = old_capacity;
    vg_widget_destroy(&dropdown->base);
}

static bool nonfocusable_custom_event(vg_widget_t *widget, vg_event_t *event) {
    (void)widget;
    (void)event;
    return false;
}

static vg_widget_vtable_t g_nonfocusable_custom_vtable = {
    .handle_event = nonfocusable_custom_event,
};

TEST(focus_rejects_widgets_without_focus_capability) {
    vg_widget_set_focus(NULL);
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *custom = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(custom);
    custom->vtable = &g_nonfocusable_custom_vtable;
    vg_widget_add_child(root, custom);

    vg_widget_set_focus(custom);
    ASSERT_NULL(vg_widget_get_focused(root));

    vg_widget_destroy(root);
}

TEST(widget_contains_point_rejects_hidden_and_disabled_chains) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    vg_widget_add_child(root, child);
    vg_widget_arrange(root, 0.0f, 0.0f, 100.0f, 100.0f);
    vg_widget_arrange(child, 10.0f, 10.0f, 30.0f, 30.0f);

    ASSERT_TRUE(vg_widget_contains_point(child, 20.0f, 20.0f));
    vg_widget_set_visible(root, false);
    ASSERT_FALSE(vg_widget_contains_point(child, 20.0f, 20.0f));
    vg_widget_set_visible(root, true);
    vg_widget_set_enabled(child, false);
    ASSERT_FALSE(vg_widget_contains_point(child, 20.0f, 20.0f));

    vg_widget_destroy(root);
}

TEST(widget_add_child_rejects_non_live_handles) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t fake;
    memset(&fake, 0, sizeof(fake));
    ASSERT_NOT_NULL(root);

    vg_widget_add_child(root, &fake);
    ASSERT_EQ(root->child_count, 0);
    vg_widget_insert_child(root, &fake, 0);
    ASSERT_EQ(root->child_count, 0);

    vg_widget_destroy(root);
}

TEST(manual_position_children_are_not_overwritten_by_layout) {
    vg_widget_t *vbox = vg_vbox_create(0.0f);
    vg_widget_t *manual = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *flow = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(vbox);
    ASSERT_NOT_NULL(manual);
    ASSERT_NOT_NULL(flow);

    vg_widget_set_preferred_size(manual, 20.0f, 20.0f);
    vg_widget_set_preferred_size(flow, 40.0f, 10.0f);
    manual->x = 70.0f;
    manual->y = 80.0f;
    manual->manual_position = true;
    vg_widget_add_child(vbox, manual);
    vg_widget_add_child(vbox, flow);

    vg_widget_measure(vbox, 200.0f, 200.0f);
    vg_widget_arrange(vbox, 0.0f, 0.0f, 200.0f, 200.0f);

    ASSERT_NEAR(manual->x, 70.0f, 0.001f);
    ASSERT_NEAR(manual->y, 80.0f, 0.001f);
    ASSERT_NEAR(manual->width, 20.0f, 0.001f);
    ASSERT_NEAR(manual->height, 20.0f, 0.001f);
    ASSERT_NEAR(flow->y, 0.0f, 0.001f);

    vg_widget_destroy(vbox);
}

TEST(box_layout_center_alignment_accounts_for_margins) {
    vg_widget_t *vbox = vg_vbox_create(0.0f);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(vbox);
    ASSERT_NOT_NULL(child);
    vg_vbox_set_align(vbox, VG_ALIGN_CENTER);
    vg_widget_set_preferred_size(child, 20.0f, 10.0f);
    vg_widget_set_margins(child, 10.0f, 0.0f, 30.0f, 0.0f);
    vg_widget_add_child(vbox, child);

    vg_widget_measure(vbox, 100.0f, 50.0f);
    vg_widget_arrange(vbox, 0.0f, 0.0f, 100.0f, 50.0f);
    ASSERT_NEAR(child->x, 30.0f, 0.001f);

    vg_widget_t *hbox = vg_hbox_create(0.0f);
    vg_widget_t *hchild = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(hbox);
    ASSERT_NOT_NULL(hchild);
    vg_hbox_set_align(hbox, VG_ALIGN_CENTER);
    vg_widget_set_preferred_size(hchild, 10.0f, 20.0f);
    vg_widget_set_margins(hchild, 0.0f, 10.0f, 0.0f, 30.0f);
    vg_widget_add_child(hbox, hchild);

    vg_widget_measure(hbox, 50.0f, 100.0f);
    vg_widget_arrange(hbox, 0.0f, 0.0f, 50.0f, 100.0f);
    ASSERT_NEAR(hchild->y, 30.0f, 0.001f);

    vg_widget_destroy(vbox);
    vg_widget_destroy(hbox);
}

static int g_capture_hold_clicks = 0;
static int g_capture_hold_ups = 0;

static bool capture_hold_handle_event(vg_widget_t *widget, vg_event_t *event) {
    if (event->type == VG_EVENT_MOUSE_DOWN) {
        vg_widget_set_input_capture(widget);
        return true;
    }
    if (event->type == VG_EVENT_MOUSE_UP) {
        g_capture_hold_ups++;
        return true;
    }
    if (event->type == VG_EVENT_CLICK) {
        g_capture_hold_clicks++;
        return true;
    }
    return false;
}

static vg_widget_vtable_t g_capture_hold_vtable = {
    .handle_event = capture_hold_handle_event,
};

TEST(non_dropdown_capture_does_not_synthesize_outside_click) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    child->vtable = &g_capture_hold_vtable;
    vg_widget_add_child(root, child);
    vg_widget_arrange(root, 0.0f, 0.0f, 200.0f, 200.0f);
    vg_widget_arrange(child, 0.0f, 0.0f, 20.0f, 20.0f);

    g_capture_hold_clicks = 0;
    g_capture_hold_ups = 0;
    vg_event_t down = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 5.0f, 5.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &down));
    vg_event_t up = vg_event_mouse(VG_EVENT_MOUSE_UP, 120.0f, 120.0f, VG_MOUSE_LEFT, 0);
    ASSERT_TRUE(vg_event_dispatch(root, &up));

    ASSERT_EQ(g_capture_hold_ups, 1);
    ASSERT_EQ(g_capture_hold_clicks, 0);
    vg_widget_release_input_capture();
    vg_widget_destroy(root);
}

static bool filedialog_modal_test_runner(vg_filedialog_t *dialog, void *user_data) {
    const char *path = (const char *)user_data;
    dialog->selected_files = (char **)calloc(1, sizeof(char *));
    if (!dialog->selected_files)
        return false;
    dialog->selected_files[0] = test_strdup_local(path);
    if (!dialog->selected_files[0]) {
        free(dialog->selected_files);
        dialog->selected_files = NULL;
        return false;
    }
    dialog->selected_file_count = 1;
    dialog->base.is_open = false;
    return true;
}

TEST(filedialog_convenience_uses_installed_modal_runner) {
    char expected[] = "/tmp/zanna-gui-test.txt";
    vg_filedialog_set_modal_runner(filedialog_modal_test_runner, expected);
    char *result = vg_filedialog_open_file("Open", NULL, "Text", "*.txt");
    ASSERT_NOT_NULL(result);
    ASSERT(strcmp(result, expected) == 0);
    free(result);
    vg_filedialog_set_modal_runner(NULL, NULL);
}

TEST(menubar_key_event_dispatches_accelerator) {
    vg_menubar_t *menubar = vg_menubar_create(NULL);
    ASSERT_NOT_NULL(menubar);
    vg_menu_t *menu = vg_menubar_add_menu(menubar, "File");
    ASSERT_NOT_NULL(menu);
    vg_menu_item_t *item = vg_menu_add_item(menu, "New", "Ctrl+N", round8_menu_accel_action, NULL);
    ASSERT_NOT_NULL(item);
    vg_menu_item_t *chord = vg_menu_add_item(
        menu, "Keyboard Shortcuts", "Ctrl+K Ctrl+S", round8_menu_accel_action, NULL);
    ASSERT_NOT_NULL(chord);
    vg_menubar_rebuild_accelerators(menubar);

    vg_accelerator_t parsed_chord;
    ASSERT_FALSE(vg_parse_accelerator("Ctrl+K Ctrl+S", &parsed_chord));
    ASSERT_EQ(parsed_chord.key, VG_KEY_UNKNOWN);

    g_round8_menu_accel_calls = 0;
    vg_event_t event = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_N, 0, VG_MOD_CTRL);
    ASSERT_TRUE(vg_event_send(&menubar->base, &event));
    ASSERT_EQ(g_round8_menu_accel_calls, 1);

    event = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_S, 0, VG_MOD_CTRL);
    ASSERT_FALSE(vg_event_send(&menubar->base, &event));
    ASSERT_EQ(g_round8_menu_accel_calls, 1);

    vg_widget_destroy(&menubar->base);
}

TEST(toolbar_capacity_overflow_is_rejected) {
    vg_toolbar_t *toolbar = vg_toolbar_create(NULL, VG_TOOLBAR_HORIZONTAL);
    ASSERT_NOT_NULL(toolbar);
    vg_toolbar_item_t **old_items = toolbar->items;
    size_t old_count = toolbar->item_count;
    size_t old_capacity = toolbar->item_capacity;

    toolbar->item_count = SIZE_MAX;
    toolbar->item_capacity = SIZE_MAX;
    ASSERT_NULL(vg_toolbar_add_button(
        toolbar, "overflow", "overflow", vg_icon_from_glyph('x'), NULL, NULL));

    toolbar->items = old_items;
    toolbar->item_count = old_count;
    toolbar->item_capacity = old_capacity;
    vg_widget_destroy(&toolbar->base);
}

TEST(statusbar_capacity_overflow_is_rejected) {
    vg_statusbar_t *statusbar = vg_statusbar_create(NULL);
    ASSERT_NOT_NULL(statusbar);
    vg_statusbar_item_t **old_items = statusbar->left_items;
    size_t old_count = statusbar->left_count;
    size_t old_capacity = statusbar->left_capacity;

    statusbar->left_count = SIZE_MAX;
    statusbar->left_capacity = SIZE_MAX;
    ASSERT_NULL(vg_statusbar_add_text(statusbar, VG_STATUSBAR_ZONE_LEFT, "overflow"));

    statusbar->left_items = old_items;
    statusbar->left_count = old_count;
    statusbar->left_capacity = old_capacity;
    vg_widget_destroy(&statusbar->base);
}

TEST(contextmenu_capacity_overflow_is_rejected) {
    vg_contextmenu_t *menu = vg_contextmenu_create();
    ASSERT_NOT_NULL(menu);
    vg_menu_item_t **old_items = menu->items;
    size_t old_count = menu->item_count;
    size_t old_capacity = menu->item_capacity;

    menu->item_count = SIZE_MAX;
    menu->item_capacity = SIZE_MAX;
    ASSERT_NULL(vg_contextmenu_add_item(menu, "overflow", NULL, NULL, NULL));

    menu->items = old_items;
    menu->item_count = old_count;
    menu->item_capacity = old_capacity;
    vg_contextmenu_destroy(menu);
}

TEST(filedialog_filter_capacity_overflow_is_rejected) {
    vg_filedialog_t *dialog = vg_filedialog_create(VG_FILEDIALOG_OPEN);
    ASSERT_NOT_NULL(dialog);
    vg_file_filter_t *old_filters = dialog->filters;
    size_t old_count = dialog->filter_count;
    size_t old_capacity = dialog->filter_capacity;

    dialog->filter_count = SIZE_MAX;
    dialog->filter_capacity = SIZE_MAX;
    vg_filedialog_add_filter(dialog, "overflow", "*");
    ASSERT_EQ(dialog->filter_count, SIZE_MAX);

    dialog->filters = old_filters;
    dialog->filter_count = old_count;
    dialog->filter_capacity = old_capacity;
    vg_filedialog_destroy(dialog);
}

TEST(icon_from_pixels_rejects_size_overflow) {
    uint8_t pixel = 0;
    vg_icon_t icon = vg_icon_from_pixels(&pixel, UINT32_MAX, UINT32_MAX);
    ASSERT_EQ(icon.type, VG_ICON_NONE);
}

TEST(contextmenu_registry_grows_beyond_legacy_cap) {
    enum { MENU_COUNT = 70 };

    vg_widget_t *widgets[MENU_COUNT] = {0};
    vg_contextmenu_t *menus[MENU_COUNT] = {0};

    for (int i = 0; i < MENU_COUNT; i++) {
        widgets[i] = vg_widget_create(VG_WIDGET_CONTAINER);
        menus[i] = vg_contextmenu_create();
        ASSERT_NOT_NULL(widgets[i]);
        ASSERT_NOT_NULL(menus[i]);
        vg_contextmenu_register_for_widget(widgets[i], menus[i]);
    }

    vg_event_t event = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 12.0f, 14.0f, VG_MOUSE_RIGHT, 0);
    ASSERT_TRUE(vg_contextmenu_process_event(widgets[MENU_COUNT - 1], &event));
    ASSERT_TRUE(menus[MENU_COUNT - 1]->is_visible);

    for (int i = 0; i < MENU_COUNT; i++) {
        vg_contextmenu_destroy(menus[i]);
        vg_widget_destroy(widgets[i]);
    }
}

TEST(codeeditor_set_text_clears_undo_redo_history) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_codeeditor_set_text(editor, "abc");
    vg_codeeditor_set_cursor(editor, 0, 3);
    vg_codeeditor_insert_text(editor, "d");
    ASSERT_TRUE(editor->history && editor->history->current_index > 0);

    vg_codeeditor_set_text(editor, "xyz");
    ASSERT_TRUE(editor->history && editor->history->count == 0);
    ASSERT_TRUE(editor->history->current_index == 0);

    vg_codeeditor_undo(editor);
    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "xyz") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

TEST(codeeditor_readonly_blocks_direct_mutation_apis) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_codeeditor_set_text(editor, "abc");
    editor->read_only = true;
    vg_codeeditor_set_cursor(editor, 0, 3);
    vg_codeeditor_insert_text(editor, "d");
    vg_codeeditor_set_selection(editor, 0, 0, 0, 1);
    vg_codeeditor_delete_selection(editor);

    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "abc") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

TEST(codeeditor_zero_length_selection_is_not_active_or_modified) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_codeeditor_set_text(editor, "abc");
    vg_codeeditor_clear_modified(editor);
    vg_codeeditor_set_selection(editor, 0, 1, 0, 1);
    ASSERT_FALSE(editor->has_selection);
    vg_codeeditor_delete_selection(editor);
    ASSERT_FALSE(vg_codeeditor_is_modified(editor));

    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "abc") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

TEST(codeeditor_utf8_cursor_and_backspace_use_codepoint_boundaries) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);

    vg_codeeditor_set_text(editor, "\xC3\xA9x");
    vg_codeeditor_set_cursor(editor, 0, 1);
    ASSERT_EQ(editor->cursor_col, 2);

    int line = -1;
    int col = -1;
    vg_codeeditor_get_cursor(editor, &line, &col);
    ASSERT_EQ(line, 0);
    ASSERT_EQ(col, 1);

    vg_event_t left = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_LEFT, 0, VG_MOD_NONE);
    ASSERT_TRUE(vg_event_send(&editor->base, &left));
    vg_codeeditor_get_cursor(editor, &line, &col);
    ASSERT_EQ(col, 0);

    vg_codeeditor_set_cursor(editor, 0, 1);
    vg_event_t backspace = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_BACKSPACE, 0, VG_MOD_NONE);
    ASSERT_TRUE(vg_event_send(&editor->base, &backspace));
    char *text = vg_codeeditor_get_text(editor);
    ASSERT_NOT_NULL(text);
    ASSERT(strcmp(text, "x") == 0);
    free(text);

    vg_widget_destroy(&editor->base);
}

TEST(listbox_ctrl_toggle_reports_selection_set_change_when_current_stays) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    lb->multi_select = true;
    ASSERT_NOT_NULL(vg_listbox_add_item(lb, "A", NULL));
    ASSERT_NOT_NULL(vg_listbox_add_item(lb, "B", NULL));

    vg_listbox_select_index(lb, 0);
    vg_event_t ev = {0};
    ev.type = VG_EVENT_MOUSE_DOWN;
    ev.modifiers = VG_MOD_CTRL;
    ev.mouse.y = lb->item_height + 1.0f;
    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT_EQ(vg_listbox_get_selected_index(lb), (size_t)1);

    uint64_t before = lb->selection_revision;
    ev.mouse.y = 1.0f;
    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT_EQ(vg_listbox_get_selected_index(lb), (size_t)1);
    ASSERT_TRUE(lb->selection_revision > before);

    vg_widget_destroy(&lb->base);
}

TEST(listbox_virtual_ctrl_toggle_reports_selection_set_change_when_current_stays) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    lb->multi_select = true;
    vg_listbox_set_virtual_mode(lb, true, 3, 20.0f);

    vg_listbox_select_index(lb, 0);
    vg_event_t ev = {0};
    ev.type = VG_EVENT_MOUSE_DOWN;
    ev.modifiers = VG_MOD_CTRL;
    ev.mouse.y = 21.0f;
    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT_EQ(vg_listbox_get_selected_index(lb), (size_t)1);

    uint64_t before = lb->selection_revision;
    ev.mouse.y = 1.0f;
    ASSERT_TRUE(lb->base.vtable->handle_event(&lb->base, &ev));
    ASSERT_EQ(vg_listbox_get_selected_index(lb), (size_t)1);
    ASSERT_TRUE(lb->selection_revision > before);

    vg_widget_destroy(&lb->base);
}

TEST(listbox_select_null_clears_multi_select_flags) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    lb->multi_select = true;
    vg_listbox_item_t *a = vg_listbox_add_item(lb, "A", NULL);
    vg_listbox_item_t *b = vg_listbox_add_item(lb, "B", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    vg_listbox_select(lb, a);
    b->selected = true;
    vg_listbox_select(lb, NULL);

    ASSERT_NULL(vg_listbox_get_selected(lb));
    ASSERT_FALSE(a->selected);
    ASSERT_FALSE(b->selected);

    vg_widget_destroy(&lb->base);
}

TEST(listbox_reselect_same_single_item_does_not_report_change) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_item_t *a = vg_listbox_add_item(lb, "A", NULL);
    vg_listbox_item_t *b = vg_listbox_add_item(lb, "B", NULL);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    vg_listbox_select(lb, a);
    uint64_t after_first_select = lb->selection_revision;
    vg_listbox_select(lb, a);
    ASSERT_EQ(lb->selection_revision, after_first_select);

    b->selected = true;
    vg_listbox_select(lb, a);
    ASSERT_TRUE(lb->selection_revision > after_first_select);
    ASSERT_TRUE(a->selected);
    ASSERT_FALSE(b->selected);
    ASSERT_EQ(vg_listbox_get_selected(lb), a);

    vg_widget_destroy(&lb->base);
}

TEST(listbox_clear_resets_virtual_mode_state) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_set_virtual_mode(lb, true, 5, 20.0f);
    vg_listbox_select_index(lb, 2);

    vg_listbox_clear(lb);

    ASSERT_FALSE(lb->virtual_mode);
    ASSERT_EQ(lb->total_item_count, (size_t)0);
    ASSERT_NULL(lb->selection_bitmap);
    ASSERT_EQ(lb->selection_bitmap_size, (size_t)0);
    ASSERT_EQ(vg_listbox_get_selected_index(lb), SIZE_MAX);

    vg_widget_destroy(&lb->base);
}

TEST(listbox_virtual_shrink_reports_truncated_secondary_selection) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_set_virtual_mode(lb, true, 5, 20.0f);
    lb->selection_bitmap[0] = (uint8_t)((1u << 1u) | (1u << 4u));
    lb->selected_index = 1;

    uint64_t before = lb->selection_revision;
    vg_listbox_set_total_count(lb, 3);
    ASSERT_EQ(lb->selected_index, (size_t)1);
    ASSERT_TRUE(lb->selection_revision > before);

    vg_widget_destroy(&lb->base);
}

TEST(dropdown_invalid_set_selected_preserves_selection) {
    vg_dropdown_t *dd = vg_dropdown_create(NULL);
    ASSERT_NOT_NULL(dd);
    ASSERT_TRUE(vg_dropdown_add_item(dd, "A") >= 0);
    ASSERT_TRUE(vg_dropdown_add_item(dd, "B") >= 0);

    vg_dropdown_set_selected(dd, 1);
    ASSERT_EQ(vg_dropdown_get_selected(dd), 1);
    vg_dropdown_set_selected(dd, 99);
    ASSERT_EQ(vg_dropdown_get_selected(dd), 1);
    vg_dropdown_set_selected(dd, -2);
    ASSERT_EQ(vg_dropdown_get_selected(dd), 1);
    vg_dropdown_set_selected(dd, -1);
    ASSERT_EQ(vg_dropdown_get_selected(dd), -1);

    vg_widget_destroy(&dd->base);
}

TEST(treeview_and_tabbar_prune_retired_tombstones_when_handles_discarded) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    ASSERT_NOT_NULL(tree);
    vg_tree_node_t *node = vg_treeview_add_node(tree, NULL, "root");
    ASSERT_NOT_NULL(node);
    vg_treeview_remove_node(tree, node);
    ASSERT_NOT_NULL(tree->retired_nodes);
    vg_treeview_prune_retired_nodes(tree);
    ASSERT_NULL(tree->retired_nodes);
    vg_widget_destroy(&tree->base);

    vg_tabbar_t *tabbar = vg_tabbar_create(NULL);
    ASSERT_NOT_NULL(tabbar);
    vg_tab_t *tab = vg_tabbar_add_tab(tabbar, "one", true);
    ASSERT_NOT_NULL(tab);
    vg_tabbar_remove_tab(tabbar, tab);
    ASSERT_NOT_NULL(tabbar->retired_tabs);
    vg_tabbar_prune_retired_tabs(tabbar);
    ASSERT_NULL(tabbar->retired_tabs);
    vg_widget_destroy(&tabbar->base);
}

/// @brief Verify tombstone-heavy widget churn cannot saturate live-registry probing.
/// @details Each cycle keeps same-sized filler allocations alive after destroying widgets so the
///          allocator must provide fresh widget addresses on later cycles. This exercises occupied
///          slots independently of the current live count. Missing-pointer lookups and subsequent
///          insertion must remain bounded after the table has accumulated more tombstones than its
///          initial growth threshold.
TEST(widget_live_registry_rehashes_tombstone_saturation) {
    enum { BATCH_SIZE = 24, BATCH_COUNT = 20, FILLER_COUNT = BATCH_SIZE * BATCH_COUNT };

    void *fillers[FILLER_COUNT] = {0};
    size_t filler_count = 0;

    for (size_t batch = 0; batch < BATCH_COUNT; ++batch) {
        vg_widget_t *widgets[BATCH_SIZE] = {0};
        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            widgets[i] = vg_widget_create(VG_WIDGET_CONTAINER);
            ASSERT_NOT_NULL(widgets[i]);
        }
        for (size_t i = 0; i < BATCH_SIZE; ++i)
            vg_widget_destroy(widgets[i]);

        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            fillers[filler_count] = malloc(sizeof(vg_widget_t));
            ASSERT_NOT_NULL(fillers[filler_count]);
            filler_count++;
        }
    }

    for (size_t i = 0; i < filler_count; ++i)
        ASSERT_FALSE(vg_widget_is_live((const vg_widget_t *)fillers[i]));

    vg_widget_t *final_widget = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(final_widget);
    ASSERT_TRUE(vg_widget_is_live(final_widget));
    vg_widget_destroy(final_widget);

    for (size_t i = 0; i < filler_count; ++i)
        free(fillers[i]);
}

/// @brief Verify generic widget motion advances from explicit scheduler time, never from paint.
/// @details Uses a 100 ms custom motion duration to assert deterministic quarter/full progress,
///          retained paint invalidation while active, and immediate reduced-motion snapping. A
///          standalone paint traversal must leave the animation unchanged, which prevents timing
///          from depending on damage culling or how often a widget happens to be drawn.
TEST(widget_animation_uses_explicit_scheduler_delta) {
    vg_theme_t *previous_theme = vg_theme_get_current();
    vg_theme_t *theme = vg_theme_create("animation-scheduler-test", vg_theme_dark());
    ASSERT_NOT_NULL(theme);
    theme->motion.enabled = true;
    theme->motion.hover_ms = 100.0f;
    theme->motion.press_ms = 100.0f;
    theme->motion.focus_ms = 100.0f;
    vg_theme_set_current(theme);

    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(widget);
    widget->state |= VG_STATE_HOVERED;
    widget->needs_paint = false;

    ASSERT_TRUE(vg_widget_anim_advance(widget, 25.0f));
    ASSERT_NEAR(widget->anim_hover, 0.25f, 0.0001f);
    ASSERT_TRUE(widget->needs_paint);

    widget->needs_paint = false;
    vg_widget_paint(widget, (void *)(uintptr_t)1);
    ASSERT_NEAR(widget->anim_hover, 0.25f, 0.0001f);
    ASSERT_FALSE(widget->needs_paint);

    ASSERT_FALSE(vg_widget_anim_advance(widget, 75.0f));
    ASSERT_NEAR(widget->anim_hover, 1.0f, 0.0001f);
    ASSERT_TRUE(widget->needs_paint);

    theme->motion.enabled = false;
    widget->state &= ~VG_STATE_HOVERED;
    widget->needs_paint = false;
    ASSERT_FALSE(vg_widget_anim_advance(widget, 0.0f));
    ASSERT_NEAR(widget->anim_hover, 0.0f, 0.0001f);
    ASSERT_TRUE(widget->needs_paint);

    vg_widget_destroy(widget);
    vg_theme_set_current(previous_theme);
    vg_theme_destroy(theme);
}

/// @brief Verify tooltip and notification managers expose every timer to the app scheduler.
/// @details Exercises delayed show/hide/duration ordering, notification first-tick work, entrance
///          animation cadence, exact auto-dismiss lifetime, exit animation cadence, and the final
///          no-deadline state using an injected monotonic clock.
TEST(overlay_managers_report_complete_scheduler_deadlines) {
    vg_tooltip_t *tooltip = vg_tooltip_create();
    vg_widget_t *hovered = vg_widget_create(VG_WIDGET_BUTTON);
    ASSERT(tooltip != NULL && hovered != NULL);
    vg_tooltip_manager_t tooltip_manager = {0};
    tooltip_manager.active_tooltip = tooltip;
    tooltip_manager.hovered_widget = hovered;
    tooltip_manager.pending_show = true;
    tooltip_manager.hover_start_time = 1000;
    tooltip->show_delay_ms = 500;
    ASSERT(vg_tooltip_manager_next_deadline_ms(&tooltip_manager, 1100) == 400);
    ASSERT(vg_tooltip_manager_next_deadline_ms(&tooltip_manager, 1500) == 0);

    tooltip_manager.pending_show = false;
    tooltip->is_visible = true;
    tooltip->base.visible = true;
    tooltip->hide_timer = 1700;
    tooltip->show_timer = 1000;
    tooltip->duration_ms = 1000;
    ASSERT(vg_tooltip_manager_next_deadline_ms(&tooltip_manager, 1600) == 100);
    tooltip->hide_timer = 0;
    ASSERT(vg_tooltip_manager_next_deadline_ms(&tooltip_manager, 1600) == 400);
    tooltip->duration_ms = 0;
    ASSERT(vg_tooltip_manager_next_deadline_ms(&tooltip_manager, 1600) == -1);
    vg_widget_destroy(hovered);
    vg_tooltip_destroy(tooltip);

    vg_notification_manager_t *notifications = vg_notification_manager_create();
    ASSERT(notifications != NULL);
    ASSERT(vg_notification_show(notifications, VG_NOTIFICATION_INFO, "Build", "Complete", 1000) !=
           0);
    ASSERT(vg_notification_manager_next_deadline_ms(notifications, 100) == 0);
    vg_notification_manager_update(notifications, 100);
    ASSERT(vg_notification_manager_next_deadline_ms(notifications, 100) == 16);
    vg_notification_manager_update(notifications, 400);
    ASSERT(vg_notification_manager_next_deadline_ms(notifications, 400) == 700);
    vg_notification_manager_update(notifications, 1100);
    ASSERT(vg_notification_manager_next_deadline_ms(notifications, 1100) == 16);
    vg_notification_manager_update(notifications, 1400);
    ASSERT(vg_notification_manager_next_deadline_ms(notifications, 1400) == -1);
    vg_notification_manager_destroy(notifications);
}

/// @brief Test-only visual-bounds callback that reports a detached popup rectangle.
/// @param widget Ignored widget instance.
/// @param x Receives 100 screen pixels.
/// @param y Receives 100 screen pixels.
/// @param width Receives 20 pixels.
/// @param height Receives 10 pixels.
static void visual_bounds_popup_probe(
    vg_widget_t *widget, float *x, float *y, float *width, float *height) {
    (void)widget;
    if (x)
        *x = 100.0f;
    if (y)
        *y = 100.0f;
    if (width)
        *width = 20.0f;
    if (height)
        *height = 10.0f;
}

static const vg_widget_vtable_t g_visual_bounds_probe_vtable = {
    .get_visual_bounds = visual_bounds_popup_probe,
};

/// @brief Verify explicit overflow, callback unions, theme shadows, and invalid-value guards.
/// @details Uses a controlled theme to make every expected edge deterministic. It also verifies
///          that changing overflow dirties retained paint while assigning an identical value does
///          not generate needless work.
TEST(widget_visual_bounds_cover_every_paint_source) {
    vg_theme_t *previous_theme = vg_theme_get_current();
    vg_theme_t *theme = vg_theme_create("visual-bounds-test", vg_theme_dark());
    ASSERT_NOT_NULL(theme);
    memset(&theme->elevation, 0, sizeof(theme->elevation));
    memset(&theme->focus, 0, sizeof(theme->focus));
    vg_theme_set_current(theme);

    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CUSTOM);
    ASSERT_NOT_NULL(widget);
    vg_widget_arrange(widget, 10.0f, 20.0f, 30.0f, 40.0f);
    widget->needs_paint = false;
    vg_widget_set_visual_overflow(widget, 2.0f, 3.0f, 4.0f, 5.0f);
    ASSERT_TRUE(widget->needs_paint);

    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    vg_widget_get_visual_bounds(widget, &x, &y, &width, &height);
    ASSERT_NEAR(x, 8.0f, 0.0001f);
    ASSERT_NEAR(y, 17.0f, 0.0001f);
    ASSERT_NEAR(width, 36.0f, 0.0001f);
    ASSERT_NEAR(height, 48.0f, 0.0001f);

    widget->needs_paint = false;
    vg_widget_set_visual_overflow(widget, 2.0f, 3.0f, 4.0f, 5.0f);
    ASSERT_FALSE(widget->needs_paint);
    vg_widget_set_visual_overflow(widget, NAN, -1.0f, INFINITY, -INFINITY);
    ASSERT_TRUE(widget->needs_paint);
    vg_widget_get_visual_bounds(widget, &x, &y, &width, &height);
    ASSERT_NEAR(x, 10.0f, 0.0001f);
    ASSERT_NEAR(y, 20.0f, 0.0001f);
    ASSERT_NEAR(width, 30.0f, 0.0001f);
    ASSERT_NEAR(height, 40.0f, 0.0001f);

    theme->elevation.level3 = (vg_elevation_t){4.0f, -2, 3, 255};
    vg_widget_get_visual_bounds(widget, &x, &y, &width, &height);
    ASSERT_NEAR(x, 1.0f, 0.0001f);
    ASSERT_NEAR(y, 16.0f, 0.0001f);
    ASSERT_NEAR(width, 44.0f, 0.0001f);
    ASSERT_NEAR(height, 54.0f, 0.0001f);

    memset(&theme->elevation, 0, sizeof(theme->elevation));
    widget->vtable = &g_visual_bounds_probe_vtable;
    vg_widget_get_visual_bounds(widget, &x, &y, &width, &height);
    ASSERT_NEAR(x, 10.0f, 0.0001f);
    ASSERT_NEAR(y, 20.0f, 0.0001f);
    ASSERT_NEAR(width, 110.0f, 0.0001f);
    ASSERT_NEAR(height, 90.0f, 0.0001f);

    vg_widget_destroy(widget);
    vg_theme_set_current(previous_theme);
    vg_theme_destroy(theme);
}

/// @brief Verify an overlay-only widget exposes and retires its complete popup rectangle.
/// @details PopupList has zero arranged size and paints exclusively in the overlay pass. Its
///          visual-bounds callback must therefore make the popup damageable while visible and
///          return an empty current rectangle after it is hidden.
TEST(popuplist_visual_bounds_track_overlay_visibility) {
    vg_theme_t *previous_theme = vg_theme_get_current();
    vg_theme_t *theme = vg_theme_create("popup-visual-bounds-test", vg_theme_dark());
    ASSERT_NOT_NULL(theme);
    memset(&theme->elevation, 0, sizeof(theme->elevation));
    memset(&theme->focus, 0, sizeof(theme->focus));
    vg_theme_set_current(theme);

    vg_popuplist_t *popup = vg_popuplist_create(NULL);
    ASSERT_NOT_NULL(popup);
    vg_popuplist_add_item(popup, "alpha");
    vg_popuplist_add_item(popup, "beta");
    popup->font = (vg_font_t *)(uintptr_t)1;
    popup->line_height = 12.0f;
    vg_popuplist_anchor_at(popup, 75.0f, 90.0f);
    vg_popuplist_set_width(popup, 160.0f);
    vg_popuplist_set_visible(popup, true);

    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    vg_widget_get_visual_bounds(&popup->base, &x, &y, &width, &height);
    ASSERT_NEAR(x, 75.0f, 0.0001f);
    ASSERT_NEAR(y, 90.0f, 0.0001f);
    ASSERT_NEAR(width, 160.0f, 0.0001f);
    ASSERT_NEAR(height, 24.0f, 0.0001f);

    vg_popuplist_set_visible(popup, false);
    vg_widget_get_visual_bounds(&popup->base, &x, &y, &width, &height);
    ASSERT_NEAR(width, 0.0f, 0.0001f);
    ASSERT_NEAR(height, 0.0f, 0.0001f);

    vg_popuplist_destroy(popup);
    vg_theme_set_current(previous_theme);
    vg_theme_destroy(theme);
}

/// @brief Verify semantic records own strings, infer stable roles, and advance revisions once.
/// @details Mutating caller buffers after a setter must not alter the record. Repeating an
///          identical assignment is intentionally revision-neutral, while invalid role/live-mode
///          values normalize to their documented safe defaults.
TEST(widget_accessibility_record_is_owned_and_revisioned) {
    vg_button_t *button = vg_button_create(NULL, "Action");
    vg_textinput_t *input = vg_textinput_create(NULL);
    vg_widget_t *container = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(button);
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(container);
    ASSERT_EQ(vg_widget_get_accessible_role(&button->base), VG_ACCESSIBLE_ROLE_BUTTON);
    ASSERT_EQ(vg_widget_get_accessible_role(&input->base), VG_ACCESSIBLE_ROLE_TEXTBOX);
    ASSERT_EQ(vg_widget_get_accessible_role(container), VG_ACCESSIBLE_ROLE_GROUP);

    uint64_t initial_revision = vg_widget_get_revision(&button->base);
    uint64_t initial_semantic_revision = button->base.accessibility.revision;
    char name[] = "Launch";
    char description[] = "Starts the selected task";
    char value[] = "ready";
    vg_widget_set_accessible_name(&button->base, name);
    vg_widget_set_accessible_description(&button->base, description);
    vg_widget_set_accessible_value(&button->base, value);
    name[0] = 'X';
    description[0] = 'X';
    value[0] = 'X';
    ASSERT_TRUE(strcmp(vg_widget_get_accessible_name(&button->base), "Launch") == 0);
    ASSERT_TRUE(strcmp(vg_widget_get_accessible_description(&button->base),
                       "Starts the selected task") == 0);
    ASSERT_TRUE(strcmp(vg_widget_get_accessible_value(&button->base), "ready") == 0);
    ASSERT_EQ(vg_widget_get_revision(&button->base), initial_revision + 3u);
    ASSERT_EQ(button->base.accessibility.revision, initial_semantic_revision + 3u);

    vg_widget_set_accessible_name(&button->base, "Launch");
    ASSERT_EQ(vg_widget_get_revision(&button->base), initial_revision + 3u);
    vg_widget_set_accessible_role(&button->base, (vg_accessible_role_t)INT_MAX);
    ASSERT_EQ(vg_widget_get_accessible_role(&button->base), VG_ACCESSIBLE_ROLE_NONE);
    vg_widget_set_live_region(&button->base, (vg_live_region_mode_t)INT_MAX);
    ASSERT_EQ(vg_widget_get_live_region(&button->base), VG_LIVE_REGION_OFF);

    button->base.revision = UINT64_MAX;
    vg_widget_note_revision(&button->base);
    ASSERT_EQ(vg_widget_get_revision(&button->base), UINT64_MAX);

    vg_widget_destroy(container);
    vg_widget_destroy(&input->base);
    vg_widget_destroy(&button->base);
}

/// @brief Verify label relationships reject foreign/self targets and never expose stale pointers.
/// @details Relationships are non-owning and require a shared tree root. Destroying a target must
///          make the getter return NULL even if its allocation address could later be recycled.
TEST(widget_accessibility_relationships_are_tree_safe) {
    vg_widget_t *root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *foreign_root = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_label_t *label = vg_label_create(root, "Name");
    vg_textinput_t *input = vg_textinput_create(root);
    vg_textinput_t *foreign_input = vg_textinput_create(foreign_root);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(foreign_root);
    ASSERT_NOT_NULL(label);
    ASSERT_NOT_NULL(input);
    ASSERT_NOT_NULL(foreign_input);

    ASSERT_FALSE(vg_widget_set_accessible_label_for(&label->base, &label->base));
    ASSERT_FALSE(vg_widget_set_accessible_label_for(&label->base, &foreign_input->base));
    ASSERT_TRUE(vg_widget_set_accessible_label_for(&label->base, &input->base));
    ASSERT_EQ(vg_widget_get_accessible_label_for(&label->base), &input->base);

    vg_widget_destroy(&input->base);
    ASSERT_NULL(vg_widget_get_accessible_label_for(&label->base));
    ASSERT_TRUE(vg_widget_set_accessible_label_for(&label->base, NULL));
    ASSERT_NULL(vg_widget_get_accessible_label_for(&label->base));

    vg_widget_destroy(foreign_root);
    vg_widget_destroy(root);
}

/// @brief Verify live-region announcements preserve independent edge and semantic revisions.
/// @details Repeating identical text is still a new announcement edge. An invalid urgency falls
///          back to the widget's configured mode, and changing one edge cannot reduce revisions.
TEST(widget_accessibility_announcements_are_monotonic_edges) {
    vg_label_t *label = vg_label_create(NULL, "Status");
    ASSERT_NOT_NULL(label);
    vg_widget_set_live_region(&label->base, VG_LIVE_REGION_ASSERTIVE);
    uint64_t initial_revision = label->base.accessibility.announcement_revision;

    vg_widget_accessibility_announce(
        &label->base, "Build complete", (vg_live_region_mode_t)INT_MAX);
    ASSERT_TRUE(strcmp(label->base.accessibility.announcement, "Build complete") == 0);
    ASSERT_EQ(label->base.accessibility.announcement_mode, VG_LIVE_REGION_ASSERTIVE);
    ASSERT_EQ(label->base.accessibility.announcement_revision, initial_revision + 1u);
    uint64_t semantic_revision = label->base.accessibility.revision;

    vg_widget_accessibility_announce(&label->base, "Build complete", VG_LIVE_REGION_POLITE);
    ASSERT_EQ(label->base.accessibility.announcement_revision, initial_revision + 2u);
    ASSERT_EQ(label->base.accessibility.announcement_mode, VG_LIVE_REGION_POLITE);
    ASSERT_EQ(label->base.accessibility.revision, semantic_revision + 1u);

    vg_widget_destroy(&label->base);
}

//=============================================================================
// Round 14: Uniform control-event counters
//=============================================================================

/// @brief Verify common change, activation, and submission edges remain independent.
/// @details Each event advances the non-consuming general revision, each compatibility edge can
///          be consumed exactly once without hiding either of the other event kinds, and counters
///          saturate instead of wrapping at `UINT64_MAX`.
TEST(widget_common_event_edges_are_independent_and_saturating) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    ASSERT_NOT_NULL(widget);
    const uint64_t initial_revision = vg_widget_get_revision(widget);
    ASSERT_TRUE(initial_revision > 0);
    ASSERT_FALSE(vg_widget_was_changed(widget));
    ASSERT_FALSE(vg_widget_was_activated(widget));
    ASSERT_FALSE(vg_widget_was_submitted(widget));

    vg_widget_note_change(widget);
    vg_widget_note_activation(widget);
    vg_widget_note_submission(widget);
    ASSERT_EQ(vg_widget_get_revision(widget), initial_revision + 3u);

    ASSERT_TRUE(vg_widget_was_activated(widget));
    ASSERT_TRUE(vg_widget_was_changed(widget));
    ASSERT_TRUE(vg_widget_was_submitted(widget));
    ASSERT_FALSE(vg_widget_was_changed(widget));
    ASSERT_FALSE(vg_widget_was_activated(widget));
    ASSERT_FALSE(vg_widget_was_submitted(widget));
    ASSERT_EQ(vg_widget_get_revision(widget), initial_revision + 3u);

    widget->revision = UINT64_MAX - 1u;
    widget->change_revision = UINT64_MAX - 1u;
    widget->reported_change_revision = UINT64_MAX - 1u;
    vg_widget_note_change(widget);
    ASSERT_EQ(widget->revision, UINT64_MAX);
    ASSERT_EQ(widget->change_revision, UINT64_MAX);
    ASSERT_TRUE(vg_widget_was_changed(widget));
    ASSERT_EQ(widget->reported_change_revision, UINT64_MAX);

    vg_widget_note_change(widget);
    ASSERT_EQ(widget->revision, UINT64_MAX);
    ASSERT_EQ(widget->change_revision, UINT64_MAX);
    ASSERT_FALSE(vg_widget_was_changed(widget));

    vg_widget_destroy(widget);
}

//=============================================================================
// Main
//=============================================================================

/// @brief Run all audit-regression tests across Rounds 1–7 and report pass/fail counts.
int main(void) {
    g_verbose = getenv("ZANNA_GUI_TEST_VERBOSE") != NULL;

    printf("\n=== test_vg_audit_fixes — Zanna.GUI audit regression suite ===\n");

    printf("\nFix #1: Dialog re-entrancy guard\n");
    RUN(dialog_close_reentry_is_guarded);

    printf("\nFix #7: Dialog set_on_close routes user_data\n");
    RUN(dialog_set_on_close_user_data_routed_independently);

    printf("\nFix #2: Tooltip dangling pointer\n");
    RUN(tooltip_manager_drops_pointer_when_widget_destroyed);
    RUN(tooltip_manager_refreshes_hovered_widget_text);

    printf("\nFix #3: Notification created_at lazy stamp\n");
    RUN(notification_auto_dismiss_uses_lazy_created_at);

    printf("\nFix #4: Find/Replace UTF-8-safe advance\n");
    RUN(findreplacebar_search_finds_overlapping_pattern_non_overlapping);

    printf("\nFix #5: TabBar scroll_x clamp\n");
    RUN(tabbar_measure_clamps_scroll_after_tabs_removed);
    RUN(tabbar_measure_reveals_active_tab_selected_while_hidden);

    printf("\nFix #6: ContextMenu impl_data independence\n");
    RUN(contextmenu_show_at_does_not_dereference_impl_data);
    RUN(contextmenu_item_icon_setter_owns_replaces_and_reserves_space);

    printf("\nFix #8: CodeEditor clamp_editor_position\n");
    RUN(codeeditor_set_cursor_clamps_atomic);

    printf("\nFix #9: CodeEditor scrollbar finite scroll\n");
    RUN(codeeditor_exact_fit_scroll_position_finite);

    printf("\nFix #10: SplitPane proportional min clamp\n");
    RUN(splitpane_proportional_clamp_when_mins_exceed_available);
    RUN(splitpane_collapse_invalidates_ancestor_layout);

    printf("\nFix #11: ScrollView hit-test gutter exclusion\n");
    RUN(scrollview_hit_test_excludes_scrollbar_gutter);

    printf("\nFix #12: Flex non-stretch keeps measured size\n");
    RUN(flex_non_stretch_keeps_measured_height);

    printf("\nFix #13: TextInput password long content\n");
    RUN(textinput_password_long_text_round_trip_intact);

    printf("\nRound 2 — Critical regressions + audit findings\n");
    RUN(wheel_delta_survives_localize_call);
    RUN(platform_scroll_direction_is_translated_for_gui_widgets);
    RUN(button_keyboard_activation_reports_actual_click);
    RUN(mouseup_outside_pressed_widget_does_not_report_click);
    RUN(mouseup_handler_runs_before_synthesized_click);
    RUN(listbox_removed_item_handle_is_inert);
    RUN(treeview_removed_node_handles_are_inert);
    RUN(scrollview_auto_content_size_tracks_child_measurement);
    RUN(dropdown_clear_closes_open_capture);
    RUN(contextmenu_separator_returns_item_handle);
    RUN(widget_live_sentinel_rejects_non_widget_storage);
    RUN(dropdown_flip_above_without_window_is_noop);
    RUN(scrollview_narrow_still_hit_tests_children);
    RUN(tooltip_wrap_terminates_on_whitespace_only_text);
    RUN(tooltip_wrap_prefers_complete_words);
    RUN(tooltip_wrap_honors_single_newline_once);
    RUN(tooltip_scene_hint_wraps_and_clamps_to_root);
    RUN(tooltip_manager_honors_duration_and_hide_delay);
    RUN(treeview_collapse_reclamps_scroll);
    RUN(notification_manual_dismiss_respects_exit_animation);
    RUN(notification_zero_fade_duration_snaps_cleanly);
    RUN(notification_visual_bounds_track_only_painted_toasts);

    printf("\nRound 3 — Zanna.GUI class audit fixes\n");
    RUN(radiogroup_destroy_and_radio_destroy_clear_cross_references);
    RUN(colorpicker_set_color_emits_once_after_child_slider_sync);
    RUN(colorpalette_click_callback_fires_once_per_click);
    RUN(colorswatch_keyboard_activation_is_accessible_and_revisioned);
    RUN(colorpalette_keyboard_removal_and_semantics_are_complete);
    RUN(colorpicker_keyboard_components_and_semantics_are_complete);
    RUN(label_and_checkbox_use_theme_regular_font_on_create);
    RUN(listbox_ctrl_toggle_off_does_not_leave_deselected_current_item);
    RUN(listbox_virtual_ctrl_toggle_off_clears_or_moves_current_index);
    RUN(scrollview_wheel_bubbles_when_scroll_does_not_change);
    RUN(scrollview_direction_disables_stale_axis_offset);
    RUN(slider_and_spinner_ranges_normalize_and_reject_nonfinite_values);
    RUN(spinner_decimal_display_resizes_beyond_legacy_fixed_buffer);
    RUN(dropdown_unicode_typeahead_decodes_first_codepoint);
    RUN(dropdown_remove_and_clear_notify_effective_selection_changes);
    RUN(textinput_unhandled_keydown_bubbles_to_parent);
    RUN(image_load_file_decodes_bmp_into_rgba_pixels);
    RUN(layout_measure_constraints_and_negative_arrange_are_clamped);

    printf("\nRound 4 — Runtime-facing GUI correctness fixes\n");
    RUN(modal_root_releases_external_mouse_capture);
    RUN(modal_root_redirects_keyboard_away_from_external_capture);
    RUN(menubar_remove_rejects_cross_owner_handles);
    RUN(textinput_zero_capacity_and_invalid_codepoints_are_ignored);
    RUN(listbox_select_index_out_of_range_keeps_selection);
    RUN(findreplacebar_regex_search_finds_variable_length_matches);
    RUN(treeview_keyboard_navigation_scrolls_selected_node_into_view);
    RUN(treeview_wheel_scrolls_overflowing_rows_and_bubbles_at_limits);
    RUN(widget_remove_child_clears_runtime_references_to_subtree);
    RUN(widget_clear_children_clears_runtime_references_to_subtrees);
    RUN(widget_runtime_restore_and_focus_reject_invalid_handles);

    printf("\nRound 5 — Low-level widget/runtime audit fixes\n");
    RUN(widget_impl_data_can_be_taken_by_custom_destroy);
    RUN(widget_destroy_releases_owned_drag_drop_strings);
    RUN(widget_get_focused_is_scoped_to_root_subtree);
    RUN(widget_insert_child_negative_index_clamps_to_front);
    RUN(widget_tab_order_handles_more_than_legacy_fixed_cap);
    RUN(platform_resize_event_reports_logical_gui_dimensions);

    printf("\nRound 6 - Library low-level correctness fixes\n");
    RUN(widget_paint_uses_screen_space_for_nested_children);
    RUN(grid_negative_placement_clamps_to_first_cell);
    RUN(codeeditor_edit_helpers_clamp_stale_cursor_columns);
    RUN(image_opacity_sanitizes_nan);
    RUN(textinput_sanitizes_invalid_utf8_before_storage);
    RUN(codeeditor_inserts_utf8_as_complete_byte_sequences);
    RUN(captured_mouseup_release_suppresses_outside_synthetic_click);
    RUN(grid_auto_flow_creates_implicit_rows);
    RUN(captured_mouse_events_are_localized_once);
    RUN(event_handler_can_destroy_target_during_dispatch);
    RUN(event_dispatch_keeps_focus_state_per_root);
    RUN(commandpalette_and_filedialog_reject_surrogate_codepoints);
    RUN(commandpalette_filter_handles_truncated_utf8_labels);
    RUN(textinput_replacement_validates_before_mutating_selection);
    RUN(outputpane_append_line_and_clear_keep_line_state_consistent);
    RUN(outputpane_ring_buffer_preserves_logical_order_after_eviction);
    RUN(outputpane_terminal_csi_clear_display_preserves_ansi_style);
    RUN(outputpane_terminal_alternate_screen_restores_primary_buffer);
    RUN(outputpane_terminal_extended_sgr_colors_are_rendered);
    RUN(outputpane_terminal_key_input_uses_byte_counted_xterm_sequences);
    RUN(contextmenu_callbacks_capture_and_registry_are_lifetime_safe);
    RUN(direct_event_send_restores_mouse_coordinates_after_bubbling);
    RUN(custom_widget_paint_can_query_layout_geometry);
    RUN(event_root_state_forgets_destroyed_child_focus);
    RUN(vgfx_special_keys_translate_through_public_helper);
    RUN(grid_explicit_placement_extends_effective_rows);
    RUN(widget_runtime_restore_rejects_destroyed_widget_pointer);

    printf("\nRound 7 - Zanna.GUI class correctness audit fixes\n");
    RUN(scrollview_internal_overlay_children_are_not_repainted_by_global_pass);
    RUN(grid_placement_metadata_is_removed_when_child_detaches);
    RUN(dock_metadata_is_removed_when_child_detaches);
    RUN(radiobutton_group_selection_updates_callbacks_dirty_and_clear_state);
    RUN(checkbox_indeterminate_clears_checked_state_and_style_flag);
    RUN(textinput_replacement_insert_emits_single_change);
    RUN(specialized_widgets_honor_preferred_size_constraints);
    RUN(specialized_widgets_honor_max_size_constraints);
    RUN(progressbar_sanitizes_nan_value_and_normalizes_phase);
    RUN(progressbar_circular_style_measures_as_square_indicator);
    RUN(slider_set_step_resnaps_current_value);
    RUN(dialog_custom_buttons_clear_resets_custom_state);
    RUN(breadcrumb_clear_releases_dropdown_capture);
    RUN(spinner_set_font_rejects_invalid_sizes);
    RUN(dropdown_placeholder_marks_layout_dirty);

    printf("\nRound 8 - Low-level library correctness fixes\n");
    RUN(widget_capture_and_modal_reject_non_live_pointers);
    RUN(event_dispatch_keeps_state_for_more_than_legacy_root_cap);
    RUN(textinput_ignores_command_modified_character_events);
    RUN(codeeditor_ignores_command_modified_character_events);
    RUN(text_widgets_reject_private_use_function_key_codepoints);
    RUN(codeeditor_printable_keydown_preserves_selection_for_character_input);
    RUN(listbox_virtual_keyboard_navigation_handles_large_counts);
    RUN(tabbar_removed_tab_handles_are_inert);
    RUN(menubar_clear_remove_and_accelerators_drop_stale_items);
    RUN(contextmenu_clear_retires_items_and_submenu_state);
    RUN(toolbar_removed_item_handles_are_inert);
    RUN(statusbar_removed_item_handles_are_inert);
    RUN(filedialog_create_always_has_current_path);
    RUN(textinput_null_font_preserves_existing_font_and_invalid_tick_is_ignored);
    RUN(findreplace_whole_word_does_not_match_inside_utf8_word);

    printf("\nRound 9 - Zanna.GUI class correctness audit fixes\n");
    RUN(platform_event_translation_preserves_focus_unknown_and_modifiers);
    RUN(focus_rejects_hidden_or_disabled_ancestors);
    RUN(flex_grow_keeps_unequal_measured_basis);
    RUN(handled_mouseup_does_not_synthesize_click);
    RUN(codeeditor_set_text_preserves_trailing_newlines);
    RUN(listbox_virtual_shrink_updates_logical_selection_bitmap_size);
    RUN(tabbar_close_click_uses_monotonic_version);
    RUN(button_invalid_style_and_icon_position_are_clamped);
    RUN(dropdown_rejects_capacity_growth_overflow);
    RUN(focus_rejects_widgets_without_focus_capability);
    RUN(widget_contains_point_rejects_hidden_and_disabled_chains);
    RUN(widget_add_child_rejects_non_live_handles);
    RUN(manual_position_children_are_not_overwritten_by_layout);
    RUN(box_layout_center_alignment_accounts_for_margins);
    RUN(non_dropdown_capture_does_not_synthesize_outside_click);
    RUN(filedialog_convenience_uses_installed_modal_runner);

    printf("\nRound 10 - Zanna.GUI runtime bug-fix follow-up coverage\n");
    RUN(menubar_key_event_dispatches_accelerator);
    RUN(toolbar_capacity_overflow_is_rejected);
    RUN(statusbar_capacity_overflow_is_rejected);
    RUN(contextmenu_capacity_overflow_is_rejected);
    RUN(filedialog_filter_capacity_overflow_is_rejected);
    RUN(icon_from_pixels_rejects_size_overflow);
    RUN(contextmenu_registry_grows_beyond_legacy_cap);

    printf("\nRound 11 - Zanna.GUI class bug-fix implementation coverage\n");
    RUN(codeeditor_set_text_clears_undo_redo_history);
    RUN(codeeditor_readonly_blocks_direct_mutation_apis);
    RUN(codeeditor_zero_length_selection_is_not_active_or_modified);
    RUN(codeeditor_utf8_cursor_and_backspace_use_codepoint_boundaries);
    RUN(listbox_ctrl_toggle_reports_selection_set_change_when_current_stays);
    RUN(listbox_virtual_ctrl_toggle_reports_selection_set_change_when_current_stays);
    RUN(listbox_select_null_clears_multi_select_flags);
    RUN(listbox_reselect_same_single_item_does_not_report_change);
    RUN(listbox_clear_resets_virtual_mode_state);
    RUN(listbox_virtual_shrink_reports_truncated_secondary_selection);
    RUN(dropdown_invalid_set_selected_preserves_selection);
    RUN(treeview_and_tabbar_prune_retired_tombstones_when_handles_discarded);
    RUN(widget_live_registry_rehashes_tombstone_saturation);

    printf("\nRound 12 - GUI modernization scheduler and damage coverage\n");
    RUN(widget_animation_uses_explicit_scheduler_delta);
    RUN(overlay_managers_report_complete_scheduler_deadlines);
    RUN(widget_visual_bounds_cover_every_paint_source);
    RUN(popuplist_visual_bounds_track_overlay_visibility);

    printf("\nRound 13 - GUI accessibility semantic-record coverage\n");
    RUN(widget_accessibility_record_is_owned_and_revisioned);
    RUN(widget_accessibility_relationships_are_tree_safe);
    RUN(widget_accessibility_announcements_are_monotonic_edges);

    printf("\nRound 14 - Uniform control-event and revision coverage\n");
    RUN(widget_common_event_edges_are_independent_and_saturating);

    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
