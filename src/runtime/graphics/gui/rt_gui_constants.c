//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/gui/rt_gui_constants.c
// Purpose: Implement allocation-free scalar getters for Zanna.GUI typed constants.
// Key invariants:
//   - Numeric values are the public runtime ABI, not inferred toolkit enum ordinals.
//   - Getters have identical behavior in graphics-enabled and headless runtime builds.
//   - Constants stay compatible with every existing integer-taking GUI operation.
// Ownership/Lifetime:
//   - No getter allocates, retains, releases, or references process-global state.
// Links: src/runtime/graphics/gui/rt_gui_constants.h,
//        src/il/runtime/defs/api/gui_layout.def,
//        docs/zannalib/gui/application.md
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_constants.c
/// @brief Defines allocation-free getters for stable public GUI constant values.
///
/// @details
/// Each macro-generated getter returns an explicit runtime ABI ordinal rather
/// than depending on lower-toolkit enum layout, so enabled and headless builds
/// expose identical scalar values.

#include "rt_gui_constants.h"

/// @brief Define one scalar constant getter with a compile-time literal result.
/// @details The public declaration supplies the semantic Doxygen contract for each generated
///          function. This implementation macro intentionally performs no casts through lower
///          toolkit enums, preserving the registry ordinals even if internal enums evolve.
/// @param function_name Exported C function name.
/// @param constant_value Stable signed 64-bit public value.
#define RT_GUI_DEFINE_CONSTANT(function_name, constant_value)                                      \
    int64_t function_name(void) {                                                                  \
        return (int64_t)(constant_value);                                                          \
    }

/// @copydoc rt_gui_align_start
RT_GUI_DEFINE_CONSTANT(rt_gui_align_start, 0)
/// @copydoc rt_gui_align_center
RT_GUI_DEFINE_CONSTANT(rt_gui_align_center, 1)
/// @copydoc rt_gui_align_end
RT_GUI_DEFINE_CONSTANT(rt_gui_align_end, 2)
/// @copydoc rt_gui_align_stretch
RT_GUI_DEFINE_CONSTANT(rt_gui_align_stretch, 3)

/// @copydoc rt_gui_justify_start
RT_GUI_DEFINE_CONSTANT(rt_gui_justify_start, 0)
/// @copydoc rt_gui_justify_center
RT_GUI_DEFINE_CONSTANT(rt_gui_justify_center, 1)
/// @copydoc rt_gui_justify_end
RT_GUI_DEFINE_CONSTANT(rt_gui_justify_end, 2)
/// @copydoc rt_gui_justify_space_between
RT_GUI_DEFINE_CONSTANT(rt_gui_justify_space_between, 3)
/// @copydoc rt_gui_justify_space_around
RT_GUI_DEFINE_CONSTANT(rt_gui_justify_space_around, 4)
/// @copydoc rt_gui_justify_space_evenly
RT_GUI_DEFINE_CONSTANT(rt_gui_justify_space_evenly, 5)

/// @copydoc rt_gui_flex_direction_row
RT_GUI_DEFINE_CONSTANT(rt_gui_flex_direction_row, 0)
/// @copydoc rt_gui_flex_direction_column
RT_GUI_DEFINE_CONSTANT(rt_gui_flex_direction_column, 1)
/// @copydoc rt_gui_flex_direction_row_reverse
RT_GUI_DEFINE_CONSTANT(rt_gui_flex_direction_row_reverse, 2)
/// @copydoc rt_gui_flex_direction_column_reverse
RT_GUI_DEFINE_CONSTANT(rt_gui_flex_direction_column_reverse, 3)

/// @copydoc rt_gui_flex_wrap_no_wrap
RT_GUI_DEFINE_CONSTANT(rt_gui_flex_wrap_no_wrap, 0)
/// @copydoc rt_gui_flex_wrap_wrap
RT_GUI_DEFINE_CONSTANT(rt_gui_flex_wrap_wrap, 1)
/// @copydoc rt_gui_flex_wrap_wrap_reverse
RT_GUI_DEFINE_CONSTANT(rt_gui_flex_wrap_wrap_reverse, 2)

/// @copydoc rt_gui_dock_left
RT_GUI_DEFINE_CONSTANT(rt_gui_dock_left, 0)
/// @copydoc rt_gui_dock_top
RT_GUI_DEFINE_CONSTANT(rt_gui_dock_top, 1)
/// @copydoc rt_gui_dock_right
RT_GUI_DEFINE_CONSTANT(rt_gui_dock_right, 2)
/// @copydoc rt_gui_dock_bottom
RT_GUI_DEFINE_CONSTANT(rt_gui_dock_bottom, 3)
/// @copydoc rt_gui_dock_fill
RT_GUI_DEFINE_CONSTANT(rt_gui_dock_fill, 4)

/// @copydoc rt_gui_theme_mode_dark
RT_GUI_DEFINE_CONSTANT(rt_gui_theme_mode_dark, 0)
/// @copydoc rt_gui_theme_mode_light
RT_GUI_DEFINE_CONSTANT(rt_gui_theme_mode_light, 1)
/// @copydoc rt_gui_theme_mode_system
RT_GUI_DEFINE_CONSTANT(rt_gui_theme_mode_system, 2)
/// @copydoc rt_gui_theme_mode_custom
RT_GUI_DEFINE_CONSTANT(rt_gui_theme_mode_custom, 3)

