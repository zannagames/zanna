//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_progress.c
// Purpose: Implements the provider-neutral Zanna.Services.Achievements, Stats,
//          and Leaderboards classes and their constant classes on top of the
//          active provider's operation tables.
// Key invariants:
//   - Every stateful member checks the main thread first, then validates
//     provider-independent argument rules (trapping on empty identifiers,
//     unknown constants, and impossible download ranges), then delegates.
//   - Run-time values that no provider accepts (non-finite floats, invalid
//     progress) return false and record a diagnostic instead of trapping.
//   - Without an active provider exposing the operation, members return
//     neutral values and leaderboard requests complete as failed.
// Ownership/Lifetime:
//   - Strings returned to callers are new references.
//   - Leaderboard members return caller-owned Zanna.Services.Request objects.
// Links: src/runtime/services/rt_services_progress.h,
//        src/runtime/services/rt_services_internal.h,
//        docs/adr/0353-platform-services-player-features.md,
//        docs/adr/0364-platform-services-achievement-icons-and-percentages.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_progress.c
 * @brief Implements Zanna.Services.Achievements, Stats, and Leaderboards.
 */

#include "rt_services_progress.h"

#include "rt_bytes.h"
#include "rt_services.h"
#include "rt_services_internal.h"
#include "rt_services_provider.h"
#include "rt_string.h"

#include <math.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Read the active provider's achievement operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_achievement_ops *progress_achievement_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->achievements : NULL;
}

/// @brief Read the active provider's stat operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_stat_ops *progress_stat_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->stats : NULL;
}

/// @brief Check the main thread and require a non-empty identifier.
/// @param member Class-qualified member name.
/// @param text Identifier argument.
/// @param what Argument description used in the trap message.
/// @return Borrowed identifier bytes, or NULL after a trap.
static const char *progress_enter(const char *member, rt_string text, const char *what) {
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    return rt_services_internal_require_name(text, member, what);
}

/// @brief Read a string attribute of an achievement.
/// @param member Class-qualified member name.
/// @param id Achievement id argument.
/// @param attribute Attribute to read.
/// @return Caller-owned text, or the empty string.
static rt_string progress_achievement_attribute(const char *member,
                                                rt_string id,
                                                rt_services_achievement_attribute attribute) {
    const char *text = progress_enter(member, id, "achievement id");
    if (!text)
        return rt_str_empty();
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    if (!ops || !ops->attribute)
        return rt_str_empty();
    return rt_services_internal_owned_or_empty(ops->attribute(text, attribute));
}

/// @brief Initialize request arguments for a leaderboard kind.
/// @param args Arguments to initialize.
/// @param kind RT_SERVICES_REQUEST_LEADERBOARD_* value.
/// @param name Validated leaderboard name.
static void progress_leaderboard_args(rt_services_request_args *args,
                                      int64_t kind,
                                      const char *name) {
    memset(args, 0, sizeof(*args));
    args->kind = kind;
    args->name = name;
    args->text = "";
}

//===----------------------------------------------------------------------===//
// Zanna.Services.Achievements
//===----------------------------------------------------------------------===//

/// @brief Unlock an achievement locally.
/// @param id Achievement id.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_achievements_unlock(rt_string id) {
    const char *text = progress_enter("Achievements.Unlock", id, "achievement id");
    if (!text)
        return 0;
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    return (ops && ops->unlock && ops->unlock(text)) ? 1 : 0;
}

/// @brief Lock an achievement again.
/// @param id Achievement id.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_achievements_clear(rt_string id) {
    const char *text = progress_enter("Achievements.Clear", id, "achievement id");
    if (!text)
        return 0;
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    return (ops && ops->clear && ops->clear(text)) ? 1 : 0;
}

/// @brief Report whether an achievement is unlocked.
/// @param id Achievement id.
/// @return 1 when unlocked, otherwise 0.
int8_t rt_services_achievements_is_unlocked(rt_string id) {
    const char *text = progress_enter("Achievements.IsUnlocked", id, "achievement id");
    if (!text)
        return 0;
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    int8_t unlocked = 0;
    int64_t unlock_time = 0;
    if (!ops || !ops->get || !ops->get(text, &unlocked, &unlock_time))
        return 0;
    return unlocked ? 1 : 0;
}

