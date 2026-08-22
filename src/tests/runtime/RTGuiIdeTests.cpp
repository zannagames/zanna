//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/tests/runtime/RTGuiIdeTests.cpp
// Purpose: Tests for GUI IDE automation, virtualization, and accessibility helpers.
// Key invariants:
//   - Virtual model IDs remain unique and hash-addressable at large row counts.
//   - Bound ListBox/TreeView controls materialize viewport slices and sever raw
//     pointers safely regardless of endpoint destruction order.
//   - Duplicate-ID traps leave the prior model structure unchanged.
// Ownership/Lifetime:
//   - Runtime objects created in this test are released when lifetime ordering
//     is itself under test; process teardown reclaims the remaining fixtures.
// Links: src/runtime/graphics/gui/rt_gui_ide.cpp,
//        src/lib/gui/src/widgets/vg_listbox.c,
//        src/lib/gui/src/widgets/vg_treeview.c
//
//===----------------------------------------------------------------------===//

#include "rt_gui_ide.h"

#include "rt_gui.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static bool g_expect_trap = false;
static std::string g_trap_message;

extern "C" void vm_trap(const char *msg) {
    if (g_expect_trap) {
        g_trap_message = msg ? msg : "";
        return;
    }
    rt_abort(msg);
}

template <typename Callback> static void expect_trap(Callback callback, const char *message) {
    g_expect_trap = true;
    g_trap_message.clear();
    callback();
    g_expect_trap = false;
    assert(g_trap_message == message);
}

static std::string take(rt_string value) {
    std::string out(rt_string_cstr(value), (size_t)rt_str_len(value));
    rt_string_unref(value);
    return out;
}

static int64_t visible_count(void *tree) {
    void *rows = rt_virtual_tree_visible_rows(tree);
    int64_t count = rt_seq_len(rows);
    if (rows && rt_obj_release_check0(rows))
        rt_obj_free(rows);
    return count;
}

