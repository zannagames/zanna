//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGuiRuntimeTests.c
// Purpose: Regression tests for GUI runtime app-scoped state, public widget
//          contracts, lifetime validation, scaling, events, and virtualization.
// Key invariants:
//   - Public handles are type/liveness checked before every dereference.
//   - Logical geometry crosses into the physical toolkit exactly once.
//   - Consuming one event edge never consumes a distinct edge or revision.
//   - Assertions stay enabled in every build configuration because test setup
//     and validation intentionally share the assertion expressions.
// Ownership/Lifetime:
//   - Each test owns its fake app/widget tree and releases returned runtime
//     strings/objects when the corresponding API transfers ownership.
// Links: runtime/graphics/gui/rt_gui.h,
//        runtime/graphics/gui/rt_gui_internal.h,
//        docs/adr/0163-stable-multiselect-and-row-aware-treeview-editing.md,
//        docs/adr/0165-scrollview-descendant-reveal.md,
//        docs/adr/0167-spinner-mixed-value-state.md,
//        docs/adr/0219-scene-probe-interaction-contract-and-menu-item-geometry.md,
//        docs/adr/0220-scene-editor-toolbars-and-vector-icon-set.md
// Cross-platform touchpoints: Temporary-file cleanup uses the runtime platform
//                             vocabulary to select the POSIX-only declaration.
//
//===----------------------------------------------------------------------===//

#if defined(NDEBUG)
#undef NDEBUG
#endif

#include "../../runtime/graphics/gui/rt_gui_app_internal.h"
#include "../../runtime/graphics/gui/rt_gui_internal.h"
#include "rt_gui.h"
#include "rt_gui_constants.h"
#include "rt_gui_ide.h"
#include "rt_map.h"
#include "rt_option.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !RT_PLATFORM_WINDOWS
#include <unistd.h>
#endif

#ifndef ZANNA_SOURCE_DIR
#define ZANNA_SOURCE_DIR "."
#endif

extern void rt_gui_set_clicked_statusbar_item(void *item);

typedef struct {
    uint64_t magic;
    vg_findreplacebar_t *bar;
    void *bound_editor;
    char *find_text;
    char *replace_text;
    int64_t case_sensitive;
    int64_t whole_word;
    int64_t regex;
    int64_t replace_mode;
} rt_findbar_data_view_t;

typedef struct {
    uint64_t magic;
    vg_breadcrumb_t *breadcrumb;
    int64_t clicked_index;
    rt_gui_string_data_t *clicked_data;
    int64_t was_clicked;
} rt_breadcrumb_data_view_t;

typedef struct {
    uint64_t magic;
    rt_gui_app_t *owner_app;
    vg_filedialog_t *dialog;
    char **selected_paths;
    size_t selected_count;
    int64_t result;
    int64_t status;
    const char *error;
    uint64_t completed_edges;
} rt_filedialog_data_view_t;

typedef struct {
    uint64_t magic;
    vg_dialog_t *dialog;
    int64_t result;
    int64_t status;
    const char *error;
    uint64_t completed_edges;
    int64_t default_button;
    int has_default_button;
    int64_t cancel_button;
    int has_cancel_button;
    rt_gui_app_t *owner_app;
    vg_dialog_button_def_t *custom_buttons;
    int64_t *custom_button_ids;
    int64_t *custom_button_roles;
    size_t custom_button_count;
    size_t custom_button_cap;
} rt_messagebox_data_view_t;

typedef struct {
    uint64_t magic;
    rt_gui_app_t *app;
    vg_commandpalette_t *palette;
    char *selected_command;
    int64_t was_selected;
} rt_commandpalette_data_view_t;

typedef struct {
    uint64_t magic;
    vg_minimap_t *minimap;
    int64_t width;
    const vg_widget_vtable_t *original_vtable;
    vg_widget_vtable_t vtable;
} rt_minimap_data_view_t;

static char *test_strdup_local(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

static int s_expect_vm_trap = 0;
static char s_expected_vm_trap_message[256];

/// @brief Test override for the runtime trap sink with opt-in expected-trap capture.
/// @details Production runtime calls normally remain fatal to this test process. A test that
///          explicitly sets @ref s_expect_vm_trap captures one stable message and lets the native
///          entry point return, allowing atomic postcondition checks after validation failures.
/// @param msg Stable runtime diagnostic supplied by the trap dispatcher.
void vm_trap(const char *msg) {
    if (s_expect_vm_trap) {
        snprintf(
            s_expected_vm_trap_message, sizeof(s_expected_vm_trap_message), "%s", msg ? msg : "");
        return;
    }
    assert(0 && "unexpected vm_trap");
}

/// @brief Begin capture of exactly one expected runtime trap.
/// @details Clears any prior message so a missing trap is observable by the matching assertion.
static void begin_expected_vm_trap(void) {
    s_expected_vm_trap_message[0] = '\0';
    s_expect_vm_trap = 1;
}

/// @brief Finish expected-trap capture and verify its exact public diagnostic.
/// @param expected Exact UTF-8 diagnostic required by the runtime contract.
static void end_expected_vm_trap(const char *expected) {
    s_expect_vm_trap = 0;
    assert(expected);
    assert(strcmp(s_expected_vm_trap_message, expected) == 0);
}

static void reset_fake_app(rt_gui_app_t *app) {
    memset(app, 0, sizeof(*app));
    app->magic = RT_GUI_APP_MAGIC;
    app->shortcuts_global_enabled = 1;
    app->theme_kind = RT_GUI_THEME_DARK;
}

static void cleanup_fake_app(rt_gui_app_t *app) {
    if (!app)
        return;
    rt_gui_activate_app(NULL);
    if (app->root) {
        vg_widget_destroy(app->root);
        app->root = NULL;
    }
    if (app->theme) {
        if (vg_theme_get_current() == app->theme)
            vg_theme_set_current(vg_theme_dark());
        vg_theme_destroy(app->theme);
        app->theme = NULL;
    }
    if (app->custom_theme_base) {
        vg_theme_destroy(app->custom_theme_base);
        app->custom_theme_base = NULL;
    }
    free(app->retired_fonts);
    free(app->retired_font_generations);
    app->retired_fonts = NULL;
    app->retired_font_generations = NULL;
    app->retired_font_count = 0;
    app->retired_font_cap = 0;
}

/// @brief Release one test-owned runtime object and run its finalizer at zero references.
/// @details Runtime `Result` values retain object payloads. Tests use this helper to exercise the
///          production ownership path instead of leaking a result or manually freeing its payload.
/// @param object Runtime-managed object returned by a public runtime function, or NULL.
static void release_test_runtime_object(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Verify capability discovery, fallible construction, and the integrated frame scheduler.
/// @details The display-backed GUI test target guarantees that the graphics implementation is
///          linked. `TryNew` must therefore return `Result.Ok`, retain the app while the result is
///          alive, and expose an empty unavailability reason. The new frame primitive must consume
///          deterministic time once, expose retained deadlines, and preserve app-current state.
///          Explicit destruction remains safe before the owning result is released.
static void test_gui_capability_and_try_new_success(void) {
    assert(rt_gui_system_is_available() == 1);
    rt_string reason = rt_gui_system_get_unavailable_reason();
    assert(reason);
    assert(rt_str_len(reason) == 0);
    rt_str_release_maybe(reason);

    void *result = rt_gui_app_try_new(rt_const_cstr("GUI TryNew Contract"), 96, 64);
    assert(result);
    assert(rt_result_is_ok(result) == 1);

    void *app = rt_result_ok_value(result);
    assert(app);
    assert(rt_gui_app_get_root(app));
    rt_gui_app_t *app_state = (rt_gui_app_t *)app;
    rt_gui_app_make_current(app);
    assert(rt_gui_get_active_app() == app_state);
    assert(rt_gui_app_get_next_deadline_ms(app) == 0);
    app_state->frames_full = UINT64_MAX;
    app_state->frames_partial = UINT64_MAX;
    assert(rt_app_get_paint_frames_full(app) == INT64_MAX);
    assert(rt_app_get_paint_frames_partial(app) == INT64_MAX);
    app_state->frames_full = 0;
    app_state->frames_partial = 0;
    assert(rt_gui_app_run_frame_with_delta(app, 16.5) == 1);
    assert(app_state->scheduler_elapsed_ms == 16.5);
    assert(rt_gui_app_get_next_deadline_ms(app) == -1);
    vg_widget_invalidate(app_state->root);
    assert(rt_gui_app_get_next_deadline_ms(app) == 0);
    assert(rt_gui_app_run_frame(app) == 1);
    rt_gui_app_destroy(app);
    release_test_runtime_object(result);

    printf("test_gui_capability_and_try_new_success: PASSED\n");
}

/// @brief Verify native and programmatic resize paths honor an app minimum.
/// @details The desktop adapter receives the constraint, while the vgfx core
///          independently clamps explicit SetWindowSize calls. Using logical
///          dimensions here also guards Retina/DPI conversion from applying the
///          floor twice or treating it as physical pixels.
static void test_app_minimum_window_size(void) {
    void *result = rt_gui_app_try_new(rt_const_cstr("Minimum Window Contract"), 160, 100);
    assert(result && rt_result_is_ok(result) == 1);
    void *app = rt_result_ok_value(result);
    assert(app);

    rt_app_set_minimum_size(app, 320, 240);
    rt_app_set_window_size(app, 32, 24);
    rt_gui_app_poll(app);
    assert(rt_app_get_logical_width(app) >= 320);
    assert(rt_app_get_logical_height(app) >= 240);

    rt_app_set_minimum_size(app, 1, 1);
    rt_gui_app_destroy(app);
    release_test_runtime_object(result);
    printf("test_app_minimum_window_size: PASSED\n");
}

/// @brief Verify every typed GUI constant getter returns its documented stable public ordinal.
/// @details This exhaustive list prevents internal toolkit enum reordering, graphics capability,
///          or registry refactoring from silently changing values persisted by applications.
static void test_gui_typed_constant_ordinals(void) {
#define ASSERT_GUI_CONSTANT(function_name, expected) assert(function_name() == (expected))
    ASSERT_GUI_CONSTANT(rt_gui_align_start, 0);
    ASSERT_GUI_CONSTANT(rt_gui_align_center, 1);
    ASSERT_GUI_CONSTANT(rt_gui_align_end, 2);
    ASSERT_GUI_CONSTANT(rt_gui_align_stretch, 3);
    ASSERT_GUI_CONSTANT(rt_gui_justify_start, 0);
    ASSERT_GUI_CONSTANT(rt_gui_justify_center, 1);
    ASSERT_GUI_CONSTANT(rt_gui_justify_end, 2);
    ASSERT_GUI_CONSTANT(rt_gui_justify_space_between, 3);
    ASSERT_GUI_CONSTANT(rt_gui_justify_space_around, 4);
    ASSERT_GUI_CONSTANT(rt_gui_justify_space_evenly, 5);
    ASSERT_GUI_CONSTANT(rt_gui_flex_direction_row, 0);
    ASSERT_GUI_CONSTANT(rt_gui_flex_direction_column, 1);
    ASSERT_GUI_CONSTANT(rt_gui_flex_direction_row_reverse, 2);
    ASSERT_GUI_CONSTANT(rt_gui_flex_direction_column_reverse, 3);
    ASSERT_GUI_CONSTANT(rt_gui_flex_wrap_no_wrap, 0);
    ASSERT_GUI_CONSTANT(rt_gui_flex_wrap_wrap, 1);
    ASSERT_GUI_CONSTANT(rt_gui_flex_wrap_wrap_reverse, 2);
    ASSERT_GUI_CONSTANT(rt_gui_dock_left, 0);
    ASSERT_GUI_CONSTANT(rt_gui_dock_top, 1);
    ASSERT_GUI_CONSTANT(rt_gui_dock_right, 2);
    ASSERT_GUI_CONSTANT(rt_gui_dock_bottom, 3);
    ASSERT_GUI_CONSTANT(rt_gui_dock_fill, 4);
    ASSERT_GUI_CONSTANT(rt_gui_theme_mode_dark, 0);
    ASSERT_GUI_CONSTANT(rt_gui_theme_mode_light, 1);
    ASSERT_GUI_CONSTANT(rt_gui_theme_mode_system, 2);
    ASSERT_GUI_CONSTANT(rt_gui_theme_mode_custom, 3);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_none, 0);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_application, 1);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_window, 2);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_group, 3);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_label, 4);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_button, 5);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_check_box, 6);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_radio_button, 7);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_text_box, 8);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_search_box, 9);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_combo_box, 10);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_list, 11);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_list_item, 12);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_tree, 13);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_tree_item, 14);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_tab_list, 15);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_tab, 16);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_table, 17);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_row, 18);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_cell, 19);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_slider, 20);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_progress_bar, 21);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_dialog, 22);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_alert, 23);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_menu, 24);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_menu_item, 25);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_tool_bar, 26);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_status_bar, 27);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_image, 28);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_video, 29);
    ASSERT_GUI_CONSTANT(rt_gui_accessible_role_link, 30);
    ASSERT_GUI_CONSTANT(rt_gui_live_region_mode_off, 0);
    ASSERT_GUI_CONSTANT(rt_gui_live_region_mode_polite, 1);
    ASSERT_GUI_CONSTANT(rt_gui_live_region_mode_assertive, 2);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_button_role_normal, 0);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_button_role_default, 1);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_button_role_cancel, 2);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_button_role_destructive, 3);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_button_role_accept, 4);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_button_role_reject, 5);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_button_role_help, 6);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_status_idle, 0);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_status_open, 1);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_status_accepted, 2);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_status_cancelled, 3);
    ASSERT_GUI_CONSTANT(rt_gui_dialog_status_failed, 4);
    ASSERT_GUI_CONSTANT(rt_gui_image_filter_nearest, 0);
    ASSERT_GUI_CONSTANT(rt_gui_image_filter_bilinear, 1);
    ASSERT_GUI_CONSTANT(rt_gui_sort_direction_none, 0);
    ASSERT_GUI_CONSTANT(rt_gui_sort_direction_ascending, 1);
    ASSERT_GUI_CONSTANT(rt_gui_sort_direction_descending, -1);
#undef ASSERT_GUI_CONSTANT
    printf("test_gui_typed_constant_ordinals: PASSED\n");
}

/// @brief Verify TestHarness drives a live App and observes its real rendered/semantic output.
/// @details Binding must retain and validate the App, deterministic RenderFrame must route a
///          journaled click through the same platform queue and App.Poll path as native input, and
///          framebuffer captures must be deep, hash-stable, clipped, and tolerance-comparable.
///          The accessibility result must be the real root tree rather than the empty unbound
///          schema. Unbinding then restores safe unavailable results without erasing legacy state.
static void test_app_bound_test_harness_uses_real_runtime_paths(void) {
    void *result = rt_gui_app_try_new(rt_const_cstr("App-bound Harness Contract"), 160, 100);
    assert(result && rt_result_is_ok(result) == 1);
    void *app = rt_result_ok_value(result);
    assert(app);
    void *root = rt_gui_app_get_root(app);
    assert(root);
    void *button = rt_button_new(root, rt_const_cstr("Harness Button"));
    assert(button);
    rt_widget_set_position(button, 12, 10);
    rt_widget_set_size(button, 96, 32);
    rt_widget_set_accessible_name(button, rt_const_cstr("Harness Action"));

    void *harness = rt_gui_test_harness_new();
    assert(harness);
    assert(rt_gui_test_harness_bind_app(harness, app) == 1);
    assert(rt_gui_test_harness_render_frame(harness, 0.0) == 1);

    vg_widget_t *button_widget = rt_gui_widget_handle_checked(button);
    assert(button_widget);
    int64_t click_x = (int64_t)button_widget->x + 4;
    int64_t click_y = (int64_t)button_widget->y + 4;
    rt_gui_test_harness_send_mouse(
        harness, rt_const_cstr("click"), click_x, click_y, VGFX_MOUSE_LEFT);
    assert(rt_gui_test_harness_render_frame(harness, 16.0) == 1);
    assert(rt_widget_was_clicked(button) == 1);

    void *capture = rt_gui_test_harness_capture_pixels(harness, 0, 0, 160, 100);
    assert(capture);
    assert(rt_pixels_width(capture) == 160);
    assert(rt_pixels_height(capture) == 100);
    const uint32_t *captured = rt_pixels_raw_buffer(capture);
    assert(captured);
    size_t nonzero = 0;
    for (size_t index = 0; index < 160u * 100u; ++index)
        nonzero += captured[index] != 0u ? 1u : 0u;
    assert(nonzero > 0u);

    rt_string first_hash = rt_gui_test_harness_capture_hash(harness, 0, 0, 160, 100);
    rt_string second_hash = rt_gui_test_harness_capture_hash(harness, 0, 0, 160, 100);
    assert(first_hash && second_hash);
    assert(rt_str_len(first_hash) == 16);
    assert(strcmp(rt_string_cstr(first_hash), rt_string_cstr(second_hash)) == 0);
    rt_str_release_maybe(first_hash);
    rt_str_release_maybe(second_hash);

    void *comparison = rt_gui_test_harness_compare_region(harness, capture, 0, 0, 0);
    assert(comparison);
    assert(rt_map_get_int(comparison, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_get_bool(comparison, rt_const_cstr("matches")) == 1);
    assert(rt_map_get_int(comparison, rt_const_cstr("differentPixels")) == 0);
    release_test_runtime_object(comparison);

    uint32_t original = captured[0];
    rt_pixels_set_rgba(capture, 0, 0, (int64_t)(original ^ UINT32_C(0x000000FF)));
    comparison = rt_gui_test_harness_compare_region(harness, capture, 0, 0, 0);
    assert(comparison);
    assert(rt_map_get_bool(comparison, rt_const_cstr("matches")) == 0);
    assert(rt_map_get_int(comparison, rt_const_cstr("differentPixels")) == 1);
    assert(rt_map_get_int(comparison, rt_const_cstr("maxChannelDelta")) > 0);
    release_test_runtime_object(comparison);

    void *semantic = rt_gui_test_harness_get_accessibility_snapshot(harness);
    assert(semantic);
    assert(rt_map_get_int(semantic, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_has(semantic, rt_const_cstr("children")) == 1);
    release_test_runtime_object(semantic);

    rt_gui_test_harness_unbind_app(harness);
    assert(rt_gui_test_harness_capture_pixels(harness, 0, 0, 1, 1) == NULL);
    semantic = rt_gui_test_harness_get_accessibility_snapshot(harness);
    assert(semantic);
    assert(rt_map_get_int(semantic, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_has(semantic, rt_const_cstr("children")) == 0);
    release_test_runtime_object(semantic);

    release_test_runtime_object(capture);
    release_test_runtime_object(harness);
    rt_gui_app_destroy(app);
    release_test_runtime_object(result);
    printf("test_app_bound_test_harness_uses_real_runtime_paths: PASSED\n");
}

/// @brief Verify detached tooltip show/hide frames remain damage-limited and idle
/// deterministically.
/// @details The first app frame initializes the framebuffer through the full path. Showing and
///          hiding a compact manual tooltip must then increment only the partial counter, retain
///          and clear the overlay union respectively, and leave both counters unchanged on the
///          following idle frame.
static void test_detached_tooltip_uses_retained_overlay_damage(void) {
    void *result = rt_gui_app_try_new(rt_const_cstr("Overlay Damage Contract"), 320, 180);
    assert(result && rt_result_is_ok(result) == 1);
    void *app = rt_result_ok_value(result);
    assert(app);
    rt_gui_app_t *state = (rt_gui_app_t *)app;
    rt_gui_app_make_current(app);
    rt_app_set_partial_paint(app, 1);

    assert(rt_gui_app_run_frame_with_delta(app, 0.0) == 1);
    uint64_t full_before = state->frames_full;
    uint64_t partial_before = state->frames_partial;
    assert(full_before >= 1);

    rt_tooltip_show(rt_const_cstr("Damage tracked tooltip"), 24, 24);
    assert(state->manual_tooltip && state->manual_tooltip->is_visible);
    assert(rt_gui_app_run_frame_with_delta(app, 16.0) == 1);
    assert(state->frames_full == full_before);
    assert(state->frames_partial == partial_before + 1);
    assert(state->overlay_last_valid == 1);
    assert(state->last_layout_width > 0 && state->last_layout_height > 0);
    assert(state->last_damage_w > 0.0f && state->last_damage_w < (float)state->last_layout_width);
    assert(state->last_damage_h > 0.0f && state->last_damage_h < (float)state->last_layout_height);

    rt_tooltip_hide();
    assert(rt_gui_app_run_frame_with_delta(app, 16.0) == 1);
    assert(state->frames_full == full_before);
    assert(state->frames_partial == partial_before + 2);
    assert(state->overlay_last_valid == 0);

    uint64_t full_idle = state->frames_full;
    uint64_t partial_idle = state->frames_partial;
    assert(rt_gui_app_run_frame_with_delta(app, 16.0) == 1);
    assert(state->frames_full == full_idle);
    assert(state->frames_partial == partial_idle);

    rt_gui_app_destroy(app);
    release_test_runtime_object(result);
    printf("test_detached_tooltip_uses_retained_overlay_damage: PASSED\n");
}

/// @brief Verify deterministic frame deltas drive toast animation and expiry through one clock.
/// @details A newly created toast requests immediate initialization, then 16 ms entrance ticks.
///          Advancing 300 deterministic milliseconds must settle the entrance and leave exactly
///          700 ms until expiry. The final expiry and exit animation are likewise exposed through
///          App.GetNextDeadlineMs, proving PollWait cannot sleep through overlay-only timers.
static void test_deterministic_scheduler_drives_notification_deadlines(void) {
    void *result = rt_gui_app_try_new(rt_const_cstr("Notification Scheduler Contract"), 240, 120);
    assert(result && rt_result_is_ok(result) == 1);
    void *app = rt_result_ok_value(result);
    assert(app);
    rt_gui_app_make_current(app);
    assert(rt_gui_app_run_frame_with_delta(app, 0.0) == 1);

    void *toast = rt_toast_new(rt_const_cstr("Scheduled"), RT_TOAST_INFO, 1000);
    assert(toast);
    assert(rt_gui_app_get_next_deadline_ms(app) == 0);
    assert(rt_gui_app_run_frame_with_delta(app, 0.0) == 1);
    assert(rt_gui_app_get_next_deadline_ms(app) == 16);
    assert(rt_gui_app_run_frame_with_delta(app, 300.0) == 1);
    assert(rt_gui_app_get_next_deadline_ms(app) == 700);
    assert(rt_gui_app_run_frame_with_delta(app, 699.0) == 1);
    assert(rt_gui_app_get_next_deadline_ms(app) == 1);
    assert(rt_gui_app_run_frame_with_delta(app, 1.0) == 1);
    assert(rt_gui_app_get_next_deadline_ms(app) == 16);
    assert(rt_gui_app_run_frame_with_delta(app, 300.0) == 1);
    assert(rt_gui_app_get_next_deadline_ms(app) == -1);

    release_test_runtime_object(toast);
    rt_gui_app_destroy(app);
    release_test_runtime_object(result);
    printf("test_deterministic_scheduler_drives_notification_deadlines: PASSED\n");
}

/// @brief Stress stable subhandle hashing and verify per-retirement-group automatic reclamation.
/// @details Thousands of ListBox records exercise bounded expected-O(1) identity lookup. Retired
///          records remain while wrappers are live and drain automatically as wrappers finalize.
///          Two independent TreeView subtrees prove that an unrelated stale wrapper does not pin
///          an unwrapped retirement group owned by the same widget.
static void test_indexed_subhandles_reclaim_after_last_wrapper(void) {
    const size_t baseline = rt_gui_subhandle_debug_live_count();
    const size_t item_count = 2048;
    void **handles = (void **)calloc(item_count, sizeof(*handles));
    vg_listbox_item_t **items = (vg_listbox_item_t **)calloc(item_count, sizeof(*items));
    assert(handles && items);
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);

    for (size_t i = 0; i < item_count; ++i) {
        items[i] = vg_listbox_add_item(listbox, "row", NULL);
        assert(items[i]);
        handles[i] = rt_gui_wrap_listbox_item(items[i]);
        assert(handles[i]);
    }
    rt_gui_subhandle_debug_reset_probes();
    for (size_t i = 0; i < item_count; ++i)
        assert(rt_gui_wrap_listbox_item(items[i]) == handles[i]);
    size_t capacity = rt_gui_subhandle_debug_index_capacity();
    assert(capacity >= item_count);
    assert(rt_gui_subhandle_debug_max_probes() > 0);
    assert(rt_gui_subhandle_debug_max_probes() < 64);

    while (listbox->first_item)
        vg_listbox_remove_item(listbox, listbox->first_item);
    rt_gui_collect_retired_subhandles(&listbox->base);
    assert(listbox->retired_items != NULL);

    for (size_t i = 0; i < item_count; ++i)
        release_test_runtime_object(handles[i]);
    free(handles);
    free(items);
    assert(listbox->retired_items == NULL);
    assert(rt_gui_subhandle_debug_live_count() == baseline);
    vg_widget_destroy(&listbox->base);

    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);
    vg_tree_node_t *held_root = vg_treeview_add_node(tree, NULL, "held");
    vg_tree_node_t *free_root = vg_treeview_add_node(tree, NULL, "free");
    assert(held_root && free_root);
    void *held_handle = rt_gui_wrap_tree_node(held_root);
    assert(held_handle);
    vg_treeview_remove_node(tree, held_root);
    vg_treeview_remove_node(tree, free_root);
    rt_gui_collect_retired_subhandles(&tree->base);
    assert(tree->retired_nodes == held_root);
    assert(held_root->retired_next == NULL);

    release_test_runtime_object(held_handle);
    assert(tree->retired_nodes == NULL);
    assert(rt_gui_subhandle_debug_live_count() == baseline);
    vg_widget_destroy(&tree->base);
    printf("test_indexed_subhandles_reclaim_after_last_wrapper: PASSED\n");
}

static void test_shortcuts_are_app_scoped(void) {
    rt_gui_app_t app_a;
    rt_gui_app_t app_b;
    reset_fake_app(&app_a);
    reset_fake_app(&app_b);

    s_current_app = &app_a;
    rt_shortcuts_register(rt_const_cstr("save"), rt_const_cstr("Ctrl+S"), rt_const_cstr(""));
    rt_shortcuts_register(
        rt_const_cstr("palette"), rt_const_cstr("Cmd+Shift+P"), rt_const_cstr(""));
    s_current_app = &app_b;
    rt_shortcuts_register(rt_const_cstr("help"), rt_const_cstr("F5"), rt_const_cstr(""));

    rt_shortcuts_clear_triggered(&app_a);
    rt_shortcuts_clear_triggered(&app_b);

    assert(rt_shortcuts_check_key(&app_a, 'S', VGFX_MOD_CTRL) == 1);

    s_current_app = &app_a;
    assert(rt_shortcuts_was_triggered(rt_const_cstr("save")) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("help")) == 0);

    rt_shortcuts_clear_triggered(&app_a);
    assert(rt_shortcuts_check_key(&app_a, 'P', VG_MOD_SUPER | VG_MOD_SHIFT) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("palette")) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("save")) == 0);

    s_current_app = &app_b;
    assert(rt_shortcuts_check_key(&app_b, VG_KEY_F5, 0) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("help")) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("save")) == 0);

    s_current_app = &app_a;
    rt_shortcuts_clear();
    s_current_app = &app_b;
    rt_shortcuts_clear();

    printf("test_shortcuts_are_app_scoped: PASSED\n");
}

static void test_shortcut_chords_trigger_and_expire(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    s_current_app = &app;

    rt_shortcuts_register(
        rt_const_cstr("recent"), rt_const_cstr("Ctrl + K Ctrl + R"), rt_const_cstr(""));
    rt_shortcuts_register(
        rt_const_cstr("keybindings"), rt_const_cstr("Ctrl+K Ctrl+S"), rt_const_cstr(""));
    rt_shortcuts_register(rt_const_cstr("quickopen"), rt_const_cstr("Ctrl+P"), rt_const_cstr(""));
    rt_shortcuts_register(
        rt_const_cstr("invalid"), rt_const_cstr("Ctrl+K Ctrl+S Ctrl+X"), rt_const_cstr(""));
    assert(app.shortcut_count == 3);
    assert(app.shortcuts[0].first.key == 'K');
    assert(app.shortcuts[0].second.key == 'R');

    app.last_event_time_ms = 100;
    assert(rt_shortcuts_check_key(&app, 'K', VG_MOD_CTRL) == 1);
    assert(app.shortcut_chord_pending == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("recent")) == 0);
    // Studio reconciles every command's enabled state once per frame. Those
    // idempotent writes must not discard a prefix entered in the prior frame.
    rt_shortcuts_set_enabled(rt_const_cstr("recent"), 1);
    rt_shortcuts_set_enabled(rt_const_cstr("keybindings"), 1);
    rt_shortcuts_set_enabled(rt_const_cstr("quickopen"), 1);
    assert(app.shortcut_chord_pending == 1);
    rt_shortcuts_clear_triggered(&app);

    app.last_event_time_ms = 200;
    assert(rt_shortcuts_check_key(&app, 'R', VG_MOD_CTRL) == 1);
    assert(app.shortcut_chord_pending == 0);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("recent")) == 1);
    rt_shortcuts_clear_triggered(&app);

    app.last_event_time_ms = 300;
    assert(rt_shortcuts_check_key(&app, 'K', VG_MOD_CTRL) == 1);
    rt_shortcuts_clear_triggered(&app);
    app.last_event_time_ms = 350;
    assert(rt_shortcuts_check_key(&app, 'S', VG_MOD_CTRL) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("keybindings")) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("recent")) == 0);
    rt_shortcuts_clear_triggered(&app);

    app.last_event_time_ms = 500;
    assert(rt_shortcuts_check_key(&app, 'K', VG_MOD_CTRL) == 1);
    rt_shortcuts_clear_triggered(&app);
    app.last_event_time_ms = 2001;
    assert(rt_shortcuts_check_key(&app, 'R', VG_MOD_CTRL) == 0);
    assert(app.shortcut_chord_pending == 0);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("recent")) == 0);

    app.last_event_time_ms = 2100;
    assert(rt_shortcuts_check_key(&app, 'K', VG_MOD_CTRL) == 1);
    rt_shortcuts_clear_triggered(&app);
    app.last_event_time_ms = 2200;
    assert(rt_shortcuts_check_key(&app, 'P', VG_MOD_CTRL) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("quickopen")) == 1);

    rt_shortcuts_clear();
    s_current_app = NULL;
    printf("test_shortcut_chords_trigger_and_expire: PASSED\n");
}

static void test_file_drop_is_app_scoped(void) {
    rt_gui_app_t app_a;
    rt_gui_app_t app_b;
    reset_fake_app(&app_a);
    reset_fake_app(&app_b);

    rt_gui_file_drop_add(&app_a, "/tmp/a.txt");
    rt_gui_file_drop_add(&app_b, "/tmp/b.txt");

    rt_gui_activate_app(&app_a);
    assert(rt_app_was_file_dropped(&app_a) == 1);
    assert(rt_app_get_dropped_file_count(&app_a) == 1);
    assert(strcmp(rt_string_cstr(rt_app_get_dropped_file(&app_a, 0)), "/tmp/a.txt") == 0);

    rt_gui_activate_app(&app_b);
    assert(rt_app_was_file_dropped(&app_b) == 1);
    assert(rt_app_get_dropped_file_count(&app_b) == 1);
    assert(strcmp(rt_string_cstr(rt_app_get_dropped_file(&app_b, 0)), "/tmp/b.txt") == 0);

    rt_gui_activate_app(&app_a);
    assert(rt_app_was_file_dropped(&app_a) == 0);
    rt_gui_activate_app(&app_b);
    assert(rt_app_was_file_dropped(&app_b) == 0);

    rt_gui_features_cleanup(&app_a);
    rt_gui_features_cleanup(&app_b);
    rt_gui_activate_app(NULL);

    printf("test_file_drop_is_app_scoped: PASSED\n");
}

static void test_statusbar_click_is_edge_triggered(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_statusbar_t *statusbar = vg_statusbar_create(app.root);
    assert(statusbar);
    vg_statusbar_item_t *item = vg_statusbar_add_text(statusbar, VG_STATUSBAR_ZONE_LEFT, "ready");
    assert(item);
    void *item_handle = rt_gui_wrap_statusbar_item(item);

    rt_gui_set_clicked_statusbar_item(item);
    assert(rt_statusbaritem_was_clicked(item_handle) == 1);
    assert(rt_statusbaritem_was_clicked(item_handle) == 0);

    cleanup_fake_app(&app);
    printf("test_statusbar_click_is_edge_triggered: PASSED\n");
}

static void test_statusbar_runtime_button_wires_click_polling(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_statusbar_t *statusbar = (vg_statusbar_t *)rt_statusbar_new(app.root);
    assert(statusbar);
    void *button_handle = rt_statusbar_add_button(statusbar, rt_const_cstr("Build"), 0);
    vg_statusbar_item_t *button = rt_gui_statusbar_item_from_handle(button_handle);
    assert(button);
    assert(button->on_click != NULL);

    button->on_click(button, button->user_data);
    assert(rt_statusbaritem_was_clicked(button_handle) == 1);
    assert(rt_statusbaritem_was_clicked(button_handle) == 0);

    cleanup_fake_app(&app);
    printf("test_statusbar_runtime_button_wires_click_polling: PASSED\n");
}

