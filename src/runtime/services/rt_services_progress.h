//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_progress.h
// Purpose: Public C ABI for the provider-neutral player progress classes of
//          Zanna.Services: Achievements, Stats, and Leaderboards, plus the
//          LeaderboardScope, LeaderboardSort, and LeaderboardDisplay constants.
// Key invariants:
//   - Stateful entry points must run on the main thread.
//   - Arguments that are malformed for every provider (empty identifiers,
//     unknown constant values, impossible ranges) trap even without a started
//     provider. Provider-specific limits return false or fail the request and
//     record a diagnostic instead.
//   - Without a provider that supports the feature every query returns a
//     neutral value and every request completes as failed.
//   - Constant ordinals are stable public values documented in ADR 0353.
// Ownership/Lifetime:
//   - String results are new caller-owned references.
//   - Leaderboard methods return caller-owned Zanna.Services.Request objects.
// Links: src/runtime/services/rt_services_progress.c,
//        src/runtime/services/rt_services.h,
//        docs/zannalib/services.md,
//        docs/adr/0353-platform-services-player-features.md,
//        docs/adr/0364-platform-services-achievement-icons-and-percentages.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_progress.h
 * @brief Declares Zanna.Services.Achievements, Stats, and Leaderboards.
 * @details Achievements and stats change locally and are committed together by
 *          @ref rt_services_stats_store, which also shows unlock notifications.
 *          Leaderboards are addressed by name; each operation is a
 *          non-blocking Zanna.Services.Request.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Stable constants
//===----------------------------------------------------------------------===//

/// @brief Download entries by absolute global rank (start and end are ranks from 1).
#define RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL INT64_C(0)
/// @brief Download entries around the user's rank (start and end are offsets, for example -4..5).
#define RT_SERVICES_LEADERBOARD_SCOPE_AROUND_USER INT64_C(1)
/// @brief Download the entries of the user and their friends (start and end are ignored).
#define RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS INT64_C(2)

/// @brief Lower scores rank higher.
#define RT_SERVICES_LEADERBOARD_SORT_ASCENDING INT64_C(1)
/// @brief Higher scores rank higher.
#define RT_SERVICES_LEADERBOARD_SORT_DESCENDING INT64_C(2)

/// @brief Scores are plain numbers.
#define RT_SERVICES_LEADERBOARD_DISPLAY_NUMERIC INT64_C(1)
/// @brief Scores are times in seconds.
#define RT_SERVICES_LEADERBOARD_DISPLAY_SECONDS INT64_C(2)
/// @brief Scores are times in milliseconds.
#define RT_SERVICES_LEADERBOARD_DISPLAY_MILLISECONDS INT64_C(3)

//===----------------------------------------------------------------------===//
// Zanna.Services.Achievements
//===----------------------------------------------------------------------===//

/// @brief Unlock an achievement locally.
/// @details The unlock is committed and its notification shown by Stats.Store.
/// @param id Achievement API id; empty traps.
/// @return 1 when the provider accepted the unlock, otherwise 0.
int8_t rt_services_achievements_unlock(rt_string id);

/// @brief Lock an achievement again (intended for testing).
/// @param id Achievement API id; empty traps.
/// @return 1 when the provider accepted the change, otherwise 0.
int8_t rt_services_achievements_clear(rt_string id);

/// @brief Report whether an achievement is unlocked.
/// @param id Achievement API id; empty traps.
/// @return 1 when unlocked, otherwise 0 (including when unavailable).
int8_t rt_services_achievements_is_unlocked(rt_string id);

/// @brief Read when an achievement was unlocked.
/// @param id Achievement API id; empty traps.
/// @return Unlock time in seconds since the Unix epoch, or 0 when locked or unknown.
int64_t rt_services_achievements_unlock_time(rt_string id);

/// @brief Show a progress notification for a locked achievement.
/// @details Reports progress only: it neither stores a stat nor unlocks the
///          achievement.
/// @param id Achievement API id; empty traps.
/// @param current Progress so far; must satisfy 0 <= current <= maximum or it traps.
/// @param maximum Progress needed to unlock; must be positive or it traps.
/// @return 1 when the provider showed the notification, otherwise 0.
int8_t rt_services_achievements_indicate_progress(rt_string id, int64_t current, int64_t maximum);

