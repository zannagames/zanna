//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_user_stats.c
// Purpose: Binds ISteamUserStats achievements, stats, and leaderboards for the
//          Steam provider: the Zanna.Services operation tables, the stats
//          callbacks, and the multi-call leaderboard request state machine.
// Key invariants:
//   - Each feature group is usable only when every export it calls resolved
//     (see rt_services_steam_bind_user_stats); otherwise it reports neutral
//     values and one diagnostic names the first missing export.
//   - Values outside Steam's ranges (int32 stats and scores, float stats,
//     uint32 progress) are rejected with a diagnostic before calling Steam.
//   - Leaderboards are addressed by name. A name resolves to a
//     SteamLeaderboard_t through FindLeaderboard once per session and is
//     cached; uploads and downloads for an uncached name run the find first
//     under the same request.
//   - Downloaded entries are read immediately, because Steam frees them once
//     every entry was fetched; at most RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY
//     are kept.
// Ownership/Lifetime:
//   - Strings returned through the operation tables are new references.
//   - The leaderboard cache lives until the provider stops.
// Links: src/runtime/services/steam/rt_steam_internal.h,
//        src/runtime/services/rt_services_progress.h,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_user_stats.c
 * @brief Implements Steam achievements, stats, and leaderboards.
 */

#include "rt_platform.h"
#include "rt_services.h"
#include "rt_services_progress.h"
#include "rt_services_provider.h"
#include "rt_steam_abi.h"
#include "rt_steam_internal.h"
#include "rt_string.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Display name Steam reports for a user it has no information about.
#define STEAM_UNKNOWN_PERSONA "[unknown]"

//===----------------------------------------------------------------------===//
// Binding
//===----------------------------------------------------------------------===//

/// @brief Resolve one export into a typed field, remembering the first missing name.
#define STEAM_BIND(field, type, name, missing)                                                     \
    do {                                                                                           \
        void *symbol_ = rt_services_steam_symbol(name);                                            \
        if (!symbol_ && !(missing))                                                                \
            (missing) = (name);                                                                    \
        (field) = symbol_ ? RT_FN_PTR_CAST((type)symbol_) : NULL;                                  \
    } while (0)