static void test_default_font_is_applied_to_text_widgets(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 17.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_label_t *label = (vg_label_t *)rt_label_new(app.root, rt_const_cstr("label"));
    vg_button_t *button = (vg_button_t *)rt_button_new(app.root, rt_const_cstr("button"));
    vg_textinput_t *input = (vg_textinput_t *)rt_textinput_new(app.root);
    vg_checkbox_t *checkbox = (vg_checkbox_t *)rt_checkbox_new(app.root, rt_const_cstr("check"));
    vg_dropdown_t *dropdown = (vg_dropdown_t *)rt_dropdown_new(app.root);
    vg_listbox_t *listbox = (vg_listbox_t *)rt_listbox_new(app.root);
    vg_datagrid_t *grid = (vg_datagrid_t *)rt_datagrid_new(app.root);
    void *group = rt_radiogroup_new();
    vg_radiobutton_t *radio =
        (vg_radiobutton_t *)rt_radiobutton_new(app.root, rt_const_cstr("radio"), group);

    assert(label && label->font == app.default_font && label->font_size == app.default_font_size);
    assert(button && button->font == app.default_font &&
           button->font_size == app.default_font_size);
    assert(input && input->font == app.default_font && input->font_size == app.default_font_size);
    assert(checkbox && checkbox->font == app.default_font &&
           checkbox->font_size == app.default_font_size);
    assert(dropdown && dropdown->font == app.default_font &&
           dropdown->font_size == app.default_font_size);
    assert(listbox && listbox->font == app.default_font &&
           listbox->font_size == app.default_font_size);
    assert(grid && grid->font == app.default_font && grid->font_size == app.default_font_size);
    assert(radio && radio->font == app.default_font && radio->font_size == app.default_font_size);

    rt_radiogroup_destroy(group);
    cleanup_fake_app(&app);
    printf("test_default_font_is_applied_to_text_widgets: PASSED\n");
}

/// @brief Verify the complete public TextInput editing and observation surface.
/// @details Exercises Unicode grapheme indices with combining, emoji-family, and flag clusters;
///          selection copies; mode flags; max-length enforcement; atomic insert/history calls;
///          independent change/submission edges; monotonic revisions; and read-only IME preedit
///          observation. Native composition creation is driven through the lower toolkit because
///          public applications receive that state from platform IME events rather than creating
///          synthetic composition sessions themselves.
static void test_textinput_runtime_exposes_complete_grapheme_editor(void) {
    static const char text[] = "e\xCC\x81"
                               "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                               "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6"
                               "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"
                               "Z";
    static const char selected_expected[] =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6"
        "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    static const char preedit[] = "\xE6\xBC\xA2\xE5\xAD\x97";

    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_textinput_t *input = (vg_textinput_t *)rt_textinput_new(app.root);
    assert(input);
    assert(rt_textinput_get_max_length(input) == 0);
    assert(rt_textinput_get_cursor(input) == 0);
    const int64_t initial_revision = rt_textinput_get_revision(input);
    assert(initial_revision >= 0);

    rt_textinput_set_password(input, 1);
    rt_textinput_set_read_only(input, 0);
    rt_textinput_set_multiline(input, 1);
    assert(rt_textinput_is_password(input) == 1);
    assert(rt_textinput_is_read_only(input) == 0);
    assert(rt_textinput_is_multiline(input) == 1);

    rt_textinput_set_max_length(input, 4);
    assert(rt_textinput_get_max_length(input) == 4);
    rt_textinput_set_text(input, rt_const_cstr(text));
    assert(rt_textinput_get_cursor(input) == 4);
    assert(rt_textinput_was_changed(input) == 1);
    assert(rt_textinput_was_changed(input) == 0);

    rt_textinput_set_cursor(input, 1);
    assert(rt_textinput_get_cursor(input) == 1);
    rt_textinput_select_range(input, 1, 3);
    assert(rt_textinput_get_selection_start(input) == 1);
    assert(rt_textinput_get_selection_end(input) == 3);
    rt_string selection = rt_textinput_get_selected_text(input);
    assert(selection);
    assert(strcmp(rt_string_cstr(selection), selected_expected) == 0);
    rt_str_release_maybe(selection);

    assert(rt_textinput_delete_selection(input) == 1);
    assert(rt_textinput_can_undo(input) == 1);
    assert(rt_textinput_can_redo(input) == 0);
    const int64_t edit_revision = rt_textinput_get_revision(input);
    assert(edit_revision > initial_revision);
    assert(rt_textinput_undo(input) == 1);
    assert(rt_textinput_can_redo(input) == 1);
    assert(rt_textinput_redo(input) == 1);
    assert(rt_textinput_get_revision(input) > edit_revision);

    rt_textinput_clear_selection(input);
    assert(rt_textinput_get_selection_start(input) == rt_textinput_get_cursor(input));
    assert(rt_textinput_get_selection_end(input) == rt_textinput_get_cursor(input));
    rt_textinput_set_read_only(input, 1);
    assert(rt_textinput_is_read_only(input) == 1);
    assert(rt_textinput_insert_text(input, rt_const_cstr("blocked")) == 0);
    rt_textinput_set_read_only(input, 0);
    rt_textinput_set_max_length(input, 0);
    assert(rt_textinput_insert_text(input, rt_const_cstr("Q")) == 1);

    assert(vg_textinput_composition_start(input, 1, 0));
    assert(vg_textinput_composition_update(input, preedit, 1, 1));
    assert(rt_textinput_is_composing(input) == 1);
    assert(rt_textinput_get_composition_start(input) == 1);
    assert(rt_textinput_get_composition_length(input) == 2);
    rt_string composition = rt_textinput_get_composition_text(input);
    assert(composition);
    assert(strcmp(rt_string_cstr(composition), preedit) == 0);
    rt_str_release_maybe(composition);
    assert(vg_textinput_composition_cancel(input));
    assert(rt_textinput_is_composing(input) == 0);

    rt_textinput_set_multiline(input, 0);
    vg_event_t enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, 0);
    assert(input->base.vtable->handle_event(&input->base, &enter));
    assert(rt_textinput_was_submitted(input) == 1);
    assert(rt_textinput_was_submitted(input) == 0);
    assert(rt_textinput_was_changed(input) == 1);
    assert(rt_textinput_was_changed(input) == 0);

    cleanup_fake_app(&app);
    printf("test_textinput_runtime_exposes_complete_grapheme_editor: PASSED\n");
}

/// @brief Verify UI zoom rebuilds every spatial theme token and reapplies logical fonts.
/// @details Starts from a clean retained tree so the test also covers the idle-render regression:
///          changing only UI scale must dirty layout and paint immediately. Spatial tokens and
///          the inherited default font double at 2x, while opacity and millisecond motion tokens
///          remain unchanged. The explicit public scale/font getters report their documented
///          logical and effective units.
static void test_ui_scale_invalidates_and_scales_complete_theme(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 14.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_label_t *label = (vg_label_t *)rt_label_new(app.root, rt_const_cstr("scaled"));
    assert(label);
    assert(label->font_size == 14.0f);
    assert(app.theme);
    uint64_t initial_revision = app.theme_revision;
    const vg_theme_t *base = vg_theme_dark();

    app.root->needs_layout = false;
    app.root->needs_paint = false;
    rt_app_set_ui_scale(&app, 2.0);

    assert(rt_app_get_ui_scale(&app) == 2.0);
    assert(rt_app_get_effective_scale(&app) == 2.0);
    assert(rt_app_get_font_size(&app) == 14.0);
    assert(rt_app_get_logical_font_size(&app) == 14.0);
    assert(app.theme_revision == initial_revision + 1);
    assert(app.theme_scale == 2.0f);
    assert(app.theme->spacing.md == base->spacing.md * 2.0f);
    assert(app.theme->radius.sm == base->radius.sm * 2.0f);
    assert(app.theme->radius.pill == base->radius.pill * 2.0f);
    assert(app.theme->elevation.level2.blur == base->elevation.level2.blur * 2.0f);
    assert(app.theme->elevation.level2.dx == base->elevation.level2.dx * 2);
    assert(app.theme->elevation.level2.dy == base->elevation.level2.dy * 2);
    assert(app.theme->elevation.level2.alpha == base->elevation.level2.alpha);
    assert(app.theme->focus.glow_width == base->focus.glow_width * 2.0f);
    assert(app.theme->focus.glow_alpha == base->focus.glow_alpha);
    assert(app.theme->motion.hover_ms == base->motion.hover_ms);
    assert(app.theme->motion.press_ms == base->motion.press_ms);
    assert(app.theme->motion.focus_ms == base->motion.focus_ms);
    assert(label->font_size == 28.0f);
    assert(app.root->needs_layout);
    assert(app.root->needs_paint);

    cleanup_fake_app(&app);
    printf("test_ui_scale_invalidates_and_scales_complete_theme: PASSED\n");
}

/// @brief Verify floating-panel public geometry follows the logical-unit boundary.
/// @details ADR 0106 requires public layout setters to cross effective scale exactly once. A
///          panel that keeps raw 1x dimensions while its children and theme grow at 2x clips every
///          Studio input overlay and positions centered panels outside the visible viewport.
static void test_floatingpanel_geometry_uses_effective_scale(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);
    rt_app_set_ui_scale(&app, 2.0);

    vg_floatingpanel_t *panel = (vg_floatingpanel_t *)rt_floatingpanel_new(&app);
    assert(panel);
    rt_floatingpanel_set_position(panel, 40.0, 50.0);
    rt_floatingpanel_set_size(panel, 320.0, 180.0);

    assert(panel->abs_x == 80.0f);
    assert(panel->abs_y == 100.0f);
    assert(panel->abs_w == 640.0f);
    assert(panel->abs_h == 360.0f);
    rt_floatingpanel_set_visible(panel, 1);
    vg_widget_arrange(&panel->base, 0.0f, 0.0f, 0.0f, 0.0f);
    assert(panel->base.x == 80.0f);
    assert(panel->base.y == 100.0f);
    assert(panel->base.width == 640.0f);
    assert(panel->base.height == 360.0f);
    assert(rt_widget_get_screen_x(panel) == 40.0);
    assert(rt_widget_get_screen_y(panel) == 50.0);
    assert(rt_widget_get_screen_width(panel) == 320.0);
    assert(rt_widget_get_screen_height(panel) == 180.0);

    cleanup_fake_app(&app);
    printf("test_floatingpanel_geometry_uses_effective_scale: PASSED\n");
}

/// @brief Verify the central deadline query covers retained dirtiness and specialized timers.
/// @details A focused text input exposes its remaining 500 ms caret interval, an indeterminate
///          progress bar tightens that deadline to one 60 Hz frame, and any dirty retained node
///          requests immediate work. This prevents `PollWait` from oversleeping GUI-owned timers.
static void test_gui_scheduler_reports_nearest_widget_deadline(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_textinput_t *input = vg_textinput_create(app.root);
    vg_progressbar_t *progress = vg_progressbar_create(app.root);
    assert(input && progress);
    input->base.state |= VG_STATE_FOCUSED;
    input->cursor_blink_time = 0.25f;
    app.root->needs_layout = false;
    app.root->needs_paint = false;
    input->base.needs_layout = false;
    input->base.needs_paint = false;
    progress->base.needs_layout = false;
    progress->base.needs_paint = false;

    assert(rt_gui_app_get_next_deadline_ms(&app) == 250);
    progress->style = VG_PROGRESS_INDETERMINATE;
    assert(rt_gui_app_get_next_deadline_ms(&app) == 16);
    vg_widget_invalidate(&input->base);
    assert(rt_gui_app_get_next_deadline_ms(&app) == 0);

    cleanup_fake_app(&app);
    printf("test_gui_scheduler_reports_nearest_widget_deadline: PASSED\n");
}

static void test_default_font_is_applied_to_complex_text_widgets(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 17.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_treeview_t *tree = (vg_treeview_t *)rt_treeview_new(app.root);
    vg_tabbar_t *tabbar = (vg_tabbar_t *)rt_tabbar_new(app.root);
    vg_codeeditor_t *editor = (vg_codeeditor_t *)rt_codeeditor_new(app.root);
    vg_slider_t *slider = (vg_slider_t *)rt_slider_new(app.root, 1);
    vg_progressbar_t *progress = (vg_progressbar_t *)rt_progressbar_new(app.root);
    vg_spinner_t *spinner = (vg_spinner_t *)rt_spinner_new(app.root);
    vg_menubar_t *menubar = (vg_menubar_t *)rt_menubar_new(app.root);
    vg_toolbar_t *toolbar = (vg_toolbar_t *)rt_toolbar_new(app.root);
    vg_statusbar_t *statusbar = (vg_statusbar_t *)rt_statusbar_new(app.root);
    void *breadcrumb = rt_breadcrumb_new(app.root);
    void *findbar = rt_findbar_new(app.root);
    void *palette = rt_commandpalette_new(app.root);

    assert(tree && tree->font == app.default_font && tree->font_size == app.default_font_size);
    assert(tabbar && tabbar->font == app.default_font &&
           tabbar->font_size == app.default_font_size);
    assert(editor && editor->font == app.default_font &&
           editor->font_size == app.default_font_size);
    assert(slider && slider->font == app.default_font &&
           slider->font_size == app.default_font_size);
    assert(progress && progress->font == app.default_font &&
           progress->font_size == app.default_font_size);
    assert(spinner && spinner->font == app.default_font &&
           spinner->font_size == app.default_font_size);
    assert(menubar && menubar->font == app.default_font &&
           menubar->font_size == app.default_font_size);
    assert(toolbar && toolbar->font == app.default_font &&
           toolbar->font_size == app.default_font_size);
    assert(statusbar && statusbar->font == app.default_font &&
           statusbar->font_size == app.default_font_size);
    rt_breadcrumb_data_view_t *breadcrumb_view = (rt_breadcrumb_data_view_t *)breadcrumb;
    assert(breadcrumb_view && breadcrumb_view->breadcrumb->font == app.default_font &&
           breadcrumb_view->breadcrumb->font_size == app.default_font_size);
    rt_findbar_data_view_t *findbar_view = (rt_findbar_data_view_t *)findbar;
    assert(findbar_view && findbar_view->bar->font == app.default_font &&
           findbar_view->bar->font_size == app.default_font_size);
    rt_commandpalette_data_view_t *palette_view = (rt_commandpalette_data_view_t *)palette;
    assert(palette_view && palette_view->palette->font == app.default_font &&
           palette_view->palette->font_size == app.default_font_size);

    rt_commandpalette_destroy(palette);
    cleanup_fake_app(&app);
    printf("test_default_font_is_applied_to_complex_text_widgets: PASSED\n");
}

// Regression: the IDE parents its status bar to a layout container (a VBox), not to the
// app handle. rt_gui_app_from_handle() does not resolve a container to its owning app,
// so rt_statusbar_new must apply the default font via the active app
// (rt_gui_apply_default_font). Without it, sb->font stays NULL, statusbar_paint
// early-returns before drawing anything, and the whole strip is invisible.
static void test_statusbar_runtime_applies_font_with_container_parent(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 13.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_widget_t *container = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(container);
    vg_widget_add_child(app.root, container);

    vg_statusbar_t *statusbar = (vg_statusbar_t *)rt_statusbar_new(container);
    assert(statusbar);
    assert(statusbar->font == app.default_font);
    assert(statusbar->font_size == app.default_font_size);

    cleanup_fake_app(&app);
    printf("test_statusbar_runtime_applies_font_with_container_parent: PASSED\n");
}

// Regression: a code editor renders text on a fixed char_width grid that must
// match the rendered glyph advances, so it needs a monospace font. The IDE sets
// a proportional chrome font app-wide via App.SetFont, which re-propagates the
// default font across the whole widget tree. An editor that already had its
// (monospace) font set explicitly must be skipped, or its grid desyncs from the
// glyphs and tokens spread apart. Once font_pinned is set, propagation is a no-op
// for that editor; un-pinned editors still follow the app font.
static void test_codeeditor_pinned_font_survives_app_font_propagation(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 17.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_codeeditor_t *pinned = (vg_codeeditor_t *)rt_codeeditor_new(app.root);
    vg_codeeditor_t *unpinned = (vg_codeeditor_t *)rt_codeeditor_new(app.root);
    assert(pinned && unpinned);
    // At creation both inherit the app default font and neither is pinned yet.
    assert(pinned->font == app.default_font && !pinned->font_pinned);

    // Simulate an explicit monospace SetFont on the first editor: a distinct font
    // handle, size, and char_width grid, marked as editor-owned.
    vg_font_t *mono = (vg_font_t *)0x2;
    pinned->font = mono;
    pinned->font_size = 13.0f;
    pinned->char_width = 7.5f;
    pinned->font_pinned = true;

    // App-wide font change (the IDE's proportional chrome font) re-propagates.
    app.default_font = (vg_font_t *)0x3;
    app.default_font_size = 21.0f;
    rt_gui_reapply_default_font(&app);

    // The pinned editor keeps its monospace font and grid untouched…
    assert(pinned->font == mono);
    assert(pinned->font_size == 13.0f);
    assert(pinned->char_width == 7.5f);
    // …while an un-pinned editor still follows the new app font.
    assert(unpinned->font == app.default_font && unpinned->font_pinned == false);

    cleanup_fake_app(&app);
    printf("test_codeeditor_pinned_font_survives_app_font_propagation: PASSED\n");
}

static void test_widget_set_font_rejects_stale_font_handles(void) {
    vg_font_t *sentinel = (vg_font_t *)(uintptr_t)0x1000;
    void *stale = (void *)(uintptr_t)0x12345678;

    vg_label_t *label = vg_label_create(NULL, "label");
    vg_button_t *button = vg_button_create(NULL, "button");
    vg_textinput_t *input = vg_textinput_create(NULL);
    vg_treeview_t *tree = vg_treeview_create(NULL);
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(label && button && input && tree && editor && listbox);

    label->font = sentinel;
    label->font_size = 11.0f;
    button->font = sentinel;
    button->font_size = 12.0f;
    input->font = sentinel;
    input->font_size = 13.0f;
    tree->font = sentinel;
    tree->font_size = 14.0f;
    editor->font = sentinel;
    editor->font_size = 15.0f;
    listbox->font = sentinel;
    listbox->font_size = 16.0f;

    rt_label_set_font(label, stale, 21.0);
    rt_button_set_font(button, stale, 22.0);
    rt_textinput_set_font(input, stale, 23.0);
    rt_treeview_set_font(tree, stale, 24.0);
    rt_codeeditor_set_font(editor, stale, 25.0);
    rt_listbox_set_font(listbox, stale, 26.0);

    assert(label->font == sentinel && label->font_size == 11.0f);
    assert(button->font == sentinel && button->font_size == 12.0f);
    assert(input->font == sentinel && input->font_size == 13.0f);
    assert(tree->font == sentinel && tree->font_size == 14.0f);
    assert(editor->font == sentinel && editor->font_size == 15.0f);
    assert(listbox->font == sentinel && listbox->font_size == 16.0f);

    vg_widget_destroy(&listbox->base);
    vg_widget_destroy(&editor->base);
    vg_widget_destroy(&tree->base);
    vg_widget_destroy(&input->base);
    vg_widget_destroy(&button->base);
    vg_widget_destroy(&label->base);
    printf("test_widget_set_font_rejects_stale_font_handles: PASSED\n");
}

static void test_dropdown_placeholder_is_copied(void) {
    vg_dropdown_t *dropdown = vg_dropdown_create(NULL);
    char placeholder[] = "Choose item";

    vg_dropdown_set_placeholder(dropdown, placeholder);
    placeholder[0] = 'X';

    assert(dropdown->placeholder);
    assert(strcmp(dropdown->placeholder, "Choose item") == 0);

    vg_widget_destroy(&dropdown->base);
    printf("test_dropdown_placeholder_is_copied: PASSED\n");
}

static void test_dialog_content_is_parented(void) {
    vg_dialog_t *dialog = vg_dialog_create("Dialog");
    vg_textinput_t *input = vg_textinput_create(NULL);

    vg_dialog_set_content(dialog, &input->base);

    assert(dialog->content == &input->base);
    assert(input->base.parent == &dialog->base);
    assert(dialog->base.first_child == &input->base);

    vg_widget_destroy(&dialog->base);
    printf("test_dialog_content_is_parented: PASSED\n");
}

static void test_notification_cleanup_runs_for_manual_dismiss(void) {
    vg_notification_manager_t *mgr = vg_notification_manager_create();
    uint32_t id_a = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "A", "first", 0);
    uint32_t id_b = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "B", "second", 0);

    assert(mgr->notification_count == 2);
    vg_notification_dismiss(mgr, id_a);
    vg_notification_manager_update(mgr, 1234);

    assert(mgr->notification_count == 1);
    assert(mgr->notifications[0] && mgr->notifications[0]->id == id_b);

    vg_notification_manager_destroy(mgr);
    printf("test_notification_cleanup_runs_for_manual_dismiss: PASSED\n");
}

static void test_notification_ids_skip_zero_on_wrap(void) {
    vg_notification_manager_t *mgr = vg_notification_manager_create();
    assert(mgr);
    mgr->next_id = UINT32_MAX;

    uint32_t max_id = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "A", "first", 0);
    uint32_t wrapped_id = vg_notification_show(mgr, VG_NOTIFICATION_INFO, "B", "second", 0);

    assert(max_id == UINT32_MAX);
    assert(wrapped_id == 1);
    assert(mgr->notification_count == 2);
    assert(mgr->notifications[0]->id == UINT32_MAX);
    assert(mgr->notifications[1]->id == 1);

    vg_notification_manager_destroy(mgr);
    printf("test_notification_ids_skip_zero_on_wrap: PASSED\n");
}

static void test_command_palette_placeholder_and_utf8_input(void) {
    vg_commandpalette_t *palette = vg_commandpalette_create();
    vg_commandpalette_set_placeholder(palette, "Run action");
    assert(palette->placeholder_text);
    assert(strcmp(palette->placeholder_text, "Run action") == 0);

    vg_commandpalette_show(palette);
    vg_event_t text_event = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, 0x00E9, 0);
    assert(palette->base.vtable->handle_event(&palette->base, &text_event));
    assert(palette->current_query);
    assert(strcmp(palette->current_query, "\xC3\xA9") == 0);

    vg_event_t backspace = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_BACKSPACE, 0, 0);
    assert(palette->base.vtable->handle_event(&palette->base, &backspace));
    assert(palette->current_query == NULL);

    vg_commandpalette_destroy(palette);
    printf("test_command_palette_placeholder_and_utf8_input: PASSED\n");
}

static void test_platform_text_events_translate_to_gui_text(void) {
    vgfx_event_t platform_event = {0};
    platform_event.type = VGFX_EVENT_KEY_DOWN;
    platform_event.data.key.key = VGFX_KEY_A;
    platform_event.data.key.modifiers = VGFX_MOD_SHIFT | VGFX_MOD_CTRL;

    vg_event_t gui_event = vg_event_from_platform(&platform_event);
    assert(gui_event.type == VG_EVENT_KEY_DOWN);
    assert(gui_event.key.key == VG_KEY_A);
    assert(gui_event.modifiers == (VG_MOD_SHIFT | VG_MOD_CTRL));

    platform_event.type = VGFX_EVENT_TEXT_INPUT;
    platform_event.data.text.codepoint = 0x00E9;
    platform_event.data.text.modifiers = VGFX_MOD_ALT;

    gui_event = vg_event_from_platform(&platform_event);
    assert(gui_event.type == VG_EVENT_KEY_CHAR);
    assert(gui_event.key.codepoint == 0x00E9);
    assert(gui_event.modifiers == VG_MOD_ALT);

    printf("test_platform_text_events_translate_to_gui_text: PASSED\n");
}

static void test_platform_scroll_events_keep_screen_coordinates_separate(void) {
    vgfx_event_t platform_event = {0};
    platform_event.type = VGFX_EVENT_SCROLL;
    platform_event.data.scroll.delta_x = 1.25f;
    platform_event.data.scroll.delta_y = -2.5f;
    platform_event.data.scroll.x = 42;
    platform_event.data.scroll.y = 84;

    vg_event_t gui_event = vg_event_from_platform(&platform_event);
    assert(gui_event.type == VG_EVENT_MOUSE_WHEEL);
    assert(gui_event.wheel.delta_x == 1.25f);
    // ZannaGFX expresses positive Y as down; GUI wheel events express it as
    // up. Coordinates remain untouched while only the vertical direction is
    // translated at the subsystem boundary.
    assert(gui_event.wheel.delta_y == 2.5f);
    assert(gui_event.wheel.screen_x == 42.0f);
    assert(gui_event.wheel.screen_y == 84.0f);

    printf("test_platform_scroll_events_keep_screen_coordinates_separate: PASSED\n");
}

static void test_scrollview_runtime_reveals_only_live_descendants(void) {
    vg_scrollview_t *scroll = vg_scrollview_create(NULL);
    assert(scroll);
    scroll->base.width = 120.0f;
    scroll->base.height = 90.0f;
    vg_scrollview_set_content_size(scroll, 120.0f, 500.0f);

    vg_widget_t *section = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *target = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *foreign = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(section && target && foreign);
    section->y = 220.0f;
    section->height = 180.0f;
    target->y = 80.0f;
    target->height = 24.0f;
    vg_widget_add_child(&scroll->base, section);
    vg_widget_add_child(section, target);

    rt_scrollview_scroll_to(scroll, target);
    float viewport_height =
        scroll->base.height - (scroll->show_h_scrollbar ? scroll->scrollbar_width : 0.0f);
    float target_top = section->y + target->y;
    assert(scroll->scroll_y > 0.0f);
    assert(scroll->scroll_y <= target_top);
    assert(scroll->scroll_y + viewport_height >= target_top + target->height);
    assert(scroll->base.needs_layout);
    assert(scroll->base.needs_paint);

    vg_scrollview_set_scroll(scroll, 0.0f, 0.0f);
    rt_scrollview_scroll_to(scroll, foreign);
    assert(scroll->scroll_x == 0.0f);
    assert(scroll->scroll_y == 0.0f);
    rt_scrollview_scroll_to(target, target);
    assert(scroll->scroll_y == 0.0f);

    vg_widget_destroy(foreign);
    vg_widget_destroy(&scroll->base);
    printf("test_scrollview_runtime_reveals_only_live_descendants: PASSED\n");
}

static void test_app_handles_resolve_to_root_widgets_for_overlays(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_floatingpanel_t *panel = (vg_floatingpanel_t *)rt_floatingpanel_new(&app);
    assert(panel);
    assert(panel->base.parent == app.root);

    void *palette_handle = rt_commandpalette_new(&app);
    assert(palette_handle);
    assert(app.command_palette_count == 1);
    assert(app.command_palettes[0] != NULL);

    rt_shortcuts_register(
        rt_const_cstr("palette"), rt_const_cstr("Ctrl+Shift+P"), rt_const_cstr(""));
    assert(app.shortcut_count == 1);
    assert(app.shortcuts != NULL);

    rt_shortcuts_clear();
    rt_commandpalette_destroy(palette_handle);
    cleanup_fake_app(&app);
    printf("test_app_handles_resolve_to_root_widgets_for_overlays: PASSED\n");
}

static void test_codeeditor_runtime_supports_multicursor_editing(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *editor = rt_codeeditor_new(app.root);
    assert(editor);

    assert(rt_codeeditor_get_tab_size(editor) == 4);
    rt_codeeditor_set_tab_size(editor, 12);
    assert(rt_codeeditor_get_tab_size(editor) == 12);
    rt_codeeditor_set_tab_size(editor, 99);
    assert(rt_codeeditor_get_tab_size(editor) == 16);
    assert(rt_codeeditor_get_word_wrap(editor) == 0);
    rt_codeeditor_set_word_wrap(editor, 1);
    assert(rt_codeeditor_get_word_wrap(editor) == 1);

    rt_codeeditor_set_text(editor, rt_const_cstr("abc\nabc"));
    rt_codeeditor_add_cursor(editor, 99, 99);
    assert(rt_codeeditor_get_cursor_count(editor) == 2);
    assert(rt_codeeditor_get_cursor_line_at(editor, 1) == 1);
    assert(rt_codeeditor_get_cursor_col_at(editor, 1) == 3);

    rt_codeeditor_set_cursor_selection(editor, 0, 1, 2, 0, 1);
    assert(rt_codeeditor_cursor_has_selection(editor, 0) == 1);
    assert(rt_codeeditor_get_selection_start_line_at(editor, 0) == 0);
    assert(rt_codeeditor_get_selection_start_col_at(editor, 0) == 1);
    assert(rt_codeeditor_get_selection_end_line_at(editor, 0) == 1);
    assert(rt_codeeditor_get_selection_end_col_at(editor, 0) == 2);

    rt_codeeditor_set_cursor_selection(editor, 1, 1, 0, 1, 2);
    assert(rt_codeeditor_cursor_has_selection(editor, 1) == 1);
    assert(rt_codeeditor_get_selection_start_line_at(editor, 1) == 1);
    assert(rt_codeeditor_get_selection_start_col_at(editor, 1) == 0);
    assert(rt_codeeditor_get_selection_end_line_at(editor, 1) == 1);
    assert(rt_codeeditor_get_selection_end_col_at(editor, 1) == 2);

    rt_codeeditor_set_cursor_position_at(editor, 0, 0, 1);
    rt_codeeditor_set_cursor_position_at(editor, 1, 1, 1);
    assert(rt_codeeditor_get_selection_end_col_at(editor, 99) == 0);
    assert(rt_codeeditor_cursor_has_selection(editor, 1) == 0);

    rt_codeeditor_insert_at_cursor(editor, rt_const_cstr("X"));
    assert(rt_codeeditor_can_undo(editor) == 1);
    assert(rt_codeeditor_can_redo(editor) == 0);
    rt_string text = rt_codeeditor_get_text(editor);
    assert(strcmp(rt_string_cstr(text), "aXbc\naXbc") == 0);

    rt_codeeditor_undo(editor);
    assert(rt_codeeditor_can_redo(editor) == 1);
    text = rt_codeeditor_get_text(editor);
    assert(strcmp(rt_string_cstr(text), "abc\nabc") == 0);

    cleanup_fake_app(&app);
    printf("test_codeeditor_runtime_supports_multicursor_editing: PASSED\n");
}

/// @brief Verify CodeEditor.GetPerfStats publishes every raw counter under stable keys.
/// @details A seeded lower-editor snapshot makes each field independently distinguishable. The
///          consolidated Map must preserve all nine values, agree with legacy aggregate getters,
///          and retain its complete schema after ResetPerfStats clears the underlying counters.
static void test_codeeditor_consolidated_perf_stats_schema(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_codeeditor_t *editor = (vg_codeeditor_t *)rt_codeeditor_new(app.root);
    assert(editor);
    editor->perf_stats.total_height_linear_scans = 11;
    editor->perf_stats.total_visual_row_linear_scans = 12;
    editor->perf_stats.visual_row_linear_scans = 13;
    editor->perf_stats.locate_visual_row_linear_scans = 14;
    editor->perf_stats.line_highlight_calls = 15;
    editor->perf_stats.syntax_state_line_scans = 16;
    editor->perf_stats.highlight_span_checks = 17;
    editor->perf_stats.full_text_copies = 18;
    editor->perf_stats.full_text_copy_bytes = 19;

    void *stats = rt_codeeditor_get_perf_stats(editor);
    assert(stats);
    assert(rt_map_get_int(stats, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_get_int(stats, rt_const_cstr("totalHeightLinearScans")) == 11);
    assert(rt_map_get_int(stats, rt_const_cstr("totalVisualRowLinearScans")) == 12);
    assert(rt_map_get_int(stats, rt_const_cstr("visualRowLinearScans")) == 13);
    assert(rt_map_get_int(stats, rt_const_cstr("locateVisualRowLinearScans")) == 14);
    assert(rt_map_get_int(stats, rt_const_cstr("lineHighlightCalls")) == 15);
    assert(rt_map_get_int(stats, rt_const_cstr("syntaxStateLineScans")) == 16);
    assert(rt_map_get_int(stats, rt_const_cstr("highlightSpanChecks")) == 17);
    assert(rt_map_get_int(stats, rt_const_cstr("fullTextCopies")) == 18);
    assert(rt_map_get_int(stats, rt_const_cstr("fullTextCopyBytes")) == 19);
    assert(rt_codeeditor_get_layout_linear_scan_count(editor) == 50);
    assert(rt_codeeditor_get_syntax_highlight_call_count(editor) == 15);
    assert(rt_codeeditor_get_syntax_state_line_scan_count(editor) == 16);
    assert(rt_codeeditor_get_highlight_span_check_count(editor) == 17);
    assert(rt_codeeditor_get_full_text_copy_count(editor) == 18);
    assert(rt_codeeditor_get_full_text_copy_byte_count(editor) == 19);
    release_test_runtime_object(stats);

    rt_codeeditor_reset_perf_stats(editor);
    stats = rt_codeeditor_get_perf_stats(editor);
    assert(stats);
    assert(rt_map_get_int(stats, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_get_int(stats, rt_const_cstr("totalHeightLinearScans")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("fullTextCopyBytes")) == 0);
    release_test_runtime_object(stats);

    cleanup_fake_app(&app);
    printf("test_codeeditor_consolidated_perf_stats_schema: PASSED\n");
}

static void test_codeeditor_set_text_round_trips_embedded_nuls(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);

    const char bytes[] = {'a', 'b', '\0', 'c', 'd'};
    rt_codeeditor_set_text(editor, rt_string_from_bytes(bytes, sizeof(bytes)));
    assert(editor->line_count == 1);
    assert(editor->lines[0].length == sizeof(bytes));
    assert(memcmp(editor->lines[0].text, bytes, sizeof(bytes)) == 0);

    rt_string text = rt_codeeditor_get_text(editor);
    assert(rt_str_len(text) == (int64_t)sizeof(bytes));
    assert(memcmp(rt_string_cstr(text), bytes, sizeof(bytes)) == 0);

    vg_widget_destroy(&editor->base);
    printf("test_codeeditor_set_text_round_trips_embedded_nuls: PASSED\n");
}

static void test_codeeditor_runtime_pixel_helpers_follow_scroll_and_wrap(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);

    vg_codeeditor_set_text(editor, "abcdefghi\nabcdef");
    editor->base.x = 100.0f;
    editor->base.y = 50.0f;
    editor->base.width = 70.0f;
    editor->base.height = 20.0f;
    editor->char_width = 10.0f;
    editor->line_height = 10.0f;
    editor->gutter_width = 20.0f;

    editor->cursor_line = 1;
    editor->cursor_col = 4;
    editor->scroll_x = 15.0f;
    editor->scroll_y = 5.0f;
    assert(rt_codeeditor_get_cursor_pixel_x(editor) == 145);
    assert(rt_codeeditor_get_cursor_pixel_y(editor) == 55);
    assert(rt_codeeditor_get_line_at_pixel(editor, 55) == 1);
    assert(rt_codeeditor_get_col_at_pixel(editor, 145, 55) == 4);

    rt_codeeditor_set_word_wrap(editor, 1);
    editor->gutter_width = 20.0f;
    editor->scroll_y = 10.0f;
    editor->cursor_line = 0;
    editor->cursor_col = 6;
    assert(rt_codeeditor_get_cursor_pixel_x(editor) == 120);
    assert(rt_codeeditor_get_cursor_pixel_y(editor) == 60);
    assert(rt_codeeditor_get_line_at_pixel(editor, 60) == 0);
    assert(rt_codeeditor_get_line_at_pixel(editor, 70) == 1);
    assert(rt_codeeditor_get_col_at_pixel(editor, 140, 60) == 8);

    editor->base.width = editor->gutter_width;
    editor->scroll_y = 0.0f;
    assert(rt_codeeditor_get_col_at_pixel(editor, 130, 50) == 1);

    vg_widget_destroy(&editor->base);
    printf("test_codeeditor_runtime_pixel_helpers_follow_scroll_and_wrap: PASSED\n");
}

