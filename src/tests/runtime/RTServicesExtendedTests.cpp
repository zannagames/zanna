//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTServicesExtendedTests.cpp
// Purpose: Verify the Zanna.Services features added after ADR 0353 against the
//          fake steam_api libraries: the recording timeline (ADR 0362) and the
//          request details it introduces, app details (ADR 0363), and
//          achievement icons and global unlock percentages (ADR 0364).
// Key invariants:
//   - Every test starts from a shut-down session and a reset fake library.
//   - Fixtures (fake library view, trap capture, helpers) come from
//     RTServicesTestSupport.hpp.
// Ownership/Lifetime:
//   - Tests release every Request and string they receive.
// Links: src/runtime/services/rt_services_timeline.c,
//        src/runtime/services/steam/rt_steam_timeline.c,
//        src/runtime/services/steam/rt_steam_user_stats.c,
//        src/tests/runtime/RTServicesFakeSteamApi.c,
//        docs/adr/0362-platform-services-timeline.md,
//        docs/adr/0363-platform-services-app-details.md,
//        docs/adr/0364-platform-services-achievement-icons-and-percentages.md
//
//===----------------------------------------------------------------------===//

#include "RTServicesTestSupport.hpp"

#include "rt_bytes.h"
#include "rt_services_progress.h"
#include "rt_services_timeline.h"
#include "rt_steam_abi.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <thread>

using namespace services_test;

namespace {

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

/// @brief Add a standard instantaneous event and return its id.
std::string addEvent(const char *title) {
    Str t(title);
    Str description("");
    Str icon("steam_star");
    return take(rt_services_timeline_add_event(
        t, description, icon, 10, 0.0, RT_SERVICES_TIMELINE_CLIP_STANDARD));
}

} // namespace

TEST(ServicesExtended, ConstantsMatchAdr) {
    EXPECT_EQ(rt_services_feature_timeline(), 13);
    EXPECT_EQ(rt_services_request_kind_timeline_event_recording(), 6);
    EXPECT_EQ(rt_services_request_kind_timeline_phase_recording(), 7);
    EXPECT_EQ(rt_services_timeline_mode_playing(), 1);
    EXPECT_EQ(rt_services_timeline_mode_staging(), 2);
    EXPECT_EQ(rt_services_timeline_mode_menus(), 3);
    EXPECT_EQ(rt_services_timeline_mode_loading_screen(), 4);
    EXPECT_EQ(rt_services_timeline_clip_none(), 1);
    EXPECT_EQ(rt_services_timeline_clip_standard(), 2);
    EXPECT_EQ(rt_services_timeline_clip_featured(), 3);
    EXPECT_EQ(rt_services_event_kind_achievement_icon_ready(), 11);
    EXPECT_EQ(rt_services_feature_achievement_icons(), 15);
    EXPECT_EQ(rt_services_feature_achievement_percentages(), 16);
    EXPECT_EQ(rt_services_request_kind_achievement_percentages(), 8);
}

TEST(ServicesExtended, TimelineLayoutsMatchRedistributablePacking) {
    const bool pack8 = RT_STEAM_CALLBACK_PACK == 8;
    EXPECT_EQ(sizeof(rt_steam_timeline_phase_recording_exists), 88u);
    EXPECT_EQ(offsetof(rt_steam_timeline_phase_recording_exists, recording_ms), 64u);
    EXPECT_EQ(offsetof(rt_steam_timeline_phase_recording_exists, longest_clip_ms), 72u);
    EXPECT_EQ(offsetof(rt_steam_timeline_phase_recording_exists, clip_count), 80u);
    EXPECT_EQ(offsetof(rt_steam_timeline_phase_recording_exists, screenshot_count), 84u);
    EXPECT_EQ(sizeof(rt_steam_timeline_event_recording_exists), pack8 ? 16u : 12u);
    EXPECT_EQ(offsetof(rt_steam_timeline_event_recording_exists, recording_exists), 8u);
}

