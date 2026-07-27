//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_controls.c
// Purpose: Input-control GUI widgets — Dropdown, Slider, ProgressBar, and
//          ListBox (plus list items). Split out of rt_gui_widgets_complex.c;
//          shares GUI types via rt_gui_internal.h.
//
// Key invariants:
//   - Mirrors rt_gui_widgets_complex.c's ZANNA_ENABLE_GRAPHICS guard: real
//     widgets when graphics is enabled, no-op stubs otherwise.
//   - Handles are validated via the rt_*_checked casts before use.
//   - ListBox reorder requests remain application-directed: bindings expose
//     source/target edges without mutating retained row linkage.
//
// Ownership/Lifetime:
//   - Widgets are owned by the GUI widget tree; this layer borrows them.
//
// Links: src/runtime/graphics/gui/rt_gui_widgets_complex.c (other complex widgets),
//        src/runtime/graphics/gui/rt_gui_internal.h (shared GUI types + API)
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_controls.c
/// @brief Implements runtime bindings for dropdown, slider, progress-bar, and list-box controls.
///
/// @details
/// Graphics-enabled functions validate runtime handles and translate managed
/// strings and scalar values into widget-layer operations. The headless branch
/// retains the same ABI with neutral return values and no side effects.

#include "rt_gui_internal.h"
#include "rt_platform.h"
#include "rt_seq.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Resolve a parent-container handle to its widget (file-local copy).
/// @param parent Candidate parent handle, or `NULL` for a detached control.
/// @return Borrowed parent widget, or `NULL` for a null or invalid handle.
static vg_widget_t *rt_widget_parent_or_null_if_invalid(void *parent) {
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    return parent_widget;
}

/// @brief Validate and cast a runtime handle to a live Dropdown.
/// @param handle Candidate runtime widget handle.
/// @return Borrowed Dropdown pointer, or `NULL` for an invalid or wrong-type handle.
static vg_dropdown_t *rt_dropdown_checked(void *handle) {
    return (vg_dropdown_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_DROPDOWN);
}

/// @brief Safe-cast a handle to a live Slider widget, or NULL.
/// @param handle Candidate runtime widget handle.
/// @return Borrowed Slider pointer, or `NULL` for invalid input.
static vg_slider_t *rt_slider_checked(void *handle) {
    return (vg_slider_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_SLIDER);
}

/// @brief Safe-cast a handle to a live ProgressBar widget, or NULL.
/// @param handle Candidate runtime widget handle.
/// @return Borrowed ProgressBar pointer, or `NULL` for invalid input.
static vg_progressbar_t *rt_progressbar_checked(void *handle) {
    return (vg_progressbar_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_PROGRESS);
}

/// @brief Safe-cast a handle to a live ListBox widget, or NULL.
/// @param handle Candidate runtime widget handle.
/// @return Borrowed ListBox pointer, or `NULL` for invalid input.
static vg_listbox_t *rt_listbox_checked(void *handle) {
    return (vg_listbox_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_LISTBOX);
}

//=============================================================================
// Dropdown Widget
//=============================================================================

/// @brief Create a dropdown (combo box) widget — a button that pops a list of choices.
/// @param parent Parent-container handle, or `NULL` for a detached widget.
/// @return New Dropdown handle, or `NULL` for an invalid parent or allocation failure.
void *rt_dropdown_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_dropdown_t *dropdown = vg_dropdown_create(parent_widget);
    rt_gui_apply_default_font((vg_widget_t *)dropdown);
    return dropdown;
}

/// @brief Add a selectable item to a dropdown list.
/// @param dropdown Dropdown widget handle.
/// @param text Runtime display text copied into the widget.
/// @return Zero-based index of the new item, or `-1` for an invalid handle.
int64_t rt_dropdown_add_item(void *dropdown, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    if (!dd)
        return -1;
    char *ctext = rt_string_to_gui_cstr(text);
    int64_t index = vg_dropdown_add_item(dd, rt_gui_cstr_or_empty(ctext));
    free(ctext);
    return index;
}

/// @brief Remove an item from a dropdown by index.
/// @param dropdown Dropdown widget handle.
/// @param index Zero-based item index; invalid values are ignored.
void rt_dropdown_remove_item(void *dropdown, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    if (dd) {
        vg_dropdown_remove_item(dd, rt_gui_clamp_i64_to_i32(index, INT32_MIN, INT32_MAX));
    }
}

/// @brief Remove all items from a dropdown, leaving it empty.
/// @param dropdown Dropdown widget handle; invalid handles are ignored.
void rt_dropdown_clear(void *dropdown) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    if (dd) {
        vg_dropdown_clear(dd);
    }
}

/// @brief Programmatically select a dropdown item by index.
/// @param dropdown Dropdown widget handle.
/// @param index Zero-based item index, or the widget's supported no-selection value.
void rt_dropdown_set_selected(void *dropdown, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    if (dd) {
        vg_dropdown_set_selected(dd, rt_gui_clamp_i64_to_i32(index, INT32_MIN, INT32_MAX));
    }
}