/// @brief Bind the achievement, stat, and leaderboard groups of an open ISteamUserStats.
void rt_services_steam_bind_user_stats(void) {
    steam_user_stats_api *api = &g_steam.user_stats;
    if (!api->self)
        return;

    const char *missing = NULL;
    STEAM_BIND(api->set_achievement,
               rt_steam_self_str_bool_fn,
               RT_STEAM_SYMBOL_USER_STATS_SET_ACHIEVEMENT,
               missing);
    STEAM_BIND(api->clear_achievement,
               rt_steam_self_str_bool_fn,
               RT_STEAM_SYMBOL_USER_STATS_CLEAR_ACHIEVEMENT,
               missing);
    STEAM_BIND(api->get_achievement,
               rt_steam_get_achievement_fn,
               RT_STEAM_SYMBOL_USER_STATS_GET_ACHIEVEMENT_TIME,
               missing);
    STEAM_BIND(api->indicate_progress,
               rt_steam_indicate_progress_fn,
               RT_STEAM_SYMBOL_USER_STATS_INDICATE_PROGRESS,
               missing);
    STEAM_BIND(api->num_achievements,
               rt_steam_self_u32_fn,
               RT_STEAM_SYMBOL_USER_STATS_NUM_ACHIEVEMENTS,
               missing);
    STEAM_BIND(api->achievement_name,
               rt_steam_achievement_name_fn,
               RT_STEAM_SYMBOL_USER_STATS_ACHIEVEMENT_NAME,
               missing);
    STEAM_BIND(api->achievement_attribute,
               rt_steam_achievement_attribute_fn,
               RT_STEAM_SYMBOL_USER_STATS_ACHIEVEMENT_ATTRIBUTE,
               missing);
    api->achievements_ready = missing == NULL;
    if (missing)
        rt_services_steam_report_missing(missing, "achievements");

    missing = NULL;
    STEAM_BIND(api->get_stat_int,
               rt_steam_get_stat_int_fn,
               RT_STEAM_SYMBOL_USER_STATS_GET_STAT_INT,
               missing);
    STEAM_BIND(api->set_stat_int,
               rt_steam_set_stat_int_fn,
               RT_STEAM_SYMBOL_USER_STATS_SET_STAT_INT,
               missing);
    STEAM_BIND(api->get_stat_float,
               rt_steam_get_stat_float_fn,
               RT_STEAM_SYMBOL_USER_STATS_GET_STAT_FLOAT,
               missing);
    STEAM_BIND(api->set_stat_float,
               rt_steam_set_stat_float_fn,
               RT_STEAM_SYMBOL_USER_STATS_SET_STAT_FLOAT,
               missing);
    STEAM_BIND(api->update_avg_rate,
               rt_steam_update_avg_rate_fn,
               RT_STEAM_SYMBOL_USER_STATS_UPDATE_AVG_RATE,
               missing);
    STEAM_BIND(api->store_stats, rt_steam_self_bool_fn, RT_STEAM_SYMBOL_USER_STATS_STORE, missing);
    STEAM_BIND(api->reset_all_stats,
               rt_steam_reset_all_stats_fn,
               RT_STEAM_SYMBOL_USER_STATS_RESET_ALL,
               missing);
    api->stats_ready = missing == NULL;
    if (missing)
        rt_services_steam_report_missing(missing, "stats");

    missing = NULL;
    STEAM_BIND(api->find_leaderboard,
               rt_steam_find_leaderboard_fn,
               RT_STEAM_SYMBOL_USER_STATS_FIND_LEADERBOARD,
               missing);
    STEAM_BIND(api->find_or_create,
               rt_steam_find_or_create_leaderboard_fn,
               RT_STEAM_SYMBOL_USER_STATS_FIND_OR_CREATE_LEADERBOARD,
               missing);
    STEAM_BIND(api->leaderboard_name,
               rt_steam_leaderboard_name_fn,
               RT_STEAM_SYMBOL_USER_STATS_LEADERBOARD_NAME,
               missing);
    STEAM_BIND(api->leaderboard_entry_count,
               rt_steam_leaderboard_entry_count_fn,
               RT_STEAM_SYMBOL_USER_STATS_LEADERBOARD_ENTRY_COUNT,
               missing);
    STEAM_BIND(api->download_entries,
               rt_steam_download_entries_fn,
               RT_STEAM_SYMBOL_USER_STATS_DOWNLOAD_ENTRIES,
               missing);
    STEAM_BIND(api->downloaded_entry,
               rt_steam_downloaded_entry_fn,
               RT_STEAM_SYMBOL_USER_STATS_DOWNLOADED_ENTRY,
               missing);
    STEAM_BIND(api->upload_score,
               rt_steam_upload_score_fn,
               RT_STEAM_SYMBOL_USER_STATS_UPLOAD_SCORE,
               missing);
    api->leaderboards_ready = missing == NULL;
    if (missing)
        rt_services_steam_report_missing(missing, "leaderboards");
}

#undef STEAM_BIND

/// @brief Forget the leaderboard cache.
void rt_services_steam_reset_user_stats(void) {
    memset(g_steam.boards, 0, sizeof(g_steam.boards));
    g_steam.next_board_slot = 0;
}

//===----------------------------------------------------------------------===//
// Achievements
//===----------------------------------------------------------------------===//

/// @brief Report whether the achievement group can be called.
/// @return 1 when started and bound, otherwise 0.
static int steam_achievements_ready(void) {
    return g_steam.started && g_steam.user_stats.achievements_ready;
}

/// @brief Unlock an achievement locally.
/// @param id Achievement API name.
/// @return 1 when Steam accepted it, otherwise 0.
static int8_t steam_achievement_unlock(const char *id) {
    if (!steam_achievements_ready())
        return 0;
    if (g_steam.user_stats.set_achievement(g_steam.user_stats.self, id))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: SetAchievement('%s') failed; check that the achievement is defined for this app",
        id);
    return 0;
}

/// @brief Lock an achievement again.
/// @param id Achievement API name.
/// @return 1 when Steam accepted it, otherwise 0.
static int8_t steam_achievement_clear(const char *id) {
    if (!steam_achievements_ready())
        return 0;
    if (g_steam.user_stats.clear_achievement(g_steam.user_stats.self, id))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: ClearAchievement('%s') failed; check that the achievement is defined for this app",
        id);
    return 0;
}