static void test_codeeditor_runtime_scroll_top_line_round_trips(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);

    vg_codeeditor_set_text(editor, "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten");
    editor->base.width = 240.0f;
    editor->base.height = 30.0f;
    editor->char_width = 10.0f;
    editor->line_height = 10.0f;
    editor->gutter_width = 20.0f;

    rt_codeeditor_set_scroll_top_line(editor, 4);
    assert(rt_codeeditor_get_scroll_top_line(editor) == 4);

    rt_codeeditor_set_scroll_top_line(editor, -10);
    assert(rt_codeeditor_get_scroll_top_line(editor) == 0);

    rt_codeeditor_set_scroll_top_line(editor, 999);
    assert(rt_codeeditor_get_scroll_top_line(editor) >= 0);
    assert(rt_codeeditor_get_scroll_top_line(editor) < editor->line_count);

    rt_codeeditor_set_word_wrap(editor, 1);
    editor->base.width = editor->gutter_width + 35.0f;
    rt_codeeditor_set_scroll_top_line(editor, 2);
    assert(rt_codeeditor_get_scroll_top_line(editor) == 2);

    vg_widget_destroy(&editor->base);
    printf("test_codeeditor_runtime_scroll_top_line_round_trips: PASSED\n");
}

static void test_codeeditor_modifier_click_selection_and_extra_cursors(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);

    vg_codeeditor_set_text(editor, "alpha\nbeta");
    editor->base.width = 200.0f;
    editor->base.height = 40.0f;
    editor->char_width = 10.0f;
    editor->line_height = 10.0f;
    editor->gutter_width = 20.0f;
    editor->cursor_line = 0;
    editor->cursor_col = 0;

    vg_event_t shift_down =
        vg_event_mouse(VG_EVENT_MOUSE_DOWN, 40.0f, 15.0f, VG_MOUSE_LEFT, VG_MOD_SHIFT);
    assert(editor->base.vtable->handle_event(&editor->base, &shift_down));
    assert(editor->cursor_line == 1);
    assert(editor->cursor_col == 2);
    assert(editor->has_selection);
    assert(editor->selection.start_line == 0);
    assert(editor->selection.start_col == 0);
    assert(editor->selection.end_line == 1);
    assert(editor->selection.end_col == 2);

    vg_event_t ctrl_down =
        vg_event_mouse(VG_EVENT_MOUSE_DOWN, 50.0f, 5.0f, VG_MOUSE_LEFT, VG_MOD_CTRL);
    assert(editor->base.vtable->handle_event(&editor->base, &ctrl_down));
    assert(editor->extra_cursor_count == 1);
    assert(editor->extra_cursors[0].line == 0);
    assert(editor->extra_cursors[0].col == 3);

    assert(editor->base.vtable->handle_event(&editor->base, &ctrl_down));
    assert(editor->extra_cursor_count == 1);

    vg_widget_destroy(&editor->base);
    printf("test_codeeditor_modifier_click_selection_and_extra_cursors: PASSED\n");
}

static void test_codeeditor_runtime_fold_helpers_skip_hidden_lines(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);

    vg_codeeditor_set_text(editor, "one\ntwo\nthree\nfour");
    editor->base.x = 100.0f;
    editor->base.y = 50.0f;
    editor->base.width = 120.0f;
    editor->base.height = 40.0f;
    editor->char_width = 8.0f;
    editor->line_height = 10.0f;

    rt_codeeditor_set_show_line_numbers(editor, 0);
    rt_codeeditor_set_show_fold_gutter(editor, 1);
    rt_codeeditor_add_fold_region(editor, 0, 2);
    rt_codeeditor_fold(editor, 0);

    assert(editor->gutter_width > 0.0f);
    assert(rt_codeeditor_get_line_at_pixel(editor, 55) == 0);
    assert(rt_codeeditor_get_line_at_pixel(editor, 65) == 3);

    vg_codeeditor_set_cursor(editor, 2, 2);
    assert(editor->cursor_line == 0);

    vg_widget_destroy(&editor->base);
    printf("test_codeeditor_runtime_fold_helpers_skip_hidden_lines: PASSED\n");
}

static void test_codeeditor_line_number_width_override_tracks_character_width(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);

    editor->char_width = 8.0f;
    rt_codeeditor_set_line_number_width(editor, 4);
    assert(editor->gutter_width == 32.0f);

    editor->char_width = 12.0f;
    vg_codeeditor_refresh_layout_state(editor);
    assert(editor->gutter_width == 48.0f);

    vg_widget_destroy(&editor->base);
    printf("test_codeeditor_line_number_width_override_tracks_character_width: PASSED\n");
}

static void test_zia_block_comment_highlighting_is_render_order_independent(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);

    rt_codeeditor_set_text(editor, rt_const_cstr("/*\ninside\n*/\nlet value = 1"));
    rt_codeeditor_set_language(editor, rt_const_cstr("zia"));
    rt_codeeditor_set_token_color(editor, 0, 0xFF222222);
    rt_codeeditor_set_token_color(editor, 4, 0xFF111111);

    uint32_t inside_colors[16] = {0};
    editor->syntax_highlighter(
        &editor->base, 1, editor->lines[1].text, inside_colors, editor->syntax_data);
    for (size_t i = 0; i < editor->lines[1].length; i++)
        assert(inside_colors[i] == 0xFF111111);

    uint32_t after_colors[32] = {0};
    editor->syntax_highlighter(
        &editor->base, 3, editor->lines[3].text, after_colors, editor->syntax_data);
    assert(after_colors[0] != 0xFF111111);

    vg_widget_destroy(&editor->base);
    printf("test_zia_block_comment_highlighting_is_render_order_independent: PASSED\n");
}

static void test_zia_string_highlighting_trailing_escape_stays_in_bounds(void) {
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);
    rt_codeeditor_set_text(editor, rt_const_cstr("\"abc\\"));
    rt_codeeditor_set_language(editor, rt_const_cstr("zia"));
    rt_codeeditor_set_token_color(editor, 3, 0xFF123456);

    uint32_t colors[6] = {0};
    colors[5] = 0xDEADBEEF;
    editor->syntax_highlighter(
        &editor->base, 0, editor->lines[0].text, colors, editor->syntax_data);
    assert(colors[5] == 0xDEADBEEF);
    for (size_t i = 0; i < editor->lines[0].length; i++)
        assert(colors[i] == 0xFF123456);

    vg_widget_destroy(&editor->base);
    printf("test_zia_string_highlighting_trailing_escape_stays_in_bounds: PASSED\n");
}

static void test_findbar_runtime_reads_live_text_and_reports_noop_replace(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *editor = rt_codeeditor_new(app.root);
    void *bar = rt_findbar_new(app.root);
    assert(editor);
    assert(bar);

    rt_codeeditor_set_text(editor, rt_const_cstr("alpha beta"));
    rt_findbar_bind_editor(bar, editor);
    rt_findbar_set_find_text(bar, rt_const_cstr("alpha"));
    rt_findbar_set_replace_text(bar, rt_const_cstr("omega"));
    rt_findbar_set_replace_mode(bar, 42);
    rt_findbar_set_case_sensitive(bar, -7);
    rt_findbar_set_whole_word(bar, 123);
    rt_findbar_set_regex(bar, 9);
    assert(rt_findbar_is_replace_mode(bar) == 1);
    assert(rt_findbar_is_case_sensitive(bar) == 1);
    assert(rt_findbar_is_whole_word(bar) == 1);
    assert(rt_findbar_is_regex(bar) == 1);

    rt_findbar_data_view_t *view = (rt_findbar_data_view_t *)bar;
    view->bar->show_replace = false;
    view->bar->options.case_sensitive = false;
    view->bar->options.whole_word = false;
    view->bar->options.use_regex = false;
    assert(rt_findbar_is_replace_mode(bar) == 0);
    assert(rt_findbar_is_case_sensitive(bar) == 0);
    assert(rt_findbar_is_whole_word(bar) == 0);
    assert(rt_findbar_is_regex(bar) == 0);

    rt_findbar_set_find_text(bar, rt_const_cstr("alpha"));
    assert(rt_findbar_find_next(bar) == 1);
    assert(rt_findbar_get_match_count(bar) == 1);
    assert(rt_findbar_get_current_match(bar) == 1);

    vg_textinput_set_text((vg_textinput_t *)view->bar->find_input, "beta");
    vg_textinput_set_text((vg_textinput_t *)view->bar->replace_input, "theta");

    assert(strcmp(rt_string_cstr(rt_findbar_get_find_text(bar)), "beta") == 0);
    assert(strcmp(rt_string_cstr(rt_findbar_get_replace_text(bar)), "theta") == 0);

    rt_findbar_set_find_text(bar, rt_const_cstr("missing"));
    assert(rt_findbar_find_next(bar) == 0);
    assert(rt_findbar_replace(bar) == 0);

    cleanup_fake_app(&app);
    printf("test_findbar_runtime_reads_live_text_and_reports_noop_replace: PASSED\n");
}

static void test_menu_and_toolbar_pixel_icons_become_image_icons(void) {
    void *pixels = rt_pixels_new(1, 1);
    assert(pixels);
    rt_pixels_fill(pixels, 0x11223344);

    vg_menubar_t *menubar = vg_menubar_create(NULL);
    assert(menubar);
    vg_menu_t *menu = vg_menubar_add_menu(menubar, "File");
    assert(menu);
    vg_menu_item_t *item = vg_menu_add_item(menu, "Open", NULL, NULL, NULL);
    assert(item);

    void *item_handle = rt_gui_wrap_menu_item(item);
    rt_menuitem_set_icon(item_handle, pixels);
    assert(item->icon.type == VG_ICON_IMAGE);

    vg_contextmenu_t *context_menu = vg_contextmenu_create();
    assert(context_menu);
    vg_menu_item_t *context_item = vg_contextmenu_add_item(context_menu, "Copy", NULL, NULL, NULL);
    assert(context_item);

    void *context_item_handle = rt_gui_wrap_menu_item(context_item);
    rt_menuitem_set_icon(context_item_handle, pixels);
    assert(context_item->icon.type == VG_ICON_IMAGE);
    assert(context_item->owner_contextmenu == context_menu);

    vg_toolbar_t *toolbar = vg_toolbar_create(NULL, VG_TOOLBAR_HORIZONTAL);
    assert(toolbar);
    vg_toolbar_item_t *tool_item =
        vg_toolbar_add_button(toolbar, "open", NULL, vg_icon_from_glyph('O'), NULL, NULL);
    assert(tool_item);

    void *tool_item_handle = rt_gui_wrap_toolbar_item(tool_item);
    rt_toolbaritem_set_icon_pixels(tool_item_handle, pixels);
    assert(tool_item->icon.type == VG_ICON_IMAGE);

    char icon_path[256];
#ifdef _WIN32
    snprintf(icon_path, sizeof(icon_path), "zanna_gui_icon_test.bmp");
#else
    snprintf(icon_path, sizeof(icon_path), "/tmp/zanna_gui_icon_%ld.bmp", (long)getpid());
#endif
    assert(rt_pixels_save_bmp(pixels, rt_const_cstr(icon_path)) == 1);
    void *path_item_handle =
        rt_toolbar_add_button(toolbar, rt_const_cstr(icon_path), rt_const_cstr("path"));
    vg_toolbar_item_t *path_item = rt_gui_toolbar_item_from_handle(path_item_handle);
    assert(path_item);
    assert(path_item->icon.type == VG_ICON_IMAGE);

    rt_toolbaritem_set_icon(path_item_handle, rt_const_cstr(icon_path));
    assert(path_item->icon.type == VG_ICON_IMAGE);

#ifdef _WIN32
    remove(icon_path);
#else
    unlink(icon_path);
#endif

    vg_widget_destroy(&toolbar->base);
    vg_widget_destroy(&context_menu->base);
    vg_widget_destroy(&menubar->base);
    printf("test_menu_and_toolbar_pixel_icons_become_image_icons: PASSED\n");
}

static void test_toolbar_named_icons_prefer_vector_icons(void) {
    vg_toolbar_t *toolbar = vg_toolbar_create(NULL, VG_TOOLBAR_HORIZONTAL);
    assert(toolbar);

    void *run_handle =
        rt_toolbar_add_named_button(toolbar, rt_const_cstr("run"), rt_const_cstr("Run"));
    vg_toolbar_item_t *run_item = rt_gui_toolbar_item_from_handle(run_handle);
    assert(run_item);
    assert(run_item->icon.type == VG_ICON_VECTOR);
    assert(run_item->icon.data.vector_id >= 0);
    int32_t run_vector_id = run_item->icon.data.vector_id;

    void *label_handle = rt_toolbar_add_named_button_with_text(toolbar,
                                                               rt_const_cstr("source-control"),
                                                               rt_const_cstr("SCM"),
                                                               rt_const_cstr("Source Control"));
    vg_toolbar_item_t *label_item = rt_gui_toolbar_item_from_handle(label_handle);
    assert(label_item);
    assert(label_item->icon.type == VG_ICON_VECTOR);
    assert(label_item->icon.data.vector_id >= 0);
    assert(label_item->icon.data.vector_id != run_vector_id);
    assert(label_item->show_label);

    void *toggle_handle =
        rt_toolbar_add_named_toggle(toolbar, rt_const_cstr("explorer"), rt_const_cstr("Explorer"));
    vg_toolbar_item_t *toggle_item = rt_gui_toolbar_item_from_handle(toggle_handle);
    assert(toggle_item);
    assert(toggle_item->icon.type == VG_ICON_VECTOR);
    assert(toggle_item->icon.data.vector_id >= 0);

    rt_toolbaritem_set_named_icon(run_handle, rt_const_cstr("save-all"));
    assert(run_item->icon.type == VG_ICON_VECTOR);
    assert(run_item->icon.data.vector_id >= 0);
    assert(run_item->icon.data.vector_id != run_vector_id);

    // Alias names absent from the vector registry keep the builtin glyph
    // fallback (ADR 0137).
    rt_toolbaritem_set_named_icon(run_handle, rt_const_cstr("play"));
    assert(run_item->icon.type == VG_ICON_GLYPH);
    assert(run_item->icon.data.glyph == 0x25B6u);

    rt_toolbaritem_set_named_icon(run_handle, rt_const_cstr("does-not-exist"));
    assert(run_item->icon.type == VG_ICON_NONE);

    vg_widget_destroy(&toolbar->base);
    printf("test_toolbar_named_icons_prefer_vector_icons: PASSED\n");
}

static void test_splitpane_runtime_boolean_matches_horizontal_semantics(void) {
    vg_splitpane_t *horizontal = (vg_splitpane_t *)rt_splitpane_new(NULL, 1);
    vg_splitpane_t *vertical = (vg_splitpane_t *)rt_splitpane_new(NULL, 0);

    assert(horizontal);
    assert(vertical);
    assert(horizontal->direction == VG_SPLIT_HORIZONTAL);
    assert(vertical->direction == VG_SPLIT_VERTICAL);

    vg_widget_destroy(&horizontal->base);
    vg_widget_destroy(&vertical->base);
    printf("test_splitpane_runtime_boolean_matches_horizontal_semantics: PASSED\n");
}

static void test_tabbar_was_changed_tracks_real_active_tab_transitions(void) {
    vg_tabbar_t *tabbar = vg_tabbar_create(NULL);
    assert(tabbar);

    vg_tab_t *first = vg_tabbar_add_tab(tabbar, "first.zia", true);
    vg_tab_t *second = vg_tabbar_add_tab(tabbar, "second.zia", true);
    assert(first);
    assert(second);
    void *first_handle = rt_gui_wrap_tab(first);
    void *second_handle = rt_gui_wrap_tab(second);

    assert(rt_tabbar_was_changed(tabbar) == 0);
    rt_tabbar_set_active(tabbar, second_handle);
    assert(rt_tabbar_was_changed(tabbar) == 1);
    assert(rt_tabbar_was_changed(tabbar) == 0);

    rt_tabbar_remove_tab(tabbar, second_handle);
    assert(rt_tabbar_get_active(tabbar) == first_handle);
    assert(rt_tabbar_was_changed(tabbar) == 1);
    assert(rt_tabbar_was_changed(tabbar) == 0);

    vg_widget_destroy(&tabbar->base);
    printf("test_tabbar_was_changed_tracks_real_active_tab_transitions: PASSED\n");
}

static void test_widget_set_position_marks_widget_dirty(void) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(widget);
    widget->needs_layout = false;
    widget->needs_paint = false;

    rt_widget_set_position(widget, 12, 34);
    assert(widget->x == 12.0f);
    assert(widget->y == 34.0f);
    assert(widget->manual_position);
    assert(!widget->needs_layout);
    assert(widget->needs_paint);

    vg_widget_destroy(widget);
    printf("test_widget_set_position_marks_widget_dirty: PASSED\n");
}

static void test_tabbar_close_click_index_survives_auto_close(void) {
    vg_tabbar_t *tabbar = vg_tabbar_create(NULL);
    assert(tabbar);

    vg_tab_t *tab = vg_tabbar_add_tab(tabbar, "main.zia", true);
    assert(tab);
    tabbar->base.x = 0.0f;
    tabbar->base.y = 0.0f;
    tabbar->base.width = 200.0f;
    tabbar->base.height = tabbar->tab_height;

    vg_event_t down = {0};
    down.type = VG_EVENT_MOUSE_DOWN;
    down.mouse.x = 180.0f;
    down.mouse.y = tabbar->tab_height / 2.0f;
    down.mouse.screen_x = down.mouse.x;
    down.mouse.screen_y = down.mouse.y;
    assert(vg_event_send(&tabbar->base, &down));

    vg_event_t up = down;
    up.type = VG_EVENT_MOUSE_UP;
    assert(vg_event_send(&tabbar->base, &up));

    assert(tabbar->tab_count == 0);
    assert(rt_tabbar_was_close_clicked(tabbar) == 1);
    assert(rt_tabbar_get_close_clicked_index(tabbar) == 0);
    assert(rt_tabbar_was_close_clicked(tabbar) == 0);
    assert(rt_tabbar_get_close_clicked_index(tabbar) == -1);

    vg_widget_destroy(&tabbar->base);
    printf("test_tabbar_close_click_index_survives_auto_close: PASSED\n");
}

static void test_widget_destroy_refuses_app_root_and_app_handle(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(child);
    vg_widget_add_child(app.root, child);
    assert(app.root->child_count == 1);

    rt_widget_destroy(app.root);
    assert(app.root != NULL);
    assert(app.root->child_count == 1);

    rt_widget_destroy(&app);
    assert(app.root != NULL);
    assert(app.root->child_count == 1);

    cleanup_fake_app(&app);
    printf("test_widget_destroy_refuses_app_root_and_app_handle: PASSED\n");
}

static void test_widget_set_size_refuses_app_root(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    rt_widget_set_size(app.root, 320, 200);
    rt_widget_set_preferred_size(app.root, 480.0, 320.0);
    rt_widget_set_max_size(app.root, 640.0, 480.0);
    assert(app.root->constraints.min_width == 0.0f);
    assert(app.root->constraints.max_width == 0.0f);
    assert(app.root->constraints.preferred_width == 0.0f);
    assert(app.root->constraints.min_height == 0.0f);
    assert(app.root->constraints.max_height == 0.0f);
    assert(app.root->constraints.preferred_height == 0.0f);

    cleanup_fake_app(&app);
    printf("test_widget_set_size_refuses_app_root: PASSED\n");
}

static void test_widget_base_apis_reject_app_handles_and_invalid_children(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    rt_widget_set_visible(&app, 0);
    rt_widget_set_enabled(&app, 0);
    rt_widget_set_size(&app, 20, 30);
    rt_widget_set_flex(&app, 2.0);
    rt_widget_set_margin(&app, 5);
    rt_widget_set_tab_index(&app, 3);

    assert(rt_widget_is_visible(&app) == 0);
    assert(rt_widget_is_enabled(&app) == 0);
    assert(rt_widget_get_width(&app) == 0);
    assert(rt_widget_get_height(&app) == 0);
    assert(rt_widget_get_x(&app) == 0);
    assert(rt_widget_get_y(&app) == 0);
    assert(rt_widget_get_flex(&app) == 0.0);

    rt_widget_add_child(&app, &app);
    assert(app.root->child_count == 0);

    cleanup_fake_app(&app);
    printf("test_widget_base_apis_reject_app_handles_and_invalid_children: PASSED\n");
}

static void test_runtime_widget_parent_validation_rejects_leaf_and_invalid_handles(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_button_t *leaf_parent = (vg_button_t *)rt_button_new(app.root, rt_const_cstr("button"));
    assert(leaf_parent);
    vg_label_t *app_parented = (vg_label_t *)rt_label_new(&app, rt_const_cstr("app parent"));
    assert(app_parented && app_parented->base.parent == app.root);
    assert(rt_label_new(leaf_parent, rt_const_cstr("bad leaf parent")) == NULL);
    assert(rt_slider_new(leaf_parent, 1) == NULL);
    assert(rt_menubar_new(leaf_parent) == NULL);
    assert(rt_toolbar_new(leaf_parent) == NULL);
    assert(rt_statusbar_new(leaf_parent) == NULL);
    assert(rt_findbar_new(leaf_parent) == NULL);
    assert(rt_breadcrumb_new(leaf_parent) == NULL);
    assert(rt_minimap_new(leaf_parent) == NULL);
    assert(rt_floatingpanel_new(leaf_parent) == NULL);

    int invalid_handle = 0;
    assert(rt_button_new(&invalid_handle, rt_const_cstr("bad invalid parent")) == NULL);

    vg_label_t *detached = (vg_label_t *)rt_label_new(NULL, rt_const_cstr("detached"));
    assert(detached);
    assert(detached->base.parent == NULL);
    rt_widget_add_child(leaf_parent, detached);
    assert(detached->base.parent == NULL);
    rt_widget_add_child(app.root, detached);
    assert(detached->base.parent == app.root);

    cleanup_fake_app(&app);
    printf("test_runtime_widget_parent_validation_rejects_leaf_and_invalid_handles: PASSED\n");
}

static void test_widget_destroy_clears_nested_toolbar_statusbar_runtime_refs(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_widget_t *container = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(container);
    vg_widget_add_child(app.root, container);

    vg_statusbar_t *statusbar = vg_statusbar_create(container);
    assert(statusbar);
    vg_statusbar_item_t *status_item =
        vg_statusbar_add_text(statusbar, VG_STATUSBAR_ZONE_LEFT, "ready");
    assert(status_item);

    vg_toolbar_t *toolbar = vg_toolbar_create(container, VG_TOOLBAR_HORIZONTAL);
    assert(toolbar);
    vg_toolbar_item_t *tool_item =
        vg_toolbar_add_button(toolbar, "open", NULL, vg_icon_from_glyph('O'), NULL, NULL);
    assert(tool_item);

    app.last_statusbar_clicked = status_item;
    app.last_toolbar_clicked = tool_item;
    rt_widget_destroy(container);

    assert(app.last_statusbar_clicked == NULL);
    assert(app.last_toolbar_clicked == NULL);
    assert(app.root->child_count == 0);

    cleanup_fake_app(&app);
    printf("test_widget_destroy_clears_nested_toolbar_statusbar_runtime_refs: PASSED\n");
}

static void test_widget_focus_null_is_noop(void) {
    rt_widget_focus(NULL);
    printf("test_widget_focus_null_is_noop: PASSED\n");
}

static void test_image_set_pixels_converts_zanna_pixels_to_rgba(void) {
    void *pixels = rt_pixels_new(1, 1);
    assert(pixels);
    rt_pixels_set(pixels, 0, 0, 0x11223344);

    vg_image_t *image = vg_image_create(NULL);
    assert(image);
    rt_image_set_pixels(image, pixels, 0, 0);

    assert(image->img_width == 1);
    assert(image->img_height == 1);
    assert(image->pixels);
    assert(image->pixels[0] == 0x11);
    assert(image->pixels[1] == 0x22);
    assert(image->pixels[2] == 0x33);
    assert(image->pixels[3] == 0x44);
    assert(rt_image_get_filter(image) == RT_IMAGE_FILTER_NEAREST);
    rt_image_set_filter(image, RT_IMAGE_FILTER_BILINEAR);
    assert(rt_image_get_filter(image) == RT_IMAGE_FILTER_BILINEAR);

    rt_image_set_pixels(image, NULL, 0, 0);
    assert(image->pixels == NULL);
    assert(image->img_width == 0);
    assert(image->img_height == 0);

    vg_widget_destroy(&image->base);
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);
    printf("test_image_set_pixels_converts_zanna_pixels_to_rgba: PASSED\n");
}

/// @brief Verify status-returning image upload and source/destination region conversion.
/// @details Invalid regions must preserve both pixel bytes and the lower content revision.
static void test_image_atomic_runtime_mutation_contract(void) {
    void *initial = rt_pixels_new(2, 2);
    void *patch = rt_pixels_new(2, 2);
    assert(initial && patch);
    rt_pixels_set(initial, 0, 0, 0x010203FF);
    rt_pixels_set(initial, 1, 0, 0x111213FF);
    rt_pixels_set(initial, 0, 1, 0x212223FF);
    rt_pixels_set(initial, 1, 1, 0x313233FF);
    rt_pixels_set(patch, 0, 0, 0xA0A1A2FF);
    rt_pixels_set(patch, 1, 0, 0xB0B1B280);
    rt_pixels_set(patch, 0, 1, 0xC0C1C2FF);
    rt_pixels_set(patch, 1, 1, 0xD0D1D2FF);

    vg_image_t *image = vg_image_create(NULL);
    assert(image);
    assert(rt_image_try_set_pixels(image, initial, 0, 0) == 1);
    uint8_t *storage = image->pixels;
    const size_t capacity = image->pixel_capacity;
    assert(rt_image_try_set_pixels(image, initial, 2, 2) == 1);
    assert(image->pixels == storage && image->pixel_capacity == capacity);

    assert(rt_image_update_region(image, patch, 1, 0, 1, 1, 0, 1) == 1);
    assert(image->pixels[8] == 0xB0);
    assert(image->pixels[9] == 0xB1);
    assert(image->pixels[10] == 0xB2);
    assert(image->pixels[11] == 0x80);
    const uint64_t revision = image->content_revision;
    uint8_t snapshot[16];
    memcpy(snapshot, image->pixels, sizeof(snapshot));

    assert(rt_image_update_region(image, patch, 1, 1, 2, 1, 0, 0) == 0);
    assert(memcmp(image->pixels, snapshot, sizeof(snapshot)) == 0);
    assert(image->content_revision == revision);
    assert(rt_image_try_set_pixels(image, NULL, 0, 0) == 0);
    assert(memcmp(image->pixels, snapshot, sizeof(snapshot)) == 0);

    vg_widget_destroy(&image->base);
    if (rt_obj_release_check0(initial))
        rt_obj_free(initial);
    if (rt_obj_release_check0(patch))
        rt_obj_free(patch);
    printf("test_image_atomic_runtime_mutation_contract: PASSED\n");
}

static void test_treeview_and_listbox_data_preserve_embedded_nuls(void) {
    char payload[] = {'a', '\0', 'b'};
    rt_string data = rt_string_from_bytes(payload, sizeof(payload));

    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);
    vg_tree_node_t *node = vg_treeview_add_node(tree, NULL, "node");
    assert(node);
    void *node_handle = rt_gui_wrap_tree_node(node);
    rt_treeview_node_set_data(node_handle, data);
    rt_string tree_data = rt_treeview_node_get_data(node_handle);
    assert(rt_str_len(tree_data) == (int64_t)sizeof(payload));
    assert(memcmp(rt_string_cstr(tree_data), payload, sizeof(payload)) == 0);

    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);
    vg_listbox_item_t *item = vg_listbox_add_item(listbox, "item", NULL);
    assert(item);
    void *item_handle = rt_gui_wrap_listbox_item(item);
    rt_listbox_item_set_data(item_handle, data);
    rt_string list_data = rt_listbox_item_get_data(item_handle);
    assert(rt_str_len(list_data) == (int64_t)sizeof(payload));
    assert(memcmp(rt_string_cstr(list_data), payload, sizeof(payload)) == 0);
    assert(item->owns_user_data);

    vg_widget_destroy(&listbox->base);
    vg_widget_destroy(&tree->base);
    printf("test_treeview_and_listbox_data_preserve_embedded_nuls: PASSED\n");
}

static void test_listbox_selection_changed_is_edge_triggered(void) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);
    vg_listbox_item_t *first = vg_listbox_add_item(listbox, "first", NULL);
    vg_listbox_item_t *second = vg_listbox_add_item(listbox, "second", NULL);
    assert(first);
    assert(second);
    void *first_handle = rt_gui_wrap_listbox_item(first);
    void *second_handle = rt_gui_wrap_listbox_item(second);

    assert(rt_listbox_was_selection_changed(listbox) == 0);
    rt_listbox_select(listbox, first_handle);
    assert(rt_listbox_was_selection_changed(listbox) == 1);
    assert(rt_listbox_was_selection_changed(listbox) == 0);

    rt_listbox_select(listbox, first_handle);
    assert(rt_listbox_was_selection_changed(listbox) == 0);

    rt_listbox_select(listbox, second_handle);
    assert(rt_listbox_was_selection_changed(listbox) == 1);
    assert(rt_listbox_was_selection_changed(listbox) == 0);

    vg_listbox_remove_item(listbox, second);
    assert(rt_listbox_get_selected(listbox) == NULL);
    assert(rt_listbox_was_selection_changed(listbox) == 1);
    assert(rt_listbox_was_selection_changed(listbox) == 0);

    rt_listbox_select(listbox, first_handle);
    assert(rt_listbox_was_selection_changed(listbox) == 1);
    vg_listbox_clear(listbox);
    assert(rt_listbox_get_selected(listbox) == NULL);
    assert(rt_listbox_was_selection_changed(listbox) == 1);
    assert(rt_listbox_was_selection_changed(listbox) == 0);

    vg_widget_destroy(&listbox->base);
    printf("test_listbox_selection_changed_is_edge_triggered: PASSED\n");
}

static void test_runtime_listbox_select_index_rejects_out_of_range_indices(void) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);
    vg_listbox_add_item(listbox, "first", NULL);
    vg_listbox_add_item(listbox, "second", NULL);

    rt_listbox_select_index(listbox, 1);
    assert(rt_listbox_get_selected_index(listbox) == 1);
    rt_listbox_select_index(listbox, INT64_MAX);
    assert(rt_listbox_get_selected_index(listbox) == 1);

    vg_listbox_set_virtual_mode(listbox, true, 2, 20.0f);
    assert(rt_listbox_get_count(listbox) == 2);
    rt_listbox_select_index(listbox, 1);
    assert(rt_listbox_get_selected_index(listbox) == 1);
    rt_listbox_select_index(listbox, 2);
    assert(rt_listbox_get_selected_index(listbox) == 1);

    vg_widget_destroy(&listbox->base);
    printf("test_runtime_listbox_select_index_rejects_out_of_range_indices: PASSED\n");
}

static void test_runtime_listbox_selected_text_copies_multi_selection(void) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);
    vg_listbox_item_t *first = vg_listbox_add_item(listbox, "first", NULL);
    vg_listbox_item_t *second = vg_listbox_add_item(listbox, "second", NULL);
    vg_listbox_item_t *third = vg_listbox_add_item(listbox, "third", NULL);
    assert(first && second && third);

    rt_listbox_set_multi_select(listbox, 1);
    rt_listbox_select(listbox, rt_gui_wrap_listbox_item(first));
    rt_listbox_select(listbox, rt_gui_wrap_listbox_item(third));
    rt_string selected = rt_listbox_get_selected_text(listbox);
    assert(strcmp(rt_string_cstr(selected), "first\nthird") == 0);
    rt_string_unref(selected);

    rt_listbox_set_multi_select(listbox, 0);
    selected = rt_listbox_get_selected_text(listbox);
    assert(strcmp(rt_string_cstr(selected), "third") == 0);
    rt_string_unref(selected);

    (void)second;
    vg_widget_destroy(&listbox->base);
    printf("test_runtime_listbox_selected_text_copies_multi_selection: PASSED\n");
}