/// @brief Read when an achievement was unlocked.
/// @param id Achievement id.
/// @return Unix seconds, or 0 when locked or unknown.
int64_t rt_services_achievements_unlock_time(rt_string id) {
    const char *text = progress_enter("Achievements.UnlockTime", id, "achievement id");
    if (!text)
        return 0;
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    int8_t unlocked = 0;
    int64_t unlock_time = 0;
    if (!ops || !ops->get || !ops->get(text, &unlocked, &unlock_time) || !unlocked)
        return 0;
    return unlock_time;
}

/// @brief Show a progress notification for an achievement.
/// @param id Achievement id.
/// @param current Progress so far.
/// @param maximum Progress needed to unlock.
/// @return 1 when shown, otherwise 0.
int8_t rt_services_achievements_indicate_progress(rt_string id, int64_t current, int64_t maximum) {
    const char *text = progress_enter("Achievements.IndicateProgress", id, "achievement id");
    if (!text)
        return 0;
    if (maximum <= 0 || current < 0 || current > maximum) {
        rt_services_provider_add_diagnostic(
            "Services: Achievements.IndicateProgress('%s') needs 0 <= current <= maximum and "
            "maximum > 0 (got %lld of %lld)",
            text,
            (long long)current,
            (long long)maximum);
        return 0;
    }
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    return (ops && ops->indicate_progress && ops->indicate_progress(text, current, maximum)) ? 1
                                                                                             : 0;
}

/// @brief Read how many achievements the app defines.
/// @return Count, or 0 when unavailable.
int64_t rt_services_achievements_get_count(void) {
    if (!rt_services_provider_require_main_thread("Achievements.Count"))
        return 0;
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    if (!ops || !ops->count)
        return 0;
    int64_t count = ops->count();
    return count > 0 ? count : 0;
}

/// @brief Read the achievement id at an index.
/// @param index Achievement index.
/// @return Caller-owned id, or the empty string when out of range or unavailable.
rt_string rt_services_achievements_id_at(int64_t index) {
    if (!rt_services_provider_require_main_thread("Achievements.IdAt") || index < 0)
        return rt_str_empty();
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    if (!ops || !ops->id_at)
        return rt_str_empty();
    return rt_services_internal_owned_or_empty(ops->id_at(index));
}

/// @brief Read an achievement's display name.
/// @param id Achievement id.
/// @return Caller-owned name, or the empty string.
rt_string rt_services_achievements_display_name(rt_string id) {
    return progress_achievement_attribute(
        "Achievements.DisplayName", id, RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_NAME);
}

/// @brief Read an achievement's description.
/// @param id Achievement id.
/// @return Caller-owned description, or the empty string.
rt_string rt_services_achievements_description(rt_string id) {
    return progress_achievement_attribute(
        "Achievements.Description", id, RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_DESCRIPTION);
}

/// @brief Report whether an achievement is hidden until unlocked.
/// @param id Achievement id.
/// @return 1 when hidden, otherwise 0.
int8_t rt_services_achievements_is_hidden(rt_string id) {
    rt_string hidden = progress_achievement_attribute(
        "Achievements.IsHidden", id, RT_SERVICES_ACHIEVEMENT_ATTRIBUTE_HIDDEN);
    const int8_t result = strcmp(rt_services_internal_cstr(hidden), "1") == 0 ? 1 : 0;
    rt_string_unref(hidden);
    return result;
}

/// @brief Read an achievement icon's size, and optionally its pixels.
/// @param member Class-qualified member name.
/// @param id Achievement id argument.
/// @param out_width Receives the width.
/// @param out_height Receives the height.
/// @param out_rgba Receives caller-owned Bytes, or NULL to read only the size.
/// @return 1 when the icon was read, otherwise 0.
static int progress_read_icon(const char *member,
                              rt_string id,
                              int64_t *out_width,
                              int64_t *out_height,
                              void **out_rgba) {
    const char *text = progress_enter(member, id, "achievement id");
    if (!text)
        return 0;
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    *out_width = 0;
    *out_height = 0;
    if (out_rgba)
        *out_rgba = NULL;
    if (!ops || !ops->icon || !ops->icon(text, out_width, out_height, out_rgba))
        return 0;
    return 1;
}