/// @brief Get the index of the currently selected dropdown item (-1 if none).
/// @param dropdown Dropdown widget handle.
/// @return Zero-based selected index, or `-1` when absent or invalid.
int64_t rt_dropdown_get_selected(void *dropdown) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    if (!dd)
        return -1;
    return vg_dropdown_get_selected(dd);
}

/// @brief Get the selected text of the dropdown.
/// @param dropdown Dropdown widget handle.
/// @return Owned selected text, or an empty runtime string when absent or invalid.
rt_string rt_dropdown_get_selected_text(void *dropdown) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    if (!dd)
        return rt_str_empty();
    const char *text = vg_dropdown_get_selected_text(dd);
    if (!text)
        return rt_str_empty();
    return rt_string_from_bytes(text, strlen(text));
}

/// @brief Set the placeholder of the dropdown.
/// @param dropdown Dropdown widget handle.
/// @param placeholder Runtime placeholder text copied into widget storage.
void rt_dropdown_set_placeholder(void *dropdown, rt_string placeholder) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    if (!dd)
        return;
    char *ctext = rt_string_to_gui_cstr(placeholder);
    vg_dropdown_set_placeholder(dd, rt_gui_cstr_or_empty(ctext));
    free(ctext);
}

/// @brief Consume the dropdown's independent selection-change edge.
/// @details Programmatic and user-driven selection transitions share this
///          edge; reading it does not consume the widget revision.
/// @param dropdown Dropdown widget handle.
/// @return 1 once after one or more unreported selection transitions, otherwise 0.
int64_t rt_dropdown_was_changed(void *dropdown) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    return dd ? (vg_widget_was_changed(&dd->base) ? 1 : 0) : 0;
}

/// @brief Return the dropdown's non-consuming state revision.
/// @param dropdown Dropdown widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_dropdown_get_revision(void *dropdown) {
    RT_ASSERT_MAIN_THREAD();
    vg_dropdown_t *dd = rt_dropdown_checked(dropdown);
    return dd ? rt_widget_get_revision(&dd->base) : 0;
}

//=============================================================================
// Slider Widget
//=============================================================================

/// @brief Create a slider widget for picking a numeric value within a range.
/// @param parent Parent-container handle, or `NULL` for a detached widget.
/// @param horizontal Non-zero for left-right slider, zero for top-bottom.
/// @return New Slider handle, or `NULL` for an invalid parent or allocation failure.
void *rt_slider_new(void *parent, int64_t horizontal) {
    RT_ASSERT_MAIN_THREAD();
    vg_slider_orientation_t orient = horizontal ? VG_SLIDER_HORIZONTAL : VG_SLIDER_VERTICAL;
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_slider_t *slider = vg_slider_create(parent_widget, orient);
    if (slider)
        rt_gui_apply_default_font((vg_widget_t *)slider);
    return slider;
}

/// @brief Set the value of the slider.
/// @param slider Slider widget handle.
/// @param value Finite numeric value, sanitized to the supported widget range.
void rt_slider_set_value(void *slider, double value) {
    RT_ASSERT_MAIN_THREAD();
    vg_slider_t *sl = rt_slider_checked(slider);
    if (sl) {
        if (!rt_gui_double_is_finite(value))
            return;
        vg_slider_set_value(sl, rt_gui_sanitize_signed_float(value, RT_GUI_MAX_LAYOUT_VALUE));
    }
}

/// @brief Get the value of the slider.
/// @param slider Slider widget handle.
/// @return Current numeric value, or `0.0` for an invalid handle.
double rt_slider_get_value(void *slider) {
    RT_ASSERT_MAIN_THREAD();
    vg_slider_t *sl = rt_slider_checked(slider);
    if (!sl)
        return 0.0;
    return (double)vg_slider_get_value(sl);
}

/// @brief Set the range of the slider.
/// @param slider Slider widget handle.
/// @param min_val Finite lower endpoint; swapped with @p max_val when greater.
/// @param max_val Finite upper endpoint.
void rt_slider_set_range(void *slider, double min_val, double max_val) {
    RT_ASSERT_MAIN_THREAD();
    vg_slider_t *sl = rt_slider_checked(slider);
    if (sl) {
        if (!rt_gui_double_is_finite(min_val) || !rt_gui_double_is_finite(max_val))
            return;
        if (min_val > max_val) {
            double tmp = min_val;
            min_val = max_val;
            max_val = tmp;
        }
        vg_slider_set_range(sl,
                            rt_gui_sanitize_signed_float(min_val, RT_GUI_MAX_LAYOUT_VALUE),
                            rt_gui_sanitize_signed_float(max_val, RT_GUI_MAX_LAYOUT_VALUE));
    }
}

/// @brief Set the step of the slider.
/// @param slider Slider widget handle.
/// @param step Requested non-negative step size.
void rt_slider_set_step(void *slider, double step) {
    RT_ASSERT_MAIN_THREAD();
    vg_slider_t *sl = rt_slider_checked(slider);
    if (sl) {
        vg_slider_set_step(sl, rt_gui_sanitize_nonnegative_float(step, RT_GUI_MAX_LAYOUT_VALUE));
    }
}