TEST(ServicesExtended, TimelineNeutralWithoutProvider) {
    rt_services_platform_shutdown();
    Str title("Home run");
    Str text("Top 9th");
    Str event_id("12345");
    Str phase("game-1");
    Str group("Opponent");
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_TIMELINE), 0);
    EXPECT_EQ(rt_services_timeline_set_game_mode(RT_SERVICES_TIMELINE_MODE_PLAYING), 0);
    EXPECT_EQ(rt_services_timeline_set_tooltip(text, 0.0), 0);
    EXPECT_EQ(rt_services_timeline_clear_tooltip(0.0), 0);
    EXPECT_EQ(take(rt_services_timeline_add_event(title, text, text, 1, 0.0, 1)), std::string(""));
    EXPECT_EQ(take(rt_services_timeline_add_range_event(title, text, text, 1, 0.0, 5.0, 1)),
              std::string(""));
    EXPECT_EQ(take(rt_services_timeline_start_range_event(title, text, text, 1, 0.0, 1)),
              std::string(""));
    EXPECT_EQ(rt_services_timeline_update_range_event(event_id, title, text, text, -1, 1), 0);
    EXPECT_EQ(rt_services_timeline_end_range_event(event_id, 0.0), 0);
    EXPECT_EQ(rt_services_timeline_remove_event(event_id), 0);
    EXPECT_EQ(rt_services_timeline_start_phase(), 0);
    EXPECT_EQ(rt_services_timeline_end_phase(), 0);
    EXPECT_EQ(rt_services_timeline_set_phase_id(phase), 0);
    EXPECT_EQ(rt_services_timeline_add_phase_tag(title, text, group, 1), 0);
    EXPECT_EQ(rt_services_timeline_set_phase_attribute(group, text, 1), 0);
    EXPECT_EQ(rt_services_timeline_open_overlay_to_phase(phase), 0);
    // A provider-specific event id format is only checked while that provider is active.
    Str malformed("not-a-number");
    EXPECT_EQ(rt_services_timeline_open_overlay_to_event(malformed), 0);

    for (void *request : {rt_services_timeline_request_event_recording(event_id),
                          rt_services_timeline_request_phase_recording(phase)}) {
        ASSERT_TRUE(request != nullptr);
        EXPECT_EQ(rt_services_request_get_is_done(request), 1);
        EXPECT_EQ(rt_services_request_get_succeeded(request), 0);
        EXPECT_EQ(rt_services_request_get_detail_count(request), 0);
        EXPECT_EQ(take(rt_services_request_get_error(request)),
                  std::string("Services: no platform services provider is started"));
        EXPECT_TRAP_MESSAGE(
            rt_services_request_detail(request, 0),
            "Services.Request.Detail: index 0 is out of range; the request holds no details");
        release(request);
    }
}

TEST(ServicesExtended, TimelineMalformedArgumentsTrap) {
    rt_services_platform_shutdown();
    Str empty("");
    Str title("Home run");
    Str id("12345");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_set_game_mode(0),
                        "Services.Timeline.SetGameMode: mode must be a TimelineMode value (got 0)");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_set_game_mode(5),
                        "Services.Timeline.SetGameMode: mode must be a TimelineMode value (got 5)");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_set_tooltip(empty, 0.0),
                        "Services.Timeline.SetTooltip: text must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_add_event(empty, empty, empty, 1, 0.0, 1),
                        "Services.Timeline.AddEvent: title must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_add_event(title, empty, empty, 1001, 0.0, 1),
                        "Services.Timeline.AddEvent: priority must be in 0..1000 (got 1001)");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_add_event(title, empty, empty, -1, 0.0, 1),
                        "Services.Timeline.AddEvent: priority must be in 0..1000 (got -1)");
    EXPECT_TRAP_MESSAGE(
        rt_services_timeline_add_range_event(title, empty, empty, 1, 0.0, 1.0, 0),
        "Services.Timeline.AddRangeEvent: clip must be a TimelineClip value (got 0)");
    EXPECT_TRAP_MESSAGE(
        rt_services_timeline_start_range_event(title, empty, empty, 1, 0.0, 4),
        "Services.Timeline.StartRangeEvent: clip must be a TimelineClip value (got 4)");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_update_range_event(empty, title, empty, empty, 1, 1),
                        "Services.Timeline.UpdateRangeEvent: event id must not be empty");
    EXPECT_TRAP_MESSAGE(
        rt_services_timeline_update_range_event(id, title, empty, empty, -2, 1),
        "Services.Timeline.UpdateRangeEvent: priority must be in 0..1000 or -1 (got -2)");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_end_range_event(empty, 0.0),
                        "Services.Timeline.EndRangeEvent: event id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_remove_event(nullptr),
                        "Services.Timeline.RemoveEvent: event id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_request_event_recording(empty),
                        "Services.Timeline.RequestEventRecording: event id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_set_phase_id(empty),
                        "Services.Timeline.SetPhaseId: phase id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_add_phase_tag(empty, empty, title, 1),
                        "Services.Timeline.AddPhaseTag: tag name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_add_phase_tag(title, empty, empty, 1),
                        "Services.Timeline.AddPhaseTag: tag group must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_set_phase_attribute(empty, title, 1),
                        "Services.Timeline.SetPhaseAttribute: attribute group must not be empty");
    EXPECT_TRAP_MESSAGE(
        rt_services_timeline_set_phase_attribute(title, title, 2000),
        "Services.Timeline.SetPhaseAttribute: priority must be in 0..1000 (got 2000)");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_request_phase_recording(empty),
                        "Services.Timeline.RequestPhaseRecording: phase id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_open_overlay_to_phase(empty),
                        "Services.Timeline.OpenOverlayToPhase: phase id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_open_overlay_to_event(empty),
                        "Services.Timeline.OpenOverlayToEvent: event id must not be empty");
}