/// @brief Read an achievement's unlock state and time.
/// @param id Achievement API name.
/// @param out_unlocked Receives 1 when unlocked.
/// @param out_unlock_time Receives the unlock time in Unix seconds.
/// @return 1 when read, otherwise 0.
static int8_t steam_achievement_get(const char *id,
                                    int8_t *out_unlocked,
                                    int64_t *out_unlock_time) {
    if (!steam_achievements_ready())
        return 0;
    bool achieved = false;
    uint32_t unlock_time = 0;
    if (!g_steam.user_stats.get_achievement(g_steam.user_stats.self, id, &achieved, &unlock_time)) {
        rt_services_provider_add_diagnostic(
            "Steam: GetAchievementAndUnlockTime('%s') failed; check that the achievement is "
            "defined for this app",
            id);
        return 0;
    }
    *out_unlocked = achieved ? 1 : 0;
    *out_unlock_time = (int64_t)unlock_time;
    return 1;
}

/// @brief Show a progress notification.
/// @param id Achievement API name.
/// @param current Progress so far.
/// @param maximum Progress maximum.
/// @return 1 when shown, otherwise 0.
static int8_t steam_achievement_indicate_progress(const char *id,
                                                  int64_t current,
                                                  int64_t maximum) {
    if (!steam_achievements_ready())
        return 0;
    if (maximum > (int64_t)UINT32_MAX) {
        rt_services_provider_add_diagnostic(
            "Steam: achievement progress for '%s' must fit in 32 bits (got %lld of %lld)",
            id,
            (long long)current,
            (long long)maximum);
        return 0;
    }
    if (g_steam.user_stats.indicate_progress(
            g_steam.user_stats.self, id, (uint32_t)current, (uint32_t)maximum))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: IndicateAchievementProgress('%s', %lld, %lld) failed; the achievement must be "
        "defined and locked, and progress must be above 0 and below the maximum",
        id,
        (long long)current,
        (long long)maximum);
    return 0;
}

/// @brief Count the app's achievements.
/// @return Count, or 0 when unavailable.
static int64_t steam_achievement_count(void) {
    if (!steam_achievements_ready())
        return 0;
    return (int64_t)g_steam.user_stats.num_achievements(g_steam.user_stats.self);
}

/// @brief Read the achievement API name at an index.
/// @param index Non-negative index.
/// @return Owned name, or NULL when out of range or unavailable.
static rt_string steam_achievement_id_at(int64_t index) {
    if (!steam_achievements_ready() || index >= steam_achievement_count())
        return NULL;
    const char *name =
        g_steam.user_stats.achievement_name(g_steam.user_stats.self, (uint32_t)index);
    return (name && *name) ? rt_const_cstr(name) : NULL;
}

/// @brief Read an achievement display attribute.
/// @param id Achievement API name.
/// @param attribute Attribute to read.
/// @return Owned text, or NULL when unavailable.
static rt_string steam_achievement_attribute(const char *id,
                                             rt_services_achievement_attribute attribute) {
    if (!steam_achievements_ready())
        return NULL;
    const char *key = attribute == RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_NAME          ? "name"
                      : attribute == RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_DESCRIPTION ? "desc"
                                                                                   : "hidden";
    const char *value = g_steam.user_stats.achievement_attribute(g_steam.user_stats.self, id, key);
    return (value && *value) ? rt_const_cstr(value) : NULL;
}

/// @brief Steam achievements operations.
const rt_services_achievement_ops rt_services_steam_achievement_ops = {
    .unlock = steam_achievement_unlock,
    .clear = steam_achievement_clear,
    .get = steam_achievement_get,
    .indicate_progress = steam_achievement_indicate_progress,
    .count = steam_achievement_count,
    .id_at = steam_achievement_id_at,
    .attribute = steam_achievement_attribute,
};

//===----------------------------------------------------------------------===//
// Stats
//===----------------------------------------------------------------------===//

/// @brief Report whether the stat group can be called.
/// @return 1 when started and bound, otherwise 0.
static int steam_stats_ready(void) {
    return g_steam.started && g_steam.user_stats.stats_ready;
}

/// @brief Read an int32 stat.
/// @param name Stat API name.
/// @param out_value Receives the value.
/// @return 1 when read, otherwise 0.
static int8_t steam_stat_get_int(const char *name, int64_t *out_value) {
    if (!steam_stats_ready())
        return 0;
    int32_t value = 0;
    if (!g_steam.user_stats.get_stat_int(g_steam.user_stats.self, name, &value)) {
        rt_services_provider_add_diagnostic(
            "Steam: GetStatInt32('%s') failed; check that the stat is defined as an INT stat",
            name);
        return 0;
    }
    *out_value = value;
    return 1;
}