/// @brief Consume the slider's independent numeric-value change edge.
/// @details Range clamping, programmatic assignments, and user input all
///          record the same edge when the effective value changes.
/// @param slider Slider widget handle.
/// @return 1 once after one or more unreported value transitions, otherwise 0.
int64_t rt_slider_was_changed(void *slider) {
    RT_ASSERT_MAIN_THREAD();
    vg_slider_t *sl = rt_slider_checked(slider);
    return sl ? (vg_widget_was_changed(&sl->base) ? 1 : 0) : 0;
}

/// @brief Return the slider's non-consuming state revision.
/// @param slider Slider widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_slider_get_revision(void *slider) {
    RT_ASSERT_MAIN_THREAD();
    vg_slider_t *sl = rt_slider_checked(slider);
    return sl ? rt_widget_get_revision(&sl->base) : 0;
}

//=============================================================================
// ProgressBar Widget
//=============================================================================

/// @brief Create a horizontal progress bar (0.0–1.0 fill range).
/// @param parent Parent-container handle, or `NULL` for a detached widget.
/// @return New ProgressBar handle, or `NULL` for an invalid parent or allocation failure.
void *rt_progressbar_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_progressbar_t *pb = vg_progressbar_create(parent_widget);
    if (pb)
        rt_gui_apply_default_font((vg_widget_t *)pb);
    return pb;
}

/// @brief Set the value of the progressbar.
/// @param progress ProgressBar widget handle.
/// @param value Fill fraction clamped to `[0.0,1.0]`; non-finite values become zero.
void rt_progressbar_set_value(void *progress, double value) {
    RT_ASSERT_MAIN_THREAD();
    vg_progressbar_t *pb = rt_progressbar_checked(progress);
    if (pb) {
        double sanitized = rt_gui_double_is_finite(value) ? value : 0.0;
        vg_progressbar_set_value(pb, (float)rt_gui_clamp_f64(sanitized, 0.0, 1.0));
    }
}

/// @brief Get the value of the progressbar.
/// @param progress ProgressBar widget handle.
/// @return Current fill fraction, or `0.0` for an invalid handle.
double rt_progressbar_get_value(void *progress) {
    RT_ASSERT_MAIN_THREAD();
    vg_progressbar_t *pb = rt_progressbar_checked(progress);
    if (!pb)
        return 0.0;
    return (double)vg_progressbar_get_value(pb);
}

/// @brief Set the visual style of a progress bar (bar, circle, or indeterminate).
/// @details Controls how progress is rendered: VG_PROGRESS_BAR for a standard
///          horizontal fill, VG_PROGRESS_INDETERMINATE for a looping animation
///          when the total duration is unknown. Out-of-range values default to bar.
/// @param progress Progress bar widget handle.
/// @param style    Style enum value (VG_PROGRESS_BAR, VG_PROGRESS_INDETERMINATE, etc.).
void rt_progressbar_set_style(void *progress, int64_t style) {
    RT_ASSERT_MAIN_THREAD();
    vg_progressbar_t *pb = rt_progressbar_checked(progress);
    if (!pb)
        return;
    if (style < (int64_t)VG_PROGRESS_BAR || style > (int64_t)VG_PROGRESS_INDETERMINATE)
        style = (int64_t)VG_PROGRESS_BAR;
    vg_progressbar_set_style(pb, (vg_progress_style_t)style);
}

/// @brief Show or hide the percentage text label on a progress bar.
/// @param progress Progress bar widget handle.
/// @param show     Non-zero to render the "XX%" label, zero to hide it.
void rt_progressbar_show_percentage(void *progress, int64_t show) {
    RT_ASSERT_MAIN_THREAD();
    vg_progressbar_t *pb = rt_progressbar_checked(progress);
    if (pb)
        vg_progressbar_show_percentage(pb, show != 0);
}

//=============================================================================
// ListBox Widget
//=============================================================================

/// @brief Create an empty scrollable list-box widget.
/// @param parent Parent-container handle, or `NULL` for a detached widget.
/// @return New ListBox handle, or `NULL` for an invalid parent or allocation failure.
void *rt_listbox_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_listbox_t *listbox = vg_listbox_create(parent_widget);
    rt_gui_apply_default_font((vg_widget_t *)listbox);
    return listbox;
}

/// @brief Append `text` as a new list-box item; returns the new item handle.
/// @param listbox ListBox widget handle.
/// @param text Runtime display text copied into the new row.
/// @return Managed subhandle for the new item, or `NULL` on invalid input or failure.
void *rt_listbox_add_item(void *listbox, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_listbox_item_t *item = vg_listbox_add_item(lb, ctext, NULL);
    free(ctext);
    return rt_gui_wrap_listbox_item(item);
}

/// @brief Remove an item from the listbox.
/// @param listbox Owning ListBox widget handle.
/// @param item Live item subhandle owned by @p listbox.
void rt_listbox_remove_item(void *listbox, void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    vg_listbox_item_t *it = item ? rt_gui_listbox_item_from_handle(item) : NULL;
    if (lb && it && vg_listbox_item_is_live(it) && it->owner == lb) {
        vg_listbox_remove_item(lb, it);
        rt_gui_collect_retired_subhandles(&lb->base);
    }
}

/// @brief Remove all entries from the listbox.
/// @param listbox ListBox widget handle; invalid handles are ignored.
void rt_listbox_clear(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (lb) {
        vg_listbox_clear(lb);
        rt_gui_collect_retired_subhandles(&lb->base);
    }
}