static void test_runtime_listbox_selected_data_preserves_rows_and_bytes(void) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);
    void *first = rt_listbox_add_item(listbox, rt_const_cstr("duplicate label"));
    void *second = rt_listbox_add_item(listbox, rt_const_cstr("duplicate label"));
    void *third = rt_listbox_add_item(listbox, rt_const_cstr("third"));
    assert(first && second && third);

    rt_string first_data = rt_string_from_bytes("alpha\none", 9);
    rt_string third_data = rt_string_from_bytes("x\0y", 3);
    assert(first_data && third_data);
    rt_listbox_item_set_data(first, first_data);
    rt_listbox_item_set_data(third, third_data);
    rt_str_release_maybe(first_data);
    rt_str_release_maybe(third_data);

    rt_listbox_set_multi_select(listbox, 1);
    rt_listbox_select(listbox, first);
    rt_listbox_select(listbox, second);
    rt_listbox_select(listbox, third);

    void *selected = rt_listbox_get_selected_data(listbox);
    assert(selected && rt_seq_len(selected) == 3);
    rt_string value = rt_seq_get_str(selected, 0);
    assert(value && rt_str_len(value) == 9 && memcmp(rt_string_cstr(value), "alpha\none", 9) == 0);
    rt_str_release_maybe(value);
    value = rt_seq_get_str(selected, 1);
    assert(value && rt_str_len(value) == 0);
    rt_str_release_maybe(value);
    value = rt_seq_get_str(selected, 2);
    assert(value && rt_str_len(value) == 3 && memcmp(rt_string_cstr(value), "x\0y", 3) == 0);
    rt_str_release_maybe(value);
    release_test_runtime_object(selected);

    vg_listbox_set_virtual_mode(listbox, true, 3, 20.0f);
    selected = rt_listbox_get_selected_data(listbox);
    assert(selected && rt_seq_len(selected) == 0);
    release_test_runtime_object(selected);

    selected = rt_listbox_get_selected_data(NULL);
    assert(selected && rt_seq_len(selected) == 0);
    release_test_runtime_object(selected);

    vg_widget_destroy(&listbox->base);
    printf("test_runtime_listbox_selected_data_preserves_rows_and_bytes: PASSED\n");
}

static void test_runtime_listbox_item_text_color_override(void) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);
    void *item_handle = rt_listbox_add_item(listbox, rt_const_cstr("error row"));
    vg_listbox_item_t *item = rt_gui_listbox_item_from_handle(item_handle);
    assert(item);
    assert(item->has_text_color == false);

    rt_listbox_item_set_text_color(item_handle, 0xE06C75);
    assert(item->has_text_color);
    assert(item->text_color == 0xE06C75u);
    assert(listbox->base.needs_paint);

    vg_widget_destroy(&listbox->base);
    printf("test_runtime_listbox_item_text_color_override: PASSED\n");
}

static void test_treeview_selection_changed_reports_removal_and_clear(void) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);
    vg_tree_node_t *first = vg_treeview_add_node(tree, NULL, "first");
    vg_tree_node_t *second = vg_treeview_add_node(tree, NULL, "second");
    assert(first && second);
    void *first_handle = rt_gui_wrap_tree_node(first);
    void *second_handle = rt_gui_wrap_tree_node(second);

    assert(rt_treeview_was_selection_changed(tree) == 0);
    rt_treeview_select(tree, first_handle);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    assert(rt_treeview_was_selection_changed(tree) == 0);

    rt_treeview_select(tree, second_handle);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    vg_treeview_remove_node(tree, second);
    assert(rt_treeview_get_selected(tree) == NULL);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    assert(rt_treeview_was_selection_changed(tree) == 0);

    rt_treeview_select(tree, first_handle);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    vg_treeview_clear(tree);
    assert(rt_treeview_get_selected(tree) == NULL);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    assert(rt_treeview_was_selection_changed(tree) == 0);

    vg_widget_destroy(&tree->base);
    printf("test_treeview_selection_changed_reports_removal_and_clear: PASSED\n");
}

static bool treeview_selected_data_virtual_provider(vg_treeview_t *tree,
                                                    size_t index,
                                                    vg_treeview_virtual_row_t *out_row,
                                                    void *user_data) {
    (void)tree;
    (void)user_data;
    if (!out_row || index != 0)
        return false;
    out_row->text = "virtual";
    out_row->depth = 0;
    out_row->expanded = false;
    out_row->has_children = false;
    out_row->loading = false;
    return true;
}

static void test_runtime_treeview_selected_data_preserves_hierarchy_and_bytes(void) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);
    void *first = rt_treeview_add_node(tree, NULL, rt_const_cstr("duplicate label"));
    void *hidden_child = rt_treeview_add_node(tree, first, rt_const_cstr("duplicate label"));
    void *last = rt_treeview_add_node(tree, NULL, rt_const_cstr("last"));
    assert(first && hidden_child && last);

    rt_string first_data = rt_string_from_bytes("alpha\none", 9);
    rt_string last_data = rt_string_from_bytes("x\0y", 3);
    assert(first_data && last_data);
    rt_treeview_node_set_data(first, first_data);
    rt_treeview_node_set_data(last, last_data);
    rt_str_release_maybe(first_data);
    rt_str_release_maybe(last_data);

    rt_treeview_set_multi_select(tree, 1);
    rt_treeview_select(tree, last);
    rt_treeview_select(tree, hidden_child);
    rt_treeview_select(tree, first);
    assert(rt_treeview_get_selected(tree) == first);

    void *selected = rt_treeview_get_selected_data(tree);
    assert(selected && rt_seq_len(selected) == 3);
    rt_string value = rt_seq_get_str(selected, 0);
    assert(value && rt_str_len(value) == 9 && memcmp(rt_string_cstr(value), "alpha\none", 9) == 0);
    rt_str_release_maybe(value);
    value = rt_seq_get_str(selected, 1);
    assert(value && rt_str_len(value) == 0);
    rt_str_release_maybe(value);
    value = rt_seq_get_str(selected, 2);
    assert(value && rt_str_len(value) == 3 && memcmp(rt_string_cstr(value), "x\0y", 3) == 0);
    rt_str_release_maybe(value);
    release_test_runtime_object(selected);

    (void)rt_treeview_was_selection_changed(tree);
    rt_treeview_set_multi_select(tree, 0);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    assert(rt_treeview_was_selection_changed(tree) == 0);
    selected = rt_treeview_get_selected_data(tree);
    assert(selected && rt_seq_len(selected) == 1);
    release_test_runtime_object(selected);

    assert(vg_treeview_bind_virtual_model(
        tree, 1, treeview_selected_data_virtual_provider, NULL, NULL, NULL));
    selected = rt_treeview_get_selected_data(tree);
    assert(selected && rt_seq_len(selected) == 0);
    release_test_runtime_object(selected);

    selected = rt_treeview_get_selected_data(NULL);
    assert(selected && rt_seq_len(selected) == 0);
    release_test_runtime_object(selected);

    vg_widget_destroy(&tree->base);
    printf("test_runtime_treeview_selected_data_preserves_hierarchy_and_bytes: PASSED\n");
}

static void test_treeview_removing_nonprimary_selection_reports_one_edge(void) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);
    vg_tree_node_t *survivor = vg_treeview_add_node(tree, NULL, "survivor");
    vg_tree_node_t *parent = vg_treeview_add_node(tree, NULL, "parent");
    vg_tree_node_t *selected_child = vg_treeview_add_node(tree, parent, "selected child");
    assert(survivor && parent && selected_child);
    void *survivor_handle = rt_gui_wrap_tree_node(survivor);
    void *child_handle = rt_gui_wrap_tree_node(selected_child);

    rt_treeview_set_multi_select(tree, 1);
    rt_treeview_select(tree, child_handle);
    rt_treeview_select(tree, survivor_handle);
    assert(tree->selected == survivor);
    assert(survivor->selected && selected_child->selected);
    (void)rt_treeview_was_selection_changed(tree);

    vg_treeview_remove_node(tree, parent);
    assert(tree->selected == survivor && survivor->selected);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    assert(rt_treeview_was_selection_changed(tree) == 0);

    vg_widget_destroy(&tree->base);
    printf("test_treeview_removing_nonprimary_selection_reports_one_edge: PASSED\n");
}

static void test_runtime_treeview_drag_drop_modes_are_validated(void) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);

    rt_treeview_set_drag_drop_mode(tree, 2);
    assert(tree->app_directed_dnd_mode == VG_TREEVIEW_APP_DND_ROW_AWARE);
    assert(tree->drag_enabled);
    rt_treeview_set_drag_drop_mode(tree, INT64_MAX);
    assert(tree->app_directed_dnd_mode == VG_TREEVIEW_APP_DND_ROW_AWARE);

    rt_treeview_set_drag_drop_enabled(tree, 1);
    assert(tree->app_directed_dnd_mode == VG_TREEVIEW_APP_DND_LEGACY_INTO);
    rt_treeview_set_drag_drop_enabled(tree, 0);
    assert(tree->app_directed_dnd_mode == VG_TREEVIEW_APP_DND_DISABLED);
    assert(!tree->drag_enabled);

    vg_widget_destroy(&tree->base);
    printf("test_runtime_treeview_drag_drop_modes_are_validated: PASSED\n");
}

static void test_removed_listbox_and_treeview_handles_are_inert(void) {
    vg_listbox_t *listbox = vg_listbox_create(NULL);
    assert(listbox);
    void *item = rt_listbox_add_item(listbox, rt_const_cstr("item"));
    assert(item);
    rt_listbox_item_set_data(item, rt_const_cstr("payload"));
    rt_listbox_remove_item(listbox, item);
    assert(rt_str_len(rt_listbox_item_get_text(item)) == 0);
    assert(rt_str_len(rt_listbox_item_get_data(item)) == 0);
    rt_listbox_item_set_text(item, rt_const_cstr("ignored"));
    rt_listbox_item_set_text_color(item, 0xE06C75);
    rt_listbox_select(listbox, item);
    assert(rt_listbox_get_selected(listbox) == NULL);

    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);
    vg_tree_node_t *node = vg_treeview_add_node(tree, NULL, "node");
    assert(node);
    vg_tree_node_t *child = vg_treeview_add_node(tree, node, "child");
    assert(child);
    void *node_handle = rt_gui_wrap_tree_node(node);
    void *child_handle = rt_gui_wrap_tree_node(child);
    rt_treeview_node_set_data(child_handle, rt_const_cstr("payload"));
    vg_treeview_remove_node(tree, node);
    assert(rt_str_len(rt_treeview_node_get_text(node_handle)) == 0);
    assert(rt_str_len(rt_treeview_node_get_text(child_handle)) == 0);
    assert(rt_str_len(rt_treeview_node_get_data(child_handle)) == 0);
    rt_treeview_node_set_data(child_handle, rt_const_cstr("ignored"));
    rt_treeview_select(tree, child_handle);
    assert(rt_treeview_get_selected(tree) == NULL);
    assert(rt_treeview_node_is_expanded(child_handle) == 0);

    vg_widget_destroy(&tree->base);
    vg_widget_destroy(&listbox->base);
    printf("test_removed_listbox_and_treeview_handles_are_inert: PASSED\n");
}

/// @brief Verify explicit tombstone pruning invalidates retained runtime wrappers before free.
/// @details Tree-node and tab wrappers may outlive removal because they are managed runtime
///          objects. The public prune operations must therefore clear the wrapper target before
///          reclaiming the lower-toolkit tombstone. Calls through those retained wrappers must
///          remain inert rather than inspecting reclaimed storage.
static void test_pruned_tree_and_tab_runtime_handles_are_inert(void) {
    vg_treeview_t *tree = vg_treeview_create(NULL);
    assert(tree);
    vg_tree_node_t *node = vg_treeview_add_node(tree, NULL, "node");
    assert(node);
    void *node_handle = rt_gui_wrap_tree_node(node);
    assert(node_handle);

    rt_treeview_remove_node(tree, node_handle);
    assert(tree->retired_nodes);
    rt_treeview_prune_retired_nodes(tree);
    assert(tree->retired_nodes == NULL);
    assert(rt_gui_tree_node_from_handle(node_handle) == NULL);
    assert(rt_str_len(rt_treeview_node_get_text(node_handle)) == 0);
    rt_treeview_node_set_data(node_handle, rt_const_cstr("ignored"));

    vg_tabbar_t *tabbar = vg_tabbar_create(NULL);
    assert(tabbar);
    vg_tab_t *tab = vg_tabbar_add_tab(tabbar, "tab", true);
    assert(tab);
    void *tab_handle = rt_gui_wrap_tab(tab);
    assert(tab_handle);

    rt_tabbar_remove_tab(tabbar, tab_handle);
    assert(tabbar->retired_tabs);
    rt_tabbar_prune_retired_tabs(tabbar);
    assert(tabbar->retired_tabs == NULL);
    assert(rt_gui_tab_from_handle(tab_handle) == NULL);
    rt_tab_set_title(tab_handle, rt_const_cstr("ignored"));
    rt_tab_set_modified(tab_handle, 1);

    vg_widget_destroy(&tabbar->base);
    vg_widget_destroy(&tree->base);
    printf("test_pruned_tree_and_tab_runtime_handles_are_inert: PASSED\n");
}

static void test_listbox_and_treeview_reject_foreign_child_handles(void) {
    vg_listbox_t *list_a = vg_listbox_create(NULL);
    vg_listbox_t *list_b = vg_listbox_create(NULL);
    assert(list_a && list_b);
    void *item_b = rt_listbox_add_item(list_b, rt_const_cstr("b"));
    vg_listbox_item_t *raw_item_b = rt_gui_listbox_item_from_handle(item_b);
    assert(raw_item_b);

    rt_listbox_select(list_a, item_b);
    assert(rt_listbox_get_selected(list_a) == NULL);
    rt_listbox_remove_item(list_a, item_b);
    assert(vg_listbox_item_is_live(raw_item_b));
    assert(raw_item_b->owner == list_b);
    assert(rt_listbox_get_count(list_b) == 1);

    vg_treeview_t *tree_a = vg_treeview_create(NULL);
    vg_treeview_t *tree_b = vg_treeview_create(NULL);
    assert(tree_a && tree_b);
    void *node_b = rt_treeview_add_node(tree_b, NULL, rt_const_cstr("b"));
    vg_tree_node_t *raw_node_b = rt_gui_tree_node_from_handle(node_b);
    assert(raw_node_b);

    assert(rt_treeview_add_node(tree_a, node_b, rt_const_cstr("bad child")) == NULL);
    rt_treeview_expand(tree_a, node_b);
    assert(raw_node_b->expanded == false);
    rt_treeview_select(tree_a, node_b);
    assert(rt_treeview_get_selected(tree_a) == NULL);
    rt_treeview_remove_node(tree_a, node_b);
    assert(vg_tree_node_is_live(raw_node_b));
    assert(raw_node_b->owner == tree_b);

    vg_widget_destroy(&tree_b->base);
    vg_widget_destroy(&tree_a->base);
    vg_widget_destroy(&list_b->base);
    vg_widget_destroy(&list_a->base);
    printf("test_listbox_and_treeview_reject_foreign_child_handles: PASSED\n");
}

/// @brief Verify the complete managed TreeView node model and independent event payloads.
/// @details Covers allocation-safe display text, exact UTF-8 icon round-tripping, stable IDs,
///          materialized and lazy child state, loading, toggle/scroll behavior, monotonic
///          revisions, independent lazy-load and activation edges, Option payload liveness, and
///          stale-node behavior after removal. The test intentionally reads event payloads before
///          their `Was*` edges to ensure those observer domains do not consume each other.
static void test_treeview_complete_node_and_lazy_event_contract(void) {
    vg_treeview_t *tree = (vg_treeview_t *)rt_treeview_new(NULL);
    assert(tree);
    void *node = rt_treeview_add_node(tree, NULL, rt_const_cstr("Original"));
    assert(node);

    int64_t revision = rt_treeview_get_revision(tree);
    rt_string visible_with_nul = rt_string_from_bytes("A\0B", 3);
    assert(visible_with_nul);
    rt_treeview_node_set_text(node, visible_with_nul);
    rt_str_release_maybe(visible_with_nul);
    rt_string value = rt_treeview_node_get_text(node);
    static const unsigned char expected_visible[] = {'A', 0xEFu, 0xBFu, 0xBDu, 'B'};
    assert(value && rt_str_len(value) == (int64_t)sizeof(expected_visible));
    assert(memcmp(rt_string_cstr(value), expected_visible, sizeof(expected_visible)) == 0);
    rt_str_release_maybe(value);
    assert(rt_treeview_get_revision(tree) > revision);

    rt_treeview_node_set_icon(node, rt_const_cstr("📁"));
    value = rt_treeview_node_get_icon(node);
    assert(value && strcmp(rt_string_cstr(value), "📁") == 0);
    rt_str_release_maybe(value);

    rt_treeview_node_set_stable_id(node, rt_const_cstr("project/src"));
    value = rt_treeview_node_get_stable_id(node);
    assert(value && strcmp(rt_string_cstr(value), "project/src") == 0);
    rt_str_release_maybe(value);
    rt_string invalid_id = rt_string_from_bytes("bad\0id", 6);
    assert(invalid_id);
    rt_treeview_node_set_stable_id(node, invalid_id);
    rt_str_release_maybe(invalid_id);
    value = rt_treeview_node_get_stable_id(node);
    assert(value && strcmp(rt_string_cstr(value), "project/src") == 0);
    rt_str_release_maybe(value);

    assert(rt_treeview_node_has_children(node) == 0);
    rt_treeview_node_set_has_children(node, 1);
    assert(rt_treeview_node_has_children(node) == 1);
    assert(rt_treeview_node_is_loading(node) == 0);
    assert(rt_treeview_was_load_children_requested(tree) == 0);

    revision = rt_treeview_get_revision(tree);
    rt_treeview_toggle(tree, node);
    assert(rt_treeview_node_is_expanded(node) == 1);
    assert(rt_treeview_node_is_loading(node) == 1);
    assert(rt_treeview_get_revision(tree) > revision);
    void *option = rt_treeview_get_load_requested_node_option(tree);
    assert(option && rt_option_is_some(option) == 1);
    assert(rt_option_unwrap(option) == node);
    release_test_runtime_object(option);
    assert(rt_treeview_was_load_children_requested(tree) == 1);
    assert(rt_treeview_was_load_children_requested(tree) == 0);

    rt_treeview_node_set_loading(node, 0);
    assert(rt_treeview_node_is_loading(node) == 0);
    rt_treeview_toggle(tree, node);
    assert(rt_treeview_node_is_expanded(node) == 0);
    rt_treeview_toggle(tree, node);
    assert(rt_treeview_was_load_children_requested(tree) == 1);
    rt_treeview_node_set_loading(node, 0);

    // Real children keep the affordance visible even when callers clear the lazy placeholder.
    void *child = rt_treeview_add_node(tree, node, rt_const_cstr("Child"));
    assert(child);
    rt_treeview_node_set_has_children(node, 0);
    assert(rt_treeview_node_has_children(node) == 1);

    rt_treeview_select(tree, node);
    vg_event_t enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, VG_MOD_NONE);
    assert(vg_event_send(&tree->base, &enter));
    option = rt_treeview_get_activated_node_option(tree);
    assert(option && rt_option_is_some(option) == 1);
    assert(rt_option_unwrap(option) == node);
    release_test_runtime_object(option);
    assert(rt_treeview_was_activated(tree) == 1);
    assert(rt_treeview_was_activated(tree) == 0);

    // A short viewport makes ScrollTo observable while retaining logical ownership checks.
    tree->base.height = tree->row_height;
    rt_treeview_scroll_to(tree, child);
    assert(tree->scroll_y >= 0.0f);

    rt_treeview_remove_node(tree, node);
    option = rt_treeview_get_load_requested_node_option(tree);
    assert(option && rt_option_is_none(option) == 1);
    release_test_runtime_object(option);
    option = rt_treeview_get_activated_node_option(tree);
    assert(option && rt_option_is_none(option) == 1);
    release_test_runtime_object(option);
    assert(rt_str_len(rt_treeview_node_get_text(node)) == 0);
    assert(rt_str_len(rt_treeview_node_get_icon(node)) == 0);
    assert(rt_str_len(rt_treeview_node_get_stable_id(node)) == 0);
    assert(rt_treeview_node_has_children(node) == 0);
    assert(rt_treeview_node_is_loading(node) == 0);

    vg_widget_destroy(&tree->base);
    printf("test_treeview_complete_node_and_lazy_event_contract: PASSED\n");
}

/// @brief Verify complete tab, split-pane, radio-group, and selectable-label runtime contracts.
/// @details Exercises managed tab metadata and byte-exact payloads, reorder edge/payload
///          independence, logical split minima and reversible collapse, radio-group membership and
///          selected-index revisions, per-radio text/data, UTF-8-safe wrapped ellipsis, alignment,
///          and keyboard label selection. This is the recommendation-29 end-to-end regression.
static void test_complete_tab_split_radio_label_contracts(void) {
    vg_font_t *font = vg_font_load_file(
        ZANNA_SOURCE_DIR "/src/runtime/graphics/text/fonts/JetBrainsMono-Regular.ttf");
    assert(font);

    vg_tabbar_t *tabbar = (vg_tabbar_t *)rt_tabbar_new(NULL);
    assert(tabbar);
    void *first_tab = rt_tabbar_add_tab(tabbar, rt_const_cstr("First"), 1);
    void *second_tab = rt_tabbar_add_tab(tabbar, rt_const_cstr("Second"), 0);
    void *third_tab = rt_tabbar_add_tab(tabbar, rt_const_cstr("Third"), 1);
    assert(first_tab && second_tab && third_tab);
    rt_tabbar_set_font(tabbar, font, 15.0);
    assert(tabbar->font == font);
    assert(tabbar->font_size == 15.0f);

    rt_string visible_with_nul = rt_string_from_bytes("A\0B", 3);
    assert(visible_with_nul);
    rt_tab_set_title(first_tab, visible_with_nul);
    rt_str_release_maybe(visible_with_nul);
    rt_string value = rt_tab_get_title(first_tab);
    static const unsigned char expected_visible[] = {'A', 0xEFu, 0xBFu, 0xBDu, 'B'};
    assert(value && rt_str_len(value) == (int64_t)sizeof(expected_visible));
    assert(memcmp(rt_string_cstr(value), expected_visible, sizeof(expected_visible)) == 0);
    rt_str_release_maybe(value);

    rt_string exact_data = rt_string_from_bytes("x\0y", 3);
    assert(exact_data);
    rt_tab_set_data(first_tab, exact_data);
    rt_str_release_maybe(exact_data);
    value = rt_tab_get_data(first_tab);
    assert(value && rt_str_len(value) == 3 && memcmp(rt_string_cstr(value), "x\0y", 3) == 0);
    rt_str_release_maybe(value);
    assert(rt_tab_is_closable(second_tab) == 0);
    rt_tab_set_closable(second_tab, 1);
    assert(rt_tab_is_closable(second_tab) == 1);

    rt_tab_set_stable_id(first_tab, rt_const_cstr("editor:first"));
    value = rt_tab_get_stable_id(first_tab);
    assert(value && strcmp(rt_string_cstr(value), "editor:first") == 0);
    rt_str_release_maybe(value);
    rt_string invalid_id = rt_string_from_bytes("bad\0id", 6);
    assert(invalid_id);
    rt_tab_set_stable_id(first_tab, invalid_id);
    rt_str_release_maybe(invalid_id);
    value = rt_tab_get_stable_id(first_tab);
    assert(value && strcmp(rt_string_cstr(value), "editor:first") == 0);
    rt_str_release_maybe(value);

    int64_t revision = rt_tabbar_get_revision(tabbar);
    assert(rt_tabbar_was_reordered(tabbar) == 0);
    assert(rt_tabbar_move_tab(tabbar, 0, 2) == 1);
    assert(rt_tabbar_get_reordered_from(tabbar) == 0);
    assert(rt_tabbar_get_reordered_to(tabbar) == 2);
    assert(rt_tabbar_get_revision(tabbar) > revision);
    assert(rt_tabbar_was_reordered(tabbar) == 1);
    assert(rt_tabbar_was_reordered(tabbar) == 0);
    assert(rt_tabbar_get_tab_at(tabbar, 2) == first_tab);
    assert(rt_tabbar_move_tab(tabbar, 2, 2) == 0);

    vg_splitpane_t *split = (vg_splitpane_t *)rt_splitpane_new(NULL, 1);
    assert(split && rt_splitpane_get_orientation(split) == 0);
    rt_splitpane_set_min_first(split, 24.5);
    rt_splitpane_set_min_second(split, 31.25);
    assert(fabs(rt_splitpane_get_min_first(split) - 24.5) < 0.001);
    assert(fabs(rt_splitpane_get_min_second(split) - 31.25) < 0.001);
    rt_splitpane_set_position(split, 0.3);
    revision = rt_widget_get_revision(split);
    vg_widget_arrange(&split->base, 0.0f, 0.0f, 300.0f, 100.0f);
    rt_splitpane_collapse_first(split);
    assert(rt_splitpane_get_collapsed_side(split) == 1);
    assert(rt_splitpane_get_position(split) == 0.0);
    vg_widget_arrange(&split->base, 0.0f, 0.0f, 300.0f, 100.0f);
    assert(vg_splitpane_get_first(split)->width == 0.0f);
    rt_splitpane_collapse_second(split);
    assert(rt_splitpane_get_collapsed_side(split) == 2);
    vg_widget_arrange(&split->base, 0.0f, 0.0f, 300.0f, 100.0f);
    assert(vg_splitpane_get_second(split)->width == 0.0f);
    rt_splitpane_restore(split);
    assert(rt_splitpane_get_collapsed_side(split) == 0);
    assert(fabs(rt_splitpane_get_position(split) - 0.3) < 0.001);
    assert(rt_widget_get_revision(split) > revision);
    vg_splitpane_t *vertical = (vg_splitpane_t *)rt_splitpane_new(NULL, 0);
    assert(vertical && rt_splitpane_get_orientation(vertical) == 1);

    void *group = rt_radiogroup_new();
    assert(group);
    vg_radiobutton_t *first_radio =
        (vg_radiobutton_t *)rt_radiobutton_new(NULL, rt_const_cstr("One"), group);
    vg_radiobutton_t *second_radio =
        (vg_radiobutton_t *)rt_radiobutton_new(NULL, rt_const_cstr("Two"), group);
    assert(first_radio && second_radio);
    assert(rt_radiogroup_get_count(group) == 2);
    assert(rt_radiogroup_get_selected_index(group) == -1);
    assert(rt_radiogroup_was_changed(group) == 0);
    revision = rt_radiogroup_get_revision(group);
    assert(rt_radiogroup_set_selected_index(group, 0) == 1);
    assert(rt_radiobutton_is_selected(first_radio) == 1);
    assert(rt_radiogroup_get_selected_index(group) == 0);
    assert(rt_radiogroup_get_revision(group) > revision);
    assert(rt_radiogroup_was_changed(group) == 1);
    assert(rt_radiogroup_was_changed(group) == 0);
    revision = rt_radiogroup_get_revision(group);
    assert(rt_radiogroup_set_selected_index(group, 2) == 0);
    assert(rt_radiogroup_get_revision(group) == revision);

    rt_radiobutton_set_text(first_radio, rt_const_cstr("Primary"));
    value = rt_radiobutton_get_text(first_radio);
    assert(value && strcmp(rt_string_cstr(value), "Primary") == 0);
    rt_str_release_maybe(value);
    exact_data = rt_string_from_bytes("r\0d", 3);
    assert(exact_data);
    rt_radiobutton_set_data(first_radio, exact_data);
    rt_str_release_maybe(exact_data);
    value = rt_radiobutton_get_data(first_radio);
    assert(value && rt_str_len(value) == 3 && memcmp(rt_string_cstr(value), "r\0d", 3) == 0);
    rt_str_release_maybe(value);
    assert(rt_radiogroup_set_selected_index(group, 1) == 1);
    assert(rt_radiogroup_was_changed(group) == 1);
    rt_widget_destroy(first_radio);
    assert(rt_radiogroup_get_count(group) == 1);
    assert(rt_radiogroup_get_selected_index(group) == 0);
    assert(rt_radiogroup_was_changed(group) == 1);

    vg_label_t *label = (vg_label_t *)rt_label_new(NULL, rt_const_cstr("alpha beta gamma delta"));
    assert(label);
    rt_label_set_font(label, font, 14.0);
    rt_label_set_alignment(label, 2);
    assert(rt_label_get_alignment(label) == 2);
    rt_label_set_word_wrap(label, 1);
    rt_label_set_max_lines(label, 1);
    rt_label_set_ellipsis(label, 1);
    vg_widget_measure(&label->base, 55.0f, 100.0f);
    assert(label->wrap_line_count == 1 && label->wrap_truncated);
    size_t rendered_len = strlen(label->wrap_line_bufs[0]);
    assert(rendered_len >= 3);
    assert(memcmp(label->wrap_line_bufs[0] + rendered_len - 3, "\xE2\x80\xA6", 3) == 0);
    rt_label_set_selectable(label, 1);
    vg_event_t select_all = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_A, 0, VG_MOD_CTRL);
    assert(vg_event_send(&label->base, &select_all));
    value = rt_label_get_selected_text(label);
    assert(value && strcmp(rt_string_cstr(value), "alpha beta gamma delta") == 0);
    rt_str_release_maybe(value);
    vg_event_t escape = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ESCAPE, 0, VG_MOD_NONE);
    assert(vg_event_send(&label->base, &escape));
    value = rt_label_get_selected_text(label);
    assert(value && rt_str_len(value) == 0);
    rt_str_release_maybe(value);
    rt_label_set_max_lines(label, -1);
    assert(label->max_lines == 0);
    rt_label_set_selectable(label, 0);

    rt_widget_destroy(label);
    rt_widget_destroy(second_radio);
    assert(rt_radiogroup_get_count(group) == 0);
    assert(rt_radiogroup_get_selected_index(group) == -1);
    rt_radiogroup_destroy(group);
    release_test_runtime_object(group);
    rt_widget_destroy(split);
    rt_widget_destroy(vertical);
    rt_widget_destroy(tabbar);
    release_test_runtime_object(first_tab);
    release_test_runtime_object(second_tab);
    release_test_runtime_object(third_tab);
    vg_font_destroy(font);
    printf("test_complete_tab_split_radio_label_contracts: PASSED\n");
}

