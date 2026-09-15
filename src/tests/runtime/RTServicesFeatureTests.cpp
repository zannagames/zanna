//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTServicesFeatureTests.cpp
// Purpose: Verify the Zanna.Services player features of ADR 0353 and their
//          Steam bindings against the fake steam_api libraries: achievements,
//          stats, leaderboards (including multi-call requests and live entry
//          names), rich presence, overlay control, both gamepad keyboards,
//          cloud storage, argument traps, neutral behavior without a provider,
//          a redistributable without the feature exports, request
//          cancellation, diagnostics, threading, and payload layouts.
// Key invariants:
//   - Every test starts from a shut-down session and a reset fake library.
//   - Fixtures (fake library view, trap capture, helpers) come from
//     RTServicesTestSupport.hpp.
// Ownership/Lifetime:
//   - Tests release every Result, Request, Bytes, Seq, and string they receive.
// Links: src/runtime/services/rt_services_progress.c,
//        src/runtime/services/rt_services_social.c,
//        src/runtime/services/rt_services_cloud.c,
//        src/runtime/services/steam/rt_steam_user_stats.c,
//        src/runtime/services/steam/rt_steam_social.c,
//        src/runtime/services/steam/rt_steam_cloud.c,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

#include "RTServicesTestSupport.hpp"

#include "rt_bytes.h"
#include "rt_services_cloud.h"
#include "rt_services_progress.h"
#include "rt_services_social.h"
#include "rt_steam_abi.h"

#include <cmath>
#include <cstddef>
#include <thread>

using namespace services_test;

namespace {

/// @brief Create Bytes holding @p text.
void *bytesOf(const char *text) {
    const size_t length = std::strlen(text);
    void *bytes = rt_bytes_new((int64_t)length);
    if (bytes && length > 0)
        std::memcpy(rt_bytes_data(bytes), text, length);
    return bytes;
}

/// @brief Start Steam against the SDK 1.65 fake with scripted startup events disabled.
const FakeSteam &startSteam() {
    const FakeSteam &fake = fake165();
    useFake(fake);
    fake.setScripted(0);
    InitOutcome outcome = init("steam", "480");
    EXPECT_TRUE(outcome.ok);
    return fake;
}

/// @brief Pump until @p request completes, at most @p frames times.
bool pumpUntilDone(void *request, int frames) {
    for (int i = 0; i < frames && !rt_services_request_get_is_done(request); ++i)
        rt_services_platform_update();
    return rt_services_request_get_is_done(request) != 0;
}

/// @brief Every feature constant added by ADR 0353.
const int64_t kPhase2Features[] = {
    RT_SERVICES_FEATURE_ACHIEVEMENTS,
    RT_SERVICES_FEATURE_STATS,
    RT_SERVICES_FEATURE_LEADERBOARDS,
    RT_SERVICES_FEATURE_PRESENCE,
    RT_SERVICES_FEATURE_OVERLAY,
    RT_SERVICES_FEATURE_TEXT_INPUT,
    RT_SERVICES_FEATURE_CLOUD,
};

} // namespace

TEST(ServicesFeatures, ConstantsMatchAdr) {
    EXPECT_EQ(rt_services_event_kind_stats_stored(), 8);
    EXPECT_EQ(rt_services_event_kind_achievement_stored(), 9);
    EXPECT_EQ(rt_services_event_kind_text_input_dismissed(), 10);
    EXPECT_EQ(rt_services_feature_achievements(), 5);
    EXPECT_EQ(rt_services_feature_stats(), 6);
    EXPECT_EQ(rt_services_feature_leaderboards(), 7);
    EXPECT_EQ(rt_services_feature_presence(), 8);
    EXPECT_EQ(rt_services_feature_overlay(), 9);
    EXPECT_EQ(rt_services_feature_text_input(), 10);
    EXPECT_EQ(rt_services_feature_cloud(), 11);
    EXPECT_EQ(rt_services_request_kind_leaderboard_find(), 2);
    EXPECT_EQ(rt_services_request_kind_leaderboard_upload(), 3);
    EXPECT_EQ(rt_services_request_kind_leaderboard_download(), 4);
    EXPECT_EQ(rt_services_request_kind_text_input(), 5);
    EXPECT_EQ(rt_services_leaderboard_scope_global(), 0);
    EXPECT_EQ(rt_services_leaderboard_scope_around_user(), 1);
    EXPECT_EQ(rt_services_leaderboard_scope_friends(), 2);
    EXPECT_EQ(rt_services_leaderboard_sort_ascending(), 1);
    EXPECT_EQ(rt_services_leaderboard_sort_descending(), 2);
    EXPECT_EQ(rt_services_leaderboard_display_numeric(), 1);
    EXPECT_EQ(rt_services_leaderboard_display_seconds(), 2);
    EXPECT_EQ(rt_services_leaderboard_display_milliseconds(), 3);
    EXPECT_EQ(rt_services_overlay_page_friends(), 1);
    EXPECT_EQ(rt_services_overlay_page_community(), 2);
    EXPECT_EQ(rt_services_overlay_page_players(), 3);
    EXPECT_EQ(rt_services_overlay_page_settings(), 4);
    EXPECT_EQ(rt_services_overlay_page_official_group(), 5);
    EXPECT_EQ(rt_services_overlay_page_stats(), 6);
    EXPECT_EQ(rt_services_overlay_page_achievements(), 7);
    EXPECT_EQ(rt_services_notification_position_top_left(), 0);
    EXPECT_EQ(rt_services_notification_position_top_right(), 1);
    EXPECT_EQ(rt_services_notification_position_bottom_left(), 2);
    EXPECT_EQ(rt_services_notification_position_bottom_right(), 3);
    EXPECT_EQ(rt_services_text_input_mode_single_line(), 0);
    EXPECT_EQ(rt_services_text_input_mode_multi_line(), 1);
    EXPECT_EQ(rt_services_text_input_mode_email(), 2);
    EXPECT_EQ(rt_services_text_input_mode_numeric(), 3);
    EXPECT_EQ(rt_services_text_input_mode_password(), 4);
}