/// @brief Read an achievement icon's width.
/// @param id Achievement id.
/// @return Width in pixels, or 0.
int64_t rt_services_achievements_icon_width(rt_string id) {
    int64_t width = 0;
    int64_t height = 0;
    return progress_read_icon("Achievements.IconWidth", id, &width, &height, NULL) ? width : 0;
}

/// @brief Read an achievement icon's height.
/// @param id Achievement id.
/// @return Height in pixels, or 0.
int64_t rt_services_achievements_icon_height(rt_string id) {
    int64_t width = 0;
    int64_t height = 0;
    return progress_read_icon("Achievements.IconHeight", id, &width, &height, NULL) ? height : 0;
}

/// @brief Read an achievement icon's RGBA bytes.
/// @param id Achievement id.
/// @return Caller-owned Bytes; empty while loading or unavailable.
void *rt_services_achievements_icon_rgba(rt_string id) {
    int64_t width = 0;
    int64_t height = 0;
    void *rgba = NULL;
    if (progress_read_icon("Achievements.IconRgba", id, &width, &height, &rgba) && rgba)
        return rgba;
    return rt_bytes_new(0);
}

/// @brief Start a global unlock percentage request.
/// @return Caller-owned request.
void *rt_services_achievements_request_global_percentages(void) {
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = RT_SERVICES_REQUEST_ACHIEVEMENT_PERCENTAGES;
    args.name = "";
    args.text = "";
    return rt_services_internal_begin_request(&args, "Achievements.RequestGlobalPercentages");
}

/// @brief Read an achievement's global unlock percentage.
/// @param id Achievement id.
/// @return Percentage in 0..100, or 0.
double rt_services_achievements_global_percent(rt_string id) {
    const char *text = progress_enter("Achievements.GlobalPercent", id, "achievement id");
    if (!text)
        return 0.0;
    const rt_services_achievement_ops *ops = progress_achievement_ops();
    double percent = 0.0;
    if (!ops || !ops->global_percent || !ops->global_percent(text, &percent))
        return 0.0;
    return percent;
}

//===----------------------------------------------------------------------===//
// Zanna.Services.Stats
//===----------------------------------------------------------------------===//

/// @brief Read an integer stat.
/// @param name Stat name.
/// @return Value, or 0 when unavailable.
int64_t rt_services_stats_get_int(rt_string name) {
    const char *text = progress_enter("Stats.GetInt", name, "stat name");
    if (!text)
        return 0;
    const rt_services_stat_ops *ops = progress_stat_ops();
    int64_t value = 0;
    if (!ops || !ops->get_int || !ops->get_int(text, &value))
        return 0;
    return value;
}

/// @brief Set an integer stat.
/// @param name Stat name.
/// @param value New value.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_stats_set_int(rt_string name, int64_t value) {
    const char *text = progress_enter("Stats.SetInt", name, "stat name");
    if (!text)
        return 0;
    const rt_services_stat_ops *ops = progress_stat_ops();
    return (ops && ops->set_int && ops->set_int(text, value)) ? 1 : 0;
}

/// @brief Read a floating-point stat.
/// @param name Stat name.
/// @return Value, or 0.0 when unavailable.
double rt_services_stats_get_float(rt_string name) {
    const char *text = progress_enter("Stats.GetFloat", name, "stat name");
    if (!text)
        return 0.0;
    const rt_services_stat_ops *ops = progress_stat_ops();
    double value = 0.0;
    if (!ops || !ops->get_float || !ops->get_float(text, &value))
        return 0.0;
    return value;
}