int main() {
    void *h = rt_gui_test_harness_new();
    rt_gui_test_harness_register_widget(h,
                                        rt_const_cstr("editor"),
                                        rt_const_cstr("CodeEditor"),
                                        rt_const_cstr("Editor"),
                                        10,
                                        10,
                                        300,
                                        200);
    rt_gui_test_harness_register_widget(h,
                                        rt_const_cstr("results"),
                                        rt_const_cstr("List"),
                                        rt_const_cstr("Search Results"),
                                        10,
                                        220,
                                        300,
                                        120);
    void *found = rt_gui_test_harness_find_by_id(h, rt_const_cstr("editor"));
    assert(rt_map_get_bool(found, rt_const_cstr("found")) == 1);
    void *foundOpt = rt_gui_test_harness_find_by_id_option(h, rt_const_cstr("editor"));
    assert(rt_option_is_some(foundOpt) == 1);
    assert(rt_map_get_bool(rt_option_unwrap(foundOpt), rt_const_cstr("found")) == 1);
    if (foundOpt && rt_obj_release_check0(foundOpt))
        rt_obj_free(foundOpt);
    void *missingOpt = rt_gui_test_harness_find_by_id_option(h, rt_const_cstr("missing"));
    assert(rt_option_is_none(missingOpt) == 1);
    if (missingOpt && rt_obj_release_check0(missingOpt))
        rt_obj_free(missingOpt);
    void *nameOpt = rt_gui_test_harness_find_by_name_option(h, rt_const_cstr("Search Results"));
    assert(rt_option_is_some(nameOpt) == 1);
    if (nameOpt && rt_obj_release_check0(nameOpt))
        rt_obj_free(nameOpt);
    void *typeOpt = rt_gui_test_harness_find_by_type_option(h, rt_const_cstr("List"));
    assert(rt_option_is_some(typeOpt) == 1);
    if (typeOpt && rt_obj_release_check0(typeOpt))
        rt_obj_free(typeOpt);
    rt_gui_test_harness_send_mouse(h, rt_const_cstr("down"), 20, 20, 1);
    assert(take(rt_gui_test_harness_get_focus(h)) == "editor");
    assert(rt_gui_test_harness_tick(h, 3) == 3);
    void *snapshot = rt_gui_test_harness_capture_region(h, 0, 0, 64, 64);
    assert(rt_gui_test_harness_assert_nonblank(snapshot) == 1);
    assert(rt_seq_len(rt_gui_test_harness_focus_order(h)) == 2);

    void *list = rt_virtual_list_new(10000, 20, 100);
    rt_virtual_list_set_row_id(list, 4242, rt_const_cstr("file://target.zia"));
    void *range = rt_virtual_list_visible_range(list, 40000);
    assert(rt_map_get_int(range, rt_const_cstr("count")) <= 9);
    rt_virtual_list_select_id(list, rt_const_cstr("file://target.zia"));
    assert(rt_virtual_list_get_selected_index(list) == 4242);
    rt_virtual_list_set_row_text(list, 4242, rt_const_cstr("Target source"));

    void *uniqueList = rt_virtual_list_new(3, 20, 100);
    rt_virtual_list_set_row_id(uniqueList, 0, rt_const_cstr("alpha"));
    expect_trap([&] { rt_virtual_list_set_row_id(uniqueList, 1, rt_const_cstr("alpha")); },
                "GUI model ID must be unique: alpha");
    assert(rt_virtual_list_get_selected_index(uniqueList) == -1);
    expect_trap([&] { rt_virtual_list_set_row_id(uniqueList, 0, rt_const_cstr("2")); },
                "GUI model ID must be unique: 2");

    // Growing a list cannot introduce a new implicit decimal ID that aliases an existing
    // explicit ID; rejection leaves the old logical count intact.
    void *growList = rt_virtual_list_new(2, 20, 100);
    rt_virtual_list_set_row_id(growList, 0, rt_const_cstr("2"));
    expect_trap([&] { rt_virtual_list_set_count(growList, 3); }, "GUI model ID must be unique: 2");
    void *growRange = rt_virtual_list_visible_range(growList, INT64_MAX);
    assert(rt_map_get_int(growRange, rt_const_cstr("end")) <= 2);

    // Real ListBox binding: selection synchronizes in both directions and visible range queries
    // do not invoke or materialize the full 10k-row model.
    void *boundList = rt_listbox_new(nullptr);
    assert(rt_virtual_list_bind(list, boundList) == 1);
    assert(rt_listbox_get_visible_first(boundList) == 0);
    assert(rt_listbox_get_visible_count(boundList) <= 2);
    rt_virtual_list_set_row_text(list, 9000, rt_const_cstr("Far target"));
    rt_listbox_select_index(boundList, 9000);
    assert(rt_virtual_list_get_selected_index(list) == 9000);
    assert(take(rt_listbox_get_selected_text(boundList)) == "Far target");
    rt_virtual_list_unbind(list);
    assert(rt_listbox_get_visible_count(boundList) == 0);
    assert(rt_listbox_set_virtual_model(boundList, list) == 1);
    rt_listbox_clear_virtual_model(boundList);

    void *tree = rt_virtual_tree_new();
    rt_virtual_tree_add_node(tree, rt_const_cstr(""), rt_const_cstr("src"), rt_const_cstr("src"));
    rt_virtual_tree_add_node(
        tree, rt_const_cstr("src"), rt_const_cstr("main.zia"), rt_const_cstr("main.zia"));
    void *expand = rt_virtual_tree_expand(tree, rt_const_cstr("src"));
    assert(rt_map_get_bool(expand, rt_const_cstr("expanded")) == 1);
    assert(visible_count(tree) == 2);
    rt_virtual_tree_select_id(tree, rt_const_cstr("main.zia"));
    assert(take(rt_virtual_tree_get_selected_id(tree)) == "main.zia");
    rt_virtual_tree_refresh_subtree(tree, rt_const_cstr("src"));
    assert(take(rt_virtual_tree_get_selected_id(tree)).empty());
    assert(visible_count(tree) == 1);
    expand = rt_virtual_tree_expand(tree, rt_const_cstr("src"));
    assert(rt_map_get_bool(expand, rt_const_cstr("needsPopulate")) == 1);

    void *tree2 = rt_virtual_tree_new();
    rt_virtual_tree_add_node(
        tree2, rt_const_cstr(""), rt_const_cstr("root"), rt_const_cstr("Root"));
    rt_virtual_tree_add_node(tree2, rt_const_cstr("root"), rt_const_cstr("a"), rt_const_cstr("A"));
    rt_virtual_tree_add_node(tree2, rt_const_cstr("root"), rt_const_cstr("b"), rt_const_cstr("B"));
    expand = rt_virtual_tree_expand(tree2, rt_const_cstr("root"));
    assert(rt_map_get_bool(expand, rt_const_cstr("expanded")) == 1);
    assert(visible_count(tree2) == 3);
    assert(rt_virtual_tree_move_node(tree2, rt_const_cstr("a"), rt_const_cstr("b")) == 1);
    assert(rt_virtual_tree_set_node_text(tree2, rt_const_cstr("a"), rt_const_cstr("A moved")) == 1);
    assert(visible_count(tree2) == 2);
    expand = rt_virtual_tree_expand(tree2, rt_const_cstr("b"));
    assert(rt_map_get_bool(expand, rt_const_cstr("expanded")) == 1);
    assert(visible_count(tree2) == 3);
    assert(rt_virtual_tree_move_node(tree2, rt_const_cstr("root"), rt_const_cstr("a")) == 0);
    assert(visible_count(tree2) == 3);
    expect_trap(
        [&] {
            rt_virtual_tree_add_node(
                tree2, rt_const_cstr("b"), rt_const_cstr("a"), rt_const_cstr("duplicate"));
        },
        "GUI model ID must be unique: a");
    assert(visible_count(tree2) == 3);

    void *selectionIds = rt_seq_new();
    rt_seq_push(selectionIds, rt_const_cstr("a"));
    rt_seq_push(selectionIds, rt_const_cstr("missing"));
    rt_seq_push(selectionIds, rt_const_cstr("a"));
    rt_seq_push(selectionIds, rt_const_cstr("b"));
    rt_virtual_tree_select_ids(tree2, selectionIds);
    void *acceptedSelection = rt_virtual_tree_get_selected_ids(tree2);
    assert(rt_seq_len(acceptedSelection) == 2);
    assert(take(rt_seq_get_str(acceptedSelection, 0)) == "a");
    assert(take(rt_seq_get_str(acceptedSelection, 1)) == "b");
    rt_virtual_tree_select_id(tree2, rt_const_cstr("missing"));
    assert(take(rt_virtual_tree_get_selected_id(tree2)).empty());

    void *batchedTree = rt_virtual_tree_new();
    void *batchedControl = rt_treeview_new(nullptr);
    assert(rt_virtual_tree_bind(batchedTree, batchedControl) == 1);
    rt_virtual_tree_begin_update(batchedTree);
    rt_virtual_tree_begin_update(batchedTree);
    rt_virtual_tree_add_node(
        batchedTree, rt_const_cstr(""), rt_const_cstr("bulk"), rt_const_cstr("Bulk"));
    rt_virtual_tree_add_node(
        batchedTree, rt_const_cstr("bulk"), rt_const_cstr("child"), rt_const_cstr("Child"));
    expand = rt_virtual_tree_expand(batchedTree, rt_const_cstr("bulk"));
    assert(rt_map_get_bool(expand, rt_const_cstr("expanded")) == 1);
    rt_virtual_tree_end_update(batchedTree);
    assert(visible_count(batchedTree) == 2);
    rt_virtual_tree_end_update(batchedTree);
    expect_trap([&] { rt_virtual_tree_end_update(batchedTree); },
                "GUI.VirtualTree: EndUpdate without BeginUpdate");

    void *tree3 = rt_virtual_tree_new();
    rt_virtual_tree_add_node(
        tree3, rt_const_cstr("missing"), rt_const_cstr("leaf"), rt_const_cstr("Leaf"));
    assert(visible_count(tree3) == 1);
    expand = rt_virtual_tree_expand(tree3, rt_const_cstr("missing"));
    assert(rt_map_get_bool(expand, rt_const_cstr("expanded")) == 1);
    assert(visible_count(tree3) == 2);

    // Slice-only tree materialization and real TreeView binding.
    void *treeSlice = rt_virtual_tree_visible_rows_range(tree2, 1, 1);
    assert(rt_seq_len(treeSlice) == 1);
    void *boundTree = rt_treeview_new(nullptr);
    assert(rt_virtual_tree_bind(tree2, boundTree) == 1);
    assert(rt_treeview_set_virtual_model(boundTree, tree2) == 1);
    rt_virtual_tree_select_id(tree2, rt_const_cstr("a"));
    assert(take(rt_virtual_tree_get_selected_id(tree2)) == "a");
    rt_treeview_clear_virtual_model(boundTree);

    // The hash-indexed model and cached flattened order remain bounded at 100k rows; only the
    // requested 20 maps are realized by VisibleRowsRange.
    void *largeTree = rt_virtual_tree_new();
    for (int i = 0; i < 100000; i++) {
        char id[32];
        int length = snprintf(id, sizeof(id), "row-%d", i);
        assert(length > 0 && static_cast<size_t>(length) < sizeof(id));
        rt_string managedId = rt_string_from_bytes(id, static_cast<size_t>(length));
        rt_virtual_tree_add_node(largeTree, rt_const_cstr(""), managedId, rt_const_cstr("Row"));
        rt_string_unref(managedId);
    }
    void *largeSlice = rt_virtual_tree_visible_rows_range(largeTree, 99980, 20);
    assert(rt_seq_len(largeSlice) == 20);

    // Destruction order is safe in both directions. Releasing the model disables virtual mode on
    // its still-live control; destroying a control first clears the model's raw pointer.
    void *lifetimeListModel = rt_virtual_list_new(100, 20, 100);
    void *lifetimeListControl = rt_listbox_new(nullptr);
    assert(rt_virtual_list_bind(lifetimeListModel, lifetimeListControl) == 1);
    if (lifetimeListModel && rt_obj_release_check0(lifetimeListModel))
        rt_obj_free(lifetimeListModel);
    assert(rt_listbox_get_visible_count(lifetimeListControl) == 0);
    rt_widget_destroy(lifetimeListControl);

    void *lifetimeTreeModel = rt_virtual_tree_new();
    rt_virtual_tree_add_node(
        lifetimeTreeModel, rt_const_cstr(""), rt_const_cstr("node"), rt_const_cstr("Node"));
    void *lifetimeTreeControl = rt_treeview_new(nullptr);
    assert(rt_virtual_tree_bind(lifetimeTreeModel, lifetimeTreeControl) == 1);
    rt_widget_destroy(lifetimeTreeControl);
    rt_virtual_tree_select_id(lifetimeTreeModel, rt_const_cstr("node"));
    assert(take(rt_virtual_tree_get_selected_id(lifetimeTreeModel)) == "node");

    void *command = rt_command_state_new(rt_const_cstr("build"), rt_const_cstr("Build"));
    rt_command_state_set_enabled(command, 0);
    rt_command_state_set_checked(command, 1);
    rt_command_state_set_accessible(
        command, rt_const_cstr("Build project"), rt_const_cstr("Compile active project"));
    void *cmd = rt_command_state_snapshot(command);
    assert(rt_map_get_bool(cmd, rt_const_cstr("enabled")) == 0);
    assert(rt_map_get_bool(cmd, rt_const_cstr("checked")) == 1);
    assert(rt_accessibility_contrast_ratio(0xffffff, 0x000000) > 20.0);
    assert(rt_accessibility_meets_contrast(0x777777, 0xffffff, 4.5) == 0);
    assert(rt_map_get_int(rt_accessibility_high_contrast_tokens(), rt_const_cstr("foreground")) ==
           0xffffff);

    // --- Zanna.GUI.Command: logical state, snapshot, and the disabled-never-invoked rule.
    //     (The widget-driven half needs a live windowed app and is covered by Zanna Studio probes.)
    void *build = rt_command_new(rt_const_cstr("build"), rt_const_cstr("Build"));
    assert(take(rt_command_get_id(build)) == "build");
    assert(take(rt_command_get_title(build)) == "Build");
    assert(rt_command_is_enabled(build) == 1); // enabled by default
    assert(rt_command_is_checkable(build) == 0);
    assert(rt_command_is_checked(build) == 0);
    assert(rt_command_was_invoked(build) == 0);
    rt_command_set_shortcut(build,
                            rt_const_cstr("Ctrl+B")); // stored even with no app to register with
    assert(take(rt_command_get_shortcut(build)) == "Ctrl+B");
    rt_command_set_checkable(build, 1);
    rt_command_set_checked(build, 1);
    assert(rt_command_is_checkable(build) == 1);
    assert(rt_command_is_checked(build) == 1);
    // No bound widgets and no app: polling never reports the command invoked.
    assert(rt_command_poll(build) == 0);
    assert(rt_command_was_invoked(build) == 0);
    rt_command_set_enabled(build, 0);
    assert(rt_command_is_enabled(build) == 0);
    void *buildSnap = rt_command_snapshot(build);
    assert(rt_map_get_bool(buildSnap, rt_const_cstr("enabled")) == 0);
    assert(rt_map_get_bool(buildSnap, rt_const_cstr("checkable")) == 1);
    assert(rt_map_get_bool(buildSnap, rt_const_cstr("checked")) == 1);
    assert(rt_map_get_bool(buildSnap, rt_const_cstr("invoked")) == 0);

    // --- Zanna.GUI.CommandRegistry: ownership, dedup, find, and an idle poll.
    void *registry = rt_command_registry_new();
    assert(rt_command_registry_count(registry) == 0);
    rt_command_registry_add(registry, build);
    assert(rt_command_registry_count(registry) == 1);
    rt_command_registry_add(registry, build); // duplicate add is ignored (no double-retain)
    assert(rt_command_registry_count(registry) == 1);
    void *run = rt_command_new(rt_const_cstr("run"), rt_const_cstr("Run"));
    rt_command_registry_add(registry, run);
    assert(rt_command_registry_count(registry) == 2);
    void *foundRun = rt_command_registry_find(registry, rt_const_cstr("run"));
    assert(foundRun != NULL);
    assert(take(rt_command_get_id(foundRun)) == "run");
    assert(rt_command_registry_find(registry, rt_const_cstr("missing")) == NULL);
    void *foundRunOption = rt_command_registry_find_option(registry, rt_const_cstr("run"));
    assert(rt_option_is_some(foundRunOption) == 1);
    assert(take(rt_command_get_id(rt_option_unwrap(foundRunOption))) == "run");
    if (foundRunOption && rt_obj_release_check0(foundRunOption))
        rt_obj_free(foundRunOption);
    void *missingCommandOption =
        rt_command_registry_find_option(registry, rt_const_cstr("missing"));
    assert(rt_option_is_none(missingCommandOption) == 1);
    if (missingCommandOption && rt_obj_release_check0(missingCommandOption))
        rt_obj_free(missingCommandOption);
    // No palette bound and nothing invoked: poll returns the empty string.
    assert(take(rt_command_registry_poll(registry)).empty());
    rt_command_registry_clear(registry);
    assert(rt_command_registry_count(registry) == 0);

    // --- R4: HiDPI logical-unit conversion (the math behind App.ToLogical / ToPhysical). ---
    assert(rt_gui_dpi_to_logical(2000, 2.0) == 1000); // Retina 2x: physical -> logical
    assert(rt_gui_dpi_to_logical(1000, 1.0) == 1000); // no scaling at 1x
    assert(rt_gui_dpi_to_logical(1000, 1.5) == 667);  // round(1000 / 1.5)
    assert(rt_gui_dpi_to_logical(0, 2.0) == 0);       // non-positive passes through
    assert(rt_gui_dpi_to_logical(-40, 2.0) == -40);   // negative passes through
    assert(rt_gui_dpi_to_logical(1000, 0.5) == 1000); // scale < 1.0 passes through
    assert(rt_gui_dpi_to_logical(1000, NAN) == 1000); // NaN passes through

    assert(rt_gui_dpi_to_physical(1000, 2.0) == 2000); // logical -> physical
    assert(rt_gui_dpi_to_physical(1000, 1.0) == 1000);
    assert(rt_gui_dpi_to_physical(0, 2.0) == 0);
    assert(rt_gui_dpi_to_physical(-40, 2.0) == -40);
    assert(rt_gui_dpi_to_physical(1000, NAN) == 1000);
    // Exact round-trip at a 2x backing scale.
    assert(rt_gui_dpi_to_physical(rt_gui_dpi_to_logical(2000, 2.0), 2.0) == 2000);

    // --- R7: CodeEditor.InsertAndPlaceCursor offset->position math. ---
    {
        int64_t line = 5, col = 10;
        rt_codeeditor_advance_position("abc", 2, &line, &col); // no newline: column advances
        assert(line == 5 && col == 12);
    }
    {
        int64_t line = 0, col = 0;
        rt_codeeditor_advance_position("a\nbc", 3, &line, &col); // 'a', '\n' (line++, col=0), 'b'
        assert(line == 1 && col == 1);
    }
    {
        int64_t line = 2, col = 4;
        rt_codeeditor_advance_position("hello", 0, &line, &col); // offset 0: no change
        assert(line == 2 && col == 4);
    }
    {
        int64_t line = 0, col = 0;
        rt_codeeditor_advance_position("ab", 100, &line, &col); // offset past end: stops at end
        assert(line == 0 && col == 2);
    }
    {
        int64_t line = 0, col = 0;
        rt_codeeditor_advance_position("\xC3\xA9x", 2, &line, &col); // 'é' is one column, then 'x'
        assert(line == 0 && col == 2);
    }
    {
        int64_t line = 0, col = 7;
        rt_codeeditor_advance_position("\n\nX", 3, &line, &col); // each newline resets the column
        assert(line == 2 && col == 1);
    }
    return 0;
}