TEST(ServicesFeatures, LayoutsMatchRedistributablePacking) {
    const bool pack8 = RT_STEAM_CALLBACK_PACK == 8;
    EXPECT_EQ(sizeof(rt_steam_gamepad_text_input_dismissed), 12u);
    EXPECT_EQ(offsetof(rt_steam_gamepad_text_input_dismissed, app_id), 8u);
    EXPECT_EQ(sizeof(rt_steam_user_stats_stored), pack8 ? 16u : 12u);
    EXPECT_EQ(sizeof(rt_steam_user_achievement_stored), pack8 ? 152u : 148u);
    EXPECT_EQ(offsetof(rt_steam_user_achievement_stored, achievement_name), 9u);
    EXPECT_EQ(offsetof(rt_steam_user_achievement_stored, current_progress), 140u);
    EXPECT_EQ(offsetof(rt_steam_user_achievement_stored, max_progress), 144u);
    EXPECT_EQ(sizeof(rt_steam_leaderboard_find_result), pack8 ? 16u : 12u);
    EXPECT_EQ(sizeof(rt_steam_leaderboard_scores_downloaded), pack8 ? 24u : 20u);
    EXPECT_EQ(offsetof(rt_steam_leaderboard_scores_downloaded, entry_count), 16u);
    EXPECT_EQ(sizeof(rt_steam_leaderboard_score_uploaded), pack8 ? 32u : 28u);
    EXPECT_EQ(offsetof(rt_steam_leaderboard_score_uploaded, leaderboard), pack8 ? 8u : 4u);
    EXPECT_EQ(offsetof(rt_steam_leaderboard_score_uploaded, global_rank_new), pack8 ? 24u : 20u);
    EXPECT_EQ(sizeof(rt_steam_leaderboard_entry), pack8 ? 32u : 28u);
    EXPECT_EQ(offsetof(rt_steam_leaderboard_entry, score), 12u);
    EXPECT_EQ(offsetof(rt_steam_leaderboard_entry, ugc), pack8 ? 24u : 20u);
}

TEST(ServicesFeatures, NeutralWithoutProvider) {
    rt_services_platform_shutdown();
    const std::string no_provider = "Services: no platform services provider is started";
    Str id("ACH_WIN_ONE_GAME");
    Str stat("NumGames");
    Str board("HOME_RUNS");
    Str key("status");
    Str file("profile.sav");
    Str url("https://example.com");
    Str bad_store("not-a-number");

    for (int64_t feature : kPhase2Features)
        EXPECT_EQ(rt_services_platform_has_feature(feature), 0);
    EXPECT_EQ(rt_services_platform_get_event_total(), 0);

    EXPECT_EQ(rt_services_achievements_unlock(id), 0);
    EXPECT_EQ(rt_services_achievements_clear(id), 0);
    EXPECT_EQ(rt_services_achievements_is_unlocked(id), 0);
    EXPECT_EQ(rt_services_achievements_unlock_time(id), 0);
    EXPECT_EQ(rt_services_achievements_indicate_progress(id, 1, 2), 0);
    EXPECT_EQ(rt_services_achievements_get_count(), 0);
    EXPECT_EQ(take(rt_services_achievements_id_at(0)), std::string(""));
    EXPECT_EQ(take(rt_services_achievements_display_name(id)), std::string(""));
    EXPECT_EQ(take(rt_services_achievements_description(id)), std::string(""));
    EXPECT_EQ(rt_services_achievements_is_hidden(id), 0);

    EXPECT_EQ(rt_services_stats_get_int(stat), 0);
    EXPECT_EQ(rt_services_stats_set_int(stat, 1), 0);
    EXPECT_EQ(rt_services_stats_get_float(stat), 0.0);
    EXPECT_EQ(rt_services_stats_set_float(stat, 1.0), 0);
    EXPECT_EQ(rt_services_stats_update_average_rate(stat, 1.0, 1.0), 0);
    EXPECT_EQ(rt_services_stats_store(), 0);
    EXPECT_EQ(rt_services_stats_reset_all(1), 0);

    void *requests[] = {
        rt_services_leaderboards_find(board),
        rt_services_leaderboards_find_or_create(
            board, RT_SERVICES_LEADERBOARD_SORT_ASCENDING, RT_SERVICES_LEADERBOARD_DISPLAY_NUMERIC),
        rt_services_leaderboards_upload(board, 10, 1),
        rt_services_leaderboards_download(board, RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL, 1, 10),
        rt_services_on_screen_keyboard_request_text(
            key, key, 16, RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE),
    };
    for (void *request : requests) {
        ASSERT_TRUE(request != nullptr);
        EXPECT_EQ(rt_services_request_get_is_done(request), 1);
        EXPECT_EQ(rt_services_request_get_succeeded(request), 0);
        EXPECT_EQ(rt_services_request_get_flag(request), 0);
        EXPECT_EQ(rt_services_request_get_entry_count(request), 0);
        EXPECT_EQ(take(rt_services_request_get_text(request)), std::string(""));
        EXPECT_EQ(take(rt_services_request_get_error(request)), no_provider);
    }
    EXPECT_TRAP_MESSAGE(rt_services_request_entry_score(requests[3], 0),
                        "Services.Request.EntryScore: index 0 is out of range; the request holds "
                        "no entries");
    for (void *request : requests)
        release(request);

    EXPECT_EQ(rt_services_presence_set(key, key), 0);
    rt_services_presence_clear();
    EXPECT_EQ(rt_services_overlay_get_is_enabled(), 0);
    EXPECT_EQ(rt_services_overlay_open(RT_SERVICES_OVERLAY_PAGE_FRIENDS), 0);
    EXPECT_EQ(rt_services_overlay_open_web_page(url, 0), 0);
    // A provider-specific store id format is only checked while that provider is active.
    EXPECT_EQ(rt_services_overlay_open_store(bad_store, 0), 0);
    EXPECT_EQ(
        rt_services_overlay_set_notification_position(RT_SERVICES_NOTIFICATION_POSITION_TOP_LEFT),
        0);
    EXPECT_EQ(rt_services_overlay_set_notification_inset(4, 4), 0);
    EXPECT_EQ(rt_services_on_screen_keyboard_show_floating(
                  RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE, 0, 0, 1, 1),
              0);
    EXPECT_EQ(rt_services_on_screen_keyboard_dismiss_floating(), 0);

    void *data = bytesOf("save");
    EXPECT_EQ(rt_services_cloud_get_is_enabled(), 0);
    EXPECT_EQ(rt_services_cloud_write(file, data), 0);
    void *read = rt_services_cloud_read(file);
    ASSERT_TRUE(read != nullptr);
    EXPECT_EQ(rt_result_is_err(read), 1);
    EXPECT_EQ(std::string(rt_string_cstr(rt_result_unwrap_err_str(read))), no_provider);
    release(read);
    EXPECT_EQ(rt_services_cloud_exists(file), 0);
    EXPECT_EQ(rt_services_cloud_delete(file), 0);
    EXPECT_EQ(rt_services_cloud_size(file), 0);
    EXPECT_EQ(rt_services_cloud_timestamp(file), 0);
    void *files = rt_services_cloud_files();
    ASSERT_TRUE(files != nullptr);
    EXPECT_EQ(rt_seq_len(files), 0);
    release(files);
    EXPECT_EQ(rt_services_cloud_get_quota_total(), 0);
    EXPECT_EQ(rt_services_cloud_get_quota_available(), 0);
    EXPECT_EQ(rt_services_cloud_begin_batch(), 0);
    EXPECT_EQ(rt_services_cloud_end_batch(), 0);
    release(data);
}

