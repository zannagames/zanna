//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_buttongroup.h
// Purpose: Button group manager for mutually exclusive selections (radio-button semantics),
// tracking which button in a group is currently selected and notifying on changes.
//
// Key invariants:
//   - At most one button is selected at any time; selecting a new button deselects the previous.
//   - Button IDs within a group are unique; Add returns false for duplicates
//     even when the group is full.
//   - Maximum group size is RT_BUTTONGROUP_MAX (256) buttons.
//   - An empty group has no selected button. Selected() returns -1 for no selection,
//     so use HasSelection when -1 is also a registered button ID.
//
// Ownership/Lifetime:
//   - ButtonGroup handles are reference-counted GC objects. IDs are stored in
//     an inline array; rt_buttongroup_destroy releases the caller's reference.
//
// Links: src/runtime/game/rt_buttongroup.c (implementation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Fixed-capacity mutually exclusive button-ID selection API.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for ButtonGroup objects.
#define RT_BUTTONGROUP_CLASS_ID INT64_C(-0x51020A)

/// @brief Maximum number of distinct IDs in one group.
#define RT_BUTTONGROUP_MAX 256
/// @brief String form of @ref RT_BUTTONGROUP_MAX for diagnostics.
#define RT_BUTTONGROUP_MAX_STR "256"

/// @brief Opaque nullable handle to a ButtonGroup instance.
typedef struct rt_buttongroup_impl *rt_buttongroup;

/// @brief Create a new ButtonGroup.
/// @return Owned empty ButtonGroup with no selection, or NULL on allocation.
rt_buttongroup rt_buttongroup_new(void);

/// @brief Destroy a ButtonGroup and free its memory.
/// @details Releases one object reference and frees only at zero.
/// @param group Owned group reference; NULL is a no-op.
void rt_buttongroup_destroy(rt_buttongroup group);

/// @brief Add a button to the group.
/// @param group The button group.
/// @param button_id Unique identifier for the button.
/// @return 1 on success, 0 if @p button_id already exists.
/// @note A genuinely new ID traps if the group already contains
///       RT_BUTTONGROUP_MAX buttons; a duplicate still returns zero.
int8_t rt_buttongroup_add(rt_buttongroup group, int64_t button_id);

/// @brief Remove a button from the group.
/// @param group The button group.
/// @param button_id The button to remove. If this button is currently
///                  selected, the selection is cleared.
/// @return 1 on success, 0 if @p button_id does not exist in the group.
int8_t rt_buttongroup_remove(rt_buttongroup group, int64_t button_id);

/// @brief Check if a button exists in the group.
/// @param group The button group.
/// @param button_id The button to check.
/// @return 1 if the button exists in the group, 0 otherwise.
int8_t rt_buttongroup_has(rt_buttongroup group, int64_t button_id);

/// @brief Get the number of buttons in the group.
/// @param group The button group.
/// @return Number of buttons currently in the group.
int64_t rt_buttongroup_count(rt_buttongroup group);

/// @brief Select a button (deselects all others).
/// @details Selecting the current ID succeeds without setting the changed
///          latch; selecting a different registered ID sets it.
/// @param group The button group.
/// @param button_id The button to select.
/// @return 1 on success, 0 if @p button_id does not exist in the group.
int8_t rt_buttongroup_select(rt_buttongroup group, int64_t button_id);

/// @brief Deselect all buttons (clear the selection).
/// @details Sets the changed latch only when a selection existed.
/// @param group The button group.
void rt_buttongroup_clear_selection(rt_buttongroup group);

/// @brief Get the currently selected button.
/// @param group The button group.
/// @return Selected button ID, or -1 if no button is selected. Use HasSelection to
///         distinguish "none" from a selected button whose ID is -1.
int64_t rt_buttongroup_selected(rt_buttongroup group);

/// @brief Check if a specific button is the currently selected one.
/// @param group The button group.
/// @param button_id The button to check.
/// @return 1 if @p button_id is the selected button, 0 otherwise.
int8_t rt_buttongroup_is_selected(rt_buttongroup group, int64_t button_id);

/// @brief Check if any button is selected.
/// @param group The button group.
/// @return 1 if any button is selected, 0 if the selection is empty.
int8_t rt_buttongroup_has_selection(rt_buttongroup group);

/// @brief Check if the selection changed since the last clear operation.
/// @details Reading does not consume the flag. Use
///          rt_buttongroup_clear_changed_flag() to reset it explicitly.
/// @param group The button group.
/// @return 1 if the selection just changed, 0 otherwise.
int8_t rt_buttongroup_selection_changed(rt_buttongroup group);

/// @brief Clear the selection-changed flag (call at end of frame).
/// @param group The button group.
void rt_buttongroup_clear_changed_flag(rt_buttongroup group);

/// @brief Get the button ID at a specific index (for iteration).
/// @param group The button group.
/// @param index Zero-based index (0 to count-1).
/// @return Button ID at @p index, or -1 if out of range.
/// @note Use bounds/count when -1 is also a registered ID.
int64_t rt_buttongroup_get_at(rt_buttongroup group, int64_t index);

/// @brief Select the next button in the group (wraps around).
/// @details With no selection, chooses the first insertion-order ID.
/// @param group The button group.
/// @return The newly selected button ID, or -1 if the group is empty.
int64_t rt_buttongroup_select_next(rt_buttongroup group);

/// @brief Select the previous button in the group (wraps around).
/// @details With no selection, chooses the last insertion-order ID.
/// @param group The button group.
/// @return The newly selected button ID, or -1 if the group is empty.
int64_t rt_buttongroup_select_prev(rt_buttongroup group);

#ifdef __cplusplus
}
#endif