/// @brief Verify the complete managed ColorSwatch, ColorPalette, and ColorPicker contracts.
/// @details Exercises the public `0x00RRGGBB` boundary, separate alpha preservation, type-safe
///          invalid-handle behavior, palette compaction, keyboard component editing, semantic
///          roles/values, independent consumable change edges, and non-consuming revisions. This
///          is the recommendation-30 end-to-end runtime regression.
static void test_complete_public_color_control_contracts(void) {
    vg_colorswatch_t *swatch =
        (vg_colorswatch_t *)rt_colorswatch_new(NULL, (int64_t)0x123456ABCDEFULL);
    assert(swatch);
    assert(swatch->color == 0xFFABCDEFu);
    assert(rt_colorswatch_get_color(swatch) == 0x00ABCDEF);
    assert(vg_widget_get_accessible_role(&swatch->base) == VG_ACCESSIBLE_ROLE_BUTTON);
    assert(strcmp(vg_widget_get_accessible_value(&swatch->base), "#ABCDEF") == 0);
    assert(rt_colorswatch_was_changed(swatch) == 0);
    int64_t revision = rt_colorswatch_get_revision(swatch);
    rt_colorswatch_set_color(swatch, (int64_t)0xFFFF00112233ULL);
    assert(swatch->color == 0xFF112233u);
    assert(rt_colorswatch_get_color(swatch) == 0x00112233);
    assert(rt_colorswatch_get_revision(swatch) > revision);
    assert(rt_colorswatch_was_changed(swatch) == 1);
    assert(rt_colorswatch_was_changed(swatch) == 0);
    revision = rt_colorswatch_get_revision(swatch);
    rt_colorswatch_set_color(swatch, (int64_t)0x9900112233ULL);
    assert(rt_colorswatch_get_revision(swatch) == revision);
    rt_colorswatch_set_selected(swatch, 1);
    assert(rt_colorswatch_is_selected(swatch) == 1);
    assert(rt_colorswatch_was_changed(swatch) == 1);
    assert(rt_colorswatch_get_revision(swatch) > revision);

    vg_colorpalette_t *palette = (vg_colorpalette_t *)rt_colorpalette_new(NULL);
    assert(palette);
    assert(vg_widget_get_accessible_role(&palette->base) == VG_ACCESSIBLE_ROLE_LIST);
    assert(rt_colorpalette_get_color_count(palette) == 0);
    rt_colorpalette_add_color(palette, (int64_t)0xAB00112233ULL);
    rt_colorpalette_add_color(palette, 0x00445566);
    rt_colorpalette_add_color(palette, 0x00000000);
    assert(rt_colorpalette_get_color_count(palette) == 3);
    assert(rt_colorpalette_get_color_at(palette, 0) == 0x00112233);
    assert(rt_colorpalette_get_color_at(palette, 1) == 0x00445566);
    assert(rt_colorpalette_get_color_at(palette, 2) == 0);
    assert(rt_colorpalette_get_color_at(palette, -1) == 0);
    assert(rt_colorpalette_get_color_at(palette, INT64_MAX) == 0);
    assert(rt_colorpalette_was_changed(palette) == 1);
    assert(rt_colorpalette_was_changed(palette) == 0);
    revision = rt_colorpalette_get_revision(palette);
    rt_colorpalette_set_selected_index(palette, 1);
    assert(rt_colorpalette_get_selected_index(palette) == 1);
    assert(strcmp(vg_widget_get_accessible_value(&palette->base), "2 of 3, #445566") == 0);
    assert(rt_colorpalette_get_revision(palette) > revision);
    assert(rt_colorpalette_was_changed(palette) == 1);
    assert(rt_colorpalette_remove_color(palette, 0) == 1);
    assert(rt_colorpalette_get_selected_index(palette) == 0);
    assert(rt_colorpalette_get_color_at(palette, 0) == 0x00445566);
    assert(rt_colorpalette_remove_color(palette, 7) == 0);
    assert(rt_colorpalette_remove_color(palette, 0) == 1);
    assert(rt_colorpalette_get_selected_index(palette) == -1);
    rt_colorpalette_clear(palette);
    assert(rt_colorpalette_get_color_count(palette) == 0);
    assert(rt_colorpalette_was_changed(palette) == 1);

    vg_colorpicker_t *picker = (vg_colorpicker_t *)rt_colorpicker_new(NULL);
    assert(picker);
    assert(vg_widget_get_accessible_role(&picker->base) == VG_ACCESSIBLE_ROLE_GROUP);
    assert(rt_colorpicker_get_color(picker) == 0);
    assert(rt_colorpicker_get_red(picker) == 0);
    assert(rt_colorpicker_get_green(picker) == 0);
    assert(rt_colorpicker_get_blue(picker) == 0);
    assert(rt_colorpicker_get_alpha(picker) == 255);
    assert(rt_colorpicker_is_alpha_enabled(picker) == 0);
    assert(rt_colorpicker_was_changed(picker) == 0);

    vg_colorpicker_set_alpha(picker, 77);
    assert(rt_colorpicker_get_alpha(picker) == 77);
    assert(rt_colorpicker_was_changed(picker) == 1);
    revision = rt_colorpicker_get_revision(picker);
    rt_colorpicker_set_color(picker, (int64_t)0xDEAD00336699ULL);
    assert(rt_colorpicker_get_color(picker) == 0x00336699);
    assert(rt_colorpicker_get_red(picker) == 0x33);
    assert(rt_colorpicker_get_green(picker) == 0x66);
    assert(rt_colorpicker_get_blue(picker) == 0x99);
    assert(rt_colorpicker_get_alpha(picker) == 77);
    assert(strcmp(vg_widget_get_accessible_value(&picker->base), "#336699 alpha 77") == 0);
    assert(rt_colorpicker_get_revision(picker) > revision);
    assert(rt_colorpicker_was_changed(picker) == 1);
    assert(rt_colorpicker_was_changed(picker) == 0);

    revision = rt_colorpicker_get_revision(picker);
    rt_colorpicker_set_alpha_enabled(picker, 1);
    assert(rt_colorpicker_is_alpha_enabled(picker) == 1);
    assert(rt_colorpicker_get_revision(picker) > revision);
    assert(rt_colorpicker_was_changed(picker) == 0);
    vg_event_t shifted_right = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_RIGHT, 0, VG_MOD_SHIFT);
    assert(picker->base.vtable->handle_event(&picker->base, &shifted_right));
    assert(rt_colorpicker_get_red(picker) == 0x3D);
    assert(rt_colorpicker_was_changed(picker) == 1);

    // Every entry point validates exact live widget type rather than blindly casting.
    assert(rt_colorpicker_get_color(swatch) == 0);
    assert(rt_colorpalette_get_selected_index(swatch) == -1);
    assert(rt_colorswatch_is_selected(picker) == 0);
    assert(rt_colorpicker_get_alpha(NULL) == 0);

    vg_widget_destroy(&picker->base);
    vg_widget_destroy(&palette->base);
    vg_widget_destroy(&swatch->base);
    printf("test_complete_public_color_control_contracts: PASSED\n");
}

static void test_contextmenu_separator_returns_item_handle(void) {
    void *menu = rt_contextmenu_new();
    assert(menu);
    void *separator = rt_contextmenu_add_separator(menu);
    assert(separator);
    assert(rt_menuitem_is_separator(separator) == 1);

    rt_contextmenu_destroy(menu);
    printf("test_contextmenu_separator_returns_item_handle: PASSED\n");
}

static void test_contextmenu_item_click_updates_menuitem_was_clicked(void) {
    void *menu_handle = rt_contextmenu_new();
    vg_contextmenu_t *menu = rt_gui_contextmenu_from_handle(menu_handle);
    assert(menu);
    void *item = rt_contextmenu_add_item(menu_handle, rt_const_cstr("Open"));
    vg_menu_item_t *raw_item = rt_gui_menu_item_from_handle(item);
    assert(item);

    rt_contextmenu_show(menu_handle, 0, 0);
    vg_event_t down = vg_event_mouse(VG_EVENT_MOUSE_DOWN, 4.0f, 4.0f, VG_MOUSE_LEFT, 0);
    assert(menu->base.vtable->handle_event(&menu->base, &down));

    assert(rt_contextmenu_get_clicked_item(menu_handle) == item);
    assert(rt_menuitem_was_clicked(item) == 1);
    assert(rt_menuitem_was_clicked(item) == 0);

    assert(raw_item == NULL || raw_item->was_clicked == false);
    rt_contextmenu_destroy(menu_handle);
    printf("test_contextmenu_item_click_updates_menuitem_was_clicked: PASSED\n");
}

// Regression: a standalone right-click menu (GUI.ContextMenu.New) is not parented into
// app->root, so the main loop only paints it and routes input to it if rt_contextmenu_show
// registers it as the app's active overlay. rt_contextmenu_hide must clear that
// registration. Without this the menu is marked visible but never appears on screen.
static void test_contextmenu_show_registers_and_clears_active_overlay(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 14.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *menu = rt_contextmenu_new();
    assert(menu);
    assert(rt_contextmenu_add_item(menu, rt_const_cstr("Open")));
    assert(app.active_context_menu == NULL);

    rt_contextmenu_show(menu, 10, 20);
    assert(app.active_context_menu == rt_gui_contextmenu_from_handle(menu));
    assert(rt_contextmenu_is_visible(menu) == 1);

    rt_contextmenu_hide(menu);
    assert(app.active_context_menu == NULL);
    assert(rt_contextmenu_is_visible(menu) == 0);

    rt_contextmenu_destroy(menu);
    cleanup_fake_app(&app);
    printf("test_contextmenu_show_registers_and_clears_active_overlay: PASSED\n");
}

/// @brief Verify lightweight toolbar/menu item geometry uses public logical coordinates.
/// @details Both lower widgets retain physical framebuffer rectangles. At 2x UI scale, the four
///          runtime getters must divide exactly once so callers can multiply once when sending
///          physical TestHarness input, matching the base Widget.GetScreen contract.
static void test_toolbar_and_menu_item_geometry_uses_logical_units(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.user_scale = 2.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_toolbar_t *toolbar = (vg_toolbar_t *)rt_toolbar_new(app.root);
    assert(toolbar);
    void *toolbar_item_handle = rt_toolbar_add_named_button_with_text(
        toolbar, rt_const_cstr("run"), rt_const_cstr("Run"), rt_const_cstr("Run"));
    vg_toolbar_item_t *toolbar_item = rt_gui_toolbar_item_from_handle(toolbar_item_handle);
    assert(toolbar_item);
    vg_widget_arrange(&toolbar->base, 40.0f, 20.0f, 400.0f, 80.0f);

    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    assert(vg_toolbar_item_screen_rect(toolbar_item, &x, &y, &width, &height));
    assert(fabs(rt_toolbaritem_get_screen_x(toolbar_item_handle) - x / 2.0) < 0.001);
    assert(fabs(rt_toolbaritem_get_screen_y(toolbar_item_handle) - y / 2.0) < 0.001);
    assert(fabs(rt_toolbaritem_get_screen_width(toolbar_item_handle) - width / 2.0) < 0.001);
    assert(fabs(rt_toolbaritem_get_screen_height(toolbar_item_handle) - height / 2.0) < 0.001);

    void *menu_handle = rt_contextmenu_new();
    assert(menu_handle);
    void *menu_item_handle = rt_contextmenu_add_item(menu_handle, rt_const_cstr("Open"));
    vg_contextmenu_t *menu = rt_gui_contextmenu_from_handle(menu_handle);
    vg_menu_item_t *menu_item = rt_gui_menu_item_from_handle(menu_item_handle);
    assert(menu && menu_item);
    rt_contextmenu_show(menu_handle, 80, 100);
    assert(vg_contextmenu_get_item_rect(menu, menu_item, &x, &y, &width, &height));
    assert(fabs(rt_menuitem_get_screen_x(menu_item_handle) - x / 2.0) < 0.001);
    assert(fabs(rt_menuitem_get_screen_y(menu_item_handle) - y / 2.0) < 0.001);
    assert(fabs(rt_menuitem_get_screen_width(menu_item_handle) - width / 2.0) < 0.001);
    assert(fabs(rt_menuitem_get_screen_height(menu_item_handle) - height / 2.0) < 0.001);

    rt_contextmenu_destroy(menu_handle);
    cleanup_fake_app(&app);
    printf("test_toolbar_and_menu_item_geometry_uses_logical_units: PASSED\n");
}

static void test_filedialog_show_without_active_window_returns_zero(void) {
    rt_gui_activate_app(NULL);
    void *dialog = rt_filedialog_new_open();
    assert(dialog);
    rt_filedialog_data_view_t *view = (rt_filedialog_data_view_t *)dialog;
    assert(view->dialog);
    assert(view->dialog->bookmark_count > 0);
    assert(rt_filedialog_show(dialog) == 0);
    assert(rt_filedialog_get_path_count(dialog) == 0);
    assert(rt_str_len(rt_filedialog_get_path(dialog)) == 0);
    rt_filedialog_destroy(dialog);

    vg_filedialog_t *raw_dialog = vg_filedialog_create(VG_FILEDIALOG_OPEN);
    assert(raw_dialog);
    vg_widget_t *raw_widget = &raw_dialog->base.base;
    assert(vg_widget_is_live(raw_widget));
    vg_filedialog_destroy(raw_dialog);
    assert(!vg_widget_is_live(raw_widget));

    vg_widget_t *probe = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(probe);
    vg_widget_destroy(probe);

    printf("test_filedialog_show_without_active_window_returns_zero: PASSED\n");
}

static void test_filedialog_show_uses_original_owner_app(void) {
    rt_gui_app_t app_a;
    rt_gui_app_t app_b;
    reset_fake_app(&app_a);
    reset_fake_app(&app_b);
    app_a.root = vg_widget_create(VG_WIDGET_CONTAINER);
    app_b.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app_a.root && app_b.root);
    app_a.root->user_data = &app_a;
    app_b.root->user_data = &app_b;

    rt_gui_activate_app(&app_a);
    void *dialog = rt_filedialog_new_open();
    assert(dialog);
    rt_filedialog_data_view_t *view = (rt_filedialog_data_view_t *)dialog;
    assert(view->owner_app == &app_a);

    rt_gui_activate_app(&app_b);
    assert(rt_filedialog_show(dialog) == 0);
    assert(view->owner_app == &app_a);

    rt_filedialog_destroy(dialog);
    cleanup_fake_app(&app_b);
    cleanup_fake_app(&app_a);
    printf("test_filedialog_show_uses_original_owner_app: PASSED\n");
}

static void test_filedialog_path_list_helpers_decode_escaped_paths(void) {
    rt_string escaped = rt_const_cstr("/a\\;b;/c\\\\d;tail\\");
    assert(rt_filedialog_path_list_count(escaped) == 3);

    rt_string first = rt_filedialog_path_list_get(escaped, 0);
    rt_string second = rt_filedialog_path_list_get(escaped, 1);
    rt_string third = rt_filedialog_path_list_get(escaped, 2);
    rt_string missing = rt_filedialog_path_list_get(escaped, 3);

    assert(strcmp(rt_string_cstr(first), "/a;b") == 0);
    assert(strcmp(rt_string_cstr(second), "/c\\d") == 0);
    assert(strcmp(rt_string_cstr(third), "tail\\") == 0);
    assert(rt_str_len(missing) == 0);
    assert(rt_filedialog_path_list_count(rt_str_empty()) == 0);

    printf("test_filedialog_path_list_helpers_decode_escaped_paths: PASSED\n");
}

static void test_commandpalette_methods_after_destroy_are_inert(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *palette = rt_commandpalette_new(&app);
    assert(palette);
    rt_commandpalette_add_command(
        palette, rt_const_cstr("open"), rt_const_cstr("Open"), rt_const_cstr("File"));
    rt_commandpalette_show(palette);
    assert(rt_commandpalette_is_visible(palette) == 1);
    rt_commandpalette_destroy(palette);
    assert(app.command_palette_count == 0);

    rt_commandpalette_add_command(
        palette, rt_const_cstr("save"), rt_const_cstr("Save"), rt_const_cstr("File"));
    rt_commandpalette_add_command_with_shortcut(palette,
                                                rt_const_cstr("close"),
                                                rt_const_cstr("Close"),
                                                rt_const_cstr("File"),
                                                rt_const_cstr("Ctrl+W"));
    rt_commandpalette_remove_command(palette, rt_const_cstr("save"));
    rt_commandpalette_clear(palette);
    rt_commandpalette_show(palette);
    rt_commandpalette_hide(palette);
    rt_commandpalette_set_placeholder(palette, rt_const_cstr("Run command"));
    assert(rt_commandpalette_is_visible(palette) == 0);
    assert(rt_commandpalette_was_command_selected(palette) == 0);
    assert(rt_str_len(rt_commandpalette_get_selected_command(palette)) == 0);

    cleanup_fake_app(&app);
    printf("test_commandpalette_methods_after_destroy_are_inert: PASSED\n");
}

static void test_commandpalette_show_clear_remove_drop_stale_selection(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *palette = rt_commandpalette_new(&app);
    assert(palette);
    rt_commandpalette_add_command(
        palette, rt_const_cstr("old"), rt_const_cstr("Old"), rt_const_cstr("File"));
    rt_commandpalette_add_command(
        palette, rt_const_cstr("keep"), rt_const_cstr("Keep"), rt_const_cstr("File"));

    rt_commandpalette_data_view_t *data = (rt_commandpalette_data_view_t *)palette;
    data->selected_command = test_strdup_local("old");
    assert(data->selected_command);
    data->was_selected = 1;

    rt_commandpalette_show(palette);
    assert(rt_commandpalette_was_command_selected(palette) == 0);
    assert(rt_str_len(rt_commandpalette_get_selected_command(palette)) == 0);

    data->selected_command = test_strdup_local("old");
    assert(data->selected_command);
    data->was_selected = 1;
    rt_commandpalette_remove_command(palette, rt_const_cstr("old"));
    assert(rt_commandpalette_was_command_selected(palette) == 0);
    assert(rt_str_len(rt_commandpalette_get_selected_command(palette)) == 0);

    data->selected_command = test_strdup_local("keep");
    assert(data->selected_command);
    data->was_selected = 1;
    rt_commandpalette_clear(palette);
    assert(rt_commandpalette_was_command_selected(palette) == 0);
    assert(rt_str_len(rt_commandpalette_get_selected_command(palette)) == 0);

    rt_commandpalette_destroy(palette);
    cleanup_fake_app(&app);
    printf("test_commandpalette_show_clear_remove_drop_stale_selection: PASSED\n");
}

static void test_commandpalette_rejects_embedded_nul_command_ids(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *palette = rt_commandpalette_new(&app);
    assert(palette);
    rt_commandpalette_data_view_t *data = (rt_commandpalette_data_view_t *)palette;
    assert(data->palette);
    data->palette->query_generation = UINT64_MAX;
    assert(rt_commandpalette_get_query_generation(palette) == INT64_MAX);

    const char bad_id[] = {'b', 'a', 'd', '\0', 'i', 'd'};
    rt_commandpalette_add_command(palette,
                                  rt_string_from_bytes(bad_id, sizeof(bad_id)),
                                  rt_const_cstr("Bad"),
                                  rt_const_cstr("File"));
    assert(data->palette->command_count == 0);

    rt_commandpalette_add_command_with_shortcut(palette,
                                                rt_string_from_bytes(bad_id, sizeof(bad_id)),
                                                rt_const_cstr("Bad"),
                                                rt_const_cstr("File"),
                                                rt_const_cstr("Ctrl+B"));
    assert(data->palette->command_count == 0);

    rt_commandpalette_destroy(palette);
    cleanup_fake_app(&app);
    printf("test_commandpalette_rejects_embedded_nul_command_ids: PASSED\n");
}

static void test_numeric_setters_sanitize_invalid_values(void) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(widget);
    rt_widget_set_size(widget, -5, INT64_MAX);
    assert(widget->constraints.preferred_width == 0.0f);
    assert(widget->constraints.preferred_height == (float)RT_GUI_MAX_LAYOUT_VALUE);

    rt_widget_set_flex(widget, NAN);
    assert(widget->layout.flex == 0.0f);
    rt_widget_set_margin(widget, -9);
    assert(widget->layout.margin_left == 0.0f);
    rt_widget_set_position(widget, INT64_MIN, INT64_MAX);
    assert(widget->x == (float)-RT_GUI_MAX_LAYOUT_VALUE);
    assert(widget->y == (float)RT_GUI_MAX_LAYOUT_VALUE);

    vg_splitpane_t *split = vg_splitpane_create(NULL, VG_SPLIT_HORIZONTAL);
    assert(split);
    rt_splitpane_set_position(split, NAN);
    assert(split->split_position == 0.5f);

    vg_slider_t *slider = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    assert(slider);
    rt_slider_set_range(slider, 10.0, -5.0);
    assert(slider->min_value == -5.0f);
    assert(slider->max_value == 10.0f);
    rt_slider_set_step(slider, -1.0);
    assert(slider->step == 0.0f);

    vg_image_t *image = vg_image_create(NULL);
    assert(image);
    rt_image_set_opacity(image, NAN);
    assert(image->opacity == 1.0f);
    rt_image_set_opacity(image, 2.0);
    assert(image->opacity == 1.0f);
    rt_image_set_opacity(image, -1.0);
    assert(image->opacity == 0.0f);

    vg_progressbar_t *progress = vg_progressbar_create(NULL);
    assert(progress);
    rt_progressbar_set_value(progress, NAN);
    assert(progress->value == 0.0f);
    rt_progressbar_set_value(progress, 2.0);
    assert(progress->value == 1.0f);

    vg_widget_destroy(&progress->base);
    vg_widget_destroy(&image->base);
    vg_widget_destroy(&slider->base);
    vg_widget_destroy(&split->base);
    vg_widget_destroy(widget);
    printf("test_numeric_setters_sanitize_invalid_values: PASSED\n");
}

/// @brief Verify every shared numeric narrowing path is finite and saturating.
/// @details Runtime callers can supply the complete signed 64-bit domain, while retained toolkit
///          geometry and revisions use floats, signed ints, size_t, and uint64_t. The conversion
///          helpers and representative widget/editor getters must never rely on undefined
///          float-to-integer conversion or implementation-defined unsigned narrowing.
static void test_numeric_narrowing_saturates_without_undefined_behavior(void) {
    size_t rgba_size = 0;
    assert(rt_gui_saturating_f64_to_i64(NAN) == 0);
    assert(rt_gui_saturating_f64_to_i64(INFINITY) == INT64_MAX);
    assert(rt_gui_saturating_f64_to_i64(-INFINITY) == INT64_MIN);
    assert(rt_gui_saturating_f64_to_i64(42.75) == 42);
    assert(rt_gui_saturating_f64_to_i32(NAN) == 0);
    assert(rt_gui_saturating_f64_to_i32(INFINITY) == INT32_MAX);
    assert(rt_gui_saturating_f64_to_i32(-INFINITY) == INT32_MIN);
    assert(rt_gui_saturating_u64_to_i64(UINT64_MAX) == INT64_MAX);
    assert(rt_gui_saturating_size_to_i64(SIZE_MAX) == INT64_MAX);
    assert(rt_gui_rgba_size_i64(8192, 8192, &rgba_size));
    assert(rgba_size == RT_GUI_MAX_IMAGE_PIXELS * 4u);
    assert(!rt_gui_rgba_size_i64(8193, 8192, &rgba_size));
    assert(!rt_gui_rgba_size_i64(INT64_MAX, 1, &rgba_size));
    size_t next_capacity = 0;
    assert(rt_gui_next_collection_capacity(0, 1, 8, sizeof(void *), &next_capacity));
    assert(next_capacity == 8);
    assert(rt_gui_next_collection_capacity(8, 9, 8, sizeof(void *), &next_capacity));
    assert(next_capacity == 16);
    assert(rt_gui_next_collection_capacity(700000, 700001, 8, sizeof(void *), &next_capacity));
    assert(next_capacity == RT_GUI_MAX_COLLECTION_ITEMS);
    assert(!rt_gui_next_collection_capacity(RT_GUI_MAX_COLLECTION_ITEMS,
                                            RT_GUI_MAX_COLLECTION_ITEMS,
                                            8,
                                            sizeof(void *),
                                            &next_capacity));
    assert(!rt_gui_next_collection_capacity(
        0, RT_GUI_MAX_COLLECTION_ITEMS + 1u, 8, sizeof(void *), &next_capacity));
    int next_i32_capacity = 0;
    assert(!rt_gui_next_collection_capacity_i32(-1, 0, 8, sizeof(void *), &next_i32_capacity));

    assert(rt_gui_dpi_to_physical(INT64_MAX, 2.0) == INT64_MAX);
    assert(rt_gui_dpi_to_physical(INT64_MAX, INFINITY) == INT64_MAX);
    assert(rt_gui_dpi_to_physical(10, 2.0) == 20);
    assert(rt_gui_dpi_to_logical(20, 2.0) == 10);

    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(widget);
    widget->width = INFINITY;
    widget->height = -INFINITY;
    widget->x = NAN;
    widget->y = 42.75f;
    assert(rt_widget_get_width(widget) == INT64_MAX);
    assert(rt_widget_get_height(widget) == INT64_MIN);
    assert(rt_widget_get_x(widget) == 0);
    assert(rt_widget_get_y(widget) == 42);
    vg_widget_destroy(widget);

    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    assert(editor);
    vg_codeeditor_set_text(editor, "abc");
    editor->base.width = 100.0f;
    editor->base.height = 20.0f;
    editor->char_width = 10.0f;
    editor->line_height = 10.0f;
    editor->gutter_width = 0.0f;
    editor->base.x = INFINITY;
    editor->base.y = -INFINITY;
    assert(rt_codeeditor_get_cursor_pixel_x(editor) == INT64_MAX);
    assert(rt_codeeditor_get_cursor_pixel_y(editor) == INT64_MIN);
    editor->base.x = 0.0f;
    editor->base.y = 0.0f;
    assert(rt_codeeditor_get_line_at_pixel(editor, INT64_MAX) == 0);
    assert(rt_codeeditor_get_line_at_pixel(editor, INT64_MIN) == 0);
    assert(rt_codeeditor_get_col_at_pixel(editor, INT64_MAX, 0) == 3);
    assert(rt_codeeditor_get_col_at_pixel(editor, INT64_MIN, 0) == 0);
    editor->gutter_icon_cap = -1;
    rt_codeeditor_set_gutter_bar(editor, 0, 0xFF00FF, 0);
    assert(editor->gutter_icon_count == 0);
    editor->gutter_icon_cap = 0;
    editor->fold_region_cap = -1;
    rt_codeeditor_add_fold_region(editor, 0, 1);
    assert(editor->fold_region_count == 0);
    editor->fold_region_cap = 0;
    editor->extra_cursor_cap = -1;
    rt_codeeditor_add_cursor(editor, 0, 1);
    assert(editor->extra_cursor_count == 0);
    editor->extra_cursor_cap = 0;
    editor->highlight_span_cap = -1;
    rt_codeeditor_add_highlight(editor, 0, 0, 0, 1, 0xFFFFFFFF);
    assert(editor->highlight_span_count == 0);
    editor->highlight_span_cap = 0;
    editor->semantic_token_cap = -1;
    rt_codeeditor_add_semantic_token(editor, 0, 0, 1, 0);
    assert(editor->semantic_token_count == 0);
    editor->semantic_token_cap = 0;
    vg_widget_destroy(&editor->base);

    printf("test_numeric_narrowing_saturates_without_undefined_behavior: PASSED\n");
}

static void test_font_destroy_defers_live_app_font(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x5000;
    app.default_font_owned = 1;
    app.default_font_size = 14.0f;
    rt_gui_activate_app(&app);

    rt_font_destroy(app.default_font);
    assert(app.retired_font_count == 1);
    assert(app.retired_fonts[0] == app.default_font);

    app.default_font = NULL;
    cleanup_fake_app(&app);
    printf("test_font_destroy_defers_live_app_font: PASSED\n");
}

/// @brief Verify managed system-role fonts, logical metadata, palette retention, and generation GC.
/// @details The test does not create a native window. It exercises deterministic embedded fallback
///          as necessary, proves Result can safely own Font payloads, proves ThemePalette retains
///          managed wrappers, and then verifies a backing font survives explicit Destroy while an
///          app references it but is reclaimed after a later unused frame generation.
static void test_managed_system_fonts_and_generation_retirement(void) {
    void *regular_result = rt_font_load_system_ui(13.5);
    assert(regular_result && rt_result_is_ok(regular_result));
    void *regular = rt_result_unwrap(regular_result);
    assert(rt_gui_font_handle_is_managed(regular));
    assert(fabs(rt_font_get_logical_size(regular) - 13.5) < 0.0001);

    void *palette = rt_theme_palette_new();
    assert(palette);
    rt_theme_palette_set_font_roles(palette, regular, regular, regular);
    release_test_runtime_object(regular_result);
    assert(rt_gui_font_handle_is_managed(regular));
    assert(rt_gui_font_handle_checked(regular));
    release_test_runtime_object(palette);
    assert(!rt_gui_font_handle_is_managed(regular));

    void *invalid = rt_font_load_system_ui(NAN);
    assert(invalid && rt_result_is_err(invalid));
    assert(strcmp(rt_string_cstr(rt_result_unwrap_err_str(invalid)),
                  "font size must be finite and between 1 and 512 logical points") == 0);
    release_test_runtime_object(invalid);

    void *bold_result = rt_font_load_system_ui_bold(16.0);
    assert(bold_result && rt_result_is_ok(bold_result));
    void *bold = rt_result_unwrap(bold_result);
    vg_font_t *backing = rt_gui_font_handle_checked(bold);
    assert(backing && vg_font_is_live(backing));
    assert(fabs(rt_font_get_logical_size(bold) - 16.0) < 0.0001);

    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = backing;
    rt_gui_activate_app(&app);
    rt_font_destroy(bold);
    assert(rt_font_get_logical_size(bold) == 0.0);
    assert(app.retired_font_count == 1);
    assert(app.retired_fonts[0] == backing);
    assert(vg_font_is_live(backing));

    app.default_font = NULL;
    if (app.theme) {
        if (vg_theme_get_current() == app.theme)
            vg_theme_set_current(vg_theme_dark());
        vg_theme_destroy(app.theme);
        app.theme = NULL;
    }
    app.frame_generation = 1;
    rt_gui_collect_retired_fonts(&app);
    assert(app.retired_font_count == 0);
    assert(!vg_font_is_live(backing));
    release_test_runtime_object(bold_result);
    cleanup_fake_app(&app);
    printf("test_managed_system_fonts_and_generation_retirement: PASSED\n");
}

static void test_detached_widgets_do_not_inherit_current_app_font(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 23.0f;
    rt_gui_activate_app(&app);

    vg_label_t *label = (vg_label_t *)rt_label_new(NULL, rt_const_cstr("detached"));
    assert(label);
    assert(label->font != app.default_font);

    vg_widget_destroy(&label->base);
    cleanup_fake_app(&app);
    printf("test_detached_widgets_do_not_inherit_current_app_font: PASSED\n");
}

static void test_app_font_size_and_late_attach_reapply_default_font(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 12.0f;
    rt_gui_activate_app(&app);

    vg_label_t *attached = (vg_label_t *)rt_label_new(app.root, rt_const_cstr("attached"));
    assert(attached);
    assert(attached->font == app.default_font);
    assert(attached->font_size == 12.0f);
    rt_app_set_font_size(&app, 21.0);
    assert(attached->font == app.default_font);
    assert(attached->font_size == 21.0f);

    vg_label_t *detached = (vg_label_t *)rt_label_new(NULL, rt_const_cstr("detached"));
    assert(detached);
    assert(detached->font != app.default_font);
    rt_widget_add_child(app.root, detached);
    assert(detached->font == app.default_font);
    assert(detached->font_size == 21.0f);

    vg_floatingpanel_t *panel = (vg_floatingpanel_t *)rt_floatingpanel_new(&app);
    assert(panel);
    vg_label_t *panel_child = (vg_label_t *)rt_label_new(NULL, rt_const_cstr("panel child"));
    assert(panel_child);
    assert(panel_child->font != app.default_font);
    rt_floatingpanel_add_child(panel, panel_child);
    assert(panel_child->font == app.default_font);
    assert(panel_child->font_size == 21.0f);

    cleanup_fake_app(&app);
    printf("test_app_font_size_and_late_attach_reapply_default_font: PASSED\n");
}

static void test_messagebox_show_after_destroy_returns_minus_one(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *box = rt_messagebox_new_info(rt_const_cstr("title"), rt_const_cstr("body"));
    assert(box);
    rt_messagebox_add_button(box, rt_const_cstr("OK"), 7);
    rt_messagebox_destroy(box);
    assert(rt_messagebox_show(box) == -1);

    cleanup_fake_app(&app);
    printf("test_messagebox_show_after_destroy_returns_minus_one: PASSED\n");
}

static void test_messagebox_one_shots_without_window_return_fallbacks(void) {
    rt_gui_activate_app(NULL);
    assert(rt_messagebox_info(rt_const_cstr("title"), rt_const_cstr("body")) == 0);
    assert(rt_messagebox_warning(rt_const_cstr("title"), rt_const_cstr("body")) == 0);
    assert(rt_messagebox_error(rt_const_cstr("title"), rt_const_cstr("body")) == 0);
    assert(rt_messagebox_question(rt_const_cstr("title"), rt_const_cstr("body")) == 0);
    assert(rt_messagebox_confirm(rt_const_cstr("title"), rt_const_cstr("body")) == 0);
    printf("test_messagebox_one_shots_without_window_return_fallbacks: PASSED\n");
}

static void test_toast_duration_is_clamped(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    rt_gui_activate_app(&app);

    void *negative = rt_toast_new(rt_const_cstr("negative"), RT_TOAST_INFO, -1);
    assert(negative);
    assert(app.notification_manager);
    assert(app.notification_manager->notification_count == 1);
    assert(app.notification_manager->notifications[0]->duration_ms == 0);

    void *huge = rt_toast_new(rt_const_cstr("huge"), RT_TOAST_INFO, (int64_t)UINT32_MAX + 99);
    assert(huge);
    assert(app.notification_manager->notification_count == 2);
    assert(app.notification_manager->notifications[1]->duration_ms == UINT32_MAX);

    rt_gui_features_cleanup(&app);
    rt_gui_activate_app(NULL);
    printf("test_toast_duration_is_clamped: PASSED\n");
}

static void test_toast_shortcuts_coalesce_exact_active_messages(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.scheduler_clock_ms = 100.0;
    rt_gui_activate_app(&app);

    rt_toast_warning(rt_const_cstr("workspace changed"));
    assert(app.notification_manager);
    assert(app.notification_manager->notification_count == 1);
    vg_notification_t *first = app.notification_manager->notifications[0];
    assert(first);
    assert(first->type == VG_NOTIFICATION_WARNING);
    assert(first->created_at == 100);

    app.scheduler_clock_ms = 250.0;
    rt_toast_warning(rt_const_cstr("workspace changed"));
    assert(app.notification_manager->notification_count == 1);
    assert(app.notification_manager->notifications[0] == first);
    assert(first->created_at == 250);

    rt_toast_info(rt_const_cstr("workspace changed"));
    rt_toast_warning(rt_const_cstr("different warning"));
    assert(app.notification_manager->notification_count == 3);

    vg_notification_dismiss(app.notification_manager, first->id);
    rt_toast_warning(rt_const_cstr("workspace changed"));
    assert(app.notification_manager->notification_count == 4);

    rt_gui_features_cleanup(&app);
    rt_gui_activate_app(NULL);
    printf("test_toast_shortcuts_coalesce_exact_active_messages: PASSED\n");
}

static void test_toast_dismissal_is_edge_triggered_and_survives_cleanup(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    rt_gui_activate_app(&app);

    void *toast = rt_toast_new(rt_const_cstr("dismiss"), RT_TOAST_INFO, 0);
    assert(toast);
    rt_toast_dismiss(toast);
    assert(rt_toast_was_dismissed(toast) == 1);
    assert(rt_toast_was_dismissed(toast) == 0);

    void *stale = rt_toast_new(rt_const_cstr("cleanup"), RT_TOAST_INFO, 0);
    assert(stale);
    rt_gui_features_cleanup(&app);
    assert(rt_toast_was_dismissed(stale) == 1);
    assert(rt_toast_was_dismissed(stale) == 0);

    rt_gui_activate_app(NULL);
    printf("test_toast_dismissal_is_edge_triggered_and_survives_cleanup: PASSED\n");
}