TEST(ServicesFeatures, MalformedArgumentsTrap) {
    rt_services_platform_shutdown();
    Str empty("");
    Str name("HOME_RUNS");
    void *data = bytesOf("x");

    EXPECT_TRAP_MESSAGE(rt_services_achievements_unlock(empty),
                        "Services.Achievements.Unlock: achievement id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_achievements_indicate_progress(nullptr, 1, 2),
                        "Services.Achievements.IndicateProgress: achievement id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_stats_set_int(empty, 1),
                        "Services.Stats.SetInt: stat name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_leaderboards_find(empty),
                        "Services.Leaderboards.Find: leaderboard name must not be empty");
    EXPECT_TRAP_MESSAGE(
        rt_services_leaderboards_find_or_create(name, 0, RT_SERVICES_LEADERBOARD_DISPLAY_NUMERIC),
        "Services.Leaderboards.FindOrCreate: sort must be a LeaderboardSort value (got 0)");
    EXPECT_TRAP_MESSAGE(
        rt_services_leaderboards_find_or_create(name, RT_SERVICES_LEADERBOARD_SORT_ASCENDING, 4),
        "Services.Leaderboards.FindOrCreate: display must be a LeaderboardDisplay value (got 4)");
    EXPECT_TRAP_MESSAGE(rt_services_leaderboards_download(name, 3, 1, 2),
                        "Services.Leaderboards.Download: scope must be a LeaderboardScope value "
                        "(got 3)");
    EXPECT_TRAP_MESSAGE(
        rt_services_leaderboards_download(name, RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL, 0, 9),
        "Services.Leaderboards.Download: Global ranks must satisfy 1 <= start <= end (got 0..9)");
    EXPECT_TRAP_MESSAGE(
        rt_services_leaderboards_download(name, RT_SERVICES_LEADERBOARD_SCOPE_AROUND_USER, 5, -5),
        "Services.Leaderboards.Download: start must not exceed end (got 5..-5)");
    EXPECT_TRAP_MESSAGE(
        rt_services_leaderboards_download(name, RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL, 1, 101),
        "Services.Leaderboards.Download: a download spans at most 100 entries (got 1..101)");
    EXPECT_TRAP_MESSAGE(rt_services_leaderboards_download(
                            name, RT_SERVICES_LEADERBOARD_SCOPE_AROUND_USER, INT64_MIN, INT64_MAX),
                        "Services.Leaderboards.Download: a download spans at most 100 entries (got "
                        "-9223372036854775808..9223372036854775807)");
    EXPECT_TRAP_MESSAGE(rt_services_presence_set(empty, name),
                        "Services.Presence.Set: key must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_overlay_open(8),
                        "Services.Overlay.Open: page must be an OverlayPage value (got 8)");
    EXPECT_TRAP_MESSAGE(rt_services_overlay_open_web_page(empty, 0),
                        "Services.Overlay.OpenWebPage: url must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_overlay_open_store(empty, 0),
                        "Services.Overlay.OpenStore: product id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_overlay_set_notification_position(4),
                        "Services.Overlay.SetNotificationPosition: position must be a "
                        "NotificationPosition value (got 4)");
    EXPECT_TRAP_MESSAGE(
        rt_services_on_screen_keyboard_show_floating(5, 0, 0, 1, 1),
        "Services.OnScreenKeyboard.ShowFloating: mode must be a TextInputMode value (got 5)");
    EXPECT_TRAP_MESSAGE(
        rt_services_on_screen_keyboard_request_text(
            name, name, 0, RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE),
        "Services.OnScreenKeyboard.RequestText: maxLength must be in 1..4096 (got 0)");
    EXPECT_TRAP_MESSAGE(
        rt_services_on_screen_keyboard_request_text(name, name, 4097, 0),
        "Services.OnScreenKeyboard.RequestText: maxLength must be in 1..4096 (got 4097)");
    EXPECT_TRAP_MESSAGE(
        rt_services_on_screen_keyboard_request_text(name, name, 16, -1),
        "Services.OnScreenKeyboard.RequestText: mode must be a TextInputMode value (got -1)");
    EXPECT_TRAP_MESSAGE(rt_services_cloud_write(empty, data),
                        "Services.Cloud.Write: file name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_cloud_write(name, nullptr),
                        "Services.Cloud.Write: data must be Zanna.Collections.Bytes");
    EXPECT_TRAP_MESSAGE(rt_services_cloud_delete(empty),
                        "Services.Cloud.Delete: file name must not be empty");
    release(data);
}

TEST(ServicesFeatures, SteamReportsEveryFeature) {
    for (const FakeSteam *fake : {&fake165(), &fake164()}) {
        useFake(*fake);
        ASSERT_TRUE(init("steam", "480").ok);
        for (int64_t feature : kPhase2Features)
            EXPECT_EQ(rt_services_platform_has_feature(feature), 1);
        rt_services_platform_shutdown();
    }
}