/// @brief Set a floating-point stat.
/// @param name Stat name.
/// @param value New value; non-finite values are rejected with a diagnostic.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_stats_set_float(rt_string name, double value) {
    const char *text = progress_enter("Stats.SetFloat", name, "stat name");
    if (!text)
        return 0;
    if (!isfinite(value)) {
        rt_services_provider_add_diagnostic(
            "Services: Stats.SetFloat('%s') rejected a non-finite value", text);
        return 0;
    }
    const rt_services_stat_ops *ops = progress_stat_ops();
    return (ops && ops->set_float && ops->set_float(text, value)) ? 1 : 0;
}

/// @brief Update an average-rate stat.
/// @param name Stat name.
/// @param count Amount accumulated during the session.
/// @param seconds Session length in seconds.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_stats_update_average_rate(rt_string name, double count, double seconds) {
    const char *text = progress_enter("Stats.UpdateAverageRate", name, "stat name");
    if (!text)
        return 0;
    if (!isfinite(count) || !isfinite(seconds) || seconds <= 0.0) {
        rt_services_provider_add_diagnostic(
            "Services: Stats.UpdateAverageRate('%s') needs a finite count and a positive, "
            "finite session length",
            text);
        return 0;
    }
    const rt_services_stat_ops *ops = progress_stat_ops();
    return (ops && ops->update_average_rate && ops->update_average_rate(text, count, seconds)) ? 1
                                                                                               : 0;
}

/// @brief Commit changed stats and achievements.
/// @return 1 when the commit started, otherwise 0.
int8_t rt_services_stats_store(void) {
    if (!rt_services_provider_require_main_thread("Stats.Store"))
        return 0;
    const rt_services_stat_ops *ops = progress_stat_ops();
    return (ops && ops->store && ops->store()) ? 1 : 0;
}

/// @brief Reset every stat and optionally every achievement.
/// @param include_achievements Nonzero to lock achievements as well.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_stats_reset_all(int8_t include_achievements) {
    if (!rt_services_provider_require_main_thread("Stats.ResetAll"))
        return 0;
    const rt_services_stat_ops *ops = progress_stat_ops();
    return (ops && ops->reset_all && ops->reset_all(include_achievements ? 1 : 0)) ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Zanna.Services.Leaderboards
//===----------------------------------------------------------------------===//

/// @brief Start a leaderboard lookup.
/// @param name Leaderboard name.
/// @return Caller-owned request.
void *rt_services_leaderboards_find(rt_string name) {
    const char *text = progress_enter("Leaderboards.Find", name, "leaderboard name");
    if (!text)
        return NULL;
    rt_services_request_args args;
    progress_leaderboard_args(&args, RT_SERVICES_REQUEST_LEADERBOARD_FIND, text);
    return rt_services_internal_begin_request(&args, "Leaderboards.Find");
}

/// @brief Start a leaderboard lookup that creates a missing board.
/// @param name Leaderboard name.
/// @param sort LeaderboardSort value.
/// @param display LeaderboardDisplay value.
/// @return Caller-owned request.
void *rt_services_leaderboards_find_or_create(rt_string name, int64_t sort, int64_t display) {
    static const char member[] = "Leaderboards.FindOrCreate";
    const char *text = progress_enter(member, name, "leaderboard name");
    if (!text)
        return NULL;
    if (sort != RT_SERVICES_LEADERBOARD_SORT_ASCENDING &&
        sort != RT_SERVICES_LEADERBOARD_SORT_DESCENDING) {
        rt_services_internal_trap_argument(
            member, "sort must be a LeaderboardSort value (got %lld)", (long long)sort);
        return NULL;
    }
    if (display < RT_SERVICES_LEADERBOARD_DISPLAY_NUMERIC ||
        display > RT_SERVICES_LEADERBOARD_DISPLAY_MILLISECONDS) {
        rt_services_internal_trap_argument(
            member, "display must be a LeaderboardDisplay value (got %lld)", (long long)display);
        return NULL;
    }
    rt_services_request_args args;
    progress_leaderboard_args(&args, RT_SERVICES_REQUEST_LEADERBOARD_FIND, text);
    args.create = 1;
    args.sort = sort;
    args.display = display;
    return rt_services_internal_begin_request(&args, member);
}