/// @brief Programmatically select a listbox item by handle.
/// @param listbox Owning ListBox widget handle.
/// @param item Item subhandle to select, or `NULL` to clear selection.
void rt_listbox_select(void *listbox, void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    vg_listbox_item_t *it = item ? rt_gui_listbox_item_from_handle(item) : NULL;
    if (!lb)
        return;
    if (item && (!it || it->owner != lb))
        return;
    vg_listbox_select(lb, it);
}

/// @brief Return the currently-selected listbox item handle (NULL when none).
/// @param listbox ListBox widget handle.
/// @return Managed subhandle for the selected retained item, or `NULL`.
void *rt_listbox_get_selected(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb)
        return NULL;
    return rt_gui_wrap_listbox_item(vg_listbox_get_selected(lb));
}

/// @brief Get the number of items in the listbox.
/// @param listbox ListBox widget handle.
/// @return Retained or virtual row count, saturated at `INT64_MAX`; zero when invalid.
int64_t rt_listbox_get_count(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb)
        return 0;
    size_t count = lb->virtual_mode ? lb->total_item_count : (size_t)lb->item_count;
    return count > (size_t)INT64_MAX ? INT64_MAX : (int64_t)count;
}

/// @brief Get the selected index of the listbox.
/// @param listbox ListBox widget handle.
/// @return Zero-based selected row index, or `-1` when absent, invalid, or unrepresentable.
int64_t rt_listbox_get_selected_index(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb)
        return -1;
    size_t idx = vg_listbox_get_selected_index(lb);
    if (idx == (size_t)-1)
        return -1;
    if (idx > (size_t)INT64_MAX)
        return -1;
    return (int64_t)idx;
}

/// @brief Select a listbox item by its zero-based index.
/// @param listbox ListBox widget handle.
/// @param index Zero-based retained or virtual row index.
void rt_listbox_select_index(void *listbox, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb || index < 0)
        return;
    if ((uintmax_t)index > (uintmax_t)SIZE_MAX)
        return;
    size_t idx = (size_t)index;
    size_t count = lb->virtual_mode ? lb->total_item_count : lb->item_count;
    if (idx >= count)
        return;
    vg_listbox_select_index(lb, idx);
}

/// @brief Scroll to the first listbox row without changing selection.
/// @param listbox ListBox widget handle; invalid handles are ignored.
void rt_listbox_scroll_to_top(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (lb)
        vg_listbox_scroll_to_top(lb);
}

/// @brief Scroll to the last listbox row without changing selection.
/// @param listbox ListBox widget handle; invalid handles are ignored.
void rt_listbox_scroll_to_bottom(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (lb)
        vg_listbox_scroll_to_bottom(lb);
}

/// @brief Enable or disable Ctrl/Shift multi-row selection.
/// @param listbox ListBox widget handle.
/// @param enabled Non-zero to permit multiple selected rows; zero to retain at most one.
void rt_listbox_set_multi_select(void *listbox, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb)
        return;

    const bool want_multi = enabled != 0;
    if (lb->multi_select == want_multi)
        return;

    if (want_multi) {
        lb->multi_select = true;
        return;
    }

    lb->multi_select = false;
    if (lb->virtual_mode) {
        size_t keep = vg_listbox_get_selected_index(lb);
        bool changed = false;
        if (lb->selection_bitmap) {
            for (size_t i = 0; i < lb->selection_bitmap_size; ++i) {
                if (lb->selection_bitmap[i])
                    changed = true;
                lb->selection_bitmap[i] = false;
            }
        }
        lb->selected_index = SIZE_MAX;
        lb->anchor_selected_index = SIZE_MAX;
        if (keep != SIZE_MAX && keep < lb->total_item_count) {
            vg_listbox_select_index(lb, keep);
            return;
        }
        if (changed) {
            lb->selection_revision++;
            lb->base.needs_paint = true;
        }
        return;
    }

    vg_listbox_item_t *keep = lb->selected;
    if (!keep) {
        for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
            if (item->selected) {
                keep = item;
                break;
            }
        }
    }
    vg_listbox_select(lb, keep);
}

/// @brief Enable application-directed retained-row reorder requests.
/// @param listbox ListBox widget handle.
/// @param enabled Non-zero to enable pointer and Alt+Arrow gestures.
void rt_listbox_set_reorderable(void *listbox, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (lb)
        vg_listbox_set_reorderable(lb, enabled != 0);
}

/// @brief Consume one application-directed retained-row reorder edge.
/// @param listbox ListBox widget handle.
/// @return 1 once for each valid non-no-op request, otherwise zero.
int64_t rt_listbox_was_reorder_requested(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    return lb && vg_listbox_was_reorder_requested(lb) ? 1 : 0;
}

/// @brief Return the most recently latched retained-row source index.
/// @param listbox ListBox widget handle.
/// @return Signed source index, or -1 when unavailable.
int64_t rt_listbox_get_reorder_source_index(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    size_t index = lb ? vg_listbox_get_reorder_source_index(lb) : SIZE_MAX;
    return index == SIZE_MAX || index > (size_t)INT64_MAX ? -1 : (int64_t)index;
}

