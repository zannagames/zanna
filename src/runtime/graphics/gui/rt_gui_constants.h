//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/gui/rt_gui_constants.h
// Purpose: Stable typed constant getters published by Zanna.GUI static classes.
// Key invariants:
//   - Values are ABI-stable public ordinals, independent of lower toolkit enum layout.
//   - Every getter is allocation-free, deterministic, and available without graphics.
//   - Legacy integer-taking APIs continue to accept the same numeric values.
// Ownership/Lifetime:
//   - Getters return scalar values and create or retain no runtime objects.
// Links: src/runtime/graphics/gui/rt_gui_constants.c,
//        src/il/runtime/defs/api/gui_layout.def,
//        misc/plans/gui_20260716/01-api-surface.md
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_constants.h
/// @brief Declares stable scalar getters for typed constants in the public GUI runtime API.
///
/// @details
/// The declarations group layout, accessibility, dialog, image, and sorting
/// ordinals behind deterministic functions that remain available when graphics
/// support is disabled.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Return `Zanna.GUI.Align.Start`.
/// @details Selects cross-axis packing at the leading edge.
/// @return Stable public ordinal 0.
int64_t rt_gui_align_start(void);
/// @brief Return `Zanna.GUI.Align.Center`.
/// @details Selects centered cross-axis placement.
/// @return Stable public ordinal 1.
int64_t rt_gui_align_center(void);
/// @brief Return `Zanna.GUI.Align.End`.
/// @details Selects cross-axis packing at the trailing edge.
/// @return Stable public ordinal 2.
int64_t rt_gui_align_end(void);
/// @brief Return `Zanna.GUI.Align.Stretch`.
/// @details Selects cross-axis stretching to the available extent.
/// @return Stable public ordinal 3.
int64_t rt_gui_align_stretch(void);

/// @brief Return `Zanna.GUI.Justify.Start`.
/// @details Packs children at the main-axis leading edge.
/// @return Stable public ordinal 0.
int64_t rt_gui_justify_start(void);
/// @brief Return `Zanna.GUI.Justify.Center`.
/// @details Centers children along the main axis.
/// @return Stable public ordinal 1.
int64_t rt_gui_justify_center(void);
/// @brief Return `Zanna.GUI.Justify.End`.
/// @details Packs children at the main-axis trailing edge.
/// @return Stable public ordinal 2.
int64_t rt_gui_justify_end(void);
/// @brief Return `Zanna.GUI.Justify.SpaceBetween`.
/// @details Distributes free space between adjacent children only.
/// @return Stable public ordinal 3.
int64_t rt_gui_justify_space_between(void);
/// @brief Return `Zanna.GUI.Justify.SpaceAround`.
/// @details Distributes free space around each child with half-sized outer gaps.
/// @return Stable public ordinal 4.
int64_t rt_gui_justify_space_around(void);
/// @brief Return `Zanna.GUI.Justify.SpaceEvenly`.
/// @details Distributes equal free space between children and container edges.
/// @return Stable public ordinal 5.
int64_t rt_gui_justify_space_evenly(void);

/// @brief Return `Zanna.GUI.FlexDirection.Row`.
/// @details Selects a leading-to-trailing horizontal main axis.
/// @return Stable public ordinal 0.
int64_t rt_gui_flex_direction_row(void);
/// @brief Return `Zanna.GUI.FlexDirection.Column`.
/// @details Selects a top-to-bottom vertical main axis.
/// @return Stable public ordinal 1.
int64_t rt_gui_flex_direction_column(void);
/// @brief Return `Zanna.GUI.FlexDirection.RowReverse`.
/// @details Selects a trailing-to-leading horizontal main axis.
/// @return Stable public ordinal 2.
int64_t rt_gui_flex_direction_row_reverse(void);
/// @brief Return `Zanna.GUI.FlexDirection.ColumnReverse`.
/// @details Selects a bottom-to-top vertical main axis.
/// @return Stable public ordinal 3.
int64_t rt_gui_flex_direction_column_reverse(void);