TEST(ServicesExtended, TimelineCallsReachSteam) {
    const FakeSteam &fake = startSteam();
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_TIMELINE), 1);

    EXPECT_EQ(rt_services_timeline_set_game_mode(RT_SERVICES_TIMELINE_MODE_STAGING), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetTimelineGameMode(2)"));
    Str tooltip("Top 9th, 3-2");
    EXPECT_EQ(rt_services_timeline_set_tooltip(tooltip, -2.5), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetTimelineTooltip(Top 9th, 3-2,-2.5)"));
    EXPECT_EQ(rt_services_timeline_clear_tooltip(1.0), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ClearTimelineTooltip(1.0)"));

    Str title("Home run");
    Str description("Two-run shot");
    Str icon("steam_star");
    std::string first = take(rt_services_timeline_add_event(
        title, description, icon, 900, -3.0, RT_SERVICES_TIMELINE_CLIP_FEATURED));
    EXPECT_EQ(first, std::string("485331304449"));
    EXPECT_EQ(fake.lastUiCall(),
              std::string("AddInstantaneousTimelineEvent(Home run,Two-run shot,steam_star,900,"
                          "-3.0,3)"));
    Str rally("Rally");
    std::string range = take(rt_services_timeline_add_range_event(
        rally, description, icon, 0, -30.0, 25.0, RT_SERVICES_TIMELINE_CLIP_NONE));
    EXPECT_EQ(range, std::string("485331304450"));
    EXPECT_EQ(fake.lastUiCall(),
              std::string("AddRangeTimelineEvent(Rally,Two-run shot,steam_star,0,-30.0,25.0,1)"));

    std::string open = take(rt_services_timeline_start_range_event(
        rally, description, icon, 500, 0.0, RT_SERVICES_TIMELINE_CLIP_STANDARD));
    EXPECT_EQ(open, std::string("485331304451"));
    Str open_id(open.c_str());
    Str updated("Big rally");
    EXPECT_EQ(rt_services_timeline_update_range_event(
                  open_id, updated, description, icon, -1, RT_SERVICES_TIMELINE_CLIP_FEATURED),
              1);
    EXPECT_EQ(fake.lastUiCall(),
              std::string("UpdateRangeTimelineEvent(485331304451,Big rally,Two-run shot,"
                          "steam_star,1000000,3)"));
    EXPECT_EQ(rt_services_timeline_update_range_event(
                  open_id, updated, description, icon, 700, RT_SERVICES_TIMELINE_CLIP_FEATURED),
              1);
    EXPECT_EQ(fake.lastUiCall(),
              std::string("UpdateRangeTimelineEvent(485331304451,Big rally,Two-run shot,"
                          "steam_star,700,3)"));
    EXPECT_EQ(rt_services_timeline_end_range_event(open_id, 0.5), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("EndRangeTimelineEvent(485331304451,0.5)"));
    Str first_id(first.c_str());
    EXPECT_EQ(rt_services_timeline_open_overlay_to_event(first_id), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("OpenOverlayToTimelineEvent(485331304449)"));
    EXPECT_EQ(rt_services_timeline_remove_event(first_id), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("RemoveTimelineEvent(485331304449)"));

    Str phase("game-1972-034");
    Str tag("Chicago");
    Str group("Opponent");
    Str score("3-2");
    Str attribute("Final score");
    EXPECT_EQ(rt_services_timeline_start_phase(), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("StartGamePhase()"));
    EXPECT_EQ(rt_services_timeline_set_phase_id(phase), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetGamePhaseID(game-1972-034)"));
    EXPECT_EQ(rt_services_timeline_add_phase_tag(tag, icon, group, 50), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("AddGamePhaseTag(Chicago,steam_star,Opponent,50)"));
    EXPECT_EQ(rt_services_timeline_set_phase_attribute(attribute, score, 60), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetGamePhaseAttribute(Final score,3-2,60)"));
    EXPECT_EQ(rt_services_timeline_open_overlay_to_phase(phase), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("OpenOverlayToGamePhase(game-1972-034)"));
    EXPECT_EQ(rt_services_timeline_end_phase(), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("EndGamePhase()"));
    rt_services_platform_shutdown();
}

TEST(ServicesExtended, TimelineRecordingRequestsComplete) {
    const FakeSteam &fake = startSteam();
    (void)fake;
    std::string kept = addEvent("Strikeout");
    std::string removed = addEvent("Error");
    Str kept_id(kept.c_str());
    Str removed_id(removed.c_str());
    EXPECT_EQ(rt_services_timeline_remove_event(removed_id), 1);

    void *present = rt_services_timeline_request_event_recording(kept_id);
    void *gone = rt_services_timeline_request_event_recording(removed_id);
    EXPECT_EQ(rt_services_request_get_kind(present), RT_SERVICES_REQUEST_TIMELINE_EVENT_RECORDING);
    EXPECT_EQ(rt_services_request_get_is_done(present), 0);
    ASSERT_TRUE(pumpUntilDone(present, 2) && pumpUntilDone(gone, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(present), 1);
    EXPECT_EQ(rt_services_request_get_flag(present), 1);
    EXPECT_EQ(take(rt_services_request_get_text(present)), kept);
    EXPECT_EQ(rt_services_request_get_detail_count(present), 0);
    EXPECT_EQ(rt_services_request_get_succeeded(gone), 1);
    EXPECT_EQ(rt_services_request_get_flag(gone), 0);
    release(present);
    release(gone);

    Str phase("game-1972-034");
    Str other("game-1972-035");
    EXPECT_EQ(rt_services_timeline_set_phase_id(phase), 1);
    void *recorded = rt_services_timeline_request_phase_recording(phase);
    void *empty = rt_services_timeline_request_phase_recording(other);
    EXPECT_EQ(rt_services_request_get_kind(recorded), RT_SERVICES_REQUEST_TIMELINE_PHASE_RECORDING);
    ASSERT_TRUE(pumpUntilDone(recorded, 2) && pumpUntilDone(empty, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(recorded), 1);
    EXPECT_EQ(rt_services_request_get_value(recorded), 90000);
    EXPECT_EQ(rt_services_request_get_flag(recorded), 1);
    EXPECT_EQ(take(rt_services_request_get_text(recorded)), std::string("game-1972-034"));
    ASSERT_EQ(rt_services_request_get_detail_count(recorded), 4);
    EXPECT_EQ(rt_services_request_detail(recorded, 0), 90000);
    EXPECT_EQ(rt_services_request_detail(recorded, 1), 30000);
    EXPECT_EQ(rt_services_request_detail(recorded, 2), 2);
    EXPECT_EQ(rt_services_request_detail(recorded, 3), 1);
    EXPECT_TRAP_MESSAGE(rt_services_request_detail(recorded, 4),
                        "Services.Request.Detail: index 4 is outside 0..3");
    EXPECT_TRAP_MESSAGE(rt_services_request_detail(recorded, -1),
                        "Services.Request.Detail: index -1 is outside 0..3");
    EXPECT_EQ(rt_services_request_get_succeeded(empty), 1);
    EXPECT_EQ(rt_services_request_get_value(empty), 0);
    EXPECT_EQ(rt_services_request_get_flag(empty), 0);
    EXPECT_EQ(rt_services_request_get_detail_count(empty), 4);
    EXPECT_EQ(rt_services_request_detail(empty, 2), 0);
    release(recorded);
    release(empty);

    // Shutdown cancels a pending query.
    void *pending = rt_services_timeline_request_phase_recording(phase);
    rt_services_platform_shutdown();
    EXPECT_EQ(rt_services_request_get_is_done(pending), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(pending), 0);
    EXPECT_EQ(rt_services_request_get_detail_count(pending), 0);
    release(pending);
}

TEST(ServicesExtended, TimelineRuntimeValuesAndSteamLimits) {
    startSteam();
    Str title("Home run");
    Str empty("");
    EXPECT_EQ(take(rt_services_timeline_add_event(title, empty, empty, 1, std::nan(""), 1)),
              std::string(""));
    EXPECT_TRUE(hasDiagnostic("Services: Timeline.AddEvent needs a finite time offset (got nan)"));
    EXPECT_EQ(take(rt_services_timeline_add_range_event(title, empty, empty, 1, 0.0, -1.0, 1)),
              std::string(""));
    EXPECT_TRUE(hasDiagnostic(
        "Services: Timeline.AddRangeEvent needs a finite duration of 0 seconds or more (got -1)"));
    EXPECT_EQ(take(rt_services_timeline_add_range_event(title, empty, empty, 1, 0.0, 601.0, 1)),
              std::string(""));
    EXPECT_TRUE(hasDiagnostic("Steam: timeline range events last at most 600 seconds (got 601)"));
    EXPECT_EQ(take(rt_services_timeline_add_event(title, empty, empty, 1, 1e39, 1)),
              std::string(""));
    EXPECT_TRUE(hasDiagnostic("Steam: timeline time 1e+39 seconds is outside the float range"));
    EXPECT_EQ(rt_services_timeline_clear_tooltip(std::numeric_limits<double>::infinity()), 0);
    EXPECT_TRUE(
        hasDiagnostic("Services: Timeline.ClearTooltip needs a finite time offset (got inf)"));

    const std::string long_id(64, 'p');
    Str long_phase(long_id.c_str());
    EXPECT_EQ(rt_services_timeline_set_phase_id(long_phase), 0);
    EXPECT_TRUE(
        hasDiagnostic("Steam: timeline phase id '" + long_id + "' is longer than 63 bytes"));
    void *too_long = rt_services_timeline_request_phase_recording(long_phase);
    EXPECT_EQ(rt_services_request_get_is_done(too_long), 1);
    EXPECT_EQ(take(rt_services_request_get_error(too_long)),
              "Steam: timeline phase id '" + long_id + "' is longer than 63 bytes");
    release(too_long);

    Str malformed("abc");
    Str zero("0");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_remove_event(malformed),
                        "Services.Timeline.RemoveEvent: Steam timeline event id 'abc' must be an "
                        "integer in 1..18446744073709551615");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_request_event_recording(zero),
                        "Services.Timeline.RequestEventRecording: Steam timeline event id '0' "
                        "must be an integer in 1..18446744073709551615");
    Str overflow("18446744073709551616");
    EXPECT_TRAP_MESSAGE(rt_services_timeline_open_overlay_to_event(overflow),
                        "Services.Timeline.OpenOverlayToEvent: Steam timeline event id "
                        "'18446744073709551616' must be an integer in 1..18446744073709551615");
    rt_services_platform_shutdown();
}

TEST(ServicesExtended, TimelineMissingFromCoreOnlyRedistributable) {
    useFake(fakeCore());
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_TIMELINE), 0);
    EXPECT_TRUE(hasDiagnostic(
        "Steam: interface SteamAPI_SteamTimeline_v004 unavailable; timeline disabled"));
    EXPECT_EQ(rt_services_timeline_start_phase(), 0);
    Str phase("game-1");
    void *request = rt_services_timeline_request_phase_recording(phase);
    EXPECT_EQ(rt_services_request_get_is_done(request), 1);
    EXPECT_EQ(take(rt_services_request_get_error(request)),
              std::string("Steam: the timeline is unavailable (see Platform.Diagnostics)"));
    release(request);
    rt_services_platform_shutdown();
}

TEST(ServicesExtended, AppDetailsReadDlcBuildAndBranch) {
    rt_services_platform_shutdown();
    EXPECT_EQ(rt_services_feature_app_details(), 14);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_APP_DETAILS), 0);
    EXPECT_EQ(rt_services_platform_get_dlc_count(), 0);
    EXPECT_EQ(take(rt_services_platform_dlc_id_at(0)), std::string(""));
    EXPECT_EQ(take(rt_services_platform_dlc_name_at(0)), std::string(""));
    EXPECT_EQ(rt_services_platform_dlc_available_at(0), 0);
    EXPECT_EQ(rt_services_platform_get_build_id(), 0);
    EXPECT_EQ(take(rt_services_platform_get_branch_name()), std::string(""));

    for (const FakeSteam *fake : {&fake165(), &fake164()}) {
        useFake(*fake);
        ASSERT_TRUE(init("steam", "480").ok);
        EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_APP_DETAILS), 1);
        EXPECT_EQ(rt_services_platform_get_dlc_count(), 2);
        EXPECT_EQ(take(rt_services_platform_dlc_id_at(0)), std::string("1234567"));
        EXPECT_EQ(take(rt_services_platform_dlc_name_at(0)), std::string("Stadium Pack"));
        EXPECT_EQ(rt_services_platform_dlc_available_at(0), 1);
        EXPECT_EQ(take(rt_services_platform_dlc_id_at(1)), std::string("7654321"));
        EXPECT_EQ(take(rt_services_platform_dlc_name_at(1)), std::string("Classic Uniforms"));
        EXPECT_EQ(rt_services_platform_dlc_available_at(1), 0);
        EXPECT_EQ(take(rt_services_platform_dlc_id_at(2)), std::string(""));
        EXPECT_EQ(take(rt_services_platform_dlc_id_at(-1)), std::string(""));
        EXPECT_EQ(rt_services_platform_dlc_available_at(INT64_C(5000000000)), 0);
        EXPECT_EQ(rt_services_platform_get_build_id(), 21042026);
        EXPECT_EQ(take(rt_services_platform_get_branch_name()), std::string("public-beta"));
        rt_services_platform_shutdown();
    }

    useFake(fakeCore());
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_APP_DETAILS), 0);
    EXPECT_EQ(rt_services_platform_get_dlc_count(), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: export SteamAPI_ISteamApps_GetDLCCount unavailable; DLC "
                              "list, build id, and branch queries disabled"));
    rt_services_platform_shutdown();
}