/// @brief Read how many achievements the app defines.
/// @return Achievement count, or 0 when unavailable.
int64_t rt_services_achievements_get_count(void);

/// @brief Read the API id of the achievement at @p index.
/// @param index Achievement index; values outside 0..Count-1 return the empty string.
/// @return Caller-owned achievement id, or the empty string.
rt_string rt_services_achievements_id_at(int64_t index);

/// @brief Read an achievement's localized display name.
/// @param id Achievement API id; empty traps.
/// @return Caller-owned display name, or the empty string when unavailable.
rt_string rt_services_achievements_display_name(rt_string id);

/// @brief Read an achievement's localized description.
/// @param id Achievement API id; empty traps.
/// @return Caller-owned description, or the empty string when unavailable.
rt_string rt_services_achievements_description(rt_string id);

/// @brief Report whether an achievement is hidden until unlocked.
/// @param id Achievement API id; empty traps.
/// @return 1 when hidden, otherwise 0.
int8_t rt_services_achievements_is_hidden(rt_string id);

/// @brief Read the width of an achievement's icon for its current state.
/// @details Platforms load icons on demand: while an icon loads this returns 0
///          and EventKind.AchievementIconReady reports when to ask again.
/// @param id Achievement API id; empty traps.
/// @return Width in pixels, or 0 while loading or unavailable.
int64_t rt_services_achievements_icon_width(rt_string id);

/// @brief Read the height of an achievement's icon for its current state.
/// @param id Achievement API id; empty traps.
/// @return Height in pixels, or 0 while loading or unavailable.
int64_t rt_services_achievements_icon_height(rt_string id);

/// @brief Read an achievement's icon for its current state as RGBA bytes.
/// @details The bytes hold IconWidth*IconHeight*4 values in row order, ready
///          for Zanna.Graphics.Pixels.FromBytes.
/// @param id Achievement API id; empty traps.
/// @return Caller-owned Zanna.Collections.Bytes; empty while loading or unavailable.
void *rt_services_achievements_icon_rgba(rt_string id);

/// @brief Ask the platform for every achievement's global unlock percentage.
/// @return Caller-owned Zanna.Services.Request of kind AchievementPercentages.
void *rt_services_achievements_request_global_percentages(void);

/// @brief Read the share of players who unlocked an achievement.
/// @param id Achievement API id; empty traps.
/// @return Percentage in 0..100, or 0 before RequestGlobalPercentages succeeded.
double rt_services_achievements_global_percent(rt_string id);

//===----------------------------------------------------------------------===//
// Zanna.Services.Stats
//===----------------------------------------------------------------------===//

/// @brief Read an integer stat.
/// @param name Stat API name; empty traps.
/// @return Stat value, or 0 when unavailable.
int64_t rt_services_stats_get_int(rt_string name);

/// @brief Set an integer stat locally; Stats.Store commits it.
/// @param name Stat API name; empty traps.
/// @param value New value; providers may limit the range (Steam: int32).
/// @return 1 when the provider accepted the value, otherwise 0.
int8_t rt_services_stats_set_int(rt_string name, int64_t value);

/// @brief Read a floating-point stat.
/// @param name Stat API name; empty traps.
/// @return Stat value, or 0.0 when unavailable.
double rt_services_stats_get_float(rt_string name);

/// @brief Set a floating-point stat locally; Stats.Store commits it.
/// @param name Stat API name; empty traps.
/// @param value New finite value; providers may limit the range (Steam: float32).
/// @return 1 when the provider accepted the value, otherwise 0.
int8_t rt_services_stats_set_float(rt_string name, double value);

/// @brief Add a session's contribution to an average-rate stat.
/// @param name Stat API name; empty traps.
/// @param count Amount accumulated during the session.
/// @param seconds Session length in seconds; must be positive and finite or it traps.
/// @return 1 when the provider accepted the update, otherwise 0.
int8_t rt_services_stats_update_average_rate(rt_string name, double count, double seconds);