static void test_breadcrumb_set_path_uses_literal_separator(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *crumb = rt_breadcrumb_new(&app);
    assert(crumb);
    rt_breadcrumb_set_path(crumb, rt_const_cstr("alpha::beta::gamma"), rt_const_cstr("::"));

    rt_breadcrumb_data_view_t *view = (rt_breadcrumb_data_view_t *)crumb;
    assert(view->breadcrumb);
    assert(view->breadcrumb->item_count == 3);
    assert(strcmp(view->breadcrumb->items[0].label, "alpha") == 0);
    assert(strcmp(view->breadcrumb->items[1].label, "beta") == 0);
    assert(strcmp(view->breadcrumb->items[2].label, "gamma") == 0);

    rt_breadcrumb_destroy(crumb);
    cleanup_fake_app(&app);
    printf("test_breadcrumb_set_path_uses_literal_separator: PASSED\n");
}

static void test_breadcrumb_clicked_data_preserves_embedded_nuls(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *crumb = rt_breadcrumb_new(&app);
    assert(crumb);
    const char payload[] = {'a', '\0', 'b'};
    rt_breadcrumb_add_item(
        crumb, rt_const_cstr("node"), rt_string_from_bytes(payload, sizeof(payload)));

    rt_breadcrumb_data_view_t *view = (rt_breadcrumb_data_view_t *)crumb;
    assert(view->breadcrumb && view->breadcrumb->item_count == 1);
    assert(view->breadcrumb->on_click);
    view->breadcrumb->on_click(view->breadcrumb, 0, view->breadcrumb->user_data);

    assert(rt_breadcrumb_was_item_clicked(crumb) == 1);
    assert(rt_breadcrumb_get_clicked_index(crumb) == 0);
    rt_string clicked = rt_breadcrumb_get_clicked_data(crumb);
    assert(rt_str_len(clicked) == (int64_t)sizeof(payload));
    assert(memcmp(rt_string_cstr(clicked), payload, sizeof(payload)) == 0);
    assert(rt_breadcrumb_was_item_clicked(crumb) == 0);
    assert(rt_breadcrumb_get_clicked_index(crumb) == -1);
    assert(rt_str_len(rt_breadcrumb_get_clicked_data(crumb)) == 0);

    rt_breadcrumb_destroy(crumb);
    cleanup_fake_app(&app);
    printf("test_breadcrumb_clicked_data_preserves_embedded_nuls: PASSED\n");
}

static void test_shortcuts_reject_invalid_bindings_atomically(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    s_current_app = &app;

    rt_shortcuts_register(rt_const_cstr("bad"), rt_const_cstr("Ctrl+NotAKey"), rt_const_cstr(""));
    assert(app.shortcut_count == 0);
    rt_shortcuts_register(rt_const_cstr("bad2"), rt_const_cstr("Ctrl+Bogus+S"), rt_const_cstr(""));
    assert(app.shortcut_count == 0);
    rt_shortcuts_register(rt_const_cstr("bad3"), rt_const_cstr("F1x"), rt_const_cstr(""));
    assert(app.shortcut_count == 0);
    const char bad_id[] = {'s', 'a', 'v', 'e', '\0', 'x'};
    const char bad_keys[] = {'C', 't', 'r', 'l', '+', 'S', '\0', 'x'};
    rt_shortcuts_register(
        rt_string_from_bytes(bad_id, sizeof(bad_id)), rt_const_cstr("Ctrl+S"), rt_const_cstr(""));
    assert(app.shortcut_count == 0);
    rt_shortcuts_register(
        rt_const_cstr("bad4"), rt_string_from_bytes(bad_keys, sizeof(bad_keys)), rt_const_cstr(""));
    assert(app.shortcut_count == 0);

    rt_shortcuts_register(rt_const_cstr("save"), rt_const_cstr("Ctrl+S"), rt_const_cstr("save"));
    assert(app.shortcut_count == 1);
    assert(app.shortcuts[0].first.key == 'S');
    rt_shortcuts_unregister(rt_string_from_bytes(bad_id, sizeof(bad_id)));
    assert(app.shortcut_count == 1);

    rt_shortcuts_register(
        rt_const_cstr("save"), rt_const_cstr("Ctrl+NotAKey"), rt_const_cstr("bad"));
    assert(app.shortcut_count == 1);
    assert(strcmp(app.shortcuts[0].keys, "Ctrl+S") == 0);
    assert(app.shortcuts[0].first.key == 'S');

    rt_shortcuts_register(rt_const_cstr("help"), rt_const_cstr("f5"), rt_const_cstr("help"));
    rt_shortcuts_register(
        rt_const_cstr("clearcursors"), rt_const_cstr("Escape"), rt_const_cstr("clear"));
    rt_shortcuts_register(
        rt_const_cstr("unsafe_plain"), rt_const_cstr("S"), rt_const_cstr("plain"));
    assert(app.shortcut_count == 4);
    assert(app.shortcuts[1].first.key == VG_KEY_F5);
    assert(rt_shortcuts_check_key(&app, VG_KEY_F5, 0) == 1);
    rt_shortcuts_clear_triggered(&app);
    assert(rt_shortcuts_check_key(&app, VG_KEY_ESCAPE, 0) == 1);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("clearcursors")) == 1);
    rt_shortcuts_clear_triggered(&app);
    assert(rt_shortcuts_check_key(&app, 'S', 0) == 0);
    assert(rt_shortcuts_was_triggered(rt_const_cstr("unsafe_plain")) == 0);
    app.shortcuts_global_enabled = 0;
    assert(rt_shortcuts_check_key(&app, 'S', VG_MOD_CTRL) == 0);

    rt_shortcuts_clear();
    s_current_app = NULL;
    printf("test_shortcuts_reject_invalid_bindings_atomically: PASSED\n");
}

static void test_close_prevention_tracks_request_without_closing(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    rt_gui_activate_app(&app);

    rt_app_set_prevent_close(&app, 1);
    assert(app.prevent_close == 1);
    app.close_requested = 1;
    app.should_close = 0;
    assert(rt_app_was_close_requested(&app) == 1);
    assert(rt_app_was_close_requested(&app) == 0);
    assert(rt_gui_app_should_close(&app) == 0);

    rt_app_set_prevent_close(&app, 0);
    assert(app.prevent_close == 0);
    rt_gui_activate_app(NULL);
    printf("test_close_prevention_tracks_request_without_closing: PASSED\n");
}

static void test_menu_context_toolbar_statusbar_handles_are_type_checked(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    assert(rt_menubar_add_menu(&app, rt_const_cstr("Bad")) == NULL);
    vg_menubar_t *menubar = (vg_menubar_t *)rt_menubar_new(app.root);
    assert(menubar);
    void *menu = rt_menubar_add_menu(menubar, rt_const_cstr("File"));
    void *submenu = rt_menu_add_submenu(menu, rt_const_cstr("Recent"));
    assert(menu && submenu);
    rt_menubar_remove_menu(menubar, menu);
    assert(rt_menu_add_item(menu, rt_const_cstr("Open")) == NULL);
    assert(rt_menu_add_item(submenu, rt_const_cstr("Stale")) == NULL);
    assert(rt_menu_get_item_count(menu) == 0);

    assert(rt_contextmenu_add_item(&app, rt_const_cstr("Bad")) == NULL);
    void *context = rt_contextmenu_new();
    assert(context);
    rt_contextmenu_destroy(context);
    assert(rt_contextmenu_add_item(context, rt_const_cstr("Stale")) == NULL);

    vg_statusbar_t *statusbar = (vg_statusbar_t *)rt_statusbar_new(app.root);
    assert(statusbar);
    assert(rt_statusbar_add_text(&app, rt_const_cstr("bad"), 0) == NULL);
    assert(rt_statusbar_add_text(statusbar, rt_const_cstr("bad"), 99) == NULL);
    assert(statusbar->left_count == 0 && statusbar->center_count == 0 &&
           statusbar->right_count == 0);

    vg_toolbar_t *toolbar = (vg_toolbar_t *)rt_toolbar_new(app.root);
    assert(toolbar);
    assert(rt_toolbar_add_button(&app, rt_const_cstr(""), rt_const_cstr("")) == NULL);
    app.root->needs_layout = false;
    rt_toolbar_set_visible(toolbar, 0);
    assert(toolbar->base.visible == false);
    assert(app.root->needs_layout == true);

    uint64_t magic_before = app.magic;
    rt_container_set_spacing(&app, 4.0);
    rt_container_set_padding(&app, 4.0);
    assert(app.magic == magic_before);

    cleanup_fake_app(&app);
    printf("test_menu_context_toolbar_statusbar_handles_are_type_checked: PASSED\n");
}

static void test_toolbar_set_style_rejects_unknown_values(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_toolbar_t *toolbar = (vg_toolbar_t *)rt_toolbar_new(app.root);
    assert(toolbar);
    rt_toolbar_set_style(toolbar, RT_TOOLBAR_STYLE_ICON_ONLY);
    assert(toolbar->show_labels == false);
    rt_toolbar_set_style(toolbar, 99);
    assert(toolbar->show_labels == false);
    rt_toolbar_set_style(toolbar, RT_TOOLBAR_STYLE_ICON_TEXT);
    assert(toolbar->show_labels == true);
    rt_toolbar_set_style(toolbar, -1);
    assert(toolbar->show_labels == true);

    cleanup_fake_app(&app);
    printf("test_toolbar_set_style_rejects_unknown_values: PASSED\n");
}

static void test_floatingpanel_and_tabbar_reject_wrong_or_out_of_range_handles(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_floatingpanel_t *panel = (vg_floatingpanel_t *)rt_floatingpanel_new(&app);
    assert(panel);
    vg_label_t *label = (vg_label_t *)rt_label_new(NULL, rt_const_cstr("child"));
    assert(label);
    rt_floatingpanel_add_child(&app, label);
    assert(label->base.parent == NULL);
    rt_floatingpanel_add_child(panel, label);
    assert(label->base.parent == &panel->base);
    rt_floatingpanel_destroy(&app);
    assert(vg_widget_is_live(&panel->base));
    rt_floatingpanel_destroy(panel);
    assert(!vg_widget_is_live(&panel->base));

    vg_tabbar_t *tabbar = (vg_tabbar_t *)rt_tabbar_new(app.root);
    assert(tabbar);
    rt_tabbar_add_tab(tabbar, rt_const_cstr("One"), 0);
    assert(rt_tabbar_get_tab_at(tabbar, -1) == NULL);
    assert(rt_tabbar_get_tab_at(tabbar, (int64_t)INT_MAX + 1) == NULL);
    assert(rt_tabbar_get_tab_at(tabbar, 1) == NULL);
    assert(rt_tabbar_get_tab_at(tabbar, 0) != NULL);

    cleanup_fake_app(&app);
    printf("test_floatingpanel_and_tabbar_reject_wrong_or_out_of_range_handles: PASSED\n");
}

static void test_breadcrumb_minimap_methods_after_destroy_are_inert(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *crumb = rt_breadcrumb_new(app.root);
    assert(crumb);
    rt_breadcrumb_destroy(crumb);
    rt_breadcrumb_set_path(crumb, rt_const_cstr("a/b"), rt_const_cstr("/"));
    rt_breadcrumb_set_visible(crumb, 1);
    assert(rt_breadcrumb_is_visible(crumb) == 0);
    assert(rt_breadcrumb_get_clicked_index(crumb) == -1);

    void *minimap = rt_minimap_new(app.root);
    assert(minimap);
    rt_minimap_destroy(minimap);
    rt_minimap_set_width(minimap, 200);
    rt_minimap_set_visible(minimap, 1);
    rt_minimap_add_marker(minimap, 1, 0xFF0000FF, 0);
    assert(rt_minimap_get_width(minimap) == 0);
    assert(rt_minimap_is_visible(minimap) == 0);

    cleanup_fake_app(&app);
    printf("test_breadcrumb_minimap_methods_after_destroy_are_inert: PASSED\n");
}

static void test_minimap_editor_destroy_unbinds_target(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *editor = rt_codeeditor_new(app.root);
    void *minimap = rt_minimap_new(app.root);
    assert(editor && minimap);
    rt_minimap_bind_editor(minimap, editor);

    rt_minimap_data_view_t *view = (rt_minimap_data_view_t *)minimap;
    assert(view->minimap->editor == (vg_codeeditor_t *)editor);

    rt_widget_destroy(editor);
    assert(view->minimap->editor == NULL);

    rt_minimap_destroy(minimap);
    cleanup_fake_app(&app);
    printf("test_minimap_editor_destroy_unbinds_target: PASSED\n");
}

/// @brief Verify the public Minimap source-revision and bounded-cache management contract.
/// @details Content changes are observed without consuming the revision; invalid ranges remain
///          inert, and disabling the cache reports zero entries deterministically.
static void test_minimap_revision_and_cache_runtime_contract(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    void *editor = rt_codeeditor_new(app.root);
    void *minimap = rt_minimap_new(app.root);
    assert(editor && minimap);
    rt_minimap_bind_editor(minimap, editor);
    const int64_t initial_revision = rt_minimap_get_source_revision(minimap);
    assert(initial_revision > 0);
    assert(rt_minimap_get_source_revision(minimap) == initial_revision);

    rt_codeeditor_set_text(editor, rt_const_cstr("alpha\nbeta\ngamma"));
    const int64_t content_revision = rt_minimap_get_source_revision(minimap);
    assert(content_revision > initial_revision);
    assert(rt_minimap_get_source_revision(minimap) == content_revision);
    rt_minimap_invalidate_lines(minimap, 1, 1);
    rt_minimap_invalidate_lines(minimap, -1, 2);
    rt_minimap_invalidate_lines(minimap, 0, 0);
    rt_minimap_set_maximum_cached_lines(minimap, 0);
    assert(rt_minimap_get_cached_line_count(minimap) == 0);
    rt_minimap_set_maximum_cached_lines(minimap, 16);
    assert(rt_minimap_get_cached_line_count(minimap) == 0);

    rt_minimap_destroy(minimap);
    cleanup_fake_app(&app);
    printf("test_minimap_revision_and_cache_runtime_contract: PASSED\n");
}

static void test_type_specific_widget_apis_reject_wrong_types(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_label_t *label = (vg_label_t *)rt_label_new(app.root, rt_const_cstr("label"));
    vg_button_t *button = (vg_button_t *)rt_button_new(app.root, rt_const_cstr("button"));
    assert(label && button);

    rt_label_set_text(button, rt_const_cstr("wrong"));
    rt_button_set_text(label, rt_const_cstr("wrong"));
    assert(strcmp(label->text, "label") == 0);
    assert(strcmp(button->text, "button") == 0);

    assert(rt_dropdown_add_item(label, rt_const_cstr("wrong")) == -1);
    rt_slider_set_value(label, 0.5);
    assert(rt_slider_get_value(label) == 0.0);

    uint64_t magic_before = app.magic;
    rt_widget_set_draggable(&app, 1);
    rt_widget_set_drag_data(&app, rt_const_cstr("text/plain"), rt_const_cstr("data"));
    assert(app.magic == magic_before);

    cleanup_fake_app(&app);
    printf("test_type_specific_widget_apis_reject_wrong_types: PASSED\n");
}

static void test_findbar_methods_after_destroy_are_inert(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *editor = rt_codeeditor_new(app.root);
    void *bar = rt_findbar_new(app.root);
    assert(editor && bar);
    rt_findbar_bind_editor(bar, editor);
    rt_findbar_destroy(bar);

    rt_findbar_bind_editor(bar, editor);
    rt_findbar_set_find_text(bar, rt_const_cstr("x"));
    rt_findbar_set_replace_text(bar, rt_const_cstr("y"));
    rt_findbar_set_case_sensitive(bar, 1);
    assert(rt_findbar_find_next(bar) == 0);
    assert(rt_findbar_replace(bar) == 0);
    assert(rt_findbar_get_match_count(bar) == 0);
    assert(rt_findbar_is_visible(bar) == 0);

    cleanup_fake_app(&app);
    printf("test_findbar_methods_after_destroy_are_inert: PASSED\n");
}

static void test_findbar_parent_destroy_disconnects_wrapper(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_widget_t *container = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(container);
    vg_widget_add_child(app.root, container);

    void *editor = rt_codeeditor_new(app.root);
    void *bar = rt_findbar_new(container);
    assert(editor && bar);
    rt_findbar_bind_editor(bar, editor);
    rt_findbar_set_find_text(bar, rt_const_cstr("before-parent-destroy"));

    rt_findbar_data_view_t *view = (rt_findbar_data_view_t *)bar;
    assert(view->bar != NULL);
    rt_widget_destroy(container);
    assert(view->bar == NULL);

    rt_findbar_set_find_text(bar, rt_const_cstr("x"));
    rt_findbar_set_replace_text(bar, rt_const_cstr("y"));
    assert(rt_findbar_find_next(bar) == 0);
    assert(rt_findbar_replace(bar) == 0);
    assert(rt_findbar_get_match_count(bar) == 0);

    rt_findbar_destroy(bar);
    assert(view->magic == 0);

    cleanup_fake_app(&app);
    printf("test_findbar_parent_destroy_disconnects_wrapper: PASSED\n");
}

static void test_findbar_editor_destroy_unbinds_target(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *editor = rt_codeeditor_new(app.root);
    void *bar = rt_findbar_new(app.root);
    assert(editor && bar);
    rt_findbar_bind_editor(bar, editor);

    rt_findbar_data_view_t *view = (rt_findbar_data_view_t *)bar;
    assert(view->bound_editor == editor);
    assert(view->bar->target_editor == (vg_codeeditor_t *)editor);

    rt_widget_destroy(editor);
    assert(view->bound_editor == NULL);
    assert(view->bar->target_editor == NULL);
    assert(rt_findbar_find_next(bar) == 0);

    cleanup_fake_app(&app);
    printf("test_findbar_editor_destroy_unbinds_target: PASSED\n");
}

static void test_unparented_editor_destroy_unbinds_findbar_and_minimap(void) {
    rt_gui_activate_app(NULL);
    void *editor = rt_codeeditor_new(NULL);
    void *bar = rt_findbar_new(NULL);
    void *minimap = rt_minimap_new(NULL);
    assert(editor && bar && minimap);

    rt_findbar_bind_editor(bar, editor);
    rt_minimap_bind_editor(minimap, editor);

    rt_findbar_data_view_t *bar_view = (rt_findbar_data_view_t *)bar;
    rt_minimap_data_view_t *minimap_view = (rt_minimap_data_view_t *)minimap;
    assert(bar_view->bound_editor == editor);
    assert(bar_view->bar->target_editor == (vg_codeeditor_t *)editor);
    assert(minimap_view->minimap->editor == (vg_codeeditor_t *)editor);

    rt_widget_destroy(editor);
    assert(bar_view->bound_editor == NULL);
    assert(bar_view->bar->target_editor == NULL);
    assert(minimap_view->minimap->editor == NULL);

    rt_findbar_destroy(bar);
    rt_minimap_destroy(minimap);
    printf("test_unparented_editor_destroy_unbinds_findbar_and_minimap: PASSED\n");
}

static void test_radiogroup_runtime_handle_invalidates_after_destroy(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *group = rt_radiogroup_new();
    assert(group);
    vg_radiobutton_t *radio =
        (vg_radiobutton_t *)rt_radiobutton_new(app.root, rt_const_cstr("one"), group);
    assert(radio && radio->group != NULL);
    rt_radiogroup_destroy(group);
    assert(radio->group == NULL);
    assert(rt_radiobutton_new(app.root, rt_const_cstr("bad"), group) == NULL);
    rt_radiobutton_set_selected(radio, 1);
    assert(rt_radiobutton_is_selected(radio) == 1);

    cleanup_fake_app(&app);
    printf("test_radiogroup_runtime_handle_invalidates_after_destroy: PASSED\n");
}

static void test_filedialog_setters_after_destroy_are_inert(void) {
    void *dialog = rt_filedialog_new_open();
    assert(dialog);
    rt_filedialog_destroy(dialog);
    rt_filedialog_set_title(dialog, rt_const_cstr("title"));
    rt_filedialog_set_path(dialog, rt_const_cstr("/tmp"));
    rt_filedialog_set_filter(dialog, rt_const_cstr("Files"), rt_const_cstr("*"));
    rt_filedialog_add_filter(dialog, rt_const_cstr("Text"), rt_const_cstr("*.txt"));
    rt_filedialog_set_default_name(dialog, rt_const_cstr("out.txt"));
    rt_filedialog_set_multiple(dialog, 1);
    assert(rt_filedialog_show(dialog) == 0);
    assert(rt_filedialog_get_path_count(dialog) == 0);
    assert(strcmp(rt_string_cstr(rt_filedialog_get_path(dialog)), "") == 0);
    printf("test_filedialog_setters_after_destroy_are_inert: PASSED\n");
}

static void test_filedialog_destroy_removes_modal_stack_entry(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *dialog = rt_filedialog_new_open();
    assert(dialog);
    rt_filedialog_data_view_t *view = (rt_filedialog_data_view_t *)dialog;
    assert(view->dialog);

    vg_filedialog_show(view->dialog);
    rt_gui_push_dialog(&app, &view->dialog->base);
    assert(app.dialog_count == 1);
    assert(vg_widget_get_modal_root() == &view->dialog->base.base);

    rt_filedialog_destroy(dialog);
    assert(app.dialog_count == 0);
    assert(vg_widget_get_modal_root() == NULL);

    cleanup_fake_app(&app);
    printf("test_filedialog_destroy_removes_modal_stack_entry: PASSED\n");
}

static void test_app_destroy_invalidates_stateful_dialog_wrappers(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *box = rt_messagebox_new_info(rt_const_cstr("title"), rt_const_cstr("body"));
    void *file = rt_filedialog_new_open();
    void *editor = rt_codeeditor_new(app.root);
    void *bar = rt_findbar_new(NULL);
    assert(box && file && editor && bar);
    rt_messagebox_data_view_t *box_view = (rt_messagebox_data_view_t *)box;
    rt_filedialog_data_view_t *file_view = (rt_filedialog_data_view_t *)file;
    rt_findbar_data_view_t *bar_view = (rt_findbar_data_view_t *)bar;
    assert(box_view->dialog && file_view->dialog);
    rt_findbar_bind_editor(bar, editor);
    assert(bar_view->bound_editor == editor);

    vg_dialog_show(box_view->dialog);
    vg_filedialog_show(file_view->dialog);
    rt_gui_push_dialog(&app, box_view->dialog);
    rt_gui_push_dialog(&app, &file_view->dialog->base);
    assert(app.dialog_count == 2);

    rt_gui_app_destroy(&app);
    assert(box_view->dialog == NULL);
    assert(file_view->dialog == NULL);
    assert(bar_view->bound_editor == NULL);
    assert(bar_view->bar->target_editor == NULL);
    assert(rt_messagebox_show(box) == -1);
    assert(rt_filedialog_show(file) == 0);

    rt_messagebox_destroy(box);
    rt_filedialog_destroy(file);
    rt_findbar_destroy(bar);
    rt_gui_activate_app(NULL);
    printf("test_app_destroy_invalidates_stateful_dialog_wrappers: PASSED\n");
}

static void test_messagebox_custom_button_ids_preserve_zero_and_i64(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *box = rt_messagebox_new_info(rt_const_cstr("title"), rt_const_cstr("body"));
    assert(box);
    int64_t large_id = ((int64_t)1 << 40) + 17;
    rt_messagebox_add_button(box, rt_const_cstr("Zero"), 0);
    rt_messagebox_add_button(box, rt_const_cstr("Large"), large_id);

    rt_messagebox_data_view_t *view = (rt_messagebox_data_view_t *)box;
    assert(view->custom_button_count == 2);
    assert(view->custom_button_ids[0] == 0);
    assert(view->custom_button_ids[1] == large_id);
    assert(view->custom_buttons[0].result == VG_DIALOG_RESULT_CUSTOM_1);
    assert(view->custom_buttons[1].result == (vg_dialog_result_t)(VG_DIALOG_RESULT_CUSTOM_1 + 1));
    assert(view->custom_buttons[0].is_default == false);
    assert(view->custom_buttons[1].is_default == false);

    rt_messagebox_set_default_button(box, large_id);
    assert(view->custom_buttons[1].is_default == true);

    vg_dialog_set_custom_buttons(view->dialog, view->custom_buttons, view->custom_button_count);
    assert(view->dialog->custom_buttons[0].result == VG_DIALOG_RESULT_CUSTOM_1);
    assert(view->dialog->custom_buttons[1].result ==
           (vg_dialog_result_t)(VG_DIALOG_RESULT_CUSTOM_1 + 1));

    rt_messagebox_destroy(box);
    cleanup_fake_app(&app);
    printf("test_messagebox_custom_button_ids_preserve_zero_and_i64: PASSED\n");
}

/// @brief Verify localizable button roles, unique IDs, and exactly-once async completion state.
/// @details Uses non-English labels to prove Enter/Escape semantics derive from roles rather than
///          text. Lower close callbacks are invoked directly so the state machine is deterministic
///          and does not require human input from the display-backed test runner.
static void test_messagebox_semantic_roles_and_async_state(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *box =
        rt_messagebox_new_question(rt_const_cstr("Lokalisierung"), rt_const_cstr("Fortfahren?"));
    assert(box);
    rt_messagebox_add_button_with_role(
        box, rt_const_cstr("Abbrechen"), 41, RT_GUI_DIALOG_BUTTON_CANCEL);
    rt_messagebox_add_button_with_role(
        box, rt_const_cstr("Weiter"), 42, RT_GUI_DIALOG_BUTTON_DEFAULT);

    rt_messagebox_data_view_t *view = (rt_messagebox_data_view_t *)box;
    assert(view->custom_button_count == 2);
    assert(view->custom_button_roles[0] == RT_GUI_DIALOG_BUTTON_CANCEL);
    assert(view->custom_button_roles[1] == RT_GUI_DIALOG_BUTTON_DEFAULT);
    assert(view->custom_buttons[0].is_cancel);
    assert(view->custom_buttons[1].is_default);
    assert(rt_messagebox_set_cancel_button(box, 41) == 1);
    assert(rt_messagebox_set_default_button(box, 42) == 1);
    assert(rt_messagebox_set_button_role(box, 42, RT_GUI_DIALOG_BUTTON_ACCEPT) == 1);
    assert(rt_messagebox_set_button_role(box, 999, RT_GUI_DIALOG_BUTTON_NORMAL) == 0);

    begin_expected_vm_trap();
    rt_messagebox_add_button_with_role(
        box, rt_const_cstr("Duplikat"), 42, RT_GUI_DIALOG_BUTTON_NORMAL);
    end_expected_vm_trap("Message box button ID must be unique: 42");
    assert(view->custom_button_count == 2);

    vg_dialog_show(view->dialog);
    view->status = RT_GUI_DIALOG_STATUS_OPEN;
    vg_dialog_close(view->dialog, view->custom_buttons[1].result);
    assert(rt_messagebox_is_open(box) == 0);
    assert(rt_messagebox_get_status(box) == RT_GUI_DIALOG_STATUS_ACCEPTED);
    assert(rt_messagebox_get_result(box) == 42);
    assert(rt_messagebox_was_completed(box) == 1);
    assert(rt_messagebox_was_completed(box) == 0);

    vg_dialog_show(view->dialog);
    view->status = RT_GUI_DIALOG_STATUS_OPEN;
    vg_dialog_close(view->dialog, view->custom_buttons[0].result);
    assert(rt_messagebox_get_status(box) == RT_GUI_DIALOG_STATUS_CANCELLED);
    assert(rt_messagebox_get_result(box) == 41);
    assert(rt_messagebox_was_completed(box) == 1);

    assert(rt_messagebox_show_async(box) == 0);
    assert(rt_messagebox_get_status(box) == RT_GUI_DIALOG_STATUS_FAILED);
    assert(rt_messagebox_was_completed(box) == 1);
    rt_string error = rt_messagebox_get_error(box);
    assert(strcmp(rt_string_cstr(error), "No active GUI application is available") == 0);
    rt_string_unref(error);

    rt_messagebox_destroy(box);
    cleanup_fake_app(&app);
    printf("test_messagebox_semantic_roles_and_async_state: PASSED\n");
}

/// @brief Verify complete cross-platform FileDialog configuration and asynchronous result
/// snapshots.
/// @details Exercises retained lower fields directly, forces an accepted callback with a path that
///          contains a semicolon, and verifies GetPaths returns an independently owned sequence.
static void test_filedialog_complete_async_contract(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    void *dialog = rt_filedialog_new_save();
    assert(dialog);
    rt_filedialog_data_view_t *view = (rt_filedialog_data_view_t *)dialog;
    assert(view->dialog);

    rt_filedialog_set_show_hidden(dialog, 1);
    rt_filedialog_set_confirm_overwrite(dialog, 1);
    rt_filedialog_set_default_extension(dialog, rt_const_cstr(".zia"));
    rt_filedialog_clear_bookmarks(dialog);
    rt_filedialog_add_bookmark(dialog, rt_const_cstr("/tmp/Zanna Projects"));
    assert(view->dialog->show_hidden);
    assert(view->dialog->confirm_overwrite);
    assert(strcmp(view->dialog->default_extension, ".zia") == 0);
    assert(view->dialog->bookmark_count == 1);
    assert(strcmp(view->dialog->bookmarks[0].path, "/tmp/Zanna Projects") == 0);

    assert(rt_filedialog_show_async(dialog) == 0);
    assert(rt_filedialog_get_status(dialog) == RT_GUI_DIALOG_STATUS_FAILED);
    assert(rt_filedialog_was_completed(dialog) == 1);
    assert(rt_filedialog_was_completed(dialog) == 0);

    vg_dialog_show(&view->dialog->base);
    view->status = RT_GUI_DIALOG_STATUS_OPEN;
    view->dialog->selected_files = (char **)calloc(1, sizeof(char *));
    assert(view->dialog->selected_files);
    view->dialog->selected_files[0] = test_strdup_local("/tmp/a;b.zia");
    assert(view->dialog->selected_files[0]);
    view->dialog->selected_file_count = 1;
    vg_dialog_close(&view->dialog->base, VG_DIALOG_RESULT_OK);

    assert(rt_filedialog_get_status(dialog) == RT_GUI_DIALOG_STATUS_ACCEPTED);
    assert(rt_filedialog_is_open(dialog) == 0);
    assert(rt_filedialog_was_completed(dialog) == 1);
    void *paths = rt_filedialog_get_paths(dialog);
    assert(paths);
    assert(rt_seq_len(paths) == 1);
    assert(strcmp(rt_string_cstr(rt_seq_get_str(paths, 0)), "/tmp/a;b.zia") == 0);
    release_test_runtime_object(paths);

    rt_filedialog_destroy(dialog);
    cleanup_fake_app(&app);
    printf("test_filedialog_complete_async_contract: PASSED\n");
}

static void test_menuitem_checkable_state_is_real_and_invalidates_context(void) {
    vg_contextmenu_t *context = vg_contextmenu_create();
    assert(context);
    vg_menu_item_t *context_item = vg_contextmenu_add_item(context, "Toggle", NULL, NULL, NULL);
    assert(context_item);
    void *context_item_handle = rt_gui_wrap_menu_item(context_item);
    assert(rt_menuitem_is_checkable(context_item_handle) == 0);
    context->base.needs_layout = false;
    rt_menuitem_set_checkable(context_item_handle, 1);
    assert(rt_menuitem_is_checkable(context_item_handle) == 1);
    rt_menuitem_set_checked(context_item_handle, 1);
    assert(rt_menuitem_is_checked(context_item_handle) == 1);
    assert(context->base.needs_layout || context->base.needs_paint);
    rt_menuitem_set_checkable(context_item_handle, 0);
    assert(rt_menuitem_is_checkable(context_item_handle) == 0);
    assert(rt_menuitem_is_checked(context_item_handle) == 0);

    vg_menubar_t *menubar = vg_menubar_create(NULL);
    assert(menubar);
    vg_menu_t *menu = vg_menubar_add_menu(menubar, "View");
    vg_menu_item_t *menu_item = vg_menu_add_item(menu, "Sidebar", NULL, NULL, NULL);
    assert(menu_item);
    void *menu_item_handle = rt_gui_wrap_menu_item(menu_item);
    assert(rt_menuitem_is_checkable(menu_item_handle) == 0);
    rt_menuitem_set_checked(menu_item_handle, 1);
    assert(rt_menuitem_is_checkable(menu_item_handle) == 1);
    assert(rt_menuitem_is_checked(menu_item_handle) == 1);

    vg_widget_destroy(&context->base);
    vg_widget_destroy(&menubar->base);
    printf("test_menuitem_checkable_state_is_real_and_invalidates_context: PASSED\n");
}

static void test_contextmenu_submenu_ownership_detaches_safely(void) {
    vg_contextmenu_t *parent = vg_contextmenu_create();
    vg_contextmenu_t *child = vg_contextmenu_create();
    assert(parent && child);
    vg_menu_item_t *item = vg_contextmenu_add_submenu(parent, "Child", child);
    assert(item);
    assert(item->submenu == (struct vg_menu *)child);
    assert(child->parent_item == item);

    vg_contextmenu_destroy(child);
    assert(item->submenu == NULL);
    vg_contextmenu_destroy(parent);

    parent = vg_contextmenu_create();
    child = vg_contextmenu_create();
    assert(parent && child);
    item = vg_contextmenu_add_submenu(parent, "Child", child);
    assert(item && item->submenu == (struct vg_menu *)child);
    vg_contextmenu_destroy(parent);

    printf("test_contextmenu_submenu_ownership_detaches_safely: PASSED\n");
}