TEST(ServicesExtended, AchievementLayoutsMatchRedistributablePacking) {
    const bool pack8 = RT_STEAM_CALLBACK_PACK == 8;
    EXPECT_EQ(sizeof(rt_steam_user_achievement_icon_fetched), 144u);
    EXPECT_EQ(offsetof(rt_steam_user_achievement_icon_fetched, achievement_name), 8u);
    EXPECT_EQ(offsetof(rt_steam_user_achievement_icon_fetched, achieved), 136u);
    EXPECT_EQ(offsetof(rt_steam_user_achievement_icon_fetched, icon_handle), 140u);
    EXPECT_EQ(sizeof(rt_steam_global_achievement_percentages_ready), pack8 ? 16u : 12u);
    EXPECT_EQ(offsetof(rt_steam_global_achievement_percentages_ready, result), 8u);
}

TEST(ServicesExtended, AchievementIconsAndPercentagesNeutralWithoutProvider) {
    rt_services_platform_shutdown();
    Str win("ACH_WIN_ONE_GAME");
    Str empty("");
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACHIEVEMENT_ICONS), 0);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACHIEVEMENT_PERCENTAGES), 0);
    EXPECT_EQ(rt_services_achievements_icon_width(win), 0);
    EXPECT_EQ(rt_services_achievements_icon_height(win), 0);
    void *rgba = rt_services_achievements_icon_rgba(win);
    ASSERT_TRUE(rgba != nullptr);
    EXPECT_EQ(rt_bytes_len(rgba), 0);
    release(rgba);
    EXPECT_EQ(rt_services_achievements_global_percent(win), 0.0);
    void *request = rt_services_achievements_request_global_percentages();
    ASSERT_TRUE(request != nullptr);
    EXPECT_EQ(rt_services_request_get_kind(request), RT_SERVICES_REQUEST_ACHIEVEMENT_PERCENTAGES);
    EXPECT_EQ(rt_services_request_get_is_done(request), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(request), 0);
    EXPECT_EQ(take(rt_services_request_get_error(request)),
              std::string("Services: no platform services provider is started"));
    release(request);

    EXPECT_TRAP_MESSAGE(rt_services_achievements_icon_width(empty),
                        "Services.Achievements.IconWidth: achievement id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_achievements_icon_height(nullptr),
                        "Services.Achievements.IconHeight: achievement id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_achievements_icon_rgba(empty),
                        "Services.Achievements.IconRgba: achievement id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_achievements_global_percent(empty),
                        "Services.Achievements.GlobalPercent: achievement id must not be empty");
}

