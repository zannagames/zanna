//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_achievement.h
// Purpose: Achievement tracking system with bitmask unlocks, stat counters,
//   and timed notification popups.
//
// Key invariants:
//   - Capacity is 1-64; invalid constructor requests select 64 slots.
//   - Max 32 stat counters.
//   - Notification display lasts configurable ms (default 3000).
//   - Unlock masks round-trip through int64_t but preserve all 64 bits and mask
//     away bits above configured capacity when restored.
//
// Ownership/Lifetime:
//   - The tracker is a reference-counted runtime object with a finalizer.
//   - Add retains nullable name/description strings until replacement or finalization.
//   - Canvas and all query/update inputs are borrowed.
//
// Links: src/runtime/game/rt_achievement.c
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Achievement definitions, unlock/stat persistence, and notifications.
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime object class identifier for AchievementTracker instances.
#define RT_ACHIEVEMENT_CLASS_ID INT64_C(-0x51021A)

/// @brief Opaque nullable handle to a native AchievementTracker object.
typedef struct rt_achievement_impl *rt_achievement;

/// @brief Create an achievement tracker sized for at most @p max_achievements (max 64).
/// @param max_achievements Requested slot count; values outside 1–64 select 64.
/// @return Owned tracker with one object reference, or NULL on allocation.
rt_achievement rt_achievement_new(int64_t max_achievements);
/// @brief Release one tracker reference and finalize it when the count reaches zero.
/// @param ach Owned tracker reference; NULL is a no-op.
void rt_achievement_destroy(rt_achievement ach);

// Define achievements
/// @brief Define an achievement with a unique @p id (0..max_achievements-1) and display strings.
/// @details The tracker retains the provided runtime strings and releases them on replacement or
///          destruction. Redefinition does not increment the total-defined count
///          or clear an existing unlock bit.
/// @param ach Borrowed tracker.
/// @param id Zero-based configured slot.
/// @param name Borrowed display name; may be NULL.
/// @param description Borrowed description; may be NULL.
void rt_achievement_add(rt_achievement ach, int64_t id, rt_string name, rt_string description);

// Unlock / query
/// @brief Unlock a defined achievement and start/replace the notification.
/// @param ach Borrowed tracker.
/// @param id Zero-based configured slot.
/// @return One only when the bit changes from locked to unlocked.
int8_t rt_achievement_unlock(rt_achievement ach, int64_t id);
/// @brief True if the achievement with @p id has been unlocked.
/// @param ach Borrowed tracker.
/// @param id Zero-based configured slot.
/// @return One when its bit is set; otherwise zero.
int8_t rt_achievement_is_unlocked(rt_achievement ach, int64_t id);
/// @brief Get the entire unlock state as a bitmask (suitable for save/load).
/// @param ach Borrowed tracker.
/// @return Signed representation of the 64-bit mask, or zero on invalid input.
int64_t rt_achievement_get_mask(rt_achievement ach);
/// @brief Restore the unlock state from a bitmask (e.g., on game load).
/// @details Clears out-of-capacity bits and dismisses an active notification if
///          its achievement is no longer unlocked.
/// @param ach Borrowed tracker.
/// @param mask Serialized 64-bit bit pattern.
void rt_achievement_set_mask(rt_achievement ach, int64_t mask);
/// @brief Count how many achievements are currently unlocked.
/// @param ach Borrowed tracker.
/// @return Population count within configured capacity.
int64_t rt_achievement_unlocked_count(rt_achievement ach);
/// @brief Count of all defined achievements.
/// @param ach Borrowed tracker.
/// @return Number of slots defined at least once, or zero on invalid input.
int64_t rt_achievement_total_count(rt_achievement ach);

// Stat tracking
/// @brief Increment a per-game stat counter (e.g., enemies_killed) by @p amount.
/// @details Signed overflow saturates at `INT64_MIN` or `INT64_MAX`.
/// @param ach Borrowed tracker.
/// @param stat_id Zero-based stat slot in range 0–31.
/// @param amount Signed increment.
void rt_achievement_inc_stat(rt_achievement ach, int64_t stat_id, int64_t amount);
/// @brief Read the current value of a stat counter.
/// @param ach Borrowed tracker.
/// @param stat_id Zero-based stat slot in range 0–31.
/// @return Stored value, or zero for invalid input.
int64_t rt_achievement_get_stat(rt_achievement ach, int64_t stat_id);
/// @brief Set a stat counter directly (typically used to restore from save).
/// @param ach Borrowed tracker.
/// @param stat_id Zero-based stat slot in range 0–31.
/// @param value New signed value.
void rt_achievement_set_stat(rt_achievement ach, int64_t stat_id, int64_t value);

// Notification
/// @brief Tick the notification timer by @p dt milliseconds (call once per frame).
/// @details Positive deltas also reduce the 300-unit slide offset and dismiss
///          the notification at timer expiry.
/// @param ach Borrowed tracker.
/// @param dt Positive elapsed milliseconds; non-positive values are ignored.
void rt_achievement_update(rt_achievement ach, int64_t dt);
/// @brief Render the active notification popup (no-op when none is showing).
/// @details Draws a fixed top-right panel, title, and optional achievement name
///          only when @p canvas is a registered Canvas handle.
/// @param ach Borrowed tracker.
/// @param canvas Borrowed Canvas handle.
void rt_achievement_draw(rt_achievement ach, void *canvas);
/// @brief Set how long each notification stays on screen in milliseconds (default 3000).
/// @details Only positive values are accepted and the active timer is unchanged.
/// @param ach Borrowed tracker.
/// @param ms Duration applied to future unlock notifications.
void rt_achievement_set_notify_duration(rt_achievement ach, int64_t ms);
/// @brief True if a notification popup is currently being displayed.
/// @param ach Borrowed tracker.
/// @return One when an active notification ID exists; otherwise zero.
int8_t rt_achievement_has_notification(rt_achievement ach);

#ifdef __cplusplus
}
#endif