static void test_toolbar_remove_item_removes_runtime_null_id_items(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_toolbar_t *toolbar = (vg_toolbar_t *)rt_toolbar_new(app.root);
    assert(toolbar);
    void *item_handle = rt_toolbar_add_button(toolbar, rt_const_cstr(""), rt_const_cstr(""));
    vg_toolbar_item_t *item = rt_gui_toolbar_item_from_handle(item_handle);
    assert(item && item->id == NULL);
    assert(toolbar->item_count == 1);
    rt_toolbar_remove_item(toolbar, item_handle);
    assert(toolbar->item_count == 0);

    cleanup_fake_app(&app);
    printf("test_toolbar_remove_item_removes_runtime_null_id_items: PASSED\n");
}

static void test_toolbar_statusbar_remove_clears_runtime_click_caches(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_statusbar_t *statusbar = (vg_statusbar_t *)rt_statusbar_new(app.root);
    assert(statusbar);
    void *status_item_handle = rt_statusbar_add_text(statusbar, rt_const_cstr("ready"), 0);
    vg_statusbar_item_t *status_item = rt_gui_statusbar_item_from_handle(status_item_handle);
    assert(status_item);
    app.last_statusbar_clicked = status_item;
    rt_statusbar_remove_item(statusbar, status_item_handle);
    assert(app.last_statusbar_clicked == NULL);
    assert(!vg_statusbar_item_is_live(status_item));
    assert(rt_statusbaritem_was_clicked(status_item_handle) == 0);

    vg_toolbar_t *toolbar = (vg_toolbar_t *)rt_toolbar_new(app.root);
    assert(toolbar);
    void *tool_item_handle =
        rt_toolbar_add_button(toolbar, rt_const_cstr(""), rt_const_cstr("tool"));
    vg_toolbar_item_t *tool_item = rt_gui_toolbar_item_from_handle(tool_item_handle);
    assert(tool_item);
    app.last_toolbar_clicked = tool_item;
    rt_toolbar_remove_item(toolbar, tool_item_handle);
    assert(app.last_toolbar_clicked == NULL);
    assert(!vg_toolbar_item_is_live(tool_item));
    assert(rt_toolbaritem_was_clicked(tool_item_handle) == 0);

    cleanup_fake_app(&app);
    printf("test_toolbar_statusbar_remove_clears_runtime_click_caches: PASSED\n");
}

static void test_runtime_subobject_handles_are_inert_after_owner_destroy(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_treeview_t *tree = (vg_treeview_t *)rt_treeview_new(app.root);
    assert(tree);
    void *node = rt_treeview_add_node(tree, NULL, rt_const_cstr("node"));
    assert(node);
    rt_widget_destroy(tree);
    assert(rt_str_len(rt_treeview_node_get_text(node)) == 0);
    rt_treeview_node_set_data(node, rt_const_cstr("ignored"));
    assert(rt_treeview_node_is_expanded(node) == 0);

    vg_listbox_t *listbox = (vg_listbox_t *)rt_listbox_new(app.root);
    assert(listbox);
    void *list_item = rt_listbox_add_item(listbox, rt_const_cstr("item"));
    assert(list_item);
    rt_widget_destroy(listbox);
    assert(rt_str_len(rt_listbox_item_get_text(list_item)) == 0);
    rt_listbox_item_set_text(list_item, rt_const_cstr("ignored"));

    vg_tabbar_t *tabbar = (vg_tabbar_t *)rt_tabbar_new(app.root);
    assert(tabbar);
    void *tab = rt_tabbar_add_tab(tabbar, rt_const_cstr("tab"), 1);
    assert(tab);
    rt_widget_destroy(tabbar);
    rt_tab_set_title(tab, rt_const_cstr("ignored"));
    rt_tab_set_modified(tab, 1);

    vg_menubar_t *menubar = (vg_menubar_t *)rt_menubar_new(app.root);
    assert(menubar);
    void *menu = rt_menubar_add_menu(menubar, rt_const_cstr("File"));
    void *menu_item = rt_menu_add_item(menu, rt_const_cstr("Open"));
    assert(menu && menu_item);
    rt_widget_destroy(menubar);
    assert(rt_menu_add_item(menu, rt_const_cstr("Ignored")) == NULL);
    assert(rt_menuitem_is_enabled(menu_item) == 0);

    vg_statusbar_t *statusbar = (vg_statusbar_t *)rt_statusbar_new(app.root);
    assert(statusbar);
    void *status_item = rt_statusbar_add_text(statusbar, rt_const_cstr("ready"), 0);
    assert(status_item);
    rt_widget_destroy(statusbar);
    rt_statusbaritem_set_text(status_item, rt_const_cstr("ignored"));
    assert(rt_str_len(rt_statusbaritem_get_text(status_item)) == 0);

    vg_toolbar_t *toolbar = (vg_toolbar_t *)rt_toolbar_new(app.root);
    assert(toolbar);
    void *tool_item = rt_toolbar_add_button(toolbar, rt_const_cstr(""), rt_const_cstr("tool"));
    assert(tool_item);
    rt_widget_destroy(toolbar);
    rt_toolbaritem_set_enabled(tool_item, 1);
    assert(rt_toolbaritem_is_enabled(tool_item) == 0);

    void *context = rt_contextmenu_new();
    assert(context);
    void *context_item = rt_contextmenu_add_item(context, rt_const_cstr("Open"));
    void *submenu = rt_contextmenu_add_submenu(context, rt_const_cstr("More"));
    assert(context_item && submenu);
    rt_contextmenu_destroy(context);
    assert(rt_contextmenu_add_item(context, rt_const_cstr("Ignored")) == NULL);
    assert(rt_contextmenu_is_visible(submenu) == 0);
    assert(rt_menuitem_is_enabled(context_item) == 0);

    cleanup_fake_app(&app);
    printf("test_runtime_subobject_handles_are_inert_after_owner_destroy: PASSED\n");
}

static void test_codeeditor_add_highlight_rejects_empty_and_inverted_spans(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_codeeditor_t *editor = (vg_codeeditor_t *)rt_codeeditor_new(app.root);
    assert(editor);
    rt_codeeditor_set_text(editor, rt_const_cstr("alpha\nbeta"));

    rt_codeeditor_add_highlight(editor, 0, 2, 0, 2, 0xFFFF0000);
    rt_codeeditor_add_highlight(editor, 1, 4, 0, 1, 0xFFFF0000);
    assert(editor->highlight_span_count == 0);

    rt_codeeditor_add_highlight(editor, 0, 0, 0, 5, 0xFF00FF00);
    assert(editor->highlight_span_count == 1);
    assert(editor->highlight_spans[0].start_line == 0);
    assert(editor->highlight_spans[0].end_col == 5);

    cleanup_fake_app(&app);
    printf("test_codeeditor_add_highlight_rejects_empty_and_inverted_spans: PASSED\n");
}

static void test_codeeditor_gutter_slots_are_validated_and_click_coords_are_fresh(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_codeeditor_t *editor = (vg_codeeditor_t *)rt_codeeditor_new(app.root);
    assert(editor);

    rt_codeeditor_set_gutter_icon(editor, 0, NULL, 4);
    rt_codeeditor_set_gutter_icon(editor, 0, NULL, -1);
    assert(editor->gutter_icon_count == 0);

    rt_codeeditor_set_gutter_icon(editor, 0, NULL, 3);
    assert(editor->gutter_icon_count == 0);
    void *pixels = rt_pixels_new(1, 1);
    assert(pixels);
    rt_pixels_set(pixels, 0, 0, 0x11223344);
    rt_codeeditor_set_gutter_icon(editor, 0, pixels, 3);
    assert(editor->gutter_icon_count == 1);
    rt_codeeditor_set_gutter_icon(editor, 0, NULL, 3);
    assert(editor->gutter_icon_count == 0);
    rt_codeeditor_set_gutter_icon(editor, 0, pixels, 3);
    assert(editor->gutter_icon_count == 1);
    rt_codeeditor_clear_gutter_icon(editor, 0, -1);
    assert(editor->gutter_icon_count == 1);
    rt_codeeditor_clear_all_gutter_icons(editor, 4);
    assert(editor->gutter_icon_count == 1);
    rt_codeeditor_clear_gutter_icon(editor, 0, 3);
    assert(editor->gutter_icon_count == 0);

    editor->gutter_clicked = true;
    editor->gutter_clicked_line = 7;
    editor->gutter_clicked_slot = 2;
    void *click = rt_codeeditor_take_gutter_click(editor);
    assert(click);
    assert(rt_map_get_bool(click, rt_const_cstr("clicked")) == 1);
    assert(rt_map_get_int(click, rt_const_cstr("line")) == 7);
    assert(rt_map_get_int(click, rt_const_cstr("slot")) == 2);
    assert(rt_codeeditor_was_gutter_clicked(editor) == 0);
    assert(rt_codeeditor_get_gutter_clicked_line(editor) == -1);
    assert(rt_codeeditor_get_gutter_clicked_slot(editor) == -1);

    editor->gutter_clicked = true;
    editor->gutter_clicked_line = 7;
    editor->gutter_clicked_slot = 2;
    assert(rt_codeeditor_get_gutter_clicked_line(editor) == 7);
    assert(rt_codeeditor_get_gutter_clicked_slot(editor) == 2);
    assert(rt_codeeditor_was_gutter_clicked(editor) == 1);
    assert(rt_codeeditor_was_gutter_clicked(editor) == 0);
    assert(rt_codeeditor_get_gutter_clicked_line(editor) == -1);
    assert(rt_codeeditor_get_gutter_clicked_slot(editor) == -1);

    cleanup_fake_app(&app);
    printf("test_codeeditor_gutter_slots_are_validated_and_click_coords_are_fresh: PASSED\n");
}

/// @brief Verify the runtime read-only flag blocks text mutation APIs.
/// @details Zanna Studio uses read-only CodeEditor instances for unsupported binary
///          or preview documents. This test ensures the public runtime flag
///          round-trips and `InsertAtCursor` respects it.
static void test_codeeditor_runtime_read_only_blocks_insertions(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_codeeditor_t *editor = (vg_codeeditor_t *)rt_codeeditor_new(app.root);
    assert(editor);
    rt_codeeditor_set_text(editor, rt_const_cstr("abc"));
    rt_codeeditor_set_cursor_position_at(editor, 0, 0, 3);
    assert(rt_codeeditor_get_read_only(editor) == 0);

    rt_codeeditor_set_read_only(editor, 1);
    assert(rt_codeeditor_get_read_only(editor) == 1);
    rt_codeeditor_insert_at_cursor(editor, rt_const_cstr("X"));
    rt_string text = rt_codeeditor_get_text(editor);
    assert(strcmp(rt_string_cstr(text), "abc") == 0);

    rt_codeeditor_set_read_only(editor, 0);
    assert(rt_codeeditor_get_read_only(editor) == 0);
    rt_codeeditor_insert_at_cursor(editor, rt_const_cstr("X"));
    rt_string text_after = rt_codeeditor_get_text(editor);
    assert(strcmp(rt_string_cstr(text_after), "abcX") == 0);

    cleanup_fake_app(&app);
    printf("test_codeeditor_runtime_read_only_blocks_insertions: PASSED\n");
}

/// @brief Verify the complete base-widget geometry, hierarchy, identity, and invalidation API.
/// @details A synthetic app at 2x scale proves public layout setters multiply exactly once while
///          legacy integer getters remain physical and new bounds getters divide exactly once.
///          The test also exercises owned Option lookup results, copied UTF-8 names, embedded-NUL
///          rejection, checked indices, direct-child detachment, non-destructive clear, clipping-
///          aware hit testing, stable IDs, and explicit paint/layout invalidation propagation.
static void test_widget_complete_logical_hierarchy_api(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.user_scale = 2.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_widget_t *parent = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *child = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *metrics = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(parent && child && metrics);
    rt_widget_add_child(app.root, parent);
    rt_widget_add_child(parent, child);
    rt_widget_add_child(parent, metrics);
    assert(rt_widget_get_child_count(parent) == 2);

    rt_widget_set_min_size(metrics, 10.0, 11.0);
    rt_widget_set_preferred_size(metrics, 20.0, 21.0);
    rt_widget_set_max_size(metrics, 30.0, 31.0);
    assert(metrics->constraints.min_width == 20.0f);
    assert(metrics->constraints.min_height == 22.0f);
    assert(metrics->constraints.preferred_width == 40.0f);
    assert(metrics->constraints.preferred_height == 42.0f);
    assert(metrics->constraints.max_width == 60.0f);
    assert(metrics->constraints.max_height == 62.0f);
    assert(rt_widget_get_min_width(metrics) == 10.0);
    assert(rt_widget_get_min_height(metrics) == 11.0);

    rt_widget_set_padding(parent, 4.0);
    assert(parent->layout.padding_left == 8.0f);
    assert(parent->layout.padding_bottom == 8.0f);
    rt_widget_set_padding_edges(parent, 1.0, 2.0, 3.0, 4.0);
    assert(parent->layout.padding_left == 2.0f);
    assert(parent->layout.padding_top == 4.0f);
    assert(parent->layout.padding_right == 6.0f);
    assert(parent->layout.padding_bottom == 8.0f);
    rt_widget_set_margin(parent, 3);
    assert(parent->layout.margin_left == 6.0f);
    rt_widget_set_margin_edges(parent, 5.0, 6.0, 7.0, 8.0);
    assert(parent->layout.margin_left == 10.0f);
    assert(parent->layout.margin_top == 12.0f);
    assert(parent->layout.margin_right == 14.0f);
    assert(parent->layout.margin_bottom == 16.0f);

    rt_widget_set_size(metrics, 40, 30);
    assert(metrics->constraints.min_width == 80.0f);
    assert(metrics->constraints.preferred_height == 60.0f);
    rt_widget_set_position(child, 5, -6);
    assert(child->x == 10.0f);
    assert(child->y == -12.0f);
    assert(child->manual_position);

    vg_widget_arrange(app.root, 0.0f, 0.0f, 400.0f, 300.0f);
    vg_widget_arrange(parent, 20.0f, 40.0f, 200.0f, 160.0f);
    vg_widget_arrange(child, 10.0f, 12.0f, 50.0f, 30.0f);
    assert(rt_widget_get_x(child) == 10);
    assert(rt_widget_get_y(child) == 12);
    assert(rt_widget_get_width(child) == 50);
    assert(rt_widget_get_height(child) == 30);
    assert(rt_widget_get_logical_x(child) == 5.0);
    assert(rt_widget_get_logical_y(child) == 6.0);
    assert(rt_widget_get_logical_width(child) == 25.0);
    assert(rt_widget_get_logical_height(child) == 15.0);
    assert(rt_widget_get_screen_x(child) == 15.0);
    assert(rt_widget_get_screen_y(child) == 26.0);
    assert(rt_widget_get_screen_width(child) == 25.0);
    assert(rt_widget_get_screen_height(child) == 15.0);
    assert(rt_widget_hit_test(child, 16.0, 27.0) == 1);
    assert(rt_widget_hit_test(child, 40.0, 27.0) == 0);
    rt_widget_set_enabled(child, 0);
    assert(rt_widget_hit_test(child, 16.0, 27.0) == 0);
    rt_widget_set_enabled(child, 1);

    void *option = rt_widget_get_parent_option(child);
    assert(option && rt_option_is_some(option) == 1);
    assert(rt_option_unwrap(option) == parent);
    release_test_runtime_object(option);
    option = rt_widget_get_parent_option(app.root);
    assert(option && rt_option_is_none(option) == 1);
    release_test_runtime_object(option);

    option = rt_widget_get_child_at_option(parent, 0);
    assert(option && rt_option_is_some(option) == 1);
    assert(rt_option_unwrap(option) == child);
    release_test_runtime_object(option);
    option = rt_widget_get_child_at_option(parent, -1);
    assert(option && rt_option_is_none(option) == 1);
    release_test_runtime_object(option);
    option = rt_widget_get_child_at_option(parent, INT64_MAX);
    assert(option && rt_option_is_none(option) == 1);
    release_test_runtime_object(option);

    rt_widget_set_name(child, rt_const_cstr("primaryChild"));
    rt_string name = rt_widget_get_name(child);
    assert(name && strcmp(rt_string_cstr(name), "primaryChild") == 0);
    rt_str_release_maybe(name);
    rt_string embedded_nul_name = rt_string_from_bytes("bad\0suffix", 10);
    assert(embedded_nul_name);
    rt_widget_set_name(child, embedded_nul_name);
    rt_str_release_maybe(embedded_nul_name);
    name = rt_widget_get_name(child);
    assert(name && strcmp(rt_string_cstr(name), "primaryChild") == 0);
    rt_str_release_maybe(name);

    int64_t child_id = rt_widget_get_id(child);
    assert(child_id > 0);
    option = rt_widget_find_by_id_option(app.root, child_id);
    assert(option && rt_option_is_some(option) == 1);
    assert(rt_option_unwrap(option) == child);
    release_test_runtime_object(option);
    option = rt_widget_find_by_id_option(app.root, -1);
    assert(option && rt_option_is_none(option) == 1);
    release_test_runtime_object(option);
    option = rt_widget_find_by_name_option(app.root, rt_const_cstr("primaryChild"));
    assert(option && rt_option_is_some(option) == 1);
    assert(rt_option_unwrap(option) == child);
    release_test_runtime_object(option);
    option = rt_widget_find_by_name_option(app.root, rt_const_cstr("missing"));
    assert(option && rt_option_is_none(option) == 1);
    release_test_runtime_object(option);

    app.root->needs_paint = false;
    parent->needs_paint = false;
    child->needs_paint = false;
    rt_widget_invalidate_paint(child);
    assert(child->needs_paint && parent->needs_paint && app.root->needs_paint);
    app.root->needs_layout = false;
    parent->needs_layout = false;
    child->needs_layout = false;
    rt_widget_invalidate_layout(child);
    assert(child->needs_layout && parent->needs_layout && app.root->needs_layout);

    assert(rt_widget_remove_child(parent, child) == 1);
    assert(rt_widget_remove_child(parent, child) == 0);
    assert(vg_widget_is_live(child));
    assert(child->parent == NULL);
    assert(rt_widget_get_child_count(parent) == 1);
    rt_widget_add_child(parent, child);
    assert(child->parent == parent);
    assert(rt_widget_get_child_count(parent) == 2);

    rt_widget_clear_children(parent);
    assert(rt_widget_get_child_count(parent) == 0);
    assert(child->parent == NULL && metrics->parent == NULL);
    assert(vg_widget_is_live(child) && vg_widget_is_live(metrics));
    vg_widget_destroy(child);
    vg_widget_destroy(metrics);

    cleanup_fake_app(&app);
    printf("test_widget_complete_logical_hierarchy_api: PASSED\n");
}

/// @brief Verify the uniform public event and revision contract across stateful controls.
/// @details Programmatic state changes create one consumable change edge and advance a
///          non-consuming revision. Legacy ListBox/TreeView selection edges remain independently
///          consumable, explicit Enter activation is separate from selection, Spinner submission
///          is separate from numeric change, no-op setters preserve revisions, and all type-
///          specific accessors reject a foreign widget handle.
static void test_uniform_control_events_and_revisions(void) {
    vg_checkbox_t *checkbox = (vg_checkbox_t *)rt_checkbox_new(NULL, rt_const_cstr("Enabled"));
    assert(checkbox);
    int64_t revision = rt_checkbox_get_revision(checkbox);
    assert(revision > 0 && rt_checkbox_was_changed(checkbox) == 0);
    rt_checkbox_set_checked(checkbox, 1);
    assert(rt_checkbox_get_revision(checkbox) > revision);
    assert(rt_checkbox_was_changed(checkbox) == 1);
    assert(rt_checkbox_was_changed(checkbox) == 0);
    revision = rt_checkbox_get_revision(checkbox);
    rt_checkbox_set_checked(checkbox, 1);
    assert(rt_checkbox_get_revision(checkbox) == revision);

    vg_dropdown_t *dropdown = (vg_dropdown_t *)rt_dropdown_new(NULL);
    assert(dropdown);
    assert(rt_dropdown_add_item(dropdown, rt_const_cstr("One")) == 0);
    revision = rt_dropdown_get_revision(dropdown);
    rt_dropdown_set_selected(dropdown, 0);
    assert(rt_dropdown_get_revision(dropdown) > revision);
    assert(rt_dropdown_was_changed(dropdown) == 1);
    assert(rt_dropdown_was_changed(dropdown) == 0);

    vg_slider_t *slider = (vg_slider_t *)rt_slider_new(NULL, 1);
    assert(slider);
    revision = rt_slider_get_revision(slider);
    rt_slider_set_value(slider, 25.0);
    assert(rt_slider_get_revision(slider) > revision);
    assert(rt_slider_was_changed(slider) == 1);
    assert(rt_slider_was_changed(slider) == 0);
    revision = rt_slider_get_revision(slider);
    rt_slider_set_value(slider, 25.0);
    assert(rt_slider_get_revision(slider) == revision);

    vg_spinner_t *spinner = (vg_spinner_t *)rt_spinner_new(NULL);
    assert(spinner);
    revision = rt_spinner_get_revision(spinner);
    rt_spinner_set_value(spinner, 4.0);
    assert(rt_spinner_get_revision(spinner) > revision);
    assert(rt_spinner_was_changed(spinner) == 1);
    assert(rt_spinner_was_changed(spinner) == 0);
    revision = rt_spinner_get_revision(spinner);
    rt_spinner_set_indeterminate(spinner, 1);
    assert(rt_spinner_is_indeterminate(spinner) == 1);
    assert(rt_spinner_get_value(spinner) == 4.0);
    assert(rt_spinner_get_revision(spinner) > revision);
    assert(rt_spinner_was_changed(spinner) == 1);
    assert(rt_spinner_was_changed(spinner) == 0);
    revision = rt_spinner_get_revision(spinner);
    rt_spinner_set_indeterminate(spinner, 1);
    assert(rt_spinner_get_revision(spinner) == revision);
    rt_spinner_set_value(spinner, 4.0);
    assert(rt_spinner_is_indeterminate(spinner) == 0);
    assert(rt_spinner_was_changed(spinner) == 1);
    vg_event_t digit = vg_event_key(VG_EVENT_KEY_CHAR, VG_KEY_UNKNOWN, '4', VG_MOD_NONE);
    vg_event_t enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, VG_MOD_NONE);
    assert(vg_event_send(&spinner->base, &digit));
    assert(vg_event_send(&spinner->base, &enter));
    assert(rt_spinner_was_submitted(spinner) == 1);
    assert(rt_spinner_was_submitted(spinner) == 0);
    assert(rt_spinner_was_changed(spinner) == 0);

    vg_radiobutton_t *radio =
        (vg_radiobutton_t *)rt_radiobutton_new(NULL, rt_const_cstr("Choice"), NULL);
    assert(radio);
    revision = rt_radiobutton_get_revision(radio);
    rt_radiobutton_set_selected(radio, 1);
    assert(rt_radiobutton_get_revision(radio) > revision);
    assert(rt_radiobutton_was_changed(radio) == 1);
    assert(rt_radiobutton_was_changed(radio) == 0);

    vg_listbox_t *listbox = (vg_listbox_t *)rt_listbox_new(NULL);
    assert(listbox);
    void *list_item = rt_listbox_add_item(listbox, rt_const_cstr("Row"));
    void *second_list_item = rt_listbox_add_item(listbox, rt_const_cstr("Second"));
    assert(list_item && second_list_item);
    revision = rt_listbox_get_revision(listbox);
    rt_listbox_select(listbox, list_item);
    assert(rt_listbox_get_revision(listbox) > revision);
    assert(rt_listbox_was_changed(listbox) == 1);
    assert(rt_listbox_was_selection_changed(listbox) == 1);
    assert(rt_listbox_was_changed(listbox) == 0);
    assert(rt_listbox_was_selection_changed(listbox) == 0);
    enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, VG_MOD_NONE);
    assert(vg_event_send(&listbox->base, &enter));
    assert(rt_listbox_was_activated(listbox) == 1);
    assert(rt_listbox_was_activated(listbox) == 0);
    assert(rt_listbox_was_changed(listbox) == 0);
    rt_listbox_set_reorderable(listbox, 1);
    rt_listbox_select_index(listbox, 1);
    assert(rt_listbox_was_changed(listbox) == 1);
    assert(rt_listbox_was_selection_changed(listbox) == 1);
    revision = rt_listbox_get_revision(listbox);
    vg_event_t alt_up = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_UP, 0, VG_MOD_ALT);
    assert(vg_event_send(&listbox->base, &alt_up));
    assert(rt_listbox_was_reorder_requested(listbox) == 1);
    assert(rt_listbox_was_reorder_requested(listbox) == 0);
    assert(rt_listbox_get_reorder_source_index(listbox) == 1);
    assert(rt_listbox_get_reorder_target_index(listbox) == 0);
    assert(rt_listbox_get_selected_index(listbox) == 1);
    assert(rt_listbox_get_revision(listbox) == revision);
    assert(rt_listbox_was_changed(listbox) == 0);
    rt_listbox_set_reorderable(listbox, 0);
    assert(rt_listbox_get_reorder_source_index(listbox) == -1);
    assert(rt_listbox_get_reorder_target_index(listbox) == -1);

    vg_treeview_t *tree = (vg_treeview_t *)rt_treeview_new(NULL);
    assert(tree);
    void *tree_node = rt_treeview_add_node(tree, NULL, rt_const_cstr("Node"));
    assert(tree_node);
    revision = rt_treeview_get_revision(tree);
    rt_treeview_select(tree, tree_node);
    assert(rt_treeview_get_revision(tree) > revision);
    assert(rt_treeview_was_changed(tree) == 1);
    assert(rt_treeview_was_selection_changed(tree) == 1);
    assert(rt_treeview_was_changed(tree) == 0);
    assert(rt_treeview_was_selection_changed(tree) == 0);
    enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, VG_MOD_NONE);
    assert(vg_event_send(&tree->base, &enter));
    assert(rt_treeview_was_activated(tree) == 1);
    assert(rt_treeview_was_activated(tree) == 0);

    vg_tabbar_t *tabbar = (vg_tabbar_t *)rt_tabbar_new(NULL);
    assert(tabbar);
    void *first_tab = rt_tabbar_add_tab(tabbar, rt_const_cstr("First"), 1);
    void *second_tab = rt_tabbar_add_tab(tabbar, rt_const_cstr("Second"), 1);
    assert(first_tab && second_tab);
    revision = rt_tabbar_get_revision(tabbar);
    rt_tabbar_set_active(tabbar, second_tab);
    assert(rt_tabbar_get_revision(tabbar) > revision);
    assert(rt_tabbar_was_changed(tabbar) == 1);
    assert(rt_tabbar_was_changed(tabbar) == 0);

    vg_datagrid_t *grid = (vg_datagrid_t *)rt_datagrid_new(NULL);
    assert(grid);
    revision = rt_datagrid_get_revision(grid);
    rt_datagrid_set_columns(grid, 2);
    assert(rt_datagrid_get_revision(grid) > revision);
    assert(rt_datagrid_was_changed(grid) == 1);
    assert(rt_datagrid_was_changed(grid) == 0);
    revision = rt_datagrid_get_revision(grid);
    rt_datagrid_set_columns(grid, 2);
    assert(rt_datagrid_get_revision(grid) == revision);
    rt_datagrid_set_cell(grid, 0, 1, rt_const_cstr("value"));
    assert(rt_datagrid_was_changed(grid) == 1);

    assert(rt_checkbox_was_changed(slider) == 0);
    assert(rt_checkbox_get_revision(slider) == 0);
    assert(rt_dropdown_was_changed(checkbox) == 0);
    assert(rt_slider_get_revision(dropdown) == 0);
    assert(rt_spinner_was_submitted(tree) == 0);
    rt_spinner_set_indeterminate(tree, 1);
    assert(rt_spinner_is_indeterminate(tree) == 0);
    assert(rt_listbox_was_activated(tabbar) == 0);
    rt_listbox_set_reorderable(tabbar, 1);
    assert(rt_listbox_was_reorder_requested(tabbar) == 0);
    assert(rt_listbox_get_reorder_source_index(tabbar) == -1);
    assert(rt_listbox_get_reorder_target_index(tabbar) == -1);
    assert(rt_treeview_get_revision(grid) == 0);
    assert(rt_tabbar_get_revision(radio) == 0);
    assert(rt_datagrid_get_revision(spinner) == 0);

    vg_widget_destroy(&grid->base);
    vg_widget_destroy(&tabbar->base);
    vg_widget_destroy(&tree->base);
    vg_widget_destroy(&listbox->base);
    vg_widget_destroy(&radio->base);
    vg_widget_destroy(&spinner->base);
    vg_widget_destroy(&slider->base);
    vg_widget_destroy(&dropdown->base);
    vg_widget_destroy(&checkbox->base);
    printf("test_uniform_control_events_and_revisions: PASSED\n");
}

/// @brief Verify the complete interactive, sparse-virtualized Grid runtime contract.
/// @details Uses a 10,000-by-20 logical table with only two materialized cells; proves exact row
///          counts, atomic invalid/no-op behavior, embedded-NUL display conversion, independent
///          selection/activation/sort/resize/edit edges, viewport scrolling, 2x logical width
///          conversion, default-font inheritance, type checks, and stale-handle safety.
static void test_interactive_virtual_grid_runtime(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.default_font = (vg_font_t *)0x1;
    app.default_font_size = 15.0f;
    app.user_scale = 2.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_datagrid_t *grid = (vg_datagrid_t *)rt_datagrid_new(app.root);
    assert(grid);
    assert(grid->font == app.default_font);
    assert(grid->font_size == 30.0f);
    rt_datagrid_set_columns(grid, 20);
    assert(rt_datagrid_get_column_count(grid) == 20);

    rt_datagrid_set_column_width(grid, 0, 25.0);
    assert(rt_datagrid_get_column_width(grid, 0) == 50);
    assert(rt_datagrid_was_column_resized(grid) == 1);
    assert(rt_datagrid_was_column_resized(grid) == 0);
    assert(rt_datagrid_get_resized_column(grid) == 0);
    int64_t revision = rt_datagrid_get_revision(grid);
    rt_datagrid_set_column_width(grid, 0, NAN);
    assert(rt_datagrid_get_revision(grid) == revision);

    rt_datagrid_set_virtual_row_count(grid, 10000);
    assert(rt_datagrid_get_row_count(grid) == 10000);
    assert(grid->virtual_mode);
    assert(grid->cells == NULL);
    assert(grid->virtual_cell_count == 0u);
    revision = rt_datagrid_get_revision(grid);
    rt_datagrid_set_virtual_row_count(grid, -1);
    assert(rt_datagrid_get_row_count(grid) == 10000);
    assert(rt_datagrid_get_revision(grid) == revision);

    static const char nul_text[] = {'a', '\0', 'b'};
    rt_string embedded = rt_string_from_bytes(nul_text, 3);
    assert(embedded);
    rt_datagrid_set_virtual_cell(grid, 9999, 19, embedded);
    rt_str_release_maybe(embedded);
    rt_datagrid_set_virtual_cell(grid, 0, 0, rt_const_cstr("first"));
    assert(grid->virtual_cell_count == 2u);
    rt_string visible = rt_datagrid_get_cell(grid, 9999, 19);
    static const unsigned char converted[] = {'a', 0xEFu, 0xBFu, 0xBDu, 'b'};
    assert(rt_str_len(visible) == 5);
    assert(memcmp(rt_string_cstr(visible), converted, sizeof(converted)) == 0);
    rt_str_release_maybe(visible);
    revision = rt_datagrid_get_revision(grid);
    rt_datagrid_set_virtual_cell(grid, -1, 0, rt_const_cstr("invalid"));
    rt_datagrid_set_virtual_cell(grid, 0, -1, rt_const_cstr("invalid"));
    rt_datagrid_set_cell(grid, INT32_MAX, 0, rt_const_cstr("dense overflow"));
    assert(grid->virtual_cell_count == 2u);
    assert(grid->virtual_mode);
    assert(rt_datagrid_get_revision(grid) == revision);

    rt_datagrid_set_viewport_rows(grid, 9980, 20);
    assert(rt_datagrid_get_scroll_row(grid) == 9980);
    revision = rt_datagrid_get_revision(grid);
    rt_datagrid_set_viewport_rows(grid, -1, 20);
    assert(rt_datagrid_get_scroll_row(grid) == 9980);
    assert(rt_datagrid_get_revision(grid) == revision);

    rt_datagrid_set_selectable(grid, 1);
    assert(rt_datagrid_select_cell(grid, 9999, 19) == 1);
    assert(rt_datagrid_get_selected_row(grid) == 9999);
    assert(rt_datagrid_get_selected_column(grid) == 19);
    assert(rt_datagrid_was_selection_changed(grid) == 1);
    assert(rt_datagrid_was_selection_changed(grid) == 0);
    assert(rt_datagrid_select_cell(grid, -1, 19) == 0);
    assert(rt_datagrid_get_selected_row(grid) == 9999);
    vg_event_t enter = vg_event_key(VG_EVENT_KEY_DOWN, VG_KEY_ENTER, 0, VG_MOD_NONE);
    assert(vg_event_send(&grid->base, &enter));
    assert(rt_datagrid_was_activated(grid) == 1);
    assert(rt_datagrid_was_activated(grid) == 0);

    rt_datagrid_set_sortable(grid, 19, 1);
    rt_datagrid_set_sort(grid, 19, -7);
    assert(rt_datagrid_get_sort_column(grid) == 19);
    assert(rt_datagrid_get_sort_direction(grid) == -1);
    assert(rt_datagrid_was_sort_changed(grid) == 1);
    assert(rt_datagrid_was_sort_changed(grid) == 0);
    revision = rt_datagrid_get_revision(grid);
    rt_datagrid_set_sort(grid, 19, -1);
    assert(rt_datagrid_get_revision(grid) == revision);

    rt_datagrid_set_editable(grid, 1);
    assert(rt_datagrid_begin_edit(grid, 9999, 19) == 1);
    assert(rt_datagrid_is_editing(grid) == 1);
    assert(rt_datagrid_commit_edit(grid, rt_const_cstr("edited")) == 1);
    assert(rt_datagrid_is_editing(grid) == 0);
    assert(rt_datagrid_was_cell_edited(grid) == 1);
    assert(rt_datagrid_was_cell_edited(grid) == 0);
    visible = rt_datagrid_get_cell(grid, 9999, 19);
    assert(rt_str_len(visible) == 6);
    assert(memcmp(rt_string_cstr(visible), "edited", 6) == 0);
    rt_str_release_maybe(visible);
    assert(rt_datagrid_begin_edit(grid, 0, 0) == 1);
    rt_datagrid_cancel_edit(grid);
    assert(rt_datagrid_is_editing(grid) == 0);
    assert(rt_datagrid_was_cell_edited(grid) == 0);

    rt_datagrid_scroll_to_row(grid, 9999);
    assert(rt_datagrid_get_scroll_row(grid) == 9999);
    revision = rt_datagrid_get_revision(grid);
    rt_datagrid_scroll_to_row(grid, -1);
    assert(rt_datagrid_get_scroll_row(grid) == 9999);
    assert(rt_datagrid_get_revision(grid) == revision);
    rt_datagrid_clear_selection(grid);
    assert(rt_datagrid_get_selected_row(grid) == -1);
    assert(rt_datagrid_get_selected_column(grid) == -1);
    assert(rt_datagrid_was_selection_changed(grid) == 1);

    assert(rt_datagrid_get_selected_row(app.root) == -1);
    assert(rt_datagrid_get_sort_column(app.root) == -1);
    assert(rt_datagrid_begin_edit(app.root, 0, 0) == 0);
    assert(rt_datagrid_was_activated(app.root) == 0);

    void *stale_grid = grid;
    cleanup_fake_app(&app);
    assert(rt_datagrid_get_row_count(stale_grid) == 0);
    assert(rt_datagrid_get_selected_row(stale_grid) == -1);
    assert(rt_datagrid_is_editing(stale_grid) == 0);
    assert(rt_datagrid_was_sort_changed(stale_grid) == 0);
    printf("test_interactive_virtual_grid_runtime: PASSED\n");
}

