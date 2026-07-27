//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGuiAvailabilityTests.cpp
// Purpose: Verify GUI capability discovery and fallible construction without requiring a display.
//
// Key invariants:
//   - Capability queries never create a native window and never trap.
//   - A graphics-disabled build reports the documented exact reason through Result.Err.
//   - A graphics-enabled build reports an empty unavailability reason.
//
// Ownership/Lifetime:
//   - Returned runtime strings and Result values are released by the test.
//
// Links: src/runtime/graphics/gui/rt_gui_app.c, docs/generated/runtime/gui.md
//
//===----------------------------------------------------------------------===//

#include "rt_gui.h"
#include "rt_gui_constants.h"
#include "rt_gui_ide.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_videowidget.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

/// @brief Release one runtime-managed object returned to this test.
/// @param object Object whose caller-owned reference should be dropped, or null.
void releaseRuntimeObject(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Verify backend-independent constants and headless diagnostic schemas.
/// @details Typed constants must remain available when graphics is compiled out. The real-app
///          TestHarness methods must preserve their ABI and return deterministic unavailable
///          values without replaying events, while CodeEditor telemetry keeps its complete
///          versioned map shape.
void testHeadlessAutomationAndConstants() {
    assert(rt_gui_align_stretch() == 3);
    assert(rt_gui_justify_space_evenly() == 5);
    assert(rt_gui_flex_direction_column_reverse() == 3);
    assert(rt_gui_flex_wrap_wrap_reverse() == 2);
    assert(rt_gui_dock_fill() == 4);
    assert(rt_gui_theme_mode_custom() == 3);
    assert(rt_gui_accessible_role_link() == 30);
    assert(rt_gui_live_region_mode_assertive() == 2);
    assert(rt_gui_dialog_button_role_help() == 6);
    assert(rt_gui_dialog_status_failed() == 4);
    assert(rt_gui_image_filter_bilinear() == 1);
    assert(rt_gui_sort_direction_descending() == -1);

    void *harness = rt_gui_test_harness_new();
    assert(harness != nullptr);
    assert(rt_gui_test_harness_bind_app(harness, nullptr) == 0);
    assert(rt_gui_test_harness_dispatch_pending(harness) == 0);
    assert(rt_gui_test_harness_render_frame(harness, 16.0) == 0);
    assert(rt_gui_test_harness_capture_pixels(harness, 0, 0, 1, 1) == nullptr);
    rt_string hash = rt_gui_test_harness_capture_hash(harness, 0, 0, 1, 1);
    assert(hash != nullptr && rt_str_len(hash) == 0);
    rt_str_release_maybe(hash);
    void *comparison = rt_gui_test_harness_compare_region(harness, nullptr, 0, 0, 7);
    assert(comparison != nullptr);
    assert(rt_map_get_int(comparison, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_get_bool(comparison, rt_const_cstr("matches")) == 0);
    assert(rt_map_get_int(comparison, rt_const_cstr("comparedPixels")) == 0);
    releaseRuntimeObject(comparison);
    void *accessibility = rt_gui_test_harness_get_accessibility_snapshot(harness);
    assert(accessibility != nullptr);
    assert(rt_map_get_int(accessibility, rt_const_cstr("schemaVersion")) == 1);
    releaseRuntimeObject(accessibility);
    rt_gui_test_harness_unbind_app(harness);
    releaseRuntimeObject(harness);

    void *stats = rt_codeeditor_get_perf_stats(nullptr);
    assert(stats != nullptr);
    assert(rt_map_get_int(stats, rt_const_cstr("schemaVersion")) == 1);
    assert(rt_map_get_int(stats, rt_const_cstr("totalHeightLinearScans")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("totalVisualRowLinearScans")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("visualRowLinearScans")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("locateVisualRowLinearScans")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("lineHighlightCalls")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("syntaxStateLineScans")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("highlightSpanChecks")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("fullTextCopies")) == 0);
    assert(rt_map_get_int(stats, rt_const_cstr("fullTextCopyBytes")) == 0);
    releaseRuntimeObject(stats);
}

/// @brief Verify that availability and explanatory text agree in every build mode.
/// @details This test deliberately avoids constructing an app in graphics-enabled builds so it is
///          safe on machines without a window server. Graphics-disabled builds additionally call
///          `TryNew` and validate the exact stable error contract.
void testAvailabilityContract() {
    assert(rt_gui_app_run_frame(nullptr) == 0);
    assert(rt_gui_app_run_frame_with_delta(nullptr, 16.0) == 0);
    assert(rt_gui_app_get_next_deadline_ms(nullptr) == -1);
    rt_gui_app_make_current(nullptr);
    assert(rt_widget_get_accessible_role(nullptr) == 0);
    assert(rt_widget_get_live_region(nullptr) == 0);
    assert(rt_widget_get_revision(nullptr) == 0);
    rt_widget_set_preferred_size(nullptr, 10.0, 20.0);
    rt_widget_set_max_size(nullptr, 30.0, 40.0);
    rt_widget_set_min_size(nullptr, 5.0, 6.0);
    assert(rt_widget_get_min_width(nullptr) == 0.0);
    assert(rt_widget_get_min_height(nullptr) == 0.0);
    rt_widget_set_padding(nullptr, 2.0);
    rt_widget_set_padding_edges(nullptr, 1.0, 2.0, 3.0, 4.0);
    rt_widget_set_margin_edges(nullptr, 1.0, 2.0, 3.0, 4.0);
    assert(rt_widget_get_logical_x(nullptr) == 0.0);
    assert(rt_widget_get_logical_y(nullptr) == 0.0);
    assert(rt_widget_get_logical_width(nullptr) == 0.0);
    assert(rt_widget_get_logical_height(nullptr) == 0.0);
    assert(rt_widget_get_screen_x(nullptr) == 0.0);
    assert(rt_widget_get_screen_y(nullptr) == 0.0);
    assert(rt_widget_get_screen_width(nullptr) == 0.0);
    assert(rt_widget_get_screen_height(nullptr) == 0.0);
    assert(rt_widget_get_child_count(nullptr) == 0);
    assert(rt_widget_remove_child(nullptr, nullptr) == 0);
    rt_widget_clear_children(nullptr);
    rt_widget_set_name(nullptr, rt_const_cstr("ignored"));
    rt_string widgetName = rt_widget_get_name(nullptr);
    assert(widgetName != nullptr && rt_str_len(widgetName) == 0);
    rt_str_release_maybe(widgetName);
    assert(rt_widget_get_id(nullptr) == 0);
    assert(rt_widget_hit_test(nullptr, 1.0, 2.0) == 0);
    rt_widget_invalidate_paint(nullptr);
    rt_widget_invalidate_layout(nullptr);
    void *widgetOption = rt_widget_get_parent_option(nullptr);
    assert(widgetOption != nullptr && rt_option_is_none(widgetOption) == 1);
    releaseRuntimeObject(widgetOption);
    widgetOption = rt_widget_get_child_at_option(nullptr, 0);
    assert(widgetOption != nullptr && rt_option_is_none(widgetOption) == 1);
    releaseRuntimeObject(widgetOption);
    widgetOption = rt_widget_find_by_id_option(nullptr, 1);
    assert(widgetOption != nullptr && rt_option_is_none(widgetOption) == 1);
    releaseRuntimeObject(widgetOption);
    widgetOption = rt_widget_find_by_name_option(nullptr, rt_const_cstr("missing"));
    assert(widgetOption != nullptr && rt_option_is_none(widgetOption) == 1);
    releaseRuntimeObject(widgetOption);
    assert(rt_checkbox_was_changed(nullptr) == 0);
    assert(rt_checkbox_get_revision(nullptr) == 0);
    assert(rt_dropdown_was_changed(nullptr) == 0);
    assert(rt_dropdown_get_revision(nullptr) == 0);
    assert(rt_slider_was_changed(nullptr) == 0);
    assert(rt_slider_get_revision(nullptr) == 0);
    assert(rt_spinner_was_changed(nullptr) == 0);
    assert(rt_spinner_was_submitted(nullptr) == 0);
    assert(rt_spinner_get_revision(nullptr) == 0);
    assert(rt_radiobutton_was_changed(nullptr) == 0);
    assert(rt_radiobutton_get_revision(nullptr) == 0);
    assert(rt_radiogroup_get_selected_index(nullptr) == -1);
    assert(rt_radiogroup_set_selected_index(nullptr, 0) == 0);
    assert(rt_radiogroup_get_count(nullptr) == 0);
    assert(rt_radiogroup_was_changed(nullptr) == 0);
    assert(rt_radiogroup_get_revision(nullptr) == 0);
    rt_radiobutton_set_text(nullptr, rt_const_cstr("ignored"));
    rt_string radioText = rt_radiobutton_get_text(nullptr);
    assert(radioText != nullptr && rt_str_len(radioText) == 0);
    rt_str_release_maybe(radioText);
    rt_radiobutton_set_data(nullptr, rt_const_cstr("ignored"));
    radioText = rt_radiobutton_get_data(nullptr);
    assert(radioText != nullptr && rt_str_len(radioText) == 0);
    rt_str_release_maybe(radioText);
    const bool colorControlsAvailable = rt_gui_system_is_available() != 0;
    void *availabilitySwatch = rt_colorswatch_new(nullptr, 0x112233);
    void *availabilityPalette = rt_colorpalette_new(nullptr);
    void *availabilityPicker = rt_colorpicker_new(nullptr);
    assert((availabilitySwatch != nullptr) == colorControlsAvailable);
    assert((availabilityPalette != nullptr) == colorControlsAvailable);
    assert((availabilityPicker != nullptr) == colorControlsAvailable);
    rt_widget_destroy(availabilitySwatch);
    rt_widget_destroy(availabilityPalette);
    rt_widget_destroy(availabilityPicker);
    rt_colorswatch_set_color(nullptr, 0x112233);
    assert(rt_colorswatch_get_color(nullptr) == 0);
    rt_colorswatch_set_selected(nullptr, 1);
    assert(rt_colorswatch_is_selected(nullptr) == 0);
    assert(rt_colorswatch_was_changed(nullptr) == 0);
    assert(rt_colorswatch_get_revision(nullptr) == 0);
    rt_colorpalette_add_color(nullptr, 0x112233);
    assert(rt_colorpalette_remove_color(nullptr, 0) == 0);
    rt_colorpalette_clear(nullptr);
    assert(rt_colorpalette_get_color_count(nullptr) == 0);
    assert(rt_colorpalette_get_color_at(nullptr, 0) == 0);
    rt_colorpalette_set_selected_index(nullptr, 0);
    assert(rt_colorpalette_get_selected_index(nullptr) == -1);
    assert(rt_colorpalette_was_changed(nullptr) == 0);
    assert(rt_colorpalette_get_revision(nullptr) == 0);
    rt_colorpicker_set_color(nullptr, 0x112233);
    assert(rt_colorpicker_get_color(nullptr) == 0);
    rt_colorpicker_set_alpha_enabled(nullptr, 1);
    assert(rt_colorpicker_is_alpha_enabled(nullptr) == 0);
    assert(rt_colorpicker_get_red(nullptr) == 0);
    assert(rt_colorpicker_get_green(nullptr) == 0);
    assert(rt_colorpicker_get_blue(nullptr) == 0);
    assert(rt_colorpicker_get_alpha(nullptr) == 0);
    assert(rt_colorpicker_was_changed(nullptr) == 0);
    assert(rt_colorpicker_get_revision(nullptr) == 0);
    rt_image_set_pixels(nullptr, nullptr, 0, 0);
    assert(rt_image_try_set_pixels(nullptr, nullptr, 0, 0) == 0);
    assert(rt_image_update_region(nullptr, nullptr, 0, 0, 1, 1, 0, 0) == 0);
    rt_image_set_filter(nullptr, RT_IMAGE_FILTER_BILINEAR);
    assert(rt_image_get_filter(nullptr) == RT_IMAGE_FILTER_NEAREST);
    assert(rt_font_get_logical_size(nullptr) == 0.0);
    void *systemFontResult = rt_font_load_system_ui(15.0);
    assert(systemFontResult != nullptr);
    if (rt_gui_system_is_available()) {
        assert(rt_result_is_ok(systemFontResult) == 1);
        void *systemFont = rt_result_unwrap(systemFontResult);
        assert(systemFont != nullptr);
        assert(rt_font_get_logical_size(systemFont) == 15.0);
        rt_font_destroy(systemFont);
    } else {
        assert(rt_result_is_err(systemFontResult) == 1);
        assert(std::strcmp(rt_string_cstr(rt_result_unwrap_err_str(systemFontResult)),
                           "GUI support is not available in this build") == 0);
    }
    releaseRuntimeObject(systemFontResult);
    rt_videowidget_set_auto_update(nullptr, 1);
    assert(rt_videowidget_is_auto_update(nullptr) == 0);
    assert(rt_videowidget_was_loaded(nullptr) == 0);
    assert(rt_videowidget_was_failed(nullptr) == 0);
    assert(rt_videowidget_was_buffering_changed(nullptr) == 0);
    assert(rt_videowidget_was_ended(nullptr) == 0);
    assert(rt_videowidget_was_seeked(nullptr) == 0);
    assert(rt_videowidget_get_revision(nullptr) == 0);
    rt_videowidget_set_controls_auto_hide(nullptr, 1);
    rt_videowidget_set_fullscreen(nullptr, 1);
    assert(rt_videowidget_is_fullscreen(nullptr) == 0);
    rt_string videoError = rt_videowidget_get_error(nullptr);
    assert(videoError != nullptr);
    assert(std::strcmp(rt_string_cstr(videoError),
                       rt_gui_system_is_available()
                           ? ""
                           : "GUI support is not available in this build") == 0);
    rt_str_release_maybe(videoError);
    assert(rt_minimap_get_source_revision(nullptr) == 0);
    rt_minimap_invalidate_lines(nullptr, 0, 1);
    rt_minimap_set_maximum_cached_lines(nullptr, 16);
    assert(rt_minimap_get_cached_line_count(nullptr) == 0);
    assert(rt_listbox_was_changed(nullptr) == 0);
    assert(rt_listbox_was_activated(nullptr) == 0);
    assert(rt_listbox_get_revision(nullptr) == 0);
    rt_listbox_set_reorderable(nullptr, 1);
    assert(rt_listbox_was_reorder_requested(nullptr) == 0);
    assert(rt_listbox_get_reorder_source_index(nullptr) == -1);
    assert(rt_listbox_get_reorder_target_index(nullptr) == -1);
    void *availabilityVirtualList = rt_virtual_list_new(10, 20, 100);
    assert(availabilityVirtualList != nullptr);
    assert(rt_virtual_list_bind(availabilityVirtualList, nullptr) == 0);
    assert(rt_listbox_set_virtual_model(nullptr, availabilityVirtualList) == 0);
    rt_listbox_clear_virtual_model(nullptr);
    assert(rt_listbox_get_visible_first(nullptr) == 0);
    assert(rt_listbox_get_visible_count(nullptr) == 0);
    rt_virtual_list_unbind(availabilityVirtualList);
    releaseRuntimeObject(availabilityVirtualList);
    assert(rt_treeview_was_changed(nullptr) == 0);
    assert(rt_treeview_was_activated(nullptr) == 0);
    assert(rt_treeview_get_revision(nullptr) == 0);
    void *availabilityVirtualTree = rt_virtual_tree_new();
    assert(availabilityVirtualTree != nullptr);
    assert(rt_virtual_tree_bind(availabilityVirtualTree, nullptr) == 0);
    assert(rt_treeview_set_virtual_model(nullptr, availabilityVirtualTree) == 0);
    rt_treeview_clear_virtual_model(nullptr);
    rt_virtual_tree_unbind(availabilityVirtualTree);
    releaseRuntimeObject(availabilityVirtualTree);
    assert(rt_tabbar_get_revision(nullptr) == 0);
    rt_tabbar_set_font(nullptr, nullptr, 14.0);
    assert(rt_tabbar_was_reordered(nullptr) == 0);
    assert(rt_tabbar_get_reordered_from(nullptr) == -1);
    assert(rt_tabbar_get_reordered_to(nullptr) == -1);
    assert(rt_tabbar_move_tab(nullptr, 0, 1) == 0);
    rt_tab_set_title(nullptr, rt_const_cstr("ignored"));
    rt_string tabText = rt_tab_get_title(nullptr);
    assert(tabText != nullptr && rt_str_len(tabText) == 0);
    rt_str_release_maybe(tabText);
    rt_tab_set_data(nullptr, rt_const_cstr("ignored"));
    tabText = rt_tab_get_data(nullptr);
    assert(tabText != nullptr && rt_str_len(tabText) == 0);
    rt_str_release_maybe(tabText);
    rt_tab_set_closable(nullptr, 1);
    assert(rt_tab_is_closable(nullptr) == 0);
    rt_tab_set_stable_id(nullptr, rt_const_cstr("ignored"));
    tabText = rt_tab_get_stable_id(nullptr);
    assert(tabText != nullptr && rt_str_len(tabText) == 0);
    rt_str_release_maybe(tabText);
    rt_splitpane_set_min_first(nullptr, 10.0);
    rt_splitpane_set_min_second(nullptr, 10.0);
    assert(rt_splitpane_get_min_first(nullptr) == 0.0);
    assert(rt_splitpane_get_min_second(nullptr) == 0.0);
    assert(rt_splitpane_get_orientation(nullptr) == -1);
    rt_splitpane_collapse_first(nullptr);
    rt_splitpane_collapse_second(nullptr);
    rt_splitpane_restore(nullptr);
    assert(rt_splitpane_get_collapsed_side(nullptr) == -1);
    rt_datagrid_set_viewport_rows(nullptr, 0, 10);
    rt_datagrid_set_virtual_row_count(nullptr, 10000);
    rt_datagrid_set_virtual_cell(nullptr, 0, 0, rt_const_cstr("ignored"));
    rt_datagrid_set_selectable(nullptr, 1);
    assert(rt_datagrid_get_selected_row(nullptr) == -1);
    assert(rt_datagrid_get_selected_column(nullptr) == -1);
    assert(rt_datagrid_select_cell(nullptr, 0, 0) == 0);
    rt_datagrid_clear_selection(nullptr);
    assert(rt_datagrid_was_selection_changed(nullptr) == 0);
    assert(rt_datagrid_was_activated(nullptr) == 0);
    rt_datagrid_set_sortable(nullptr, 0, 1);
    rt_datagrid_set_sort(nullptr, 0, 1);
    assert(rt_datagrid_get_sort_column(nullptr) == -1);
    assert(rt_datagrid_get_sort_direction(nullptr) == 0);
    assert(rt_datagrid_was_sort_changed(nullptr) == 0);
    rt_datagrid_set_column_width(nullptr, 0, 100.0);
    rt_datagrid_set_column_resizable(nullptr, 0, 1);
    assert(rt_datagrid_was_column_resized(nullptr) == 0);
    assert(rt_datagrid_get_resized_column(nullptr) == -1);
    rt_datagrid_set_editable(nullptr, 1);
    assert(rt_datagrid_begin_edit(nullptr, 0, 0) == 0);
    assert(rt_datagrid_commit_edit(nullptr, rt_const_cstr("ignored")) == 0);
    rt_datagrid_cancel_edit(nullptr);
    assert(rt_datagrid_is_editing(nullptr) == 0);
    assert(rt_datagrid_was_cell_edited(nullptr) == 0);
    rt_datagrid_scroll_to_row(nullptr, 50);
    assert(rt_datagrid_get_scroll_row(nullptr) == 0);
    assert(rt_datagrid_was_changed(nullptr) == 0);
    assert(rt_datagrid_get_revision(nullptr) == 0);
    rt_vbox_set_align(nullptr, 1);
    assert(rt_vbox_get_align(nullptr) == 0);
    rt_vbox_set_justify(nullptr, 2);
    assert(rt_vbox_get_justify(nullptr) == 0);
    rt_hbox_set_align(nullptr, 1);
    assert(rt_hbox_get_align(nullptr) == 0);
    rt_hbox_set_justify(nullptr, 2);
    assert(rt_hbox_get_justify(nullptr) == 0);
    rt_flex_set_direction(nullptr, 1);
    rt_flex_set_wrap(nullptr, 2);
    rt_flex_set_align(nullptr, 3);
    rt_flex_set_justify(nullptr, 5);
    rt_flex_set_gap(nullptr, 2.0);
    rt_flex_set_padding(nullptr, 2.0);
    rt_layoutgrid_set_rows(nullptr, 2);
    rt_layoutgrid_set_columns(nullptr, 2);
    rt_layoutgrid_set_row_size(nullptr, 0, -1.0);
    rt_layoutgrid_set_column_size(nullptr, 0, 10.0);
    rt_layoutgrid_set_gap(nullptr, 1.0, 2.0);
    rt_layoutgrid_set_padding(nullptr, 3.0);
    assert(rt_layoutgrid_place(nullptr, nullptr, 0, 0, 1, 1) == 0);
    rt_dockpanel_set_padding(nullptr, 2.0);
    rt_dockpanel_set_gap(nullptr, 1.0);
    assert(rt_dockpanel_dock_child(nullptr, nullptr, 4) == 0);
    rt_theme_set_mode(2);
    rt_theme_follow_system();
    const bool themeBackendAvailable = rt_gui_system_is_available() != 0;
    assert(rt_theme_get_mode() == (themeBackendAvailable ? 2 : 0));
    assert(rt_theme_set_palette(nullptr) == 0);
    void *selectedPalette = rt_theme_get_palette();
    assert((selectedPalette != nullptr) == themeBackendAvailable);
    releaseRuntimeObject(selectedPalette);
    rt_theme_reset_custom();
    assert(rt_theme_was_changed() == (themeBackendAvailable ? 1 : 0));
    assert(rt_theme_was_changed() == 0);
    assert((rt_theme_get_revision() > 0) == themeBackendAvailable);
    void *newPalette = rt_theme_palette_new();
    void *darkPalette = rt_theme_palette_from_dark();
    void *lightPalette = rt_theme_palette_from_light();
    assert((newPalette != nullptr) == themeBackendAvailable);
    assert((darkPalette != nullptr) == themeBackendAvailable);
    assert((lightPalette != nullptr) == themeBackendAvailable);
    assert(rt_theme_palette_clone(nullptr) == nullptr);
    assert(rt_theme_palette_set_color(nullptr, rt_const_cstr("bgPrimary"), 0) == 0);
    assert(rt_theme_palette_get_color(nullptr, rt_const_cstr("bgPrimary")) == 0);
    assert(rt_theme_palette_set_metric(nullptr, rt_const_cstr("buttonHeight"), 1.0) == 0);
    assert(rt_theme_palette_get_metric(nullptr, rt_const_cstr("buttonHeight")) == 0.0);
    rt_theme_palette_set_motion_enabled(nullptr, 1);
    rt_theme_palette_set_font_roles(nullptr, nullptr, nullptr, nullptr);
    void *themeValidation = rt_theme_palette_validate(nullptr);
    assert(themeValidation != nullptr && rt_result_is_err(themeValidation) == 1);
    const char *themeValidationError = rt_string_cstr(rt_result_unwrap_err_str(themeValidation));
    assert(std::strcmp(themeValidationError,
                       themeBackendAvailable ? "GUI theme token palette has an invalid value"
                                             : "GUI support is not available in this build") == 0);
    releaseRuntimeObject(themeValidation);
    releaseRuntimeObject(newPalette);
    releaseRuntimeObject(darkPalette);
    releaseRuntimeObject(lightPalette);
    rt_textinput_set_max_length(nullptr, 8);
    assert(rt_textinput_get_max_length(nullptr) == 0);
    rt_textinput_set_password(nullptr, 1);
    assert(rt_textinput_is_password(nullptr) == 0);
    rt_textinput_set_read_only(nullptr, 1);
    assert(rt_textinput_is_read_only(nullptr) == 0);
    rt_textinput_set_multiline(nullptr, 1);
    assert(rt_textinput_is_multiline(nullptr) == 0);
    rt_textinput_set_cursor(nullptr, 4);
    assert(rt_textinput_get_cursor(nullptr) == 0);
    rt_textinput_select_range(nullptr, 1, 3);
    rt_textinput_clear_selection(nullptr);
    assert(rt_textinput_get_selection_start(nullptr) == 0);
    assert(rt_textinput_get_selection_end(nullptr) == 0);
    rt_string selectedText = rt_textinput_get_selected_text(nullptr);
    assert(selectedText != nullptr && rt_str_len(selectedText) == 0);
    rt_str_release_maybe(selectedText);
    assert(rt_textinput_insert_text(nullptr, rt_const_cstr("ignored")) == 0);
    assert(rt_textinput_delete_selection(nullptr) == 0);
    assert(rt_textinput_undo(nullptr) == 0);
    assert(rt_textinput_redo(nullptr) == 0);
    assert(rt_textinput_can_undo(nullptr) == 0);
    assert(rt_textinput_can_redo(nullptr) == 0);
    assert(rt_textinput_was_changed(nullptr) == 0);
    assert(rt_textinput_was_submitted(nullptr) == 0);
    assert(rt_textinput_get_revision(nullptr) == 0);
    assert(rt_textinput_is_composing(nullptr) == 0);
    rt_string compositionText = rt_textinput_get_composition_text(nullptr);
    assert(compositionText != nullptr && rt_str_len(compositionText) == 0);
    rt_str_release_maybe(compositionText);
    assert(rt_textinput_get_composition_start(nullptr) == 0);
    assert(rt_textinput_get_composition_length(nullptr) == 0);
    rt_label_set_alignment(nullptr, 1);
    assert(rt_label_get_alignment(nullptr) == -1);
    rt_label_set_ellipsis(nullptr, 1);
    rt_label_set_max_lines(nullptr, 2);
    rt_label_set_selectable(nullptr, 1);
    rt_string labelSelection = rt_label_get_selected_text(nullptr);
    assert(labelSelection != nullptr && rt_str_len(labelSelection) == 0);
    rt_str_release_maybe(labelSelection);
    rt_treeview_toggle(nullptr, nullptr);
    rt_treeview_scroll_to(nullptr, nullptr);
    assert(rt_treeview_was_load_children_requested(nullptr) == 0);
    void *treeOption = rt_treeview_get_load_requested_node_option(nullptr);
    assert(treeOption != nullptr && rt_option_is_none(treeOption) == 1);
    releaseRuntimeObject(treeOption);
    treeOption = rt_treeview_get_activated_node_option(nullptr);
    assert(treeOption != nullptr && rt_option_is_none(treeOption) == 1);
    releaseRuntimeObject(treeOption);
    rt_treeview_node_set_text(nullptr, rt_const_cstr("ignored"));
    rt_treeview_node_set_icon(nullptr, rt_const_cstr("ignored"));
    rt_treeview_node_set_has_children(nullptr, 1);
    rt_treeview_node_set_loading(nullptr, 1);
    rt_treeview_node_set_stable_id(nullptr, rt_const_cstr("ignored"));
    rt_string treeText = rt_treeview_node_get_icon(nullptr);
    assert(treeText != nullptr && rt_str_len(treeText) == 0);
    rt_str_release_maybe(treeText);
    treeText = rt_treeview_node_get_stable_id(nullptr);
    assert(treeText != nullptr && rt_str_len(treeText) == 0);
    rt_str_release_maybe(treeText);
    assert(rt_treeview_node_has_children(nullptr) == 0);
    assert(rt_treeview_node_is_loading(nullptr) == 0);
    rt_widget_set_accessible_role(nullptr, 5);
    rt_widget_set_accessible_name(nullptr, rt_const_cstr("ignored"));
    rt_widget_set_accessible_description(nullptr, rt_const_cstr("ignored"));
    rt_widget_set_accessible_value(nullptr, rt_const_cstr("ignored"));
    rt_widget_set_accessible_label_for(nullptr, nullptr);
    rt_widget_clear_accessible_label_for(nullptr);
    rt_widget_set_live_region(nullptr, 2);
    rt_accessibility_announce(nullptr, rt_const_cstr("ignored"), 1);
    rt_accessibility_set_high_contrast(1);
    rt_accessibility_set_reduced_motion(1);
    assert(rt_accessibility_is_high_contrast() == 0);
    assert(rt_accessibility_is_reduced_motion() == 0);
    const int64_t systemContrast = rt_accessibility_get_system_high_contrast();
    const int64_t systemMotion = rt_accessibility_get_system_reduced_motion();
    assert(systemContrast == 0 || systemContrast == 1);
    assert(systemMotion == 0 || systemMotion == 1);

    rt_string semantic = rt_widget_get_accessible_name(nullptr);
    assert(semantic != nullptr && rt_str_len(semantic) == 0);
    rt_str_release_maybe(semantic);
    semantic = rt_widget_get_accessible_description(nullptr);
    assert(semantic != nullptr && rt_str_len(semantic) == 0);
    rt_str_release_maybe(semantic);
    semantic = rt_widget_get_accessible_value(nullptr);
    assert(semantic != nullptr && rt_str_len(semantic) == 0);
    rt_str_release_maybe(semantic);
    void *snapshot = rt_accessibility_snapshot(nullptr);
    assert(snapshot != nullptr);
    assert(rt_map_get_int(snapshot, rt_const_cstr("schemaVersion")) == 1);
    releaseRuntimeObject(snapshot);

    const int64_t available = rt_gui_system_is_available();
    assert(available == 0 || available == 1);

    rt_string reason = rt_gui_system_get_unavailable_reason();
    assert(reason != nullptr);
    if (available) {
        assert(rt_str_len(reason) == 0);
        assert(rt_messagebox_show_async(nullptr) == 0);
        assert(rt_messagebox_is_open(nullptr) == 0);
        assert(rt_messagebox_was_completed(nullptr) == 0);
        assert(rt_messagebox_get_status(nullptr) == RT_GUI_DIALOG_STATUS_FAILED);
        assert(rt_filedialog_show_async(nullptr) == 0);
        assert(rt_filedialog_is_open(nullptr) == 0);
        assert(rt_filedialog_was_completed(nullptr) == 0);
        assert(rt_filedialog_get_status(nullptr) == RT_GUI_DIALOG_STATUS_FAILED);
        rt_str_release_maybe(reason);
        return;
    }

    constexpr const char *expected = "GUI support is not available in this build";
    assert(std::strcmp(rt_string_cstr(reason), expected) == 0);
    rt_str_release_maybe(reason);

    rt_messagebox_add_button_with_role(
        nullptr, rt_const_cstr("ignored"), 7, RT_GUI_DIALOG_BUTTON_DEFAULT);
    assert(rt_messagebox_set_button_role(nullptr, 7, RT_GUI_DIALOG_BUTTON_CANCEL) == 0);
    assert(rt_messagebox_set_cancel_button(nullptr, 7) == 0);
    assert(rt_messagebox_set_default_button(nullptr, 7) == 0);
    assert(rt_messagebox_show_async(nullptr) == 0);
    assert(rt_messagebox_is_open(nullptr) == 0);
    assert(rt_messagebox_was_completed(nullptr) == 0);
    assert(rt_messagebox_get_status(nullptr) == RT_GUI_DIALOG_STATUS_FAILED);
    assert(rt_messagebox_get_result(nullptr) == -1);
    rt_string dialogError = rt_messagebox_get_error(nullptr);
    assert(dialogError && std::strcmp(rt_string_cstr(dialogError), expected) == 0);
    rt_str_release_maybe(dialogError);
    void *prompt = rt_messagebox_prompt_option(rt_const_cstr("title"), rt_const_cstr("prompt"));
    assert(prompt && rt_option_is_none(prompt) == 1);
    releaseRuntimeObject(prompt);

    rt_filedialog_set_show_hidden(nullptr, 1);
    rt_filedialog_set_confirm_overwrite(nullptr, 1);
    rt_filedialog_set_default_extension(nullptr, rt_const_cstr(".zia"));
    rt_filedialog_add_bookmark(nullptr, rt_const_cstr("/tmp"));
    rt_filedialog_clear_bookmarks(nullptr);
    assert(rt_filedialog_show_async(nullptr) == 0);
    assert(rt_filedialog_is_open(nullptr) == 0);
    assert(rt_filedialog_was_completed(nullptr) == 0);
    assert(rt_filedialog_get_status(nullptr) == RT_GUI_DIALOG_STATUS_FAILED);
    dialogError = rt_filedialog_get_error(nullptr);
    assert(dialogError && std::strcmp(rt_string_cstr(dialogError), expected) == 0);
    rt_str_release_maybe(dialogError);
    void *paths = rt_filedialog_get_paths(nullptr);
    assert(paths && rt_seq_len(paths) == 0);
    releaseRuntimeObject(paths);
    void *pathOption = rt_filedialog_open_option(
        rt_const_cstr("Open"), rt_const_cstr("/tmp"), rt_const_cstr("*.zia"));
    assert(pathOption && rt_option_is_none(pathOption) == 1);
    releaseRuntimeObject(pathOption);
    paths = rt_filedialog_open_multiple_seq(
        rt_const_cstr("Open"), rt_const_cstr("/tmp"), rt_const_cstr("*.zia"));
    assert(paths && rt_seq_len(paths) == 0);
    releaseRuntimeObject(paths);

    void *result = rt_gui_app_try_new(rt_const_cstr("unavailable"), 320, 200);
    assert(result != nullptr);
    assert(rt_result_is_err(result) == 1);
    rt_string error = rt_result_unwrap_err_str(result);
    assert(error != nullptr);
    assert(std::strcmp(rt_string_cstr(error), expected) == 0);
    releaseRuntimeObject(result);
}

} // namespace

/// @brief Run the GUI capability contract tests.
/// @return Zero after all assertions pass.
int main() {
    testHeadlessAutomationAndConstants();
    testAvailabilityContract();
    std::printf("RTGuiAvailabilityTests passed.\n");
    return 0;
}