/// @copydoc rt_gui_accessible_role_none
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_none, 0)
/// @copydoc rt_gui_accessible_role_application
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_application, 1)
/// @copydoc rt_gui_accessible_role_window
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_window, 2)
/// @copydoc rt_gui_accessible_role_group
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_group, 3)
/// @copydoc rt_gui_accessible_role_label
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_label, 4)
/// @copydoc rt_gui_accessible_role_button
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_button, 5)
/// @copydoc rt_gui_accessible_role_check_box
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_check_box, 6)
/// @copydoc rt_gui_accessible_role_radio_button
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_radio_button, 7)
/// @copydoc rt_gui_accessible_role_text_box
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_text_box, 8)
/// @copydoc rt_gui_accessible_role_search_box
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_search_box, 9)
/// @copydoc rt_gui_accessible_role_combo_box
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_combo_box, 10)
/// @copydoc rt_gui_accessible_role_list
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_list, 11)
/// @copydoc rt_gui_accessible_role_list_item
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_list_item, 12)
/// @copydoc rt_gui_accessible_role_tree
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_tree, 13)
/// @copydoc rt_gui_accessible_role_tree_item
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_tree_item, 14)
/// @copydoc rt_gui_accessible_role_tab_list
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_tab_list, 15)
/// @copydoc rt_gui_accessible_role_tab
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_tab, 16)
/// @copydoc rt_gui_accessible_role_table
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_table, 17)
/// @copydoc rt_gui_accessible_role_row
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_row, 18)
/// @copydoc rt_gui_accessible_role_cell
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_cell, 19)
/// @copydoc rt_gui_accessible_role_slider
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_slider, 20)
/// @copydoc rt_gui_accessible_role_progress_bar
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_progress_bar, 21)
/// @copydoc rt_gui_accessible_role_dialog
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_dialog, 22)
/// @copydoc rt_gui_accessible_role_alert
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_alert, 23)
/// @copydoc rt_gui_accessible_role_menu
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_menu, 24)
/// @copydoc rt_gui_accessible_role_menu_item
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_menu_item, 25)
/// @copydoc rt_gui_accessible_role_tool_bar
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_tool_bar, 26)
/// @copydoc rt_gui_accessible_role_status_bar
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_status_bar, 27)
/// @copydoc rt_gui_accessible_role_image
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_image, 28)
/// @copydoc rt_gui_accessible_role_video
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_video, 29)
/// @copydoc rt_gui_accessible_role_link
RT_GUI_DEFINE_CONSTANT(rt_gui_accessible_role_link, 30)

/// @copydoc rt_gui_live_region_mode_off
RT_GUI_DEFINE_CONSTANT(rt_gui_live_region_mode_off, 0)
/// @copydoc rt_gui_live_region_mode_polite
RT_GUI_DEFINE_CONSTANT(rt_gui_live_region_mode_polite, 1)
/// @copydoc rt_gui_live_region_mode_assertive
RT_GUI_DEFINE_CONSTANT(rt_gui_live_region_mode_assertive, 2)

/// @copydoc rt_gui_dialog_button_role_normal
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_button_role_normal, 0)
/// @copydoc rt_gui_dialog_button_role_default
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_button_role_default, 1)
/// @copydoc rt_gui_dialog_button_role_cancel
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_button_role_cancel, 2)
/// @copydoc rt_gui_dialog_button_role_destructive
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_button_role_destructive, 3)
/// @copydoc rt_gui_dialog_button_role_accept
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_button_role_accept, 4)
/// @copydoc rt_gui_dialog_button_role_reject
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_button_role_reject, 5)
/// @copydoc rt_gui_dialog_button_role_help
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_button_role_help, 6)

/// @copydoc rt_gui_dialog_status_idle
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_status_idle, 0)
/// @copydoc rt_gui_dialog_status_open
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_status_open, 1)
/// @copydoc rt_gui_dialog_status_accepted
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_status_accepted, 2)
/// @copydoc rt_gui_dialog_status_cancelled
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_status_cancelled, 3)
/// @copydoc rt_gui_dialog_status_failed
RT_GUI_DEFINE_CONSTANT(rt_gui_dialog_status_failed, 4)

/// @copydoc rt_gui_image_filter_nearest
RT_GUI_DEFINE_CONSTANT(rt_gui_image_filter_nearest, 0)
/// @copydoc rt_gui_image_filter_bilinear
RT_GUI_DEFINE_CONSTANT(rt_gui_image_filter_bilinear, 1)

/// @copydoc rt_gui_sort_direction_none
RT_GUI_DEFINE_CONSTANT(rt_gui_sort_direction_none, 0)
/// @copydoc rt_gui_sort_direction_ascending
RT_GUI_DEFINE_CONSTANT(rt_gui_sort_direction_ascending, 1)
/// @copydoc rt_gui_sort_direction_descending
RT_GUI_DEFINE_CONSTANT(rt_gui_sort_direction_descending, -1)

#undef RT_GUI_DEFINE_CONSTANT