/// @brief Start a score upload.
/// @param name Leaderboard name.
/// @param score Score to upload.
/// @param keep_best Nonzero to keep a better existing score.
/// @return Caller-owned request.
void *rt_services_leaderboards_upload(rt_string name, int64_t score, int8_t keep_best) {
    const char *text = progress_enter("Leaderboards.Upload", name, "leaderboard name");
    if (!text)
        return NULL;
    rt_services_request_args args;
    progress_leaderboard_args(&args, RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD, text);
    args.score = score;
    args.keep_best = keep_best ? 1 : 0;
    return rt_services_internal_begin_request(&args, "Leaderboards.Upload");
}

/// @brief Start an entry download.
/// @param name Leaderboard name.
/// @param scope LeaderboardScope value.
/// @param start First rank or offset.
/// @param end Last rank or offset.
/// @return Caller-owned request.
void *rt_services_leaderboards_download(rt_string name, int64_t scope, int64_t start, int64_t end) {
    static const char member[] = "Leaderboards.Download";
    const char *text = progress_enter(member, name, "leaderboard name");
    if (!text)
        return NULL;
    if (scope < RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL ||
        scope > RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS) {
        rt_services_internal_trap_argument(
            member, "scope must be a LeaderboardScope value (got %lld)", (long long)scope);
        return NULL;
    }
    if (scope != RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS) {
        if (scope == RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL && (start < 1 || start > end)) {
            rt_services_internal_trap_argument(member,
                                               "Global ranks must satisfy 1 <= start <= end "
                                               "(got %lld..%lld)",
                                               (long long)start,
                                               (long long)end);
            return NULL;
        }
        if (start > end) {
            rt_services_internal_trap_argument(member,
                                               "start must not exceed end (got %lld..%lld)",
                                               (long long)start,
                                               (long long)end);
            return NULL;
        }
        // start <= end here, so the unsigned difference is the exact span and
        // cannot overflow for extreme offsets.
        if ((uint64_t)end - (uint64_t)start >= (uint64_t)RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY) {
            rt_services_internal_trap_argument(member,
                                               "a download spans at most %d entries (got "
                                               "%lld..%lld)",
                                               RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY,
                                               (long long)start,
                                               (long long)end);
            return NULL;
        }
    }
    rt_services_request_args args;
    progress_leaderboard_args(&args, RT_SERVICES_REQUEST_LEADERBOARD_DOWNLOAD, text);
    args.scope = scope;
    args.range_start = scope == RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS ? 0 : start;
    args.range_end = scope == RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS ? 0 : end;
    return rt_services_internal_begin_request(&args, member);
}

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return LeaderboardScope.Global. @return 0.
int64_t rt_services_leaderboard_scope_global(void) {
    return RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL;
}

/// @brief Return LeaderboardScope.AroundUser. @return 1.
int64_t rt_services_leaderboard_scope_around_user(void) {
    return RT_SERVICES_LEADERBOARD_SCOPE_AROUND_USER;
}

/// @brief Return LeaderboardScope.Friends. @return 2.
int64_t rt_services_leaderboard_scope_friends(void) {
    return RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS;
}

/// @brief Return LeaderboardSort.Ascending. @return 1.
int64_t rt_services_leaderboard_sort_ascending(void) {
    return RT_SERVICES_LEADERBOARD_SORT_ASCENDING;
}

/// @brief Return LeaderboardSort.Descending. @return 2.
int64_t rt_services_leaderboard_sort_descending(void) {
    return RT_SERVICES_LEADERBOARD_SORT_DESCENDING;
}

/// @brief Return LeaderboardDisplay.Numeric. @return 1.
int64_t rt_services_leaderboard_display_numeric(void) {
    return RT_SERVICES_LEADERBOARD_DISPLAY_NUMERIC;
}

/// @brief Return LeaderboardDisplay.Seconds. @return 2.
int64_t rt_services_leaderboard_display_seconds(void) {
    return RT_SERVICES_LEADERBOARD_DISPLAY_SECONDS;
}

/// @brief Return LeaderboardDisplay.Milliseconds. @return 3.
int64_t rt_services_leaderboard_display_milliseconds(void) {
    return RT_SERVICES_LEADERBOARD_DISPLAY_MILLISECONDS;
}