TEST(ServicesExtended, AchievementIconsLoadOnDemand) {
    const FakeSteam &fake = startSteam();
    Str win("ACH_WIN_ONE_GAME");
    Str travel("ACH_TRAVEL_FAR");
    Str unknown("ACH_NOPE");
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACHIEVEMENT_ICONS), 1);

    // The first read starts loading the icon for the current (locked) state.
    EXPECT_EQ(rt_services_achievements_icon_width(win), 0);
    EXPECT_EQ(rt_services_achievements_icon_height(win), 0);
    void *loading = rt_services_achievements_icon_rgba(win);
    EXPECT_EQ(rt_bytes_len(loading), 0);
    release(loading);
    EXPECT_FALSE(hasDiagnostic("Steam: GetAchievementAndUnlockTime('ACH_WIN_ONE_GAME') failed; "
                               "check that the achievement is defined for this app"));
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);

    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_ACHIEVEMENT_ICON_READY);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string("ACH_WIN_ONE_GAME"));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 0);
    EXPECT_EQ(rt_services_platform_get_event_value(), 1);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);

    EXPECT_EQ(rt_services_achievements_icon_width(win), 2);
    EXPECT_EQ(rt_services_achievements_icon_height(win), 2);
    void *locked = rt_services_achievements_icon_rgba(win);
    ASSERT_EQ(rt_bytes_len(locked), 16);
    EXPECT_EQ(rt_bytes_get(locked, 0), 16);
    EXPECT_EQ(rt_bytes_get(locked, 15), 31);
    release(locked);

    // Unlocking selects the other icon, which loads on its own.
    EXPECT_EQ(rt_services_achievements_unlock(win), 1);
    EXPECT_EQ(rt_services_achievements_icon_width(win), 0);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_ACHIEVEMENT_ICON_READY);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string("ACH_WIN_ONE_GAME"));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    void *unlocked = rt_services_achievements_icon_rgba(win);
    ASSERT_EQ(rt_bytes_len(unlocked), 16);
    EXPECT_EQ(rt_bytes_get(unlocked, 0), 32);
    release(unlocked);

    // Steam answers every request for an unset icon with another report. The
    // first report is passed on with EventValue 0; later reads do not ask again.
    const int requests = fake.iconRequestCount();
    EXPECT_EQ(rt_services_achievements_icon_width(travel), 0);
    EXPECT_EQ(rt_services_achievements_icon_height(travel), 0);
    EXPECT_EQ(fake.iconRequestCount(), requests + 2);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_ACHIEVEMENT_ICON_READY);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string("ACH_TRAVEL_FAR"));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 0);
    EXPECT_EQ(rt_services_platform_get_event_value(), 0);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);
    EXPECT_EQ(rt_services_achievements_icon_width(travel), 0);
    void *unset = rt_services_achievements_icon_rgba(travel);
    EXPECT_EQ(rt_bytes_len(unset), 0);
    release(unset);
    EXPECT_EQ(fake.iconRequestCount(), requests + 2);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);

    // An undefined achievement is reported without asking Steam for its icon.
    EXPECT_EQ(rt_services_achievements_icon_width(unknown), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: GetAchievementAndUnlockTime('ACH_NOPE') failed; check that "
                              "the achievement is defined for this app"));
    EXPECT_EQ(fake.iconRequestCount(), requests + 2);

    // An implausible image size is refused before any pixels are read.
    fake.setIconSize(5000, 2);
    EXPECT_EQ(rt_services_achievements_icon_width(win), 0);
    void *oversized = rt_services_achievements_icon_rgba(win);
    EXPECT_EQ(rt_bytes_len(oversized), 0);
    release(oversized);
    EXPECT_TRUE(hasDiagnostic("Steam: achievement 'ACH_WIN_ONE_GAME' icon is 5000x2 pixels; the "
                              "binding reads at most 4096x4096"));

    // A new session asks about unset icons again.
    rt_services_platform_shutdown();
    ASSERT_TRUE(init("steam", "480").ok);
    const int before_restart = fake.iconRequestCount();
    EXPECT_EQ(rt_services_achievements_icon_width(travel), 0);
    EXPECT_EQ(fake.iconRequestCount(), before_restart + 1);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_ACHIEVEMENT_ICON_READY);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string("ACH_TRAVEL_FAR"));
    rt_services_platform_shutdown();
}