/// @brief Commit changed stats and achievements to the platform.
/// @details Completion arrives as EventKind.StatsStored, followed by one
///          EventKind.AchievementStored per newly unlocked achievement.
///          Platforms rate-limit this call; commit at natural break points.
/// @return 1 when the commit was started, otherwise 0.
int8_t rt_services_stats_store(void);

/// @brief Reset every stat, and optionally every achievement (intended for testing).
/// @param include_achievements Nonzero to lock every achievement as well.
/// @return 1 when the provider accepted the reset, otherwise 0.
int8_t rt_services_stats_reset_all(int8_t include_achievements);

//===----------------------------------------------------------------------===//
// Zanna.Services.Leaderboards
//===----------------------------------------------------------------------===//

/// @brief Start looking up a leaderboard by name.
/// @details The completed request reports the board's total entry count in
///          Value and its name in Text; a missing board fails the request.
/// @param name Leaderboard name; empty traps.
/// @return Caller-owned Zanna.Services.Request of kind LeaderboardFind.
void *rt_services_leaderboards_find(rt_string name);

/// @brief Start looking up a leaderboard, creating it when it does not exist.
/// @param name Leaderboard name; empty traps.
/// @param sort LeaderboardSort value used when creating; other values trap.
/// @param display LeaderboardDisplay value used when creating; other values trap.
/// @return Caller-owned Zanna.Services.Request of kind LeaderboardFind.
void *rt_services_leaderboards_find_or_create(rt_string name, int64_t sort, int64_t display);

/// @brief Start uploading a score for the signed-in user.
/// @details The completed request reports the user's new global rank in Value
///          and whether the stored score changed in Flag.
/// @param name Leaderboard name; empty traps.
/// @param score Score to upload; providers may limit the range (Steam: int32).
/// @param keep_best Nonzero to keep the user's better existing score.
/// @return Caller-owned Zanna.Services.Request of kind LeaderboardUpload.
void *rt_services_leaderboards_upload(rt_string name, int64_t score, int8_t keep_best);

/// @brief Start downloading a range of leaderboard entries.
/// @details Global ranges must satisfy 1 <= start <= end, AroundUser ranges
///          start <= end, and both may span at most
///          RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY entries; violations trap.
///          Friends ignores the range and keeps at most that many entries.
/// @param name Leaderboard name; empty traps.
/// @param scope LeaderboardScope value; other values trap.
/// @param start First rank (Global) or offset from the user (AroundUser).
/// @param end Last rank (Global) or offset from the user (AroundUser).
/// @return Caller-owned Zanna.Services.Request of kind LeaderboardDownload.
void *rt_services_leaderboards_download(rt_string name, int64_t scope, int64_t start, int64_t end);

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return `Zanna.Services.LeaderboardScope.Global`.
/// @return Stable ordinal 0.
int64_t rt_services_leaderboard_scope_global(void);
/// @brief Return `Zanna.Services.LeaderboardScope.AroundUser`.
/// @return Stable ordinal 1.
int64_t rt_services_leaderboard_scope_around_user(void);
/// @brief Return `Zanna.Services.LeaderboardScope.Friends`.
/// @return Stable ordinal 2.
int64_t rt_services_leaderboard_scope_friends(void);

/// @brief Return `Zanna.Services.LeaderboardSort.Ascending`.
/// @return Stable ordinal 1.
int64_t rt_services_leaderboard_sort_ascending(void);
/// @brief Return `Zanna.Services.LeaderboardSort.Descending`.
/// @return Stable ordinal 2.
int64_t rt_services_leaderboard_sort_descending(void);

/// @brief Return `Zanna.Services.LeaderboardDisplay.Numeric`.
/// @return Stable ordinal 1.
int64_t rt_services_leaderboard_display_numeric(void);
/// @brief Return `Zanna.Services.LeaderboardDisplay.Seconds`.
/// @return Stable ordinal 2.
int64_t rt_services_leaderboard_display_seconds(void);
/// @brief Return `Zanna.Services.LeaderboardDisplay.Milliseconds`.
/// @return Stable ordinal 3.
int64_t rt_services_leaderboard_display_milliseconds(void);

#ifdef __cplusplus
}
#endif