/// @brief Return the most recently latched retained-row final target index.
/// @param listbox ListBox widget handle.
/// @return Signed target index, or -1 when unavailable.
int64_t rt_listbox_get_reorder_target_index(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    size_t index = lb ? vg_listbox_get_reorder_target_index(lb) : SIZE_MAX;
    return index == SIZE_MAX || index > (size_t)INT64_MAX ? -1 : (int64_t)index;
}

/// @brief Append one selected row's text to a newline-delimited dynamic buffer.
/// @param[in,out] buffer Address of the allocated output buffer.
/// @param[in,out] length Current byte length, updated after the append.
/// @param[in,out] capacity Current allocation capacity, expanded as needed.
/// @param[in,out] first Whether this is the first row; updated to false on success.
/// @param text Row text bytes; may be `NULL` when @p text_len is zero.
/// @param text_len Number of row-text bytes to append.
/// @return `true` on success; `false` for invalid state, overflow, or allocation failure.
static bool rt_listbox_append_selected_text(char **buffer,
                                            size_t *length,
                                            size_t *capacity,
                                            bool *first,
                                            const char *text,
                                            size_t text_len) {
    if (!buffer || !length || !capacity || !first)
        return false;
    size_t prefix = *first ? 0u : 1u;
    if (text_len > SIZE_MAX - *length - prefix)
        return false;
    size_t needed = *length + prefix + text_len;
    if (needed + 1 > *capacity) {
        size_t next_cap = *capacity ? *capacity : 64u;
        while (next_cap < needed + 1) {
            if (next_cap > SIZE_MAX / 2) {
                next_cap = needed + 1;
                break;
            }
            next_cap *= 2u;
        }
        char *next = (char *)realloc(*buffer, next_cap);
        if (!next)
            return false;
        *buffer = next;
        *capacity = next_cap;
    }
    if (!*first)
        (*buffer)[(*length)++] = '\n';
    if (text_len > 0 && text)
        memcpy(*buffer + *length, text, text_len);
    *length += text_len;
    (*buffer)[*length] = '\0';
    *first = false;
    return true;
}

/// @brief Return selected row text joined by newlines.
/// @param listbox ListBox widget handle.
/// @return Owned newline-delimited selected text, or an empty runtime string when none.
rt_string rt_listbox_get_selected_text(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb)
        return rt_str_empty();

    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    bool first = true;

    if (lb->virtual_mode) {
        if (lb->selection_bitmap && lb->data_provider) {
            for (size_t i = 0; i < lb->total_item_count && i < lb->selection_bitmap_size; ++i) {
                if (!lb->selection_bitmap[i])
                    continue;
                const char *text = "";
                lb->data_provider(&lb->base, i, &text, NULL, lb->data_provider_user_data);
                if (!text)
                    text = "";
                if (!rt_listbox_append_selected_text(
                        &buffer, &length, &capacity, &first, text, strlen(text))) {
                    free(buffer);
                    return rt_str_empty();
                }
            }
        }
    } else {
        for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
            if (!item->selected)
                continue;
            if (!rt_listbox_append_selected_text(&buffer,
                                                 &length,
                                                 &capacity,
                                                 &first,
                                                 item->text ? item->text : "",
                                                 item->text ? item->text_len : 0u)) {
                free(buffer);
                return rt_str_empty();
            }
        }
    }

    if (first) {
        free(buffer);
        return rt_str_empty();
    }
    rt_string result = rt_string_from_bytes(buffer ? buffer : "", length);
    free(buffer);
    return result;
}

/// @brief Return byte-exact data for selected retained rows, in row order.
/// @param listbox ListBox widget handle.
/// @return New owned sequence of selected row data; empty for invalid or virtual lists, or `NULL`
///         if sequence allocation fails.
void *rt_listbox_get_selected_data(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    void *result = rt_seq_new_owned();
    if (!result)
        return NULL;

    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb || lb->virtual_mode)
        return result;

    for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
        if (!item->selected)
            continue;
        rt_string data = item->user_data && item->owns_user_data
                             ? rt_gui_string_data_to_rt_string(item->user_data)
                             : rt_str_empty();
        rt_seq_push(result, data);
        rt_str_release_maybe(data);
    }
    return result;
}

/// @brief Check if the listbox selection changed since the last call (edge-triggered).
/// @param listbox ListBox widget handle.
/// @return `1` once after an unreported selection revision; otherwise `0`.
int64_t rt_listbox_was_selection_changed(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (!lb)
        return 0;

    if (lb->selection_revision != lb->reported_selection_revision) {
        lb->reported_selection_revision = lb->selection_revision;
        lb->prev_selected = lb->selected;
        lb->prev_selected_index = lb->selected_index;
        return 1;
    }
    return 0;
}

/// @brief Consume the ListBox's common selection-change edge.
/// @details This edge is independent from the compatibility
///          rt_listbox_was_selection_changed edge and from activation events.
/// @param listbox ListBox widget handle.
/// @return 1 once after one or more unreported selection transitions, otherwise 0.
int64_t rt_listbox_was_changed(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    return lb ? (vg_widget_was_changed(&lb->base) ? 1 : 0) : 0;
}