TEST(ServicesExtended, AchievementPercentagesLoadThroughRequest) {
    Str win("ACH_WIN_ONE_GAME");
    Str cycle("ACH_HIT_FOR_CYCLE");
    Str travel("ACH_TRAVEL_FAR");
    Str unknown("ACH_NOPE");
    for (const FakeSteam *fake : {&fake165(), &fake164()}) {
        useFake(*fake);
        fake->setScripted(0);
        ASSERT_TRUE(init("steam", "480").ok);
        EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACHIEVEMENT_PERCENTAGES), 1);
        EXPECT_EQ(rt_services_achievements_global_percent(win), 0.0);
        EXPECT_TRUE(hasDiagnostic("Steam: global achievement percentages are not loaded; call "
                                  "Achievements.RequestGlobalPercentages first"));

        void *request = rt_services_achievements_request_global_percentages();
        EXPECT_EQ(rt_services_request_get_kind(request),
                  RT_SERVICES_REQUEST_ACHIEVEMENT_PERCENTAGES);
        EXPECT_EQ(rt_services_request_get_is_done(request), 0);
        ASSERT_TRUE(pumpUntilDone(request, 2));
        EXPECT_EQ(rt_services_request_get_succeeded(request), 1);
        EXPECT_EQ(rt_services_request_get_result_code(request), 1);
        release(request);
        EXPECT_EQ(rt_services_achievements_global_percent(win), 62.5);
        EXPECT_EQ(rt_services_achievements_global_percent(cycle), 3.25);
        EXPECT_EQ(rt_services_achievements_global_percent(travel), 18.0);
        EXPECT_EQ(rt_services_achievements_global_percent(unknown), 0.0);
        EXPECT_TRUE(hasDiagnostic("Steam: GetAchievementAchievedPercent('ACH_NOPE') failed; check "
                                  "that the achievement is defined for this app"));
        rt_services_platform_shutdown();
    }

    // A failed download completes the request with the provider result and loads nothing.
    const FakeSteam &fake = startSteam();
    fake.setGlobalPercentagesResult(2);
    void *failed = rt_services_achievements_request_global_percentages();
    ASSERT_TRUE(pumpUntilDone(failed, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(failed), 0);
    EXPECT_EQ(rt_services_request_get_result_code(failed), 2);
    EXPECT_EQ(take(rt_services_request_get_error(failed)),
              std::string("Steam: global achievement percentages are unavailable (EResult 2)"));
    release(failed);
    EXPECT_EQ(rt_services_achievements_global_percent(win), 0.0);

    // Shutdown cancels a pending download.
    void *pending = rt_services_achievements_request_global_percentages();
    rt_services_platform_shutdown();
    EXPECT_EQ(rt_services_request_get_is_done(pending), 1);
    EXPECT_EQ(rt_services_request_get_succeeded(pending), 0);
    release(pending);
}