/// @brief Set an int32 stat.
/// @param name Stat API name.
/// @param value New value.
/// @return 1 when accepted, otherwise 0.
static int8_t steam_stat_set_int(const char *name, int64_t value) {
    if (!steam_stats_ready())
        return 0;
    if (value < INT32_MIN || value > INT32_MAX) {
        rt_services_provider_add_diagnostic(
            "Steam: stat '%s' value %lld is outside the int32 range", name, (long long)value);
        return 0;
    }
    if (g_steam.user_stats.set_stat_int(g_steam.user_stats.self, name, (int32_t)value))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: SetStatInt32('%s', %lld) failed; check the stat's type, client write access, "
        "and limits",
        name,
        (long long)value);
    return 0;
}

/// @brief Read a float stat.
/// @param name Stat API name.
/// @param out_value Receives the value.
/// @return 1 when read, otherwise 0.
static int8_t steam_stat_get_float(const char *name, double *out_value) {
    if (!steam_stats_ready())
        return 0;
    float value = 0.0f;
    if (!g_steam.user_stats.get_stat_float(g_steam.user_stats.self, name, &value)) {
        rt_services_provider_add_diagnostic(
            "Steam: GetStatFloat('%s') failed; check that the stat is defined as a FLOAT or "
            "AVGRATE stat",
            name);
        return 0;
    }
    *out_value = (double)value;
    return 1;
}

/// @brief Set a float stat.
/// @param name Stat API name.
/// @param value New finite value.
/// @return 1 when accepted, otherwise 0.
static int8_t steam_stat_set_float(const char *name, double value) {
    if (!steam_stats_ready())
        return 0;
    if (value > FLT_MAX || value < -FLT_MAX) {
        rt_services_provider_add_diagnostic(
            "Steam: stat '%s' value %g is outside the float range", name, value);
        return 0;
    }
    if (g_steam.user_stats.set_stat_float(g_steam.user_stats.self, name, (float)value))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: SetStatFloat('%s', %g) failed; check the stat's type, client write access, and "
        "limits",
        name,
        value);
    return 0;
}

/// @brief Update an AVGRATE stat.
/// @param name Stat API name.
/// @param count Amount accumulated this session.
/// @param seconds Session length in seconds.
/// @return 1 when accepted, otherwise 0.
static int8_t steam_stat_update_average_rate(const char *name, double count, double seconds) {
    if (!steam_stats_ready())
        return 0;
    if (count > FLT_MAX || count < -FLT_MAX) {
        rt_services_provider_add_diagnostic(
            "Steam: stat '%s' session count %g is outside the float range", name, count);
        return 0;
    }
    if (g_steam.user_stats.update_avg_rate(g_steam.user_stats.self, name, (float)count, seconds))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: UpdateAvgRateStat('%s') failed; check that the stat is defined as an AVGRATE stat",
        name);
    return 0;
}

/// @brief Commit stats and achievements.
/// @return 1 when the commit started, otherwise 0.
static int8_t steam_stat_store(void) {
    if (!steam_stats_ready())
        return 0;
    if (g_steam.user_stats.store_stats(g_steam.user_stats.self))
        return 1;
    rt_services_provider_add_diagnostic("Steam: StoreStats failed");
    return 0;
}

/// @brief Reset stats and optionally achievements.
/// @param include_achievements Nonzero to lock achievements too.
/// @return 1 when accepted, otherwise 0.
static int8_t steam_stat_reset_all(int8_t include_achievements) {
    if (!steam_stats_ready())
        return 0;
    if (g_steam.user_stats.reset_all_stats(g_steam.user_stats.self, include_achievements != 0))
        return 1;
    rt_services_provider_add_diagnostic("Steam: ResetAllStats failed");
    return 0;
}

/// @brief Steam stats operations.
const rt_services_stat_ops rt_services_steam_stat_ops = {
    .get_int = steam_stat_get_int,
    .set_int = steam_stat_set_int,
    .get_float = steam_stat_get_float,
    .set_float = steam_stat_set_float,
    .update_average_rate = steam_stat_update_average_rate,
    .store = steam_stat_store,
    .reset_all = steam_stat_reset_all,
};

//===----------------------------------------------------------------------===//
// Stats callbacks
//===----------------------------------------------------------------------===//