/// @brief Return `Zanna.GUI.FlexWrap.NoWrap`.
/// @details Keeps all flex children on a single line.
/// @return Stable public ordinal 0.
int64_t rt_gui_flex_wrap_no_wrap(void);
/// @brief Return `Zanna.GUI.FlexWrap.Wrap`.
/// @details Wraps overflowing children toward the cross-axis end.
/// @return Stable public ordinal 1.
int64_t rt_gui_flex_wrap_wrap(void);
/// @brief Return `Zanna.GUI.FlexWrap.WrapReverse`.
/// @details Wraps lines toward the cross-axis start.
/// @return Stable public ordinal 2.
int64_t rt_gui_flex_wrap_wrap_reverse(void);

/// @brief Return `Zanna.GUI.Dock.Left`.
/// @details Claims space from the remaining layout rectangle's left edge.
/// @return Stable public ordinal 0.
int64_t rt_gui_dock_left(void);
/// @brief Return `Zanna.GUI.Dock.Top`.
/// @details Claims space from the remaining layout rectangle's top edge.
/// @return Stable public ordinal 1.
int64_t rt_gui_dock_top(void);
/// @brief Return `Zanna.GUI.Dock.Right`.
/// @details Claims space from the remaining layout rectangle's right edge.
/// @return Stable public ordinal 2.
int64_t rt_gui_dock_right(void);
/// @brief Return `Zanna.GUI.Dock.Bottom`.
/// @details Claims space from the remaining layout rectangle's bottom edge.
/// @return Stable public ordinal 3.
int64_t rt_gui_dock_bottom(void);
/// @brief Return `Zanna.GUI.Dock.Fill`.
/// @details Assigns all space left after edge-docked siblings.
/// @return Stable public ordinal 4.
int64_t rt_gui_dock_fill(void);

/// @brief Return `Zanna.GUI.ThemeMode.Dark`.
/// @details Selects the built-in dark theme family.
/// @return Stable public ordinal 0.
int64_t rt_gui_theme_mode_dark(void);
/// @brief Return `Zanna.GUI.ThemeMode.Light`.
/// @details Selects the built-in light theme family.
/// @return Stable public ordinal 1.
int64_t rt_gui_theme_mode_light(void);
/// @brief Return `Zanna.GUI.ThemeMode.System`.
/// @details Follows the current platform appearance preference.
/// @return Stable public ordinal 2.
int64_t rt_gui_theme_mode_system(void);
/// @brief Return `Zanna.GUI.ThemeMode.Custom`.
/// @details Identifies an application-supplied theme palette.
/// @return Stable public ordinal 3.
int64_t rt_gui_theme_mode_custom(void);