TEST(ServicesExtended, AchievementIconsAndPercentagesMissingFromCoreOnlyRedistributable) {
    useFake(fakeCore());
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACHIEVEMENT_ICONS), 0);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACHIEVEMENT_PERCENTAGES), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: export SteamAPI_ISteamUserStats_GetAchievementIcon "
                              "unavailable; achievement icons disabled"));
    EXPECT_TRUE(
        hasDiagnostic("Steam: export SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages "
                      "unavailable; global achievement percentages disabled"));
    Str win("ACH_WIN_ONE_GAME");
    EXPECT_EQ(rt_services_achievements_icon_width(win), 0);
    EXPECT_EQ(rt_services_achievements_global_percent(win), 0.0);
    void *request = rt_services_achievements_request_global_percentages();
    EXPECT_EQ(rt_services_request_get_is_done(request), 1);
    EXPECT_EQ(take(rt_services_request_get_error(request)),
              std::string("Steam: global achievement percentages are unavailable (see "
                          "Platform.Diagnostics)"));
    release(request);
    rt_services_platform_shutdown();
}

TEST(ServicesExtended, TimelineStatefulMembersRequireMainThread) {
    rt_services_platform_shutdown();
    g_trap_jump = false;
    g_last_trap.clear();
    int8_t result = -1;
    std::thread worker([&result]() { result = rt_services_timeline_start_phase(); });
    worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: Timeline.StartPhase must be called on the main thread"));
    EXPECT_EQ(result, 0);
    g_last_trap.clear();
    int64_t mode = -1;
    std::thread constant_worker([&mode]() { mode = rt_services_timeline_mode_menus(); });
    constant_worker.join();
    EXPECT_EQ(mode, 3);
    EXPECT_EQ(g_last_trap, std::string(""));
    int64_t width = -1;
    std::thread icon_worker([&width]() {
        Str win("ACH_WIN_ONE_GAME");
        width = rt_services_achievements_icon_width(win);
    });
    icon_worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: Achievements.IconWidth must be called on the main thread"));
    EXPECT_EQ(width, 0);
}

int main(int argc, char **argv) {
    rt_set_main_thread();
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