TEST(ServicesFeatures, AchievementsUnlockStoreAndReport) {
    const FakeSteam &fake = startSteam();
    (void)fake;
    Str win("ACH_WIN_ONE_GAME");
    Str cycle("ACH_HIT_FOR_CYCLE");
    Str travel("ACH_TRAVEL_FAR");
    Str unknown("ACH_NOPE");

    EXPECT_EQ(rt_services_achievements_get_count(), 3);
    EXPECT_EQ(take(rt_services_achievements_id_at(0)), std::string("ACH_WIN_ONE_GAME"));
    EXPECT_EQ(take(rt_services_achievements_id_at(2)), std::string("ACH_TRAVEL_FAR"));
    EXPECT_EQ(take(rt_services_achievements_id_at(3)), std::string(""));
    EXPECT_EQ(take(rt_services_achievements_id_at(-1)), std::string(""));
    EXPECT_EQ(take(rt_services_achievements_display_name(win)), std::string("Winner"));
    EXPECT_EQ(take(rt_services_achievements_description(win)), std::string("Win one game"));
    EXPECT_EQ(rt_services_achievements_is_hidden(win), 0);
    EXPECT_EQ(rt_services_achievements_is_hidden(cycle), 1);

    EXPECT_EQ(rt_services_achievements_is_unlocked(win), 0);
    EXPECT_EQ(rt_services_achievements_unlock_time(win), 0);
    EXPECT_EQ(rt_services_achievements_unlock(win), 1);
    EXPECT_EQ(rt_services_achievements_is_unlocked(win), 1);
    EXPECT_EQ(rt_services_achievements_unlock_time(win), 1700000000);

    // The Steam client reports each new unlock before the stats commit.
    EXPECT_EQ(rt_services_stats_store(), 1);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_ACHIEVEMENT_STORED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string("ACH_WIN_ONE_GAME"));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    EXPECT_EQ(rt_services_platform_get_event_value(), 0);
    EXPECT_EQ(rt_services_platform_get_event_total(), 0);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_STATS_STORED);
    EXPECT_EQ(rt_services_platform_get_event_result_code(), 1);
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);

    EXPECT_EQ(rt_services_achievements_indicate_progress(travel, 2640, 5280), 1);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_ACHIEVEMENT_STORED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string("ACH_TRAVEL_FAR"));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 0);
    EXPECT_EQ(rt_services_platform_get_event_value(), 2640);
    EXPECT_EQ(rt_services_platform_get_event_total(), 5280);

    EXPECT_EQ(rt_services_achievements_indicate_progress(travel, 5, 3), 0);
    EXPECT_TRUE(hasDiagnostic("Services: Achievements.IndicateProgress('ACH_TRAVEL_FAR') needs "
                              "0 <= current <= maximum and maximum > 0 (got 5 of 3)"));
    EXPECT_EQ(rt_services_achievements_indicate_progress(travel, 0, 10), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: IndicateAchievementProgress('ACH_TRAVEL_FAR', 0, 10) failed; "
                              "the achievement must be defined and locked, and progress must be "
                              "above 0 and below the maximum"));
    EXPECT_EQ(rt_services_achievements_indicate_progress(travel, 1, INT64_C(5000000000)), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: achievement progress for 'ACH_TRAVEL_FAR' must fit in 32 "
                              "bits (got 1 of 5000000000)"));

    EXPECT_EQ(rt_services_achievements_unlock(unknown), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: SetAchievement('ACH_NOPE') failed; check that the "
                              "achievement is defined for this app"));
    EXPECT_EQ(rt_services_achievements_is_unlocked(unknown), 0);

    EXPECT_EQ(rt_services_achievements_clear(win), 1);
    EXPECT_EQ(rt_services_achievements_is_unlocked(win), 0);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, StatsReadWriteAndLimits) {
    const FakeSteam &fake = startSteam();
    Str games("NumGames");
    Str feet("FeetTraveled");
    Str speed("AverageSpeed");

    EXPECT_EQ(rt_services_stats_set_int(games, 12), 1);
    EXPECT_EQ(rt_services_stats_get_int(games), 12);
    EXPECT_EQ(rt_services_stats_set_int(games, INT64_C(3000000000)), 0);
    EXPECT_TRUE(
        hasDiagnostic("Steam: stat 'NumGames' value 3000000000 is outside the int32 range"));
    EXPECT_EQ(rt_services_stats_get_int(games), 12);

    EXPECT_EQ(rt_services_stats_set_int(feet, 5), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: SetStatInt32('FeetTraveled', 5) failed; check the stat's "
                              "type, client write access, and limits"));
    EXPECT_EQ(rt_services_stats_get_int(feet), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: GetStatInt32('FeetTraveled') failed; check that the stat is "
                              "defined as an INT stat"));

    EXPECT_EQ(rt_services_stats_set_float(feet, 1234.5), 1);
    EXPECT_NEAR(rt_services_stats_get_float(feet), 1234.5, 1e-3);
    EXPECT_EQ(rt_services_stats_set_float(feet, std::nan("")), 0);
    EXPECT_TRUE(
        hasDiagnostic("Services: Stats.SetFloat('FeetTraveled') rejected a non-finite value"));
    EXPECT_EQ(rt_services_stats_set_float(feet, 1e39), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: stat 'FeetTraveled' value 1e+39 is outside the float range"));
    EXPECT_NEAR(rt_services_stats_get_float(feet), 1234.5, 1e-3);

    EXPECT_EQ(rt_services_stats_update_average_rate(speed, 90.0, 30.0), 1);
    EXPECT_NEAR(rt_services_stats_get_float(speed), 3.0, 1e-6);
    EXPECT_EQ(rt_services_stats_update_average_rate(speed, 1.0, 0.0), 0);
    EXPECT_TRUE(hasDiagnostic("Services: Stats.UpdateAverageRate('AverageSpeed') needs a finite "
                              "count and a positive, finite session length"));
    EXPECT_EQ(rt_services_stats_set_float(speed, 2.0), 0);

    EXPECT_EQ(rt_services_stats_reset_all(0), 1);
    EXPECT_EQ(rt_services_stats_get_int(games), 0);

    fake.setStoreStatsResult(RT_STEAM_RESULT_INVALID_PARAM);
    EXPECT_EQ(rt_services_stats_store(), 1);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_STATS_STORED);
    EXPECT_EQ(rt_services_platform_get_event_result_code(), 8);
    EXPECT_EQ(rt_services_platform_get_event_flag(), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: StoreStats rejected one or more stats (EResult 8); Steam "
                              "reverted them to the stored values"));
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, LeaderboardsFindUploadAndDownload) {
    const FakeSteam &fake = startSteam();
    (void)fake;
    Str board("HOME_RUNS");
    Str missing("NOPE");
    Str season("SEASON_1972");

    void *find = rt_services_leaderboards_find(board);
    EXPECT_EQ(rt_services_request_get_kind(find), RT_SERVICES_REQUEST_LEADERBOARD_FIND);
    EXPECT_EQ(rt_services_request_get_is_done(find), 0);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_request_get_is_done(find), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(find), 1);
    EXPECT_EQ(rt_services_request_get_result_code(find), 1);
    EXPECT_EQ(rt_services_request_get_value(find), 3);
    EXPECT_EQ(take(rt_services_request_get_text(find)), std::string("HOME_RUNS"));
    release(find);

    void *lost = rt_services_leaderboards_find(missing);
    ASSERT_TRUE(pumpUntilDone(lost, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(lost), 0);
    EXPECT_EQ(take(rt_services_request_get_error(lost)),
              std::string("Steam: leaderboard 'NOPE' was not found"));
    release(lost);

    void *made = rt_services_leaderboards_find_or_create(
        season, RT_SERVICES_LEADERBOARD_SORT_ASCENDING, RT_SERVICES_LEADERBOARD_DISPLAY_SECONDS);
    ASSERT_TRUE(pumpUntilDone(made, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(made), 1);
    EXPECT_EQ(rt_services_request_get_value(made), 0);
    EXPECT_EQ(take(rt_services_request_get_text(made)), std::string("SEASON_1972"));
    release(made);

    // HOME_RUNS is cached now, so the upload is a single Steam call.
    void *best = rt_services_leaderboards_upload(board, 70, 1);
    EXPECT_EQ(rt_services_request_get_kind(best), RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_request_get_is_done(best), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(best), 1);
    EXPECT_EQ(rt_services_request_get_value(best), 1);
    EXPECT_EQ(rt_services_request_get_flag(best), 1);
    EXPECT_EQ(take(rt_services_request_get_text(best)), std::string("HOME_RUNS"));
    release(best);

    void *worse = rt_services_leaderboards_upload(board, 5, 1);
    ASSERT_TRUE(pumpUntilDone(worse, 1));
    EXPECT_EQ(rt_services_request_get_succeeded(worse), 1);
    EXPECT_EQ(rt_services_request_get_flag(worse), 0);
    EXPECT_EQ(rt_services_request_get_value(worse), 1);
    release(worse);

    void *top =
        rt_services_leaderboards_download(board, RT_SERVICES_LEADERBOARD_SCOPE_GLOBAL, 1, 3);
    EXPECT_EQ(rt_services_request_get_kind(top), RT_SERVICES_REQUEST_LEADERBOARD_DOWNLOAD);
    ASSERT_TRUE(pumpUntilDone(top, 1));
    EXPECT_EQ(rt_services_request_get_succeeded(top), 1);
    EXPECT_EQ(rt_services_request_get_value(top), 3);
    EXPECT_EQ(take(rt_services_request_get_text(top)), std::string("HOME_RUNS"));
    ASSERT_EQ(rt_services_request_get_entry_count(top), 3);
    EXPECT_EQ(rt_services_request_entry_rank(top, 0), 1);
    EXPECT_EQ(rt_services_request_entry_score(top, 0), 70);
    EXPECT_EQ(take(rt_services_request_entry_user_id(top, 0)), std::string("76561198000000001"));
    EXPECT_EQ(take(rt_services_request_entry_user_name(top, 0)), std::string("Zanna Tester"));
    EXPECT_EQ(rt_services_request_entry_rank(top, 1), 2);
    EXPECT_EQ(rt_services_request_entry_score(top, 1), 61);
    EXPECT_EQ(take(rt_services_request_entry_user_name(top, 1)), std::string("Slugger"));
    EXPECT_EQ(rt_services_request_entry_score(top, 2), 30);
    // Steam learns the third name only after RequestUserInformation and a later frame.
    EXPECT_EQ(take(rt_services_request_entry_user_name(top, 2)), std::string(""));
    rt_services_platform_update();
    EXPECT_EQ(take(rt_services_request_entry_user_name(top, 2)), std::string("Rookie"));
    EXPECT_TRAP_MESSAGE(rt_services_request_entry_rank(top, 3),
                        "Services.Request.EntryRank: index 3 is outside 0..2");
    EXPECT_TRAP_MESSAGE(rt_services_request_entry_user_id(top, -1),
                        "Services.Request.EntryUserId: index -1 is outside 0..2");

    void *around =
        rt_services_leaderboards_download(board, RT_SERVICES_LEADERBOARD_SCOPE_AROUND_USER, -1, 1);
    ASSERT_TRUE(pumpUntilDone(around, 1));
    ASSERT_EQ(rt_services_request_get_entry_count(around), 2);
    EXPECT_EQ(rt_services_request_entry_rank(around, 0), 1);
    EXPECT_EQ(rt_services_request_entry_rank(around, 1), 2);
    release(around);

    void *friends =
        rt_services_leaderboards_download(board, RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS, 0, 0);
    ASSERT_TRUE(pumpUntilDone(friends, 1));
    EXPECT_EQ(rt_services_request_get_entry_count(friends), 3);

    rt_services_platform_shutdown();
    // After shutdown, names come from the snapshot taken at completion.
    EXPECT_EQ(take(rt_services_request_entry_user_name(friends, 2)), std::string("Rookie"));
    EXPECT_EQ(take(rt_services_request_entry_user_name(top, 2)), std::string(""));
    release(friends);
    release(top);

    // A new session starts with an empty handle cache: the upload runs the find first.
    ASSERT_TRUE(init("steam", "480").ok);
    void *chained = rt_services_leaderboards_upload(board, 80, 0);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_request_get_is_done(chained), 0);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_request_get_is_done(chained), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(chained), 1);
    EXPECT_EQ(rt_services_request_get_value(chained), 1);
    EXPECT_EQ(rt_services_request_get_flag(chained), 1);
    release(chained);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, LeaderboardProviderLimits) {
    const FakeSteam &fake = startSteam();
    Str board("HOME_RUNS");
    const std::string long_name(128, 'x');
    Str too_long(long_name.c_str());

    void *named = rt_services_leaderboards_find(too_long);
    EXPECT_EQ(rt_services_request_get_is_done(named), 1);
    EXPECT_EQ(take(rt_services_request_get_error(named)),
              "Steam: leaderboard name '" + long_name + "' is longer than 127 bytes");
    release(named);

    void *huge = rt_services_leaderboards_upload(board, INT64_C(3000000000), 1);
    EXPECT_EQ(rt_services_request_get_is_done(huge), 1);
    EXPECT_EQ(take(rt_services_request_get_error(huge)),
              std::string("Steam: leaderboard score 3000000000 is outside the int32 range"));
    release(huge);

    fake.setUploadSuccess(0);
    void *failed = rt_services_leaderboards_upload(board, 1, 1);
    ASSERT_TRUE(pumpUntilDone(failed, 3));
    EXPECT_EQ(rt_services_request_get_succeeded(failed), 0);
    EXPECT_EQ(take(rt_services_request_get_error(failed)),
              std::string("Steam: score upload to leaderboard 'HOME_RUNS' failed"));
    release(failed);

    fake.fillLeaderboard("HOME_RUNS", 150);
    void *crowd =
        rt_services_leaderboards_download(board, RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS, 0, 0);
    ASSERT_TRUE(pumpUntilDone(crowd, 3));
    EXPECT_EQ(rt_services_request_get_succeeded(crowd), 1);
    EXPECT_EQ(rt_services_request_get_value(crowd), 150);
    ASSERT_EQ(rt_services_request_get_entry_count(crowd), RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY);
    EXPECT_EQ(rt_services_request_entry_rank(crowd, 99), 100);
    EXPECT_TRUE(hasDiagnostic(
        "Steam: leaderboard 'HOME_RUNS' download returned 150 entries; kept the first 100"));
    release(crowd);

    // Every entry was read, so the fake released its download record and a
    // further download of all 150 entries still succeeds.
    void *again =
        rt_services_leaderboards_download(board, RT_SERVICES_LEADERBOARD_SCOPE_FRIENDS, 0, 0);
    ASSERT_TRUE(pumpUntilDone(again, 1));
    EXPECT_EQ(rt_services_request_get_succeeded(again), 1);
    release(again);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, PresenceSetAndClear) {
    const FakeSteam &fake = startSteam();
    Str status("status");
    Str batting("Batting .300");
    Str empty("");
    const std::string long_key(64, 'k');
    Str too_long(long_key.c_str());

    EXPECT_EQ(rt_services_presence_set(status, batting), 1);
    EXPECT_EQ(fake.richPresence("status"), std::string("Batting .300"));
    EXPECT_EQ(rt_services_presence_set(status, empty), 1);
    EXPECT_EQ(fake.richPresence("status"), std::string("<unset>"));
    EXPECT_EQ(rt_services_presence_set(too_long, batting), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: SetRichPresence('" + long_key +
                              "') failed; keys must be shorter than 64 bytes, values shorter than "
                              "256 bytes, and at most 30 keys may be set"));
    EXPECT_EQ(rt_services_presence_set(status, batting), 1);
    rt_services_presence_clear();
    EXPECT_EQ(fake.richPresence("status"), std::string("<unset>"));
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, OverlayRequestsReachSteam) {
    const FakeSteam &fake = startSteam();
    Str url("https://example.com/news");
    Str app("480");
    Str malformed("abc");

    EXPECT_EQ(rt_services_overlay_get_is_enabled(), 1);
    fake.setOverlayEnabled(0);
    EXPECT_EQ(rt_services_overlay_get_is_enabled(), 0);

    const struct {
        int64_t page;
        const char *dialog;
    } pages[] = {
        {RT_SERVICES_OVERLAY_PAGE_FRIENDS, "friends"},
        {RT_SERVICES_OVERLAY_PAGE_COMMUNITY, "community"},
        {RT_SERVICES_OVERLAY_PAGE_PLAYERS, "players"},
        {RT_SERVICES_OVERLAY_PAGE_SETTINGS, "settings"},
        {RT_SERVICES_OVERLAY_PAGE_OFFICIAL_GROUP, "officialgamegroup"},
        {RT_SERVICES_OVERLAY_PAGE_STATS, "stats"},
        {RT_SERVICES_OVERLAY_PAGE_ACHIEVEMENTS, "achievements"},
    };

    for (const auto &page : pages) {
        EXPECT_EQ(rt_services_overlay_open(page.page), 1);
        EXPECT_EQ(fake.lastUiCall(), std::string("ActivateGameOverlay(") + page.dialog + ")");
    }

    EXPECT_EQ(rt_services_overlay_open_web_page(url, 1), 1);
    EXPECT_EQ(fake.lastUiCall(),
              std::string("ActivateGameOverlayToWebPage(https://example.com/news,1)"));
    EXPECT_EQ(rt_services_overlay_open_web_page(url, 0), 1);
    EXPECT_EQ(fake.lastUiCall(),
              std::string("ActivateGameOverlayToWebPage(https://example.com/news,0)"));
    EXPECT_EQ(rt_services_overlay_open_store(app, 1), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ActivateGameOverlayToStore(480,2)"));
    EXPECT_EQ(rt_services_overlay_open_store(app, 0), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ActivateGameOverlayToStore(480,0)"));
    EXPECT_TRAP_MESSAGE(rt_services_overlay_open_store(malformed, 0),
                        "Services.Overlay.OpenStore: Steam app id 'abc' must be an integer in "
                        "1..4294967295");

    EXPECT_EQ(rt_services_overlay_set_notification_position(
                  RT_SERVICES_NOTIFICATION_POSITION_BOTTOM_RIGHT),
              1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetOverlayNotificationPosition(3)"));
    EXPECT_EQ(rt_services_overlay_set_notification_inset(12, 34), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetOverlayNotificationInset(12,34)"));
    EXPECT_EQ(rt_services_overlay_set_notification_inset(INT64_C(1099511627776), 0), 0);
    EXPECT_TRUE(
        hasDiagnostic("Steam: notification inset 1099511627776x0 is outside the int32 range"));
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, TextInputKeyboards) {
    const FakeSteam &fake = startSteam();
    Str prompt("Team name");
    Str initial("Sox");

    EXPECT_EQ(rt_services_on_screen_keyboard_show_floating(
                  RT_SERVICES_TEXT_INPUT_MODE_EMAIL, 10, 20, 300, 40),
              1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ShowFloatingGamepadTextInput(2,10,20,300,40)"));
    EXPECT_EQ(rt_services_on_screen_keyboard_show_floating(
                  RT_SERVICES_TEXT_INPUT_MODE_PASSWORD, 0, 0, 1, 1),
              1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ShowFloatingGamepadTextInput(0,0,0,1,1)"));
    EXPECT_EQ(rt_services_on_screen_keyboard_show_floating(
                  RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE, 0, 0, -1, 5),
              0);
    EXPECT_TRUE(hasDiagnostic("Services: OnScreenKeyboard.ShowFloating needs a non-negative width "
                              "and height (got -1x5)"));
    EXPECT_EQ(rt_services_on_screen_keyboard_dismiss_floating(), 1);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_TEXT_INPUT_DISMISSED);

    fake.setTextInput("Boston Nine", 1);
    void *entry = rt_services_on_screen_keyboard_request_text(
        prompt, initial, 32, RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE);
    EXPECT_EQ(rt_services_request_get_kind(entry), RT_SERVICES_REQUEST_TEXT_INPUT);
    EXPECT_EQ(fake.lastUiCall(), std::string("ShowGamepadTextInput(0,0,Team name,32,Sox)"));
    EXPECT_EQ(rt_services_request_get_is_done(entry), 0);
    void *second = rt_services_on_screen_keyboard_request_text(
        prompt, initial, 32, RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE);
    EXPECT_EQ(rt_services_request_get_is_done(second), 1);
    EXPECT_EQ(take(rt_services_request_get_error(second)),
              std::string("Steam: a text input request is already pending"));
    release(second);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_request_get_is_done(entry), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(entry), 1);
    EXPECT_EQ(take(rt_services_request_get_text(entry)), std::string("Boston Nine"));
    EXPECT_EQ(rt_services_request_get_value(entry), 11);
    release(entry);

    fake.setTextInput("", 0);
    void *secret = rt_services_on_screen_keyboard_request_text(
        prompt, initial, 16, RT_SERVICES_TEXT_INPUT_MODE_PASSWORD);
    EXPECT_EQ(fake.lastUiCall(), std::string("ShowGamepadTextInput(1,0,Team name,16,Sox)"));
    ASSERT_TRUE(pumpUntilDone(secret, 1));
    EXPECT_EQ(rt_services_request_get_succeeded(secret), 0);
    EXPECT_EQ(take(rt_services_request_get_error(secret)),
              std::string("Steam: text input was cancelled"));
    release(secret);

    void *lines = rt_services_on_screen_keyboard_request_text(
        prompt, initial, 64, RT_SERVICES_TEXT_INPUT_MODE_MULTI_LINE);
    EXPECT_EQ(fake.lastUiCall(), std::string("ShowGamepadTextInput(0,1,Team name,64,Sox)"));
    ASSERT_TRUE(pumpUntilDone(lines, 1));
    release(lines);

    fake.setKeyboardsSupported(0);
    EXPECT_EQ(rt_services_on_screen_keyboard_show_floating(
                  RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE, 0, 0, 1, 1),
              0);
    EXPECT_TRUE(hasDiagnostic("Steam: ShowFloatingGamepadTextInput returned false; the floating "
                              "keyboard needs Steam Deck or Big Picture mode"));
    void *refused = rt_services_on_screen_keyboard_request_text(
        prompt, initial, 16, RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE);
    EXPECT_EQ(rt_services_request_get_is_done(refused), 1);
    EXPECT_EQ(take(rt_services_request_get_error(refused)),
              std::string("Steam: the gamepad text input could not be shown; it needs Steam Deck "
                          "or Big Picture mode"));
    release(refused);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, CloudFileRoundTrip) {
    const FakeSteam &fake = startSteam();
    Str name("profile.sav");
    Str empty_name("empty.sav");
    void *data = bytesOf("season 1972");
    void *nothing = bytesOf("");

    EXPECT_EQ(rt_services_cloud_get_is_enabled(), 1);
    fake.setCloudAccountEnabled(0);
    EXPECT_EQ(rt_services_cloud_get_is_enabled(), 0);
    fake.setCloudAccountEnabled(1);
    EXPECT_EQ(rt_services_cloud_get_quota_total(), 1048576);
    EXPECT_EQ(rt_services_cloud_get_quota_available(), 1048576);

    EXPECT_EQ(rt_services_cloud_exists(name), 0);
    EXPECT_EQ(rt_services_cloud_write(name, data), 1);
    EXPECT_EQ(rt_services_cloud_exists(name), 1);
    EXPECT_EQ(rt_services_cloud_size(name), 11);
    EXPECT_EQ(rt_services_cloud_timestamp(name), 1700000100);
    EXPECT_EQ(rt_services_cloud_get_quota_available(), 1048576 - 11);

    void *read = rt_services_cloud_read(name);
    ASSERT_TRUE(read != nullptr);
    ASSERT_EQ(rt_result_is_ok(read), 1);
    void *contents = rt_result_unwrap(read);
    ASSERT_TRUE(contents != nullptr);
    ASSERT_EQ(rt_bytes_len(contents), 11);
    EXPECT_EQ(std::string((const char *)rt_bytes_data_const(contents), 11),
              std::string("season 1972"));
    release(read);

    EXPECT_EQ(rt_services_cloud_write(empty_name, nothing), 1);
    EXPECT_EQ(rt_services_cloud_size(empty_name), 0);
    void *empty_read = rt_services_cloud_read(empty_name);
    ASSERT_EQ(rt_result_is_ok(empty_read), 1);
    EXPECT_EQ(rt_bytes_len(rt_result_unwrap(empty_read)), 0);
    release(empty_read);

    void *files = rt_services_cloud_files();
    ASSERT_EQ(rt_seq_len(files), 2);
    EXPECT_EQ(take(rt_seq_get_str(files, 0)), std::string("profile.sav"));
    EXPECT_EQ(take(rt_seq_get_str(files, 1)), std::string("empty.sav"));
    release(files);

    EXPECT_EQ(rt_services_cloud_begin_batch(), 1);
    EXPECT_EQ(rt_services_cloud_begin_batch(), 0);
    EXPECT_EQ(rt_services_cloud_end_batch(), 1);
    EXPECT_EQ(rt_services_cloud_end_batch(), 0);

    EXPECT_EQ(rt_services_cloud_delete(name), 1);
    EXPECT_EQ(rt_services_cloud_exists(name), 0);
    EXPECT_EQ(rt_services_cloud_size(name), 0);
    EXPECT_EQ(rt_services_cloud_delete(name), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: FileDelete('profile.sav') failed"));
    void *gone = rt_services_cloud_read(name);
    ASSERT_EQ(rt_result_is_err(gone), 1);
    EXPECT_EQ(std::string(rt_string_cstr(rt_result_unwrap_err_str(gone))),
              std::string("Steam: cloud file 'profile.sav' does not exist"));
    release(gone);

    release(data);
    release(nothing);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, CoreOnlyRedistributableDisablesFeatures) {
    useFake(fakeCore());
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_IDENTITY), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_PLAYER_COUNT), 1);
    for (int64_t feature : kPhase2Features)
        EXPECT_EQ(rt_services_platform_has_feature(feature), 0);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_LICENSING), 1);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_LAUNCH_PARAMETERS), 0);
    for (const char *message : {
             "Steam: export SteamAPI_ISteamApps_GetLaunchQueryParam unavailable; launch "
             "parameters disabled",
             "Steam: export SteamAPI_ISteamApps_GetDLCCount unavailable; DLC list, build id, and "
             "branch queries disabled",
             "Steam: export SteamAPI_ISteamUserStats_SetAchievement unavailable; achievements "
             "disabled",
             "Steam: export SteamAPI_ISteamUserStats_GetStatInt32 unavailable; stats disabled",
             "Steam: export SteamAPI_ISteamUserStats_FindLeaderboard unavailable; leaderboards "
             "disabled",
             "Steam: export SteamAPI_ISteamFriends_SetRichPresence unavailable; rich presence "
             "disabled",
             "Steam: export SteamAPI_ISteamFriends_ActivateGameOverlay unavailable; overlay pages "
             "disabled",
             "Steam: export SteamAPI_ISteamFriends_GetFriendPersonaName unavailable; leaderboard "
             "user names disabled",
             "Steam: export SteamAPI_ISteamUtils_IsOverlayEnabled unavailable; overlay status and "
             "notification placement disabled",
             "Steam: export SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput unavailable; text "
             "input disabled",
             "Steam: interface SteamAPI_SteamRemoteStorage_v016 unavailable; cloud storage "
             "disabled",
         }) {
        EXPECT_TRUE(hasDiagnostic(message));
    }

    Str id("ACH_WIN_ONE_GAME");
    Str board("HOME_RUNS");
    Str file("profile.sav");
    EXPECT_EQ(rt_services_achievements_unlock(id), 0);
    EXPECT_EQ(rt_services_stats_store(), 0);
    EXPECT_EQ(rt_services_overlay_open(RT_SERVICES_OVERLAY_PAGE_FRIENDS), 0);
    void *find = rt_services_leaderboards_find(board);
    EXPECT_EQ(take(rt_services_request_get_error(find)),
              std::string("Steam: leaderboards are unavailable (see Platform.Diagnostics)"));
    release(find);
    void *text = rt_services_on_screen_keyboard_request_text(file, file, 8, 0);
    EXPECT_EQ(take(rt_services_request_get_error(text)),
              std::string("Steam: text input is unavailable (see Platform.Diagnostics)"));
    release(text);
    void *read = rt_services_cloud_read(file);
    EXPECT_EQ(std::string(rt_string_cstr(rt_result_unwrap_err_str(read))),
              std::string("Steam: cloud storage is unavailable"));
    release(read);

    void *players = rt_services_platform_request_player_count();
    ASSERT_TRUE(pumpUntilDone(players, 1));
    EXPECT_EQ(rt_services_request_get_value(players), 42);
    release(players);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, ShutdownCancelsMultiCallRequests) {
    const FakeSteam &fake = startSteam();
    Str board("HOME_RUNS");
    Str prompt("Name");

    // The first frame resolves the board and starts the upload call; the text
    // request starts afterwards so neither has completed at shutdown.
    void *upload = rt_services_leaderboards_upload(board, 99, 1);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_request_get_is_done(upload), 0);
    void *text = rt_services_on_screen_keyboard_request_text(prompt, prompt, 8, 0);
    EXPECT_EQ(rt_services_request_get_is_done(text), 0);
    rt_services_platform_shutdown();
    for (void *request : {upload, text}) {
        EXPECT_EQ(rt_services_request_get_is_done(request), 1);
        EXPECT_EQ(rt_services_request_get_succeeded(request), 0);
        EXPECT_EQ(take(rt_services_request_get_error(request)),
                  std::string("Services: request cancelled by Platform.Shutdown()"));
    }
    release(upload);
    release(text);

    ASSERT_TRUE(init("steam", "480").ok);
    rt_services_platform_update();
    rt_services_platform_update();
    EXPECT_EQ(fake.protocolViolation(), 0);
    void *fresh = rt_services_on_screen_keyboard_request_text(prompt, prompt, 8, 0);
    ASSERT_TRUE(pumpUntilDone(fresh, 1));
    EXPECT_EQ(rt_services_request_get_succeeded(fresh), 1);
    release(fresh);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, RepeatedDiagnosticIsRecordedOnce) {
    startSteam();
    Str id("ACH_DIAGNOSTIC_REPEAT");
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(rt_services_achievements_unlock(id), 0);
    EXPECT_EQ(countDiagnostic("Steam: SetAchievement('ACH_DIAGNOSTIC_REPEAT') failed; check that "
                              "the achievement is defined for this app"),
              1);
    rt_services_platform_shutdown();
}

TEST(ServicesFeatures, StatefulMembersRequireMainThread) {
    startSteam();
    Str board("HOME_RUNS");
    void *request = rt_services_leaderboards_find(board);
    g_trap_jump = false;

    g_last_trap.clear();
    int8_t stored = -1;
    std::thread store_worker([&stored]() { stored = rt_services_stats_store(); });
    store_worker.join();
    EXPECT_EQ(g_last_trap, std::string("Services: Stats.Store must be called on the main thread"));
    EXPECT_EQ(stored, 0);

    g_last_trap.clear();
    void *started = &g_last_trap;
    std::thread find_worker(
        [&started, &board]() { started = rt_services_leaderboards_find(board); });
    find_worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: Leaderboards.Find must be called on the main thread"));
    EXPECT_TRUE(started == nullptr);

    g_last_trap.clear();
    int64_t rank = -1;
    std::thread rank_worker(
        [&rank, request]() { rank = rt_services_request_entry_rank(request, 0); });
    rank_worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: Request.EntryRank must be called on the main thread"));
    EXPECT_EQ(rank, 0);

    release(request);
    rt_services_platform_shutdown();
}

int main(int argc, char **argv) {
    rt_set_main_thread();
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