/// @brief Verify complete box, Flex, LayoutGrid, and DockPanel runtime behavior.
/// @details A synthetic 2x app proves logical distances are scaled exactly once while fractional
///          tracks remain dimensionless. The test covers stable public enum mapping, wrap-reverse
///          line order, fixed/auto/fractional grid tracks, checked direct-child placement,
///          DockPanel ownership rejection, dock gaps, wrong-type safety, and revision-preserving
///          no-op setters.
static void test_complete_layout_container_runtime(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.user_scale = 2.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_widget_t *vbox = (vg_widget_t *)rt_vbox_new();
    vg_widget_t *hbox = (vg_widget_t *)rt_hbox_new();
    assert(vbox && hbox);
    rt_widget_add_child(app.root, vbox);
    rt_widget_add_child(app.root, hbox);
    rt_vbox_set_align(vbox, 1);
    rt_vbox_set_justify(vbox, 5);
    rt_hbox_set_align(hbox, 2);
    rt_hbox_set_justify(hbox, 3);
    assert(rt_vbox_get_align(vbox) == 1);
    assert(rt_vbox_get_justify(vbox) == 5);
    assert(rt_hbox_get_align(hbox) == 2);
    assert(rt_hbox_get_justify(hbox) == 3);
    uint64_t vbox_revision = vg_widget_get_revision(vbox);
    rt_vbox_set_align(vbox, 1);
    assert(vg_widget_get_revision(vbox) == vbox_revision);
    rt_vbox_set_align(vbox, INT64_MAX);
    assert(rt_vbox_get_align(vbox) == 0);
    rt_container_set_spacing(vbox, 3.0);
    rt_container_set_padding(vbox, 4.0);
    vg_vbox_layout_t *vbox_layout = (vg_vbox_layout_t *)vbox->impl_data;
    assert(vbox_layout->spacing == 6.0f);
    assert(vbox->layout.padding_left == 8.0f);
    assert(rt_vbox_get_align(hbox) == 0);
    assert(rt_hbox_get_justify(vbox) == 0);

    vg_widget_t *flex = (vg_widget_t *)rt_flex_new();
    assert(flex);
    rt_widget_add_child(app.root, flex);
    rt_flex_set_direction(flex, 1);
    vg_flex_layout_t *flex_layout = (vg_flex_layout_t *)flex->impl_data;
    assert(flex_layout->direction == VG_DIRECTION_COLUMN);
    rt_flex_set_direction(flex, 2);
    assert(flex_layout->direction == VG_DIRECTION_ROW_REVERSE);
    rt_flex_set_direction(flex, 0);
    rt_flex_set_wrap(flex, 2);
    rt_flex_set_align(flex, 0);
    rt_flex_set_justify(flex, 0);
    rt_flex_set_gap(flex, 1.0);
    rt_flex_set_padding(flex, 0.0);
    assert(flex_layout->wrap == VG_FLEX_WRAP_REVERSE);
    assert(flex_layout->gap == 2.0f);

    vg_widget_t *flex_a = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *flex_b = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *flex_c = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(flex_a && flex_b && flex_c);
    vg_widget_set_fixed_size(flex_a, 30.0f, 10.0f);
    vg_widget_set_fixed_size(flex_b, 30.0f, 10.0f);
    vg_widget_set_fixed_size(flex_c, 30.0f, 10.0f);
    rt_widget_add_child(flex, flex_a);
    rt_widget_add_child(flex, flex_b);
    rt_widget_add_child(flex, flex_c);
    vg_widget_measure(flex, 70.0f, 50.0f);
    vg_widget_arrange(flex, 0.0f, 0.0f, 70.0f, 50.0f);
    assert(flex_a->y > flex_c->y);
    assert(flex_a->y == flex_b->y);

    vg_widget_t *layout_grid = (vg_widget_t *)rt_layoutgrid_new();
    assert(layout_grid);
    rt_widget_add_child(app.root, layout_grid);
    rt_layoutgrid_set_rows(layout_grid, 2);
    rt_layoutgrid_set_columns(layout_grid, 3);
    rt_layoutgrid_set_row_size(layout_grid, 0, 10.0);
    rt_layoutgrid_set_row_size(layout_grid, 1, -2.0);
    rt_layoutgrid_set_column_size(layout_grid, 0, 0.0);
    rt_layoutgrid_set_column_size(layout_grid, 1, 20.0);
    rt_layoutgrid_set_column_size(layout_grid, 2, -1.0);
    rt_layoutgrid_set_gap(layout_grid, 3.0, 4.0);
    rt_layoutgrid_set_padding(layout_grid, 5.0);
    vg_grid_layout_t *grid_layout = (vg_grid_layout_t *)layout_grid->impl_data;
    assert(grid_layout->rows == 2 && grid_layout->columns == 3);
    assert(grid_layout->row_heights[0] == 20.0f);
    assert(grid_layout->row_heights[1] == -2.0f);
    assert(grid_layout->column_widths[0] == 0.0f);
    assert(grid_layout->column_widths[1] == 40.0f);
    assert(grid_layout->column_widths[2] == -1.0f);
    assert(grid_layout->column_gap == 6.0f && grid_layout->row_gap == 8.0f);
    assert(layout_grid->layout.padding_left == 10.0f);

    vg_widget_t *auto_child = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *fixed_child = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *fraction_child = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *detached = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(auto_child && fixed_child && fraction_child && detached);
    vg_widget_set_preferred_size(auto_child, 24.0f, 12.0f);
    vg_widget_set_preferred_size(fixed_child, 12.0f, 12.0f);
    vg_widget_set_preferred_size(fraction_child, 12.0f, 12.0f);
    rt_widget_add_child(layout_grid, auto_child);
    rt_widget_add_child(layout_grid, fixed_child);
    rt_widget_add_child(layout_grid, fraction_child);
    assert(rt_layoutgrid_place(layout_grid, auto_child, 0, 0, 1, 1) == 1);
    assert(rt_layoutgrid_place(layout_grid, fixed_child, 0, 1, 1, 1) == 1);
    assert(rt_layoutgrid_place(layout_grid, fraction_child, 0, 2, 1, 1) == 1);
    assert(rt_layoutgrid_place(layout_grid, detached, 0, 0, 1, 1) == 0);
    assert(detached->parent == NULL);
    assert(rt_layoutgrid_place(layout_grid, auto_child, -1, 0, 1, 1) == 0);
    assert(rt_layoutgrid_place(layout_grid, auto_child, 0, 2, 1, 2) == 0);
    vg_widget_measure(layout_grid, 300.0f, 120.0f);
    vg_widget_arrange(layout_grid, 0.0f, 0.0f, 300.0f, 120.0f);
    assert(auto_child->width == 24.0f);
    assert(fixed_child->width == 40.0f);
    assert(fraction_child->width > 190.0f);

    vg_widget_t *dock = (vg_widget_t *)rt_dockpanel_new();
    assert(dock);
    rt_widget_add_child(app.root, dock);
    rt_dockpanel_set_padding(dock, 2.0);
    rt_dockpanel_set_gap(dock, 5.0);
    assert(dock->layout.padding_left == 4.0f);
    vg_widget_t *dock_left = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *dock_fill = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *foreign_parent = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *foreign_child = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(dock_left && dock_fill && foreign_parent && foreign_child);
    vg_widget_set_fixed_size(dock_left, 20.0f, 10.0f);
    assert(rt_dockpanel_dock_child(dock, dock_left, 0) == 1);
    assert(rt_dockpanel_dock_child(dock, dock_fill, 4) == 1);
    assert(dock_left->parent == dock && dock_fill->parent == dock);
    rt_widget_add_child(foreign_parent, foreign_child);
    assert(rt_dockpanel_dock_child(dock, foreign_child, 1) == 0);
    assert(foreign_child->parent == foreign_parent);
    assert(rt_dockpanel_dock_child(dock, detached, 99) == 0);
    assert(detached->parent == NULL);
    vg_widget_measure(dock, 200.0f, 100.0f);
    vg_widget_arrange(dock, 0.0f, 0.0f, 200.0f, 100.0f);
    assert(dock_left->x == 4.0f && dock_left->width == 20.0f);
    assert(dock_fill->x == 34.0f);

    vg_widget_destroy(foreign_parent);
    vg_widget_destroy(detached);
    cleanup_fake_app(&app);
    printf("test_complete_layout_container_runtime: PASSED\n");
}

/// @brief Verify the runtime semantic API and deterministic recursive snapshot schema.
/// @details The fixture uses a 2x app scale so the test proves bounds are converted to logical
///          units exactly once. It also covers copied string overrides, inferred input values,
///          label relationships, invisible-subtree omission, and live-region edge snapshots.
static void test_accessibility_snapshot_and_widget_semantics(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.user_scale = 2.0f;
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);

    vg_widget_set_accessible_role(app.root, VG_ACCESSIBLE_ROLE_APPLICATION);
    vg_widget_set_accessible_name(app.root, "Snapshot App");
    vg_label_t *label = vg_label_create(app.root, "User name");
    vg_textinput_t *input = vg_textinput_create(app.root);
    vg_spinner_t *mixed_spinner = vg_spinner_create(app.root);
    vg_label_t *hidden = vg_label_create(app.root, "Hidden decoration");
    assert(label && input && mixed_spinner && hidden);
    vg_textinput_set_text(input, "Ada");
    vg_spinner_set_value(mixed_spinner, 0.65);
    vg_spinner_set_indeterminate(mixed_spinner, true);
    vg_widget_set_visible(&hidden->base, false);
    vg_widget_arrange(app.root, 0.0f, 0.0f, 400.0f, 200.0f);
    vg_widget_arrange(&label->base, 20.0f, 30.0f, 100.0f, 24.0f);
    vg_widget_arrange(&input->base, 140.0f, 30.0f, 200.0f, 32.0f);
    vg_widget_arrange(&mixed_spinner->base, 140.0f, 70.0f, 120.0f, 32.0f);

    rt_widget_set_accessible_description(&input->base, rt_const_cstr("Account display name"));
    rt_widget_set_accessible_label_for(&label->base, &input->base);
    rt_widget_set_live_region(&label->base, VG_LIVE_REGION_POLITE);
    assert(rt_widget_get_accessible_role(&input->base) == VG_ACCESSIBLE_ROLE_TEXTBOX);
    assert(rt_widget_get_live_region(&label->base) == VG_LIVE_REGION_POLITE);
    assert(rt_widget_get_revision(&input->base) > 0);
    rt_string description = rt_widget_get_accessible_description(&input->base);
    assert(strcmp(rt_string_cstr(description), "Account display name") == 0);
    rt_str_release_maybe(description);

    void *snapshot = rt_accessibility_snapshot(app.root);
    assert(snapshot);
    assert(rt_map_get_int(snapshot, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_get_int(snapshot, rt_const_cstr("role")) == VG_ACCESSIBLE_ROLE_APPLICATION);
    assert(strcmp(rt_string_cstr(rt_map_get_str(snapshot, rt_const_cstr("name"))),
                  "Snapshot App") == 0);
    assert(rt_map_get_float(snapshot, rt_const_cstr("logicalWidth")) == 200.0);
    assert(rt_map_get_float(snapshot, rt_const_cstr("screenWidth")) == 400.0);
    assert(rt_map_get_bool(snapshot, rt_const_cstr("truncated")) == 0);

    void *children = rt_map_get(snapshot, rt_const_cstr("children"));
    assert(children);
    assert(rt_seq_len(children) == 3);
    void *label_node = rt_seq_get(children, 0);
    void *input_node = rt_seq_get(children, 1);
    void *spinner_node = rt_seq_get(children, 2);
    assert(label_node && input_node && spinner_node);
    assert(strcmp(rt_string_cstr(rt_map_get_str(label_node, rt_const_cstr("name"))), "User name") ==
           0);
    assert(rt_map_get_int(label_node, rt_const_cstr("labelForId")) == (int64_t)input->base.id);
    assert(strcmp(rt_string_cstr(rt_map_get_str(input_node, rt_const_cstr("value"))), "Ada") == 0);
    assert(strcmp(rt_string_cstr(rt_map_get_str(spinner_node, rt_const_cstr("value"))), "mixed") ==
           0);
    assert(rt_map_get_float(input_node, rt_const_cstr("logicalX")) == 70.0);
    assert(rt_map_get_float(input_node, rt_const_cstr("logicalWidth")) == 100.0);
    release_test_runtime_object(snapshot);

    hidden->base.next_sibling = &hidden->base;
    snapshot = rt_accessibility_snapshot(app.root);
    assert(snapshot);
    assert(rt_map_get_bool(snapshot, rt_const_cstr("truncated")) == 1);
    children = rt_map_get(snapshot, rt_const_cstr("children"));
    assert(children && rt_seq_len(children) == 3);
    release_test_runtime_object(snapshot);
    hidden->base.next_sibling = NULL;

    app.root->id = UINT64_MAX;
    app.root->revision = UINT64_MAX;
    app.root->accessibility.revision = UINT64_MAX;
    app.root->accessibility.announcement_revision = UINT64_MAX;
    app.root->x = NAN;
    app.root->y = -INFINITY;
    app.root->width = INFINITY;
    app.root->height = -INFINITY;
    snapshot = rt_accessibility_snapshot(app.root);
    assert(snapshot);
    assert(rt_map_get_int(snapshot, rt_const_cstr("id")) == INT64_MAX);
    assert(rt_map_get_int(snapshot, rt_const_cstr("revision")) == INT64_MAX);
    assert(rt_map_get_int(snapshot, rt_const_cstr("semanticRevision")) == INT64_MAX);
    assert(rt_map_get_int(snapshot, rt_const_cstr("announcementRevision")) == INT64_MAX);
    assert(rt_map_get_float(snapshot, rt_const_cstr("screenX")) == 0.0);
    assert(rt_map_get_float(snapshot, rt_const_cstr("screenY")) == 0.0);
    assert(rt_map_get_float(snapshot, rt_const_cstr("screenWidth")) == 0.0);
    assert(rt_map_get_float(snapshot, rt_const_cstr("screenHeight")) == 0.0);
    release_test_runtime_object(snapshot);
    app.root->x = 0.0f;
    app.root->y = 0.0f;
    app.root->width = 400.0f;
    app.root->height = 200.0f;

    rt_accessibility_announce(
        &label->base, rt_const_cstr("Name field ready"), VG_LIVE_REGION_ASSERTIVE);
    snapshot = rt_accessibility_snapshot(app.root);
    children = rt_map_get(snapshot, rt_const_cstr("children"));
    label_node = rt_seq_get(children, 0);
    assert(rt_map_get_int(label_node, rt_const_cstr("announcementRevision")) == 1);
    assert(rt_map_get_int(label_node, rt_const_cstr("announcementMode")) ==
           VG_LIVE_REGION_ASSERTIVE);
    assert(strcmp(rt_string_cstr(rt_map_get_str(label_node, rt_const_cstr("announcement"))),
                  "Name field ready") == 0);
    release_test_runtime_object(snapshot);

    cleanup_fake_app(&app);
    printf("test_accessibility_snapshot_and_widget_semantics: PASSED\n");
}

/// @brief Verify explicit high-contrast and reduced-motion preferences rebuild the app theme.
/// @details High contrast uses deterministic accessible surface/text pairs. Reduced motion must
///          disable animation scheduling without discarding the selected high-contrast palette;
///          toggling either preference is immediately observable through the public getters.
static void test_accessibility_preferences_rebuild_theme(void) {
    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);
    uint64_t initial_theme_revision = app.theme_revision;

    rt_accessibility_set_high_contrast(1);
    assert(rt_accessibility_is_high_contrast() == 1);
    assert(app.theme_revision == initial_theme_revision + 1u);
    assert(app.theme->colors.bg_primary == 0x000000u);
    assert(app.theme->colors.fg_primary == 0xFFFFFFu);
    assert(rt_accessibility_contrast_ratio(app.theme->colors.fg_primary,
                                           app.theme->colors.bg_primary) >= 4.5);

    rt_accessibility_set_reduced_motion(1);
    assert(rt_accessibility_is_reduced_motion() == 1);
    assert(app.theme->colors.bg_primary == 0x000000u);
    assert(!app.theme->motion.enabled);
    int64_t system_contrast = rt_accessibility_get_system_high_contrast();
    int64_t system_motion = rt_accessibility_get_system_reduced_motion();
    assert(system_contrast == 0 || system_contrast == 1);
    assert(system_motion == 0 || system_motion == 1);

    rt_accessibility_set_reduced_motion(0);
    rt_accessibility_set_high_contrast(0);
    assert(rt_accessibility_is_reduced_motion() == 0);
    assert(rt_accessibility_is_high_contrast() == 0);
    cleanup_fake_app(&app);
    printf("test_accessibility_preferences_rebuild_theme: PASSED\n");
}

/// @brief Verify custom palettes, validation, cloning, mode selection, and theme revision edges.
/// @details Exercises canonical and alias tokens, recognized-invalid versus unknown setters,
///          WCAG validation, independent palette/app ownership, logical-to-physical scaling,
///          high-contrast composition, System-mode selection, legacy forwarding, and ResetCustom.
static void test_custom_system_theme_palette_contract(void) {
    void *palette = rt_theme_palette_from_dark();
    assert(palette);
    assert(rt_theme_palette_get_color(palette, rt_const_cstr("bgPrimary")) == 0x0D1A1C);
    assert(rt_theme_palette_get_metric(palette, rt_const_cstr("buttonHeight")) == 28.0);
    assert(rt_theme_palette_set_color(palette, rt_const_cstr("notAColor"), 0x123456) == 0);
    assert(rt_theme_palette_set_metric(palette, rt_const_cstr("notAMetric"), 1.0) == 0);
    assert(rt_theme_palette_set_color(palette, rt_const_cstr("accentPrimary"), 0x3366CC) == 1);
    assert(rt_theme_palette_set_metric(palette, rt_const_cstr("buttonHeight"), 31.5) == 1);
    assert(rt_theme_palette_set_metric(palette, rt_const_cstr("radiusMedium"), 7.0) == 1);
    assert(rt_theme_palette_get_metric(palette, rt_const_cstr("radiusMd")) == 7.0);
    rt_theme_palette_set_motion_enabled(palette, 0);
    assert(rt_theme_palette_get_metric(palette, rt_const_cstr("motionEnabled")) == 0.0);

    // Recognized invalid writes preserve the old value and remain diagnosable until repaired.
    assert(rt_theme_palette_set_color(palette, rt_const_cstr("accentWarning"), -1) == 1);
    assert(rt_theme_palette_get_color(palette, rt_const_cstr("accentWarning")) == 0xE8B04C);
    void *validation = rt_theme_palette_validate(palette);
    assert(validation && rt_result_is_err(validation));
    assert(strcmp(rt_string_cstr(rt_result_unwrap_err_str(validation)),
                  "GUI theme token accentWarning has an invalid value") == 0);
    release_test_runtime_object(validation);
    assert(rt_theme_palette_set_color(palette, rt_const_cstr("accentWarning"), 0xFFCC44) == 1);

    assert(rt_theme_palette_set_metric(palette, rt_const_cstr("focusGlowAlpha"), NAN) == 1);
    validation = rt_theme_palette_validate(palette);
    assert(validation && rt_result_is_err(validation));
    assert(strcmp(rt_string_cstr(rt_result_unwrap_err_str(validation)),
                  "GUI theme token focusGlowAlpha has an invalid value") == 0);
    release_test_runtime_object(validation);
    assert(rt_theme_palette_set_metric(palette, rt_const_cstr("focusGlowAlpha"), 144.0) == 1);

    rt_gui_app_t app;
    reset_fake_app(&app);
    app.root = vg_widget_create(VG_WIDGET_CONTAINER);
    assert(app.root);
    app.root->user_data = &app;
    rt_gui_activate_app(&app);
    app.theme_reported_revision = app.theme_revision;

    // A wrong font handle marks validation state without partially changing any role.
    rt_theme_palette_set_font_roles(palette, &app, NULL, NULL);
    validation = rt_theme_palette_validate(palette);
    assert(validation && rt_result_is_err(validation));
    assert(strcmp(rt_string_cstr(rt_result_unwrap_err_str(validation)),
                  "GUI theme token fontRegular has an invalid value") == 0);
    release_test_runtime_object(validation);
    rt_theme_palette_set_font_roles(palette, NULL, NULL, NULL);
    validation = rt_theme_palette_validate(palette);
    assert(validation && rt_result_is_ok(validation));
    assert(rt_result_unwrap_i64(validation) == 1);
    release_test_runtime_object(validation);

    void *clone = rt_theme_palette_clone(palette);
    assert(clone);
    assert(rt_theme_palette_set_metric(clone, rt_const_cstr("buttonHeight"), 44.0) == 1);
    assert(rt_theme_palette_get_metric(palette, rt_const_cstr("buttonHeight")) == 31.5);
    assert(rt_theme_palette_get_metric(clone, rt_const_cstr("buttonHeight")) == 44.0);

    uint64_t before_custom = app.theme_revision;
    assert(rt_theme_set_palette(palette) == 1);
    assert(rt_theme_get_mode() == RT_GUI_THEME_CUSTOM);
    assert(app.theme_revision == before_custom + 1u);
    assert(app.custom_theme_base);
    assert(app.custom_theme_base->button.height == 31.5f);
    assert(app.theme->button.height == 31.5f);
    assert(app.theme->colors.accent_primary == 0x3366CC);
    assert(!app.theme->motion.enabled);
    assert(rt_theme_was_changed() == 1);
    assert(rt_theme_was_changed() == 0);
    assert(rt_theme_get_revision() == (int64_t)app.theme_revision);

    void *snapshot = rt_theme_get_palette();
    assert(snapshot);
    assert(rt_theme_palette_get_metric(snapshot, rt_const_cstr("buttonHeight")) == 31.5);
    assert(rt_theme_palette_set_metric(snapshot, rt_const_cstr("buttonHeight"), 52.0) == 1);
    assert(app.custom_theme_base->button.height == 31.5f);
    release_test_runtime_object(snapshot);

    rt_app_set_ui_scale(&app, 2.0);
    assert(app.custom_theme_base->button.height == 31.5f);
    assert(app.theme->button.height == 63.0f);
    assert(rt_theme_was_changed() == 1);
    rt_accessibility_set_high_contrast(1);
    assert(app.theme->colors.bg_primary == 0x000000);
    assert(app.theme->colors.fg_primary == 0xFFFFFF);
    assert(app.theme->button.height == 63.0f);
    rt_accessibility_set_high_contrast(0);

    rt_theme_set_mode(999);
    assert(rt_theme_get_mode() == RT_GUI_THEME_CUSTOM);
    rt_theme_follow_system();
    assert(rt_theme_get_mode() == RT_GUI_THEME_SYSTEM);
    assert(app.system_prefers_dark == 0 || app.system_prefers_dark == 1);
    assert(rt_gui_app_theme_base(&app) ==
           (app.system_prefers_dark ? vg_theme_dark() : vg_theme_light()));
    rt_theme_set_light();
    assert(rt_theme_get_mode() == RT_GUI_THEME_LIGHT);
    rt_theme_set_dark();
    assert(rt_theme_get_mode() == RT_GUI_THEME_DARK);

    // Re-select custom, then ResetCustom must change mode before reclaiming the logical base.
    assert(rt_theme_set_palette(palette) == 1);
    assert(app.custom_theme_base);
    rt_theme_reset_custom();
    assert(rt_theme_get_mode() == RT_GUI_THEME_DARK);
    assert(app.custom_theme_base == NULL);
    assert(app.theme_base == vg_theme_dark());

    release_test_runtime_object(clone);
    release_test_runtime_object(palette);
    cleanup_fake_app(&app);
    printf("test_custom_system_theme_palette_contract: PASSED\n");
}

int main(void) {
    printf("=== GUI Runtime Regression Tests ===\n\n");

    test_gui_capability_and_try_new_success();
    test_app_minimum_window_size();
    test_gui_typed_constant_ordinals();
    test_app_bound_test_harness_uses_real_runtime_paths();
    test_detached_tooltip_uses_retained_overlay_damage();
    test_deterministic_scheduler_drives_notification_deadlines();
    test_indexed_subhandles_reclaim_after_last_wrapper();
    test_shortcuts_are_app_scoped();
    test_shortcut_chords_trigger_and_expire();
    test_file_drop_is_app_scoped();
    test_statusbar_click_is_edge_triggered();
    test_statusbar_runtime_button_wires_click_polling();
    test_default_font_is_applied_to_text_widgets();
    test_textinput_runtime_exposes_complete_grapheme_editor();
    test_ui_scale_invalidates_and_scales_complete_theme();
    test_floatingpanel_geometry_uses_effective_scale();
    test_gui_scheduler_reports_nearest_widget_deadline();
    test_default_font_is_applied_to_complex_text_widgets();
    test_statusbar_runtime_applies_font_with_container_parent();
    test_codeeditor_pinned_font_survives_app_font_propagation();
    test_widget_set_font_rejects_stale_font_handles();
    test_dropdown_placeholder_is_copied();
    test_dialog_content_is_parented();
    test_notification_cleanup_runs_for_manual_dismiss();
    test_notification_ids_skip_zero_on_wrap();
    test_command_palette_placeholder_and_utf8_input();
    test_platform_text_events_translate_to_gui_text();
    test_platform_scroll_events_keep_screen_coordinates_separate();
    test_scrollview_runtime_reveals_only_live_descendants();
    test_app_handles_resolve_to_root_widgets_for_overlays();
    test_codeeditor_runtime_supports_multicursor_editing();
    test_codeeditor_consolidated_perf_stats_schema();
    test_codeeditor_set_text_round_trips_embedded_nuls();
    test_codeeditor_runtime_pixel_helpers_follow_scroll_and_wrap();
    test_codeeditor_runtime_scroll_top_line_round_trips();
    test_codeeditor_modifier_click_selection_and_extra_cursors();
    test_codeeditor_runtime_fold_helpers_skip_hidden_lines();
    test_codeeditor_line_number_width_override_tracks_character_width();
    test_zia_block_comment_highlighting_is_render_order_independent();
    test_zia_string_highlighting_trailing_escape_stays_in_bounds();
    test_splitpane_runtime_boolean_matches_horizontal_semantics();
    test_tabbar_was_changed_tracks_real_active_tab_transitions();
    test_widget_set_position_marks_widget_dirty();
    test_tabbar_close_click_index_survives_auto_close();
    test_findbar_runtime_reads_live_text_and_reports_noop_replace();
    test_menu_and_toolbar_pixel_icons_become_image_icons();
    test_toolbar_named_icons_prefer_vector_icons();
    test_widget_destroy_refuses_app_root_and_app_handle();
    test_widget_set_size_refuses_app_root();
    test_widget_base_apis_reject_app_handles_and_invalid_children();
    test_runtime_widget_parent_validation_rejects_leaf_and_invalid_handles();
    test_widget_destroy_clears_nested_toolbar_statusbar_runtime_refs();
    test_widget_focus_null_is_noop();
    test_image_set_pixels_converts_zanna_pixels_to_rgba();
    test_image_atomic_runtime_mutation_contract();
    test_treeview_and_listbox_data_preserve_embedded_nuls();
    test_listbox_selection_changed_is_edge_triggered();
    test_runtime_listbox_select_index_rejects_out_of_range_indices();
    test_runtime_listbox_selected_text_copies_multi_selection();
    test_runtime_listbox_selected_data_preserves_rows_and_bytes();
    test_runtime_listbox_item_text_color_override();
    test_treeview_selection_changed_reports_removal_and_clear();
    test_runtime_treeview_selected_data_preserves_hierarchy_and_bytes();
    test_treeview_removing_nonprimary_selection_reports_one_edge();
    test_runtime_treeview_drag_drop_modes_are_validated();
    test_removed_listbox_and_treeview_handles_are_inert();
    test_pruned_tree_and_tab_runtime_handles_are_inert();
    test_listbox_and_treeview_reject_foreign_child_handles();
    test_treeview_complete_node_and_lazy_event_contract();
    test_complete_tab_split_radio_label_contracts();
    test_complete_public_color_control_contracts();
    test_contextmenu_separator_returns_item_handle();
    test_contextmenu_item_click_updates_menuitem_was_clicked();
    test_contextmenu_show_registers_and_clears_active_overlay();
    test_toolbar_and_menu_item_geometry_uses_logical_units();
    test_filedialog_show_without_active_window_returns_zero();
    test_filedialog_show_uses_original_owner_app();
    test_filedialog_path_list_helpers_decode_escaped_paths();
    test_commandpalette_methods_after_destroy_are_inert();
    test_commandpalette_show_clear_remove_drop_stale_selection();
    test_commandpalette_rejects_embedded_nul_command_ids();
    test_numeric_setters_sanitize_invalid_values();
    test_numeric_narrowing_saturates_without_undefined_behavior();
    test_font_destroy_defers_live_app_font();
    test_managed_system_fonts_and_generation_retirement();
    test_detached_widgets_do_not_inherit_current_app_font();
    test_app_font_size_and_late_attach_reapply_default_font();
    test_messagebox_show_after_destroy_returns_minus_one();
    test_messagebox_one_shots_without_window_return_fallbacks();
    test_toast_duration_is_clamped();
    test_toast_shortcuts_coalesce_exact_active_messages();
    test_toast_dismissal_is_edge_triggered_and_survives_cleanup();
    test_breadcrumb_set_path_uses_literal_separator();
    test_breadcrumb_clicked_data_preserves_embedded_nuls();
    test_shortcuts_reject_invalid_bindings_atomically();
    test_close_prevention_tracks_request_without_closing();
    test_menu_context_toolbar_statusbar_handles_are_type_checked();
    test_toolbar_set_style_rejects_unknown_values();
    test_floatingpanel_and_tabbar_reject_wrong_or_out_of_range_handles();
    test_breadcrumb_minimap_methods_after_destroy_are_inert();
    test_minimap_editor_destroy_unbinds_target();
    test_minimap_revision_and_cache_runtime_contract();
    test_type_specific_widget_apis_reject_wrong_types();
    test_findbar_methods_after_destroy_are_inert();
    test_findbar_parent_destroy_disconnects_wrapper();
    test_findbar_editor_destroy_unbinds_target();
    test_unparented_editor_destroy_unbinds_findbar_and_minimap();
    test_radiogroup_runtime_handle_invalidates_after_destroy();
    test_filedialog_setters_after_destroy_are_inert();
    test_filedialog_destroy_removes_modal_stack_entry();
    test_app_destroy_invalidates_stateful_dialog_wrappers();
    test_messagebox_custom_button_ids_preserve_zero_and_i64();
    test_messagebox_semantic_roles_and_async_state();
    test_filedialog_complete_async_contract();
    test_menuitem_checkable_state_is_real_and_invalidates_context();
    test_contextmenu_submenu_ownership_detaches_safely();
    test_toolbar_remove_item_removes_runtime_null_id_items();
    test_toolbar_statusbar_remove_clears_runtime_click_caches();
    test_runtime_subobject_handles_are_inert_after_owner_destroy();
    test_codeeditor_add_highlight_rejects_empty_and_inverted_spans();
    test_codeeditor_gutter_slots_are_validated_and_click_coords_are_fresh();
    test_codeeditor_runtime_read_only_blocks_insertions();
    test_widget_complete_logical_hierarchy_api();
    test_uniform_control_events_and_revisions();
    test_interactive_virtual_grid_runtime();
    test_complete_layout_container_runtime();
    test_accessibility_snapshot_and_widget_semantics();
    test_accessibility_preferences_rebuild_theme();
    test_custom_system_theme_palette_contract();

    printf("\nAll GUI runtime regression tests passed!\n");
    return 0;
}