/// @brief Return `Zanna.GUI.AccessibleRole.None`.
/// @details Marks structural or decorative content without a semantic control role.
/// @return Stable public ordinal 0.
int64_t rt_gui_accessible_role_none(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Application`.
/// @details Identifies the semantic root of an application.
/// @return Stable public ordinal 1.
int64_t rt_gui_accessible_role_application(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Window`.
/// @details Identifies an application window.
/// @return Stable public ordinal 2.
int64_t rt_gui_accessible_role_window(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Group`.
/// @details Identifies a semantic grouping container.
/// @return Stable public ordinal 3.
int64_t rt_gui_accessible_role_group(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Label`.
/// @details Identifies static or control-labelling text.
/// @return Stable public ordinal 4.
int64_t rt_gui_accessible_role_label(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Button`.
/// @details Identifies an invokable button control.
/// @return Stable public ordinal 5.
int64_t rt_gui_accessible_role_button(void);
/// @brief Return `Zanna.GUI.AccessibleRole.CheckBox`.
/// @details Identifies an independently checked toggle.
/// @return Stable public ordinal 6.
int64_t rt_gui_accessible_role_check_box(void);
/// @brief Return `Zanna.GUI.AccessibleRole.RadioButton`.
/// @details Identifies one mutually exclusive radio choice.
/// @return Stable public ordinal 7.
int64_t rt_gui_accessible_role_radio_button(void);
/// @brief Return `Zanna.GUI.AccessibleRole.TextBox`.
/// @details Identifies an editable text field.
/// @return Stable public ordinal 8.
int64_t rt_gui_accessible_role_text_box(void);
/// @brief Return `Zanna.GUI.AccessibleRole.SearchBox`.
/// @details Identifies a text field whose purpose is search.
/// @return Stable public ordinal 9.
int64_t rt_gui_accessible_role_search_box(void);
/// @brief Return `Zanna.GUI.AccessibleRole.ComboBox`.
/// @details Identifies a combined value and popup-choice control.
/// @return Stable public ordinal 10.
int64_t rt_gui_accessible_role_combo_box(void);
/// @brief Return `Zanna.GUI.AccessibleRole.List`.
/// @details Identifies a semantic list container.
/// @return Stable public ordinal 11.
int64_t rt_gui_accessible_role_list(void);
/// @brief Return `Zanna.GUI.AccessibleRole.ListItem`.
/// @details Identifies one item in a semantic list.
/// @return Stable public ordinal 12.
int64_t rt_gui_accessible_role_list_item(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Tree`.
/// @details Identifies a hierarchical tree container.
/// @return Stable public ordinal 13.
int64_t rt_gui_accessible_role_tree(void);
/// @brief Return `Zanna.GUI.AccessibleRole.TreeItem`.
/// @details Identifies one expandable item in a tree.
/// @return Stable public ordinal 14.
int64_t rt_gui_accessible_role_tree_item(void);
/// @brief Return `Zanna.GUI.AccessibleRole.TabList`.
/// @details Identifies the container for a set of selectable tabs.
/// @return Stable public ordinal 15.
int64_t rt_gui_accessible_role_tab_list(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Tab`.
/// @details Identifies one selectable tab.
/// @return Stable public ordinal 16.
int64_t rt_gui_accessible_role_tab(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Table`.
/// @details Identifies a row-and-column data table.
/// @return Stable public ordinal 17.
int64_t rt_gui_accessible_role_table(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Row`.
/// @details Identifies one semantic table row.
/// @return Stable public ordinal 18.
int64_t rt_gui_accessible_role_row(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Cell`.
/// @details Identifies one semantic table cell.
/// @return Stable public ordinal 19.
int64_t rt_gui_accessible_role_cell(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Slider`.
/// @details Identifies a bounded continuous-value control.
/// @return Stable public ordinal 20.
int64_t rt_gui_accessible_role_slider(void);
/// @brief Return `Zanna.GUI.AccessibleRole.ProgressBar`.
/// @details Identifies a read-only progress indicator.
/// @return Stable public ordinal 21.
int64_t rt_gui_accessible_role_progress_bar(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Dialog`.
/// @details Identifies a dialog surface.
/// @return Stable public ordinal 22.
int64_t rt_gui_accessible_role_dialog(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Alert`.
/// @details Identifies an urgent alert surface.
/// @return Stable public ordinal 23.
int64_t rt_gui_accessible_role_alert(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Menu`.
/// @details Identifies a menu container.
/// @return Stable public ordinal 24.
int64_t rt_gui_accessible_role_menu(void);
/// @brief Return `Zanna.GUI.AccessibleRole.MenuItem`.
/// @details Identifies one actionable menu entry.
/// @return Stable public ordinal 25.
int64_t rt_gui_accessible_role_menu_item(void);
/// @brief Return `Zanna.GUI.AccessibleRole.ToolBar`.
/// @details Identifies a toolbar grouping of commands.
/// @return Stable public ordinal 26.
int64_t rt_gui_accessible_role_tool_bar(void);
/// @brief Return `Zanna.GUI.AccessibleRole.StatusBar`.
/// @details Identifies a status-information bar.
/// @return Stable public ordinal 27.
int64_t rt_gui_accessible_role_status_bar(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Image`.
/// @details Identifies meaningful image content.
/// @return Stable public ordinal 28.
int64_t rt_gui_accessible_role_image(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Video`.
/// @details Identifies video or moving-image content.
/// @return Stable public ordinal 29.
int64_t rt_gui_accessible_role_video(void);
/// @brief Return `Zanna.GUI.AccessibleRole.Link`.
/// @details Identifies a navigable hyperlink.
/// @return Stable public ordinal 30.
int64_t rt_gui_accessible_role_link(void);

/// @brief Return `Zanna.GUI.LiveRegionMode.Off`.
/// @details Disables automatic assistive announcements for changes.
/// @return Stable public ordinal 0.
int64_t rt_gui_live_region_mode_off(void);
/// @brief Return `Zanna.GUI.LiveRegionMode.Polite`.
/// @details Queues announcements after the current assistive utterance.
/// @return Stable public ordinal 1.
int64_t rt_gui_live_region_mode_polite(void);
/// @brief Return `Zanna.GUI.LiveRegionMode.Assertive`.
/// @details Requests that a new announcement interrupt current speech.
/// @return Stable public ordinal 2.
int64_t rt_gui_live_region_mode_assertive(void);

/// @brief Return `Zanna.GUI.DialogButtonRole.Normal`.
/// @details Identifies an ordinary action without implicit keyboard behavior.
/// @return Stable public ordinal 0.
int64_t rt_gui_dialog_button_role_normal(void);
/// @brief Return `Zanna.GUI.DialogButtonRole.Default`.
/// @details Identifies the primary Enter-key action.
/// @return Stable public ordinal 1.
int64_t rt_gui_dialog_button_role_default(void);
/// @brief Return `Zanna.GUI.DialogButtonRole.Cancel`.
/// @details Identifies the Escape-key cancellation action.
/// @return Stable public ordinal 2.
int64_t rt_gui_dialog_button_role_cancel(void);
/// @brief Return `Zanna.GUI.DialogButtonRole.Destructive`.
/// @details Identifies an irreversible action requiring destructive styling.
/// @return Stable public ordinal 3.
int64_t rt_gui_dialog_button_role_destructive(void);
/// @brief Return `Zanna.GUI.DialogButtonRole.Accept`.
/// @details Identifies an explicit acceptance action.
/// @return Stable public ordinal 4.
int64_t rt_gui_dialog_button_role_accept(void);
/// @brief Return `Zanna.GUI.DialogButtonRole.Reject`.
/// @details Identifies an explicit rejection or cancellation action.
/// @return Stable public ordinal 5.
int64_t rt_gui_dialog_button_role_reject(void);
/// @brief Return `Zanna.GUI.DialogButtonRole.Help`.
/// @details Identifies a non-terminal help action.
/// @return Stable public ordinal 6.
int64_t rt_gui_dialog_button_role_help(void);

/// @brief Return `Zanna.GUI.DialogStatus.Idle`.
/// @details Identifies a constructed dialog that is not currently presented.
/// @return Stable public ordinal 0.
int64_t rt_gui_dialog_status_idle(void);
/// @brief Return `Zanna.GUI.DialogStatus.Open`.
/// @details Identifies a presented dialog awaiting a semantic outcome.
/// @return Stable public ordinal 1.
int64_t rt_gui_dialog_status_open(void);
/// @brief Return `Zanna.GUI.DialogStatus.Accepted`.
/// @details Identifies a terminal accepting outcome.
/// @return Stable public ordinal 2.
int64_t rt_gui_dialog_status_accepted(void);
/// @brief Return `Zanna.GUI.DialogStatus.Cancelled`.
/// @details Identifies a terminal cancellation or rejection outcome.
/// @return Stable public ordinal 3.
int64_t rt_gui_dialog_status_cancelled(void);
/// @brief Return `Zanna.GUI.DialogStatus.Failed`.
/// @details Identifies presentation or result-processing failure.
/// @return Stable public ordinal 4.
int64_t rt_gui_dialog_status_failed(void);

/// @brief Return `Zanna.GUI.ImageFilter.Nearest`.
/// @details Selects deterministic nearest-neighbour image sampling.
/// @return Stable public ordinal 0.
int64_t rt_gui_image_filter_nearest(void);
/// @brief Return `Zanna.GUI.ImageFilter.Bilinear`.
/// @details Selects four-sample bilinear image interpolation.
/// @return Stable public ordinal 1.
int64_t rt_gui_image_filter_bilinear(void);

/// @brief Return `Zanna.GUI.SortDirection.None`.
/// @details Clears the active data-grid sort direction.
/// @return Stable public ordinal 0.
int64_t rt_gui_sort_direction_none(void);
/// @brief Return `Zanna.GUI.SortDirection.Ascending`.
/// @details Requests increasing-order data-grid sorting.
/// @return Stable public ordinal 1.
int64_t rt_gui_sort_direction_ascending(void);
/// @brief Return `Zanna.GUI.SortDirection.Descending`.
/// @details Requests decreasing-order data-grid sorting.
/// @return Stable public ordinal -1.
int64_t rt_gui_sort_direction_descending(void);

#ifdef __cplusplus
}
#endif