/// @brief Decode stats callbacks into events.
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a stats callback, otherwise 0.
int rt_services_steam_user_stats_callback(const rt_steam_callback_msg *msg) {
    switch (msg->callback_id) {
        case RT_STEAM_CB_USER_STATS_STORED: {
            rt_steam_user_stats_stored payload;
            if (rt_services_steam_payload_matches(msg, sizeof(payload))) {
                memcpy(&payload, msg->param, sizeof(payload));
                if (payload.result == RT_STEAM_RESULT_INVALID_PARAM) {
                    rt_services_provider_add_diagnostic(
                        "Steam: StoreStats rejected one or more stats (EResult 8); Steam "
                        "reverted them to the stored values");
                }
                rt_services_provider_emit_event(RT_SERVICES_EVENT_STATS_STORED,
                                                payload.result,
                                                0,
                                                payload.result == RT_STEAM_RESULT_OK ? 1 : 0,
                                                NULL);
            }
            return 1;
        }
        case RT_STEAM_CB_USER_ACHIEVEMENT_STORED: {
            rt_steam_user_achievement_stored payload;
            if (rt_services_steam_payload_matches(msg, sizeof(payload))) {
                memcpy(&payload, msg->param, sizeof(payload));
                payload.achievement_name[sizeof(payload.achievement_name) - 1] = '\0';
                const int unlocked = payload.current_progress == 0 && payload.max_progress == 0;
                rt_services_provider_emit_progress_event(RT_SERVICES_EVENT_ACHIEVEMENT_STORED,
                                                         0,
                                                         (int64_t)payload.current_progress,
                                                         (int64_t)payload.max_progress,
                                                         unlocked ? 1 : 0,
                                                         payload.achievement_name);
            }
            return 1;
        }
        default:
            return 0;
    }
}

//===----------------------------------------------------------------------===//
// Leaderboards
//===----------------------------------------------------------------------===//

/// @brief Look up a cached leaderboard handle.
/// @param name Leaderboard name.
/// @return SteamLeaderboard_t, or 0 when not cached.
static uint64_t steam_board_cached(const char *name) {
    for (int i = 0; i < STEAM_LEADERBOARD_CACHE_CAPACITY; ++i) {
        if (g_steam.boards[i].name[0] && strcmp(g_steam.boards[i].name, name) == 0)
            return g_steam.boards[i].leaderboard;
    }
    return 0;
}