/// @brief Consume the ListBox's row-activation edge.
/// @details Double-click and Enter activation are recorded separately from
///          selection changes and optional callbacks.
/// @param listbox ListBox widget handle.
/// @return 1 once after one or more unreported activations, otherwise 0.
int64_t rt_listbox_was_activated(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    return lb ? (vg_widget_was_activated(&lb->base) ? 1 : 0) : 0;
}

/// @brief Return the ListBox's non-consuming state revision.
/// @param listbox ListBox widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_listbox_get_revision(void *listbox) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    return lb ? rt_widget_get_revision(&lb->base) : 0;
}

/// @brief Get the display text of a listbox item.
/// @param item Live ListBox item subhandle.
/// @return Owned display text, or an empty runtime string for invalid input.
rt_string rt_listbox_item_get_text(void *item) {
    RT_ASSERT_MAIN_THREAD();
    if (!item)
        return rt_str_empty();
    vg_listbox_item_t *it = rt_gui_listbox_item_from_handle(item);
    if (!it)
        return rt_str_empty();
    if (it->text)
        return rt_string_from_bytes(it->text, it->text_len);
    return rt_str_empty();
}

/// @brief Update the display text of a listbox item.
/// @param item Live ListBox item subhandle.
/// @param text Runtime text copied into item storage.
void rt_listbox_item_set_text(void *item, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    if (!item)
        return;
    vg_listbox_item_t *it = rt_gui_listbox_item_from_handle(item);
    if (!it)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    free(it->text);
    it->text = ctext; // Takes ownership
    it->text_len = strlen(ctext);
    if (it->owner) {
        it->owner->base.needs_layout = true;
        it->owner->base.needs_paint = true;
    }
}

/// @brief Attach arbitrary string data to a listbox item (replaces previous data).
/// @param item Live ListBox item subhandle.
/// @param data Runtime string copied into owned item data, or null to clear it.
void rt_listbox_item_set_data(void *item, rt_string data) {
    RT_ASSERT_MAIN_THREAD();
    if (!item)
        return;
    vg_listbox_item_t *it = rt_gui_listbox_item_from_handle(item);
    if (!it)
        return;
    rt_gui_string_data_t *new_data = data ? rt_gui_string_data_new(data) : NULL;
    if (data && !new_data)
        return;
    if (it->owns_user_data && it->user_data)
        rt_gui_string_data_free_if_owned(it->user_data);
    it->user_data = new_data;
    it->owns_user_data = new_data != NULL;
}

/// @brief Retrieve the string data previously attached to a listbox item.
/// @param item Live ListBox item subhandle.
/// @return Owned copy of attached data, or an empty runtime string when absent or invalid.
rt_string rt_listbox_item_get_data(void *item) {
    RT_ASSERT_MAIN_THREAD();
    if (!item)
        return rt_str_empty();
    vg_listbox_item_t *it = rt_gui_listbox_item_from_handle(item);
    if (!it)
        return rt_str_empty();
    if (!it->user_data || !it->owns_user_data)
        return rt_str_empty();
    return rt_gui_string_data_to_rt_string(it->user_data);
}

/// @brief Override a listbox item's text color.
/// @param item Live ListBox item subhandle.
/// @param color Packed widget-layer text color.
void rt_listbox_item_set_text_color(void *item, int64_t color) {
    RT_ASSERT_MAIN_THREAD();
    if (!item)
        return;
    vg_listbox_item_t *it = rt_gui_listbox_item_from_handle(item);
    if (!it)
        return;
    vg_listbox_item_set_text_color(it, (uint32_t)color);
}