/// @brief Cache a leaderboard handle, replacing the oldest entry when full.
/// @param name Leaderboard name (shorter than RT_STEAM_LEADERBOARD_NAME_CAPACITY).
/// @param leaderboard SteamLeaderboard_t.
static void steam_board_remember(const char *name, uint64_t leaderboard) {
    int slot = -1;
    for (int i = 0; i < STEAM_LEADERBOARD_CACHE_CAPACITY; ++i) {
        if (g_steam.boards[i].name[0] == '\0' || strcmp(g_steam.boards[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = g_steam.next_board_slot;
        g_steam.next_board_slot = (g_steam.next_board_slot + 1) % STEAM_LEADERBOARD_CACHE_CAPACITY;
    }
    snprintf(g_steam.boards[slot].name, sizeof(g_steam.boards[slot].name), "%s", name);
    g_steam.boards[slot].leaderboard = leaderboard;
}

/// @brief Look up a Steam user's display name.
/// @param steam_id SteamID64.
/// @param out Receives the name.
/// @param capacity Size of @p out in bytes.
/// @param request_when_unknown Nonzero to ask Steam to fetch an unknown name.
/// @return 1 when a name was written, otherwise 0.
static int steam_lookup_user_name(uint64_t steam_id,
                                  char *out,
                                  size_t capacity,
                                  int request_when_unknown) {
    if (!g_steam.started || capacity == 0)
        return 0;
    const char *name = NULL;
    if (g_steam.user.self && g_steam.friends.self &&
        g_steam.user.get_steam_id(g_steam.user.self) == steam_id) {
        name = g_steam.friends.persona_name(g_steam.friends.self);
    } else if (g_steam.friends.names_ready) {
        name = g_steam.friends.friend_persona_name(g_steam.friends.self, steam_id);
        if (!name || !*name || strcmp(name, STEAM_UNKNOWN_PERSONA) == 0) {
            if (request_when_unknown)
                (void)g_steam.friends.request_user_information(
                    g_steam.friends.self, steam_id, true);
            name = NULL;
        }
    }
    if (!name || !*name)
        return 0;
    snprintf(out, capacity, "%s", name);
    return 1;
}

/// @brief Look up a leaderboard entry's user name live.
/// @param user_id Decimal SteamID64.
/// @return Owned name, or NULL while unknown.
static rt_string steam_leaderboard_user_name(const char *user_id) {
    char *end = NULL;
    unsigned long long value = strtoull(user_id, &end, 10);
    if (!user_id[0] || !end || *end != '\0')
        return NULL;
    char name[RT_SERVICES_USER_NAME_CAPACITY];
    return steam_lookup_user_name((uint64_t)value, name, sizeof(name), 0) ? rt_const_cstr(name)
                                                                          : NULL;
}

/// @brief Steam leaderboard operations beyond begin_request.
const rt_services_leaderboard_ops rt_services_steam_leaderboard_ops = {
    .user_name = steam_leaderboard_user_name,
};

/// @brief Map a neutral download scope to ELeaderboardDataRequest.
/// @param scope RT_SERVICES_LEADERBOARD_SCOPE_* value.
/// @return ELeaderboardDataRequest value.
static int steam_data_request(int64_t scope) {
    switch (scope) {
        case RT_SERVICES_LEADERBOARD_SCOPE_AROUND_USER:
            return RT_STEAM_LEADERBOARD_REQUEST_GLOBAL_AROUND_USER;
        case RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS:
            return RT_STEAM_LEADERBOARD_REQUEST_FRIENDS;
        default:
            return RT_STEAM_LEADERBOARD_REQUEST_GLOBAL;
    }
}

/// @brief Issue the upload call of an upload operation whose board is known.
/// @param op Operation with a resolved leaderboard.
/// @return Steam call handle, or 0 when Steam refused the call.
static rt_steam_api_call steam_issue_upload(steam_request_op *op) {
    op->stage = STEAM_OP_UPLOAD;
    return g_steam.user_stats.upload_score(g_steam.user_stats.self,
                                           op->leaderboard,
                                           op->keep_best ? RT_STEAM_LEADERBOARD_UPLOAD_KEEP_BEST
                                                         : RT_STEAM_LEADERBOARD_UPLOAD_FORCE_UPDATE,
                                           op->score,
                                           NULL,
                                           0);
}

/// @brief Issue the download call of a download operation whose board is known.
/// @param op Operation with a resolved leaderboard.
/// @return Steam call handle, or 0 when Steam refused the call.
static rt_steam_api_call steam_issue_download(steam_request_op *op) {
    op->stage = STEAM_OP_DOWNLOAD;
    return g_steam.user_stats.download_entries(
        g_steam.user_stats.self, op->leaderboard, op->data_request, op->range_start, op->range_end);
}

/// @brief Start a leaderboard request.
/// @param args Validated leaderboard arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when started, otherwise 0.
int8_t rt_services_steam_begin_leaderboard(const rt_services_request_args *args,
                                           uint64_t *out_handle,
                                           char *message,
                                           size_t message_capacity) {
    if (!g_steam.started || !g_steam.user_stats.leaderboards_ready) {
        snprintf(message,
                 message_capacity,
                 "Steam: leaderboards are unavailable (see Platform.Diagnostics)");
        return 0;
    }
    if (strlen(args->name) >= RT_STEAM_LEADERBOARD_NAME_CAPACITY) {
        snprintf(message,
                 message_capacity,
                 "Steam: leaderboard name '%s' is longer than %d bytes",
                 args->name,
                 RT_STEAM_LEADERBOARD_NAME_CAPACITY - 1);
        return 0;
    }
    if (args->kind == RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD &&
        (args->score < INT32_MIN || args->score > INT32_MAX)) {
        snprintf(message,
                 message_capacity,
                 "Steam: leaderboard score %lld is outside the int32 range",
                 (long long)args->score);
        return 0;
    }
    if (args->kind == RT_SERVICES_REQUEST_LEADERBOARD_DOWNLOAD &&
        (args->range_start < INT32_MIN || args->range_end > INT32_MAX)) {
        snprintf(message,
                 message_capacity,
                 "Steam: leaderboard range %lld..%lld is outside the int32 range",
                 (long long)args->range_start,
                 (long long)args->range_end);
        return 0;
    }

    steam_request_op *op = rt_services_steam_op_alloc(STEAM_OP_FIND);
    if (!op) {
        snprintf(message, message_capacity, "Steam: too many pending requests");
        return 0;
    }
    snprintf(op->board, sizeof(op->board), "%s", args->name);
    op->score = (int32_t)args->score;
    op->keep_best = args->keep_best != 0;
    op->data_request = steam_data_request(args->scope);
    op->range_start = (int)args->range_start;
    op->range_end = (int)args->range_end;
    op->leaderboard = steam_board_cached(args->name);

    const char *method = "FindLeaderboard";
    rt_steam_api_call call = 0;
    if (args->kind == RT_SERVICES_REQUEST_LEADERBOARD_FIND) {
        if (args->create) {
            method = "FindOrCreateLeaderboard";
            call = g_steam.user_stats.find_or_create(g_steam.user_stats.self,
                                                     args->name,
                                                     args->sort ==
                                                             RT_SERVICES_LEADERBOARD_SORT_ASCENDING
                                                         ? RT_STEAM_LEADERBOARD_SORT_ASCENDING
                                                         : RT_STEAM_LEADERBOARD_SORT_DESCENDING,
                                                     (int)args->display);
        } else {
            call = g_steam.user_stats.find_leaderboard(g_steam.user_stats.self, args->name);
        }
    } else if (op->leaderboard) {
        const int upload = args->kind == RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD;
        method = upload ? "UploadLeaderboardScore" : "DownloadLeaderboardEntries";
        call = upload ? steam_issue_upload(op) : steam_issue_download(op);
    } else {
        op->stage = args->kind == RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD
                        ? STEAM_OP_FIND_FOR_UPLOAD
                        : STEAM_OP_FIND_FOR_DOWNLOAD;
        call = g_steam.user_stats.find_leaderboard(g_steam.user_stats.self, args->name);
    }
    if (call == 0) {
        rt_services_steam_op_free(op);
        snprintf(message, message_capacity, "Steam: %s returned an invalid call handle", method);
        return 0;
    }
    op->call = call;
    *out_handle = op->token;
    return 1;
}

/// @brief Complete a find stage: finish a Find request or issue the follow-up call.
/// @param op Operation in a find stage.
/// @param completed Decoded completion record.
static void steam_leaderboard_found(steam_request_op *op,
                                    const rt_steam_api_call_completed *completed) {
    rt_steam_leaderboard_find_result found;
    char error[RT_SERVICES_MESSAGE_CAPACITY];
    if (!rt_services_steam_fetch_call_result(completed,
                                             RT_STEAM_CB_LEADERBOARD_FIND_RESULT,
                                             &found,
                                             sizeof(found),
                                             "FindLeaderboard",
                                             error,
                                             sizeof(error))) {
        rt_services_steam_op_fail(op, error);
        return;
    }
    if (!found.found || found.leaderboard == 0) {
        snprintf(error, sizeof(error), "Steam: leaderboard '%s' was not found", op->board);
        rt_services_steam_op_fail(op, error);
        return;
    }
    op->leaderboard = found.leaderboard;
    steam_board_remember(op->board, found.leaderboard);

    if (op->stage == STEAM_OP_FIND) {
        const char *reported =
            g_steam.user_stats.leaderboard_name(g_steam.user_stats.self, found.leaderboard);
        rt_services_request_result result;
        memset(&result, 0, sizeof(result));
        result.succeeded = 1;
        result.result_code = RT_STEAM_RESULT_OK;
        result.value =
            g_steam.user_stats.leaderboard_entry_count(g_steam.user_stats.self, found.leaderboard);
        result.text = (reported && *reported) ? reported : op->board;
        // Finish before freeing the slot: result.text may point into op->board.
        rt_services_provider_finish_request(op->token, &result);
        rt_services_steam_op_free(op);
        return;
    }

    const int upload = op->stage == STEAM_OP_FIND_FOR_UPLOAD;
    rt_steam_api_call call = upload ? steam_issue_upload(op) : steam_issue_download(op);
    if (call == 0) {
        snprintf(error,
                 sizeof(error),
                 "Steam: %s returned an invalid call handle",
                 upload ? "UploadLeaderboardScore" : "DownloadLeaderboardEntries");
        rt_services_steam_op_fail(op, error);
        return;
    }
    op->call = call;
}

/// @brief Complete an upload stage.
/// @param op Operation in the upload stage.
/// @param completed Decoded completion record.
static void steam_leaderboard_uploaded(steam_request_op *op,
                                       const rt_steam_api_call_completed *completed) {
    rt_steam_leaderboard_score_uploaded uploaded;
    char error[RT_SERVICES_MESSAGE_CAPACITY];
    if (!rt_services_steam_fetch_call_result(completed,
                                             RT_STEAM_CB_LEADERBOARD_SCORE_UPLOADED,
                                             &uploaded,
                                             sizeof(uploaded),
                                             "UploadLeaderboardScore",
                                             error,
                                             sizeof(error))) {
        rt_services_steam_op_fail(op, error);
        return;
    }
    if (uploaded.success != 1) {
        snprintf(error, sizeof(error), "Steam: score upload to leaderboard '%s' failed", op->board);
        rt_services_steam_op_fail(op, error);
        return;
    }
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.succeeded = 1;
    result.result_code = RT_STEAM_RESULT_OK;
    result.value = uploaded.global_rank_new;
    result.flag = uploaded.score_changed ? 1 : 0;
    result.text = op->board;
    // Finish before freeing the slot: result.text points into op->board.
    rt_services_provider_finish_request(op->token, &result);
    rt_services_steam_op_free(op);
}

/// @brief Complete a download stage by reading every downloaded entry.
/// @param op Operation in the download stage.
/// @param completed Decoded completion record.
static void steam_leaderboard_downloaded(steam_request_op *op,
                                         const rt_steam_api_call_completed *completed) {
    rt_steam_leaderboard_scores_downloaded downloaded;
    char error[RT_SERVICES_MESSAGE_CAPACITY];
    if (!rt_services_steam_fetch_call_result(completed,
                                             RT_STEAM_CB_LEADERBOARD_SCORES_DOWNLOADED,
                                             &downloaded,
                                             sizeof(downloaded),
                                             "DownloadLeaderboardEntries",
                                             error,
                                             sizeof(error))) {
        rt_services_steam_op_fail(op, error);
        return;
    }
    const int total = downloaded.entry_count > 0 ? downloaded.entry_count : 0;
    const int keep = total < RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY
                         ? total
                         : RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY;
    rt_services_leaderboard_entry *entries = NULL;
    if (keep > 0) {
        entries = (rt_services_leaderboard_entry *)calloc((size_t)keep, sizeof(*entries));
        if (!entries) {
            rt_services_steam_op_fail(op, "Steam: out of memory reading leaderboard entries");
            return;
        }
    }
    int stored = 0;
    // Steam frees the downloaded data once every entry has been read, so read
    // them all even when only the first `keep` are stored.
    for (int i = 0; i < total; ++i) {
        rt_steam_leaderboard_entry entry;
        memset(&entry, 0, sizeof(entry));
        if (!g_steam.user_stats.downloaded_entry(
                g_steam.user_stats.self, downloaded.entries, i, &entry, NULL, 0))
            continue;
        if (stored >= keep)
            continue;
        rt_services_leaderboard_entry *out = &entries[stored++];
        out->rank = entry.global_rank;
        out->score = entry.score;
        snprintf(out->user_id, sizeof(out->user_id), "%llu", (unsigned long long)entry.steam_id);
        (void)steam_lookup_user_name(entry.steam_id, out->user_name, sizeof(out->user_name), 1);
    }
    if (total > keep) {
        rt_services_provider_add_diagnostic(
            "Steam: leaderboard '%s' download returned %d entries; kept the first %d",
            op->board,
            total,
            keep);
    }
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.succeeded = 1;
    result.result_code = RT_STEAM_RESULT_OK;
    result.value =
        g_steam.user_stats.leaderboard_entry_count(g_steam.user_stats.self, downloaded.leaderboard);
    result.text = op->board;
    result.entries = entries;
    result.entry_count = stored;
    // Finish before freeing the slot: result.text points into op->board.
    rt_services_provider_finish_request(op->token, &result);
    rt_services_steam_op_free(op);
    free(entries);
}

/// @brief Advance a leaderboard operation whose Steam call completed.
/// @param op Operation in a leaderboard stage.
/// @param completed Decoded completion record.
void rt_services_steam_leaderboard_call_completed(steam_request_op *op,
                                                  const rt_steam_api_call_completed *completed) {
    switch (op->stage) {
        case STEAM_OP_FIND:
        case STEAM_OP_FIND_FOR_UPLOAD:
        case STEAM_OP_FIND_FOR_DOWNLOAD:
            steam_leaderboard_found(op, completed);
            break;
        case STEAM_OP_UPLOAD:
            steam_leaderboard_uploaded(op, completed);
            break;
        case STEAM_OP_DOWNLOAD:
            steam_leaderboard_downloaded(op, completed);
            break;
        default:
            break;
    }
}