/// @brief Set the font of the listbox.
/// @param listbox ListBox widget handle.
/// @param font Live runtime font handle.
/// @param size Requested point size, sanitized to the supported range.
void rt_listbox_set_font(void *listbox, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_listbox_t *lb = rt_listbox_checked(listbox);
    if (lb) {
        vg_font_t *checked_font = rt_gui_font_handle_checked(font);
        if (!checked_font)
            return;
        vg_listbox_set_font(lb, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
        lb->base.runtime_font_reference = checked_font;
    }
}

//=============================================================================

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: graphics disabled — returns NULL; no dropdown widget is created.
/// @param parent Ignored parent handle.
/// @return Always `NULL`.
void *rt_dropdown_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Add a selectable item to a dropdown list.
/// @param dropdown Ignored Dropdown handle.
/// @param text Ignored item text.
/// @return Always `-1`.
int64_t rt_dropdown_add_item(void *dropdown, rt_string text) {
    (void)dropdown;
    (void)text;
    return -1;
}

/// @brief Remove an item from a dropdown by index.
/// @param dropdown Ignored Dropdown handle.
/// @param index Ignored item index.
void rt_dropdown_remove_item(void *dropdown, int64_t index) {
    (void)dropdown;
    (void)index;
}

/// @brief Remove all items from a dropdown, leaving it empty.
/// @param dropdown Ignored Dropdown handle.
void rt_dropdown_clear(void *dropdown) {
    (void)dropdown;
}

/// @brief Programmatically select a dropdown item by index.
/// @param dropdown Ignored Dropdown handle.
/// @param index Ignored item index.
void rt_dropdown_set_selected(void *dropdown, int64_t index) {
    (void)dropdown;
    (void)index;
}

/// @brief Get the index of the currently selected dropdown item (-1 if none).
/// @param dropdown Ignored Dropdown handle.
/// @return Always `-1`.
int64_t rt_dropdown_get_selected(void *dropdown) {
    (void)dropdown;
    return -1;
}

/// @brief Get the selected text of the dropdown.
/// @param dropdown Ignored Dropdown handle.
/// @return Empty runtime string.
rt_string rt_dropdown_get_selected_text(void *dropdown) {
    (void)dropdown;
    return rt_str_empty();
}

/// @brief Set the placeholder of the dropdown.
/// @param dropdown Ignored Dropdown handle.
/// @param placeholder Ignored placeholder text.
void rt_dropdown_set_placeholder(void *dropdown, rt_string placeholder) {
    (void)dropdown;
    (void)placeholder;
}

/// @brief Stub: no dropdown change edge exists when graphics is disabled.
/// @param dropdown Ignored dropdown handle.
/// @return Always zero.
int64_t rt_dropdown_was_changed(void *dropdown) {
    (void)dropdown;
    return 0;
}

/// @brief Stub: no dropdown revision exists when graphics is disabled.
/// @param dropdown Ignored dropdown handle.
/// @return Always zero.
int64_t rt_dropdown_get_revision(void *dropdown) {
    (void)dropdown;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no slider widget is created.
/// @param parent Ignored parent handle.
/// @param horizontal Ignored orientation flag.
/// @return Always `NULL`.
void *rt_slider_new(void *parent, int64_t horizontal) {
    (void)parent;
    (void)horizontal;
    return NULL;
}

/// @brief Set the value of the slider.
/// @param slider Ignored Slider handle.
/// @param value Ignored numeric value.
void rt_slider_set_value(void *slider, double value) {
    (void)slider;
    (void)value;
}

/// @brief Get the value of the slider.
/// @param slider Ignored Slider handle.
/// @return Always `0.0`.
double rt_slider_get_value(void *slider) {
    (void)slider;
    return 0.0;
}

/// @brief Set the range of the slider.
/// @param slider Ignored Slider handle.
/// @param min_val Ignored lower endpoint.
/// @param max_val Ignored upper endpoint.
void rt_slider_set_range(void *slider, double min_val, double max_val) {
    (void)slider;
    (void)min_val;
    (void)max_val;
}

/// @brief Set the step of the slider.
/// @param slider Ignored Slider handle.
/// @param step Ignored step size.
void rt_slider_set_step(void *slider, double step) {
    (void)slider;
    (void)step;
}

/// @brief Stub: no slider change edge exists when graphics is disabled.
/// @param slider Ignored slider handle.
/// @return Always zero.
int64_t rt_slider_was_changed(void *slider) {
    (void)slider;
    return 0;
}

/// @brief Stub: no slider revision exists when graphics is disabled.
/// @param slider Ignored slider handle.
/// @return Always zero.
int64_t rt_slider_get_revision(void *slider) {
    (void)slider;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no progress bar widget is created.
/// @param parent Ignored parent handle.
/// @return Always `NULL`.
void *rt_progressbar_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Set the value of the progressbar.
/// @param progress Ignored ProgressBar handle.
/// @param value Ignored fill fraction.
void rt_progressbar_set_value(void *progress, double value) {
    (void)progress;
    (void)value;
}

/// @brief Get the value of the progressbar.
/// @param progress Ignored ProgressBar handle.
/// @return Always `0.0`.
double rt_progressbar_get_value(void *progress) {
    (void)progress;
    return 0.0;
}

/// @brief Stub: graphics disabled — returns NULL; no list box widget is created.
/// @param parent Ignored parent handle.
/// @return Always `NULL`.
void *rt_listbox_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no list item is added.
/// @param listbox Ignored ListBox handle.
/// @param text Ignored row text.
/// @return Always `NULL`.
void *rt_listbox_add_item(void *listbox, rt_string text) {
    (void)listbox;
    (void)text;
    return NULL;
}

/// @brief Remove an item from the listbox.
/// @param listbox Ignored ListBox handle.
/// @param item Ignored item subhandle.
void rt_listbox_remove_item(void *listbox, void *item) {
    (void)listbox;
    (void)item;
}

/// @brief Remove all entries from the listbox.
/// @param listbox Ignored ListBox handle.
void rt_listbox_clear(void *listbox) {
    (void)listbox;
}

/// @brief Programmatically select a listbox item by handle.
/// @param listbox Ignored ListBox handle.
/// @param item Ignored item subhandle.
void rt_listbox_select(void *listbox, void *item) {
    (void)listbox;
    (void)item;
}

/// @brief Stub: graphics disabled — returns NULL; no selection exists without a list box.
/// @param listbox Ignored ListBox handle.
/// @return Always `NULL`.
void *rt_listbox_get_selected(void *listbox) {
    (void)listbox;
    return NULL;
}

/// @brief Get the number of items in the listbox.
/// @param listbox Ignored ListBox handle.
/// @return Always `0`.
int64_t rt_listbox_get_count(void *listbox) {
    (void)listbox;
    return 0;
}

/// @brief Get the selected index of the listbox.
/// @param listbox Ignored ListBox handle.
/// @return Always `-1`.
int64_t rt_listbox_get_selected_index(void *listbox) {
    (void)listbox;
    return -1;
}

/// @brief Select a listbox item by its zero-based index.
/// @param listbox Ignored ListBox handle.
/// @param index Ignored row index.
void rt_listbox_select_index(void *listbox, int64_t index) {
    (void)listbox;
    (void)index;
}

/// @brief Stub: graphics disabled — no listbox scroll state exists.
/// @param listbox Ignored ListBox handle.
void rt_listbox_scroll_to_top(void *listbox) {
    (void)listbox;
}

/// @brief Stub: graphics disabled — no listbox scroll state exists.
/// @param listbox Ignored ListBox handle.
void rt_listbox_scroll_to_bottom(void *listbox) {
    (void)listbox;
}

/// @brief Stub: graphics disabled — no listbox selection exists.
/// @param listbox Ignored ListBox handle.
/// @param enabled Ignored multi-selection flag.
void rt_listbox_set_multi_select(void *listbox, int64_t enabled) {
    (void)listbox;
    (void)enabled;
}

/// @brief Stub: graphics disabled — no retained-row reorder gesture exists.
/// @param listbox Ignored ListBox handle.
/// @param enabled Ignored reorderable flag.
void rt_listbox_set_reorderable(void *listbox, int64_t enabled) {
    (void)listbox;
    (void)enabled;
}

/// @brief Stub: graphics disabled — no reorder request edge exists.
/// @param listbox Ignored ListBox handle.
/// @return Always zero.
int64_t rt_listbox_was_reorder_requested(void *listbox) {
    (void)listbox;
    return 0;
}

/// @brief Stub: graphics disabled — no reorder source exists.
/// @param listbox Ignored ListBox handle.
/// @return Always -1.
int64_t rt_listbox_get_reorder_source_index(void *listbox) {
    (void)listbox;
    return -1;
}

/// @brief Stub: graphics disabled — no reorder target exists.
/// @param listbox Ignored ListBox handle.
/// @return Always -1.
int64_t rt_listbox_get_reorder_target_index(void *listbox) {
    (void)listbox;
    return -1;
}

/// @brief Stub: graphics disabled — no selected row text exists.
/// @param listbox Ignored ListBox handle.
/// @return Empty runtime string.
rt_string rt_listbox_get_selected_text(void *listbox) {
    (void)listbox;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — no selected retained-row data exists.
/// @param listbox Ignored ListBox handle.
/// @return New empty owned sequence, or `NULL` if sequence allocation fails.
void *rt_listbox_get_selected_data(void *listbox) {
    (void)listbox;
    return rt_seq_new_owned();
}

/// @brief Check if the listbox selection changed since the last call (edge-triggered).
/// @param listbox Ignored ListBox handle.
/// @return Always `0`.
int64_t rt_listbox_was_selection_changed(void *listbox) {
    (void)listbox;
    return 0;
}

/// @brief Stub: no ListBox change edge exists when graphics is disabled.
/// @param listbox Ignored ListBox handle.
/// @return Always zero.
int64_t rt_listbox_was_changed(void *listbox) {
    (void)listbox;
    return 0;
}

/// @brief Stub: no ListBox activation edge exists when graphics is disabled.
/// @param listbox Ignored ListBox handle.
/// @return Always zero.
int64_t rt_listbox_was_activated(void *listbox) {
    (void)listbox;
    return 0;
}

/// @brief Stub: no ListBox revision exists when graphics is disabled.
/// @param listbox Ignored ListBox handle.
/// @return Always zero.
int64_t rt_listbox_get_revision(void *listbox) {
    (void)listbox;
    return 0;
}

/// @brief Get the display text of a listbox item.
/// @param item Ignored item subhandle.
/// @return Empty runtime string.
rt_string rt_listbox_item_get_text(void *item) {
    (void)item;
    return rt_str_empty();
}

/// @brief Update the display text of a listbox item.
/// @param item Ignored item subhandle.
/// @param text Ignored display text.
void rt_listbox_item_set_text(void *item, rt_string text) {
    (void)item;
    (void)text;
}

/// @brief Attach arbitrary string data to a listbox item (replaces previous data).
/// @param item Ignored item subhandle.
/// @param data Ignored row data.
void rt_listbox_item_set_data(void *item, rt_string data) {
    (void)item;
    (void)data;
}

/// @brief Retrieve the string data previously attached to a listbox item.
/// @param item Ignored item subhandle.
/// @return Empty runtime string.
rt_string rt_listbox_item_get_data(void *item) {
    (void)item;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — no listbox item exists.
/// @param item Ignored item subhandle.
/// @param color Ignored packed color.
void rt_listbox_item_set_text_color(void *item, int64_t color) {
    (void)item;
    (void)color;
}

/// @brief Set the font of the listbox.
/// @param listbox Ignored ListBox handle.
/// @param font Ignored font handle.
/// @param size Ignored point size.
void rt_listbox_set_font(void *listbox, void *font, double size) {
    (void)listbox;
    (void)font;
    (void)size;
}


#endif /* ZANNA_ENABLE_GRAPHICS */
