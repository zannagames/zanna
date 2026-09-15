//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTServicesWorkshopTests.cpp
// Purpose: Verify Zanna.Services.Workshop and WorkshopItem (ADR 0366) against
//          the fake steam_api libraries: the neutral contract without a
//          provider, argument traps, subscribed items with install and
//          download state and events, queries returning WorkshopItem objects,
//          subscriptions, publishing, Steam id traps, the SDK 1.61 subscribed
//          item signatures, and the core-only redistributable.
// Key invariants:
//   - Every test starts from a shut-down session and a reset fake library.
//   - Content folders and preview files are created in the system temporary
//     directory and removed again.
// Ownership/Lifetime:
//   - Tests release every request, item, and runtime string they receive.
// Links: src/runtime/services/rt_services_workshop.c,
//        src/runtime/services/steam/rt_steam_workshop.c,
//        src/tests/runtime/RTServicesFakeSteamApi.c,
//        docs/adr/0366-platform-services-workshop.md
//
//===----------------------------------------------------------------------===//

#include "RTServicesTestSupport.hpp"

#include "rt_services_workshop.h"
#include "rt_steam_abi.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace services_test;

namespace {

/// @brief Id of the fake's installed league item.
const char *const kLeague = "3000000001";
/// @brief Id of the fake's subscribed parks item that still needs a download.
const char *const kParks = "3000000002";
/// @brief Id of the fake's uniforms item, owned by the player and not subscribed.
const char *const kUniforms = "3000000003";

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

/// @brief Read an item's title and release nothing else.
std::string itemTitle(void *request, int64_t index) {
    void *item = rt_services_request_item_at(request, index);
    std::string title = take(rt_services_workshop_item_get_title(item));
    release(item);
    return title;
}

/// @brief Temporary content folder and preview image for publishing tests.
struct PublishFiles {
    std::filesystem::path folder;
    std::filesystem::path preview;

    PublishFiles() {
        folder = std::filesystem::temp_directory_path() / "zanna_workshop_content";
        preview = std::filesystem::temp_directory_path() / "zanna_workshop_preview.png";
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "league.json") << "{}";
        std::ofstream(preview, std::ios::binary) << "png";
    }

    ~PublishFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        std::filesystem::remove(preview, ignored);
    }
};

} // namespace

TEST(ServicesWorkshop, ConstantsMatchAdr) {
    EXPECT_EQ(rt_services_event_kind_workshop_item_installed(), 15);
    EXPECT_EQ(rt_services_event_kind_workshop_item_downloaded(), 16);
    EXPECT_EQ(rt_services_event_kind_workshop_subscription_changed(), 17);
    EXPECT_EQ(rt_services_feature_workshop(), 18);
    EXPECT_EQ(rt_services_request_kind_workshop_query(), 9);
    EXPECT_EQ(rt_services_request_kind_workshop_subscribe(), 10);
    EXPECT_EQ(rt_services_request_kind_workshop_unsubscribe(), 11);
    EXPECT_EQ(rt_services_request_kind_workshop_create(), 12);
    EXPECT_EQ(rt_services_request_kind_workshop_submit(), 13);
    EXPECT_EQ(rt_services_request_kind_workshop_delete(), 14);
    EXPECT_EQ(rt_services_workshop_query_popular(), 1);
    EXPECT_EQ(rt_services_workshop_query_newest(), 2);
    EXPECT_EQ(rt_services_workshop_query_trending(), 3);
    EXPECT_EQ(rt_services_workshop_query_most_subscribed(), 4);
    EXPECT_EQ(rt_services_workshop_query_recently_updated(), 5);
    EXPECT_EQ(rt_services_workshop_query_text_search(), 6);
    EXPECT_EQ(rt_services_workshop_list_published(), 1);
    EXPECT_EQ(rt_services_workshop_list_subscribed(), 2);
    EXPECT_EQ(rt_services_workshop_list_favorited(), 3);
    EXPECT_EQ(rt_services_workshop_list_voted_up(), 4);
    EXPECT_EQ(rt_services_workshop_list_played(), 5);
    EXPECT_EQ(rt_services_workshop_visibility_public(), 0);
    EXPECT_EQ(rt_services_workshop_visibility_friends_only(), 1);
    EXPECT_EQ(rt_services_workshop_visibility_private(), 2);
    EXPECT_EQ(rt_services_workshop_visibility_unlisted(), 3);
    EXPECT_EQ(rt_services_workshop_update_status_none(), 0);
    EXPECT_EQ(rt_services_workshop_update_status_preparing_config(), 1);
    EXPECT_EQ(rt_services_workshop_update_status_preparing_content(), 2);
    EXPECT_EQ(rt_services_workshop_update_status_uploading_content(), 3);
    EXPECT_EQ(rt_services_workshop_update_status_uploading_preview(), 4);
    EXPECT_EQ(rt_services_workshop_update_status_committing(), 5);
}

TEST(ServicesWorkshop, LayoutsMatchRedistributablePacking) {
    const bool pack8 = RT_STEAM_CALLBACK_PACK == 8;
    EXPECT_EQ(sizeof(rt_steam_ugc_details), pack8 ? 9784u : 9772u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, title), 24u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, description), 153u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, owner), pack8 ? 8160u : 8156u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, visibility), pack8 ? 8180u : 8176u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, tags), pack8 ? 8187u : 8183u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, file_size), pack8 ? 9492u : 9484u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, votes_up), pack8 ? 9756u : 9748u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, score), pack8 ? 9764u : 9756u);
    EXPECT_EQ(offsetof(rt_steam_ugc_details, total_files_size), pack8 ? 9776u : 9764u);
    EXPECT_EQ(sizeof(rt_steam_ugc_query_completed), 280u);
    EXPECT_EQ(offsetof(rt_steam_ugc_query_completed, total), 16u);
    EXPECT_EQ(sizeof(rt_steam_ugc_create_item_result), pack8 ? 24u : 16u);
    EXPECT_EQ(offsetof(rt_steam_ugc_create_item_result, file_id), pack8 ? 8u : 4u);
    EXPECT_EQ(sizeof(rt_steam_ugc_submit_item_update_result), 16u);
    EXPECT_EQ(sizeof(rt_steam_ugc_item_installed), pack8 ? 32u : 28u);
    EXPECT_EQ(sizeof(rt_steam_ugc_download_item_result), pack8 ? 24u : 16u);
    EXPECT_EQ(sizeof(rt_steam_ugc_file_result), pack8 ? 16u : 12u);
    EXPECT_EQ(sizeof(rt_steam_ugc_file_subscription), pack8 ? 16u : 12u);
    EXPECT_EQ(offsetof(rt_steam_ugc_file_subscription, app_id), 8u);
    EXPECT_EQ(sizeof(rt_steam_param_string_array), pack8 ? 16u : 12u);
}

TEST(ServicesWorkshop, NeutralWithoutProvider) {
    rt_services_platform_shutdown();
    Str league(kLeague);
    Str malformed("not-a-number");
    Str empty("");
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_WORKSHOP), 0);
    EXPECT_EQ(rt_services_workshop_get_subscribed_count(), 0);
    EXPECT_EQ(take(rt_services_workshop_subscribed_id_at(0)), std::string(""));
    // A provider-specific id format is only checked while that provider is active.
    EXPECT_EQ(rt_services_workshop_is_subscribed(malformed), 0);
    EXPECT_EQ(rt_services_workshop_is_installed(league), 0);
    EXPECT_EQ(rt_services_workshop_needs_update(league), 0);
    EXPECT_EQ(rt_services_workshop_is_downloading(league), 0);
    EXPECT_EQ(take(rt_services_workshop_install_folder(league)), std::string(""));
    EXPECT_EQ(rt_services_workshop_install_size(league), 0);
    EXPECT_EQ(rt_services_workshop_install_time(league), 0);
    EXPECT_EQ(rt_services_workshop_downloaded_bytes(league), 0);
    EXPECT_EQ(rt_services_workshop_download_total_bytes(league), 0);
    EXPECT_EQ(rt_services_workshop_download(league, 1), 0);
    EXPECT_EQ(take(rt_services_workshop_start_update(league)), std::string(""));
    EXPECT_EQ(rt_services_workshop_set_title(league, league), 0);
    EXPECT_EQ(rt_services_workshop_set_visibility(league, 0), 0);
    EXPECT_EQ(rt_services_workshop_update_status(league), 0);
    EXPECT_EQ(rt_services_workshop_update_progress(league), 0.0);
    for (void *request : {rt_services_workshop_subscribe(league),
                          rt_services_workshop_unsubscribe(league),
                          rt_services_workshop_query(1, 1, empty, empty),
                          rt_services_workshop_query_user(2, 1),
                          rt_services_workshop_query_items(league),
                          rt_services_workshop_create_item(),
                          rt_services_workshop_submit_update(league, empty),
                          rt_services_workshop_delete_item(league)}) {
        ASSERT_TRUE(request != nullptr);
        EXPECT_EQ(rt_services_request_get_is_done(request), 1);
        EXPECT_EQ(rt_services_request_get_succeeded(request), 0);
        EXPECT_EQ(rt_services_request_get_item_count(request), 0);
        EXPECT_EQ(take(rt_services_request_get_error(request)),
                  std::string("Services: no platform services provider is started"));
        EXPECT_TRAP_MESSAGE(
            rt_services_request_item_at(request, 0),
            "Services.Request.ItemAt: index 0 is out of range; the request holds no items");
        release(request);
    }
}

TEST(ServicesWorkshop, MalformedArgumentsTrap) {
    rt_services_platform_shutdown();
    Str empty("");
    Str id("1");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_is_subscribed(empty),
                        "Services.Workshop.IsSubscribed: item id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_install_folder(nullptr),
                        "Services.Workshop.InstallFolder: item id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_subscribe(empty),
                        "Services.Workshop.Subscribe: item id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_set_title(empty, id),
                        "Services.Workshop.SetTitle: update id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_submit_update(empty, empty),
                        "Services.Workshop.SubmitUpdate: update id must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query(0, 1, empty, empty),
                        "Services.Workshop.Query: order must be a WorkshopQuery value (got 0)");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query(7, 1, empty, empty),
                        "Services.Workshop.Query: order must be a WorkshopQuery value (got 7)");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query(1, 0, empty, empty),
                        "Services.Workshop.Query: page must be 1 or more (got 0)");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query_user(6, 1),
                        "Services.Workshop.QueryUser: list must be a WorkshopList value (got 6)");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query_user(1, -1),
                        "Services.Workshop.QueryUser: page must be 1 or more (got -1)");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query_items(empty),
                        "Services.Workshop.QueryItems: item id list must not be empty");
    Str gap("1,,2");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query_items(gap),
                        "Services.Workshop.QueryItems: item id 2 in the list is empty");
    std::string many;
    for (int i = 1; i <= 101; ++i)
        many += (i > 1 ? "," : "") + std::to_string(i);
    Str too_many(many.c_str());
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query_items(too_many),
                        "Services.Workshop.QueryItems: the list holds more than 100 item ids");
    EXPECT_TRAP_MESSAGE(
        rt_services_workshop_set_visibility(id, 4),
        "Services.Workshop.SetVisibility: visibility must be a WorkshopVisibility value (got 4)");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_set_content(id, empty),
                        "Services.Workshop.SetContent: folder must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_set_preview(id, empty),
                        "Services.Workshop.SetPreview: file must not be empty");
}

TEST(ServicesWorkshop, SubscribedItemsStateAndDownloads) {
    startSteam();
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_WORKSHOP), 1);
    ASSERT_EQ(rt_services_workshop_get_subscribed_count(), 2);
    EXPECT_EQ(take(rt_services_workshop_subscribed_id_at(0)), std::string(kLeague));
    EXPECT_EQ(take(rt_services_workshop_subscribed_id_at(1)), std::string(kParks));
    EXPECT_EQ(take(rt_services_workshop_subscribed_id_at(2)), std::string(""));
    EXPECT_EQ(take(rt_services_workshop_subscribed_id_at(-1)), std::string(""));

    Str league(kLeague);
    Str parks(kParks);
    Str uniforms(kUniforms);
    EXPECT_EQ(rt_services_workshop_is_subscribed(league), 1);
    EXPECT_EQ(rt_services_workshop_is_installed(league), 1);
    EXPECT_EQ(rt_services_workshop_needs_update(league), 0);
    EXPECT_EQ(take(rt_services_workshop_install_folder(league)),
              std::string("/fake/workshop/content/480/3000000001"));
    EXPECT_EQ(rt_services_workshop_install_size(league), 4096);
    EXPECT_EQ(rt_services_workshop_install_time(league), 1695000000);
    EXPECT_EQ(rt_services_workshop_is_subscribed(uniforms), 0);

    EXPECT_EQ(rt_services_workshop_is_installed(parks), 0);
    EXPECT_EQ(rt_services_workshop_needs_update(parks), 1);
    EXPECT_EQ(take(rt_services_workshop_install_folder(parks)), std::string(""));
    EXPECT_EQ(rt_services_workshop_downloaded_bytes(parks), 512);
    EXPECT_EQ(rt_services_workshop_download_total_bytes(parks), 2048);
    EXPECT_EQ(rt_services_workshop_download(parks, 1), 1);
    EXPECT_EQ(rt_services_workshop_is_downloading(parks), 1);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_WORKSHOP_ITEM_INSTALLED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kParks));
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_WORKSHOP_ITEM_DOWNLOADED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kParks));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    EXPECT_EQ(rt_services_platform_get_event_result_code(), 1);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);
    EXPECT_EQ(rt_services_workshop_is_installed(parks), 1);
    EXPECT_EQ(rt_services_workshop_is_downloading(parks), 0);
    EXPECT_EQ(rt_services_workshop_downloaded_bytes(parks), 2048);

    Str unknown("4000000000");
    EXPECT_EQ(rt_services_workshop_download(unknown, 0), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: DownloadItem('4000000000') failed; check the item id and "
                              "that Steam is online"));
    rt_services_platform_shutdown();
}

TEST(ServicesWorkshop, QueriesReturnItems) {
    const FakeSteam &fake = startSteam();
    Str empty("");
    void *popular = rt_services_workshop_query(RT_SERVICES_WORKSHOP_QUERY_POPULAR, 1, empty, empty);
    EXPECT_EQ(rt_services_request_get_kind(popular), RT_SERVICES_REQUEST_WORKSHOP_QUERY);
    EXPECT_EQ(rt_services_request_get_is_done(popular), 0);
    EXPECT_EQ(fake.ugcOpenQueries(), 1);
    ASSERT_TRUE(pumpUntilDone(popular, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(popular), 1);
    EXPECT_EQ(rt_services_request_get_value(popular), 3);
    EXPECT_EQ(rt_services_request_get_flag(popular), 0);
    ASSERT_EQ(rt_services_request_get_item_count(popular), 3);
    EXPECT_EQ(fake.ugcOpenQueries(), 0);
    EXPECT_EQ(fake.ugcLastQuery(),
              std::string("all(type=0,matching=2,creator=480,consumer=480,page=1) tags= search= "
                          "long=1 metadata=1"));

    void *first = rt_services_request_item_at(popular, 0);
    EXPECT_EQ(take(rt_services_workshop_item_get_id(first)), std::string(kLeague));
    EXPECT_EQ(take(rt_services_workshop_item_get_title(first)), std::string("Boston 1972 League"));
    EXPECT_EQ(take(rt_services_workshop_item_get_description(first)),
              std::string("Every Boston game of the 1972 season."));
    EXPECT_EQ(take(rt_services_workshop_item_get_owner_id(first)),
              std::string("76561198000000002"));
    EXPECT_EQ(take(rt_services_workshop_item_get_tags(first)), std::string("league,1972"));
    EXPECT_EQ(take(rt_services_workshop_item_get_preview_url(first)),
              std::string("https://fake.example/workshop/3000000001.png"));
    EXPECT_EQ(take(rt_services_workshop_item_get_metadata(first)), std::string("{\"format\":2}"));
    EXPECT_EQ(rt_services_workshop_item_get_created(first), 1690000000);
    EXPECT_EQ(rt_services_workshop_item_get_updated(first), 1695000000);
    EXPECT_EQ(rt_services_workshop_item_get_visibility(first),
              RT_SERVICES_WORKSHOP_VISIBILITY_PUBLIC);
    EXPECT_EQ(rt_services_workshop_item_get_votes_up(first), 120);
    EXPECT_EQ(rt_services_workshop_item_get_votes_down(first), 4);
    EXPECT_EQ(rt_services_workshop_item_get_size(first), 4096);
    EXPECT_EQ(rt_services_workshop_item_get_score(first), 0.9375);
    EXPECT_EQ(itemTitle(popular, 1), std::string("Classic Parks Pack"));
    EXPECT_EQ(itemTitle(popular, 2), std::string("Night Uniforms"));
    EXPECT_TRAP_MESSAGE(rt_services_request_item_at(popular, 3),
                        "Services.Request.ItemAt: index 3 is outside 0..2");
    // Items outlive the request they came from.
    release(popular);
    EXPECT_EQ(take(rt_services_workshop_item_get_title(first)), std::string("Boston 1972 League"));
    release(first);

    Str tags("league");
    Str search("Boston");
    void *filtered =
        rt_services_workshop_query(RT_SERVICES_WORKSHOP_QUERY_TEXT_SEARCH, 1, tags, search);
    ASSERT_TRUE(pumpUntilDone(filtered, 2));
    ASSERT_EQ(rt_services_request_get_item_count(filtered), 1);
    EXPECT_EQ(itemTitle(filtered, 0), std::string("Boston 1972 League"));
    EXPECT_EQ(fake.ugcLastQuery(),
              std::string("all(type=11,matching=2,creator=480,consumer=480,page=1) tags=league "
                          "search=Boston long=1 metadata=1"));
    release(filtered);

    void *newest = rt_services_workshop_query(RT_SERVICES_WORKSHOP_QUERY_NEWEST, 2, empty, empty);
    ASSERT_TRUE(pumpUntilDone(newest, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(newest), 1);
    EXPECT_EQ(rt_services_request_get_item_count(newest), 0);
    release(newest);

    void *subscribed = rt_services_workshop_query_user(RT_SERVICES_WORKSHOP_LIST_SUBSCRIBED, 1);
    ASSERT_TRUE(pumpUntilDone(subscribed, 2));
    ASSERT_EQ(rt_services_request_get_item_count(subscribed), 2);
    EXPECT_EQ(fake.ugcLastQuery(),
              std::string("user(account=39734273,list=6,matching=2,sort=4,creator=480,consumer=480,"
                          "page=1) tags= search= long=1 metadata=1"));
    release(subscribed);
    void *published = rt_services_workshop_query_user(RT_SERVICES_WORKSHOP_LIST_PUBLISHED, 1);
    ASSERT_TRUE(pumpUntilDone(published, 2));
    ASSERT_EQ(rt_services_request_get_item_count(published), 1);
    EXPECT_EQ(itemTitle(published, 0), std::string("Night Uniforms"));
    release(published);

    Str ids(" 3000000003 , 3000000001,4000000000");
    void *details = rt_services_workshop_query_items(ids);
    ASSERT_TRUE(pumpUntilDone(details, 2));
    EXPECT_EQ(fake.ugcLastQuery(),
              std::string("details(3000000003,3000000001,4000000000) tags= search= long=1 "
                          "metadata=1"));
    ASSERT_EQ(rt_services_request_get_item_count(details), 2);
    EXPECT_EQ(itemTitle(details, 0), std::string("Night Uniforms"));
    EXPECT_EQ(itemTitle(details, 1), std::string("Boston 1972 League"));
    release(details);

    std::string sixty;
    for (int i = 1; i <= 60; ++i)
        sixty += (i > 1 ? "," : "") + std::to_string(i);
    Str too_many(sixty.c_str());
    void *refused = rt_services_workshop_query_items(too_many);
    EXPECT_EQ(rt_services_request_get_is_done(refused), 1);
    EXPECT_EQ(take(rt_services_request_get_error(refused)),
              std::string("Steam: Workshop.QueryItems takes at most 50 item ids (got 60)"));
    release(refused);

    // Shutdown releases the handle of a query still in flight.
    void *pending = rt_services_workshop_query(RT_SERVICES_WORKSHOP_QUERY_POPULAR, 1, empty, empty);
    EXPECT_EQ(fake.ugcOpenQueries(), 1);
    rt_services_platform_shutdown();
    EXPECT_EQ(fake.ugcOpenQueries(), 0);
    EXPECT_EQ(rt_services_request_get_succeeded(pending), 0);
    release(pending);
}

TEST(ServicesWorkshop, SubscriptionsChange) {
    startSteam();
    Str uniforms(kUniforms);
    void *subscribe = rt_services_workshop_subscribe(uniforms);
    EXPECT_EQ(rt_services_request_get_kind(subscribe), RT_SERVICES_REQUEST_WORKSHOP_SUBSCRIBE);
    ASSERT_TRUE(pumpUntilDone(subscribe, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(subscribe), 1);
    EXPECT_EQ(take(rt_services_request_get_text(subscribe)), std::string(kUniforms));
    release(subscribe);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_WORKSHOP_SUBSCRIPTION_CHANGED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kUniforms));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    EXPECT_EQ(rt_services_workshop_is_subscribed(uniforms), 1);
    EXPECT_EQ(rt_services_workshop_needs_update(uniforms), 1);
    EXPECT_EQ(rt_services_workshop_get_subscribed_count(), 3);

    void *unsubscribe = rt_services_workshop_unsubscribe(uniforms);
    ASSERT_TRUE(pumpUntilDone(unsubscribe, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(unsubscribe), 1);
    release(unsubscribe);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_WORKSHOP_SUBSCRIPTION_CHANGED);
    EXPECT_EQ(rt_services_platform_get_event_flag(), 0);
    EXPECT_EQ(rt_services_workshop_get_subscribed_count(), 2);

    Str unknown("4000000000");
    void *missing = rt_services_workshop_subscribe(unknown);
    ASSERT_TRUE(pumpUntilDone(missing, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(missing), 0);
    EXPECT_EQ(rt_services_request_get_result_code(missing), 9);
    EXPECT_EQ(take(rt_services_request_get_error(missing)),
              std::string("Steam: SubscribeItem('4000000000') failed (EResult 9)"));
    release(missing);
    rt_services_platform_shutdown();
}

TEST(ServicesWorkshop, PublishingCreatesUpdatesAndDeletes) {
    const FakeSteam &fake = startSteam();
    PublishFiles files;
    void *create = rt_services_workshop_create_item();
    EXPECT_EQ(rt_services_request_get_kind(create), RT_SERVICES_REQUEST_WORKSHOP_CREATE);
    ASSERT_TRUE(pumpUntilDone(create, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(create), 1);
    EXPECT_EQ(rt_services_request_get_flag(create), 1);
    const std::string created = take(rt_services_request_get_text(create));
    EXPECT_EQ(created, std::string("3000000004"));
    release(create);

    Str item(created.c_str());
    const std::string update = take(rt_services_workshop_start_update(item));
    EXPECT_EQ(update, std::string("365072220161"));
    Str update_id(update.c_str());
    Str title("Chicago 1972 League");
    Str description("The whole Chicago season.");
    Str metadata("{\"format\":2}");
    Str tags(" league , 1972,chicago ");
    Str folder(files.folder.string().c_str());
    Str preview(files.preview.string().c_str());
    EXPECT_EQ(rt_services_workshop_set_title(update_id, title), 1);
    EXPECT_EQ(rt_services_workshop_set_description(update_id, description), 1);
    EXPECT_EQ(rt_services_workshop_set_metadata(update_id, metadata), 1);
    EXPECT_EQ(rt_services_workshop_set_tags(update_id, tags), 1);
    EXPECT_EQ(
        rt_services_workshop_set_visibility(update_id, RT_SERVICES_WORKSHOP_VISIBILITY_PUBLIC), 1);
    EXPECT_EQ(rt_services_workshop_set_content(update_id, folder), 1);
    EXPECT_EQ(rt_services_workshop_set_preview(update_id, preview), 1);
    const std::string long_title(200, 't');
    Str too_long(long_title.c_str());
    EXPECT_EQ(rt_services_workshop_set_title(update_id, too_long), 0);
    EXPECT_TRUE(
        hasDiagnostic("Steam: SetItemTitle(365072220161) failed; check the update id and the "
                      "length of the text (200 bytes)"));
    Str missing_folder("/definitely/not/here");
    EXPECT_EQ(rt_services_workshop_set_content(update_id, missing_folder), 0);
    EXPECT_TRUE(
        hasDiagnostic("Services: Workshop.SetContent found no folder at '/definitely/not/here'"));
    EXPECT_EQ(rt_services_workshop_update_status(update_id),
              RT_SERVICES_WORKSHOP_UPDATE_STATUS_PREPARING_CONFIG);

    Str note("First upload");
    void *submit = rt_services_workshop_submit_update(update_id, note);
    EXPECT_EQ(rt_services_request_get_kind(submit), RT_SERVICES_REQUEST_WORKSHOP_SUBMIT);
    EXPECT_EQ(rt_services_workshop_update_status(update_id),
              RT_SERVICES_WORKSHOP_UPDATE_STATUS_UPLOADING_CONTENT);
    EXPECT_EQ(rt_services_workshop_update_progress(update_id), 0.25);
    ASSERT_TRUE(pumpUntilDone(submit, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(submit), 1);
    EXPECT_EQ(rt_services_request_get_flag(submit), 0);
    EXPECT_EQ(take(rt_services_request_get_text(submit)), created);
    release(submit);
    EXPECT_EQ(rt_services_workshop_update_status(update_id),
              RT_SERVICES_WORKSHOP_UPDATE_STATUS_NONE);
    const std::string last = fake.ugcLastUpdate();
    EXPECT_TRUE(
        last.find("title=Chicago 1972 League|description=The whole Chicago season.|"
                  "metadata={\"format\":2}|tags=league,1972,chicago|visibility=0|content=") == 0);
    EXPECT_TRUE(last.find("zanna_workshop_content|preview=") != std::string::npos);
    EXPECT_TRUE(last.find("zanna_workshop_preview.png|note=First upload") != std::string::npos);

    void *details = rt_services_workshop_query_items(item);
    ASSERT_TRUE(pumpUntilDone(details, 2));
    ASSERT_EQ(rt_services_request_get_item_count(details), 1);
    void *published = rt_services_request_item_at(details, 0);
    EXPECT_EQ(take(rt_services_workshop_item_get_title(published)),
              std::string("Chicago 1972 League"));
    EXPECT_EQ(take(rt_services_workshop_item_get_tags(published)),
              std::string("league,1972,chicago"));
    release(published);
    release(details);

    // Items owned by other players cannot be updated or deleted.
    Str league(kLeague);
    EXPECT_EQ(take(rt_services_workshop_start_update(league)), std::string(""));
    EXPECT_TRUE(hasDiagnostic("Steam: StartItemUpdate('3000000001') failed"));
    void *refused = rt_services_workshop_delete_item(league);
    ASSERT_TRUE(pumpUntilDone(refused, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(refused), 0);
    EXPECT_EQ(take(rt_services_request_get_error(refused)),
              std::string("Steam: DeleteItem('3000000001') failed (EResult 15)"));
    release(refused);

    void *remove = rt_services_workshop_delete_item(item);
    EXPECT_EQ(rt_services_request_get_kind(remove), RT_SERVICES_REQUEST_WORKSHOP_DELETE);
    ASSERT_TRUE(pumpUntilDone(remove, 2));
    EXPECT_EQ(rt_services_request_get_succeeded(remove), 1);
    EXPECT_EQ(take(rt_services_request_get_text(remove)), created);
    release(remove);
    rt_services_platform_shutdown();
}

TEST(ServicesWorkshop, SteamIdsTrapWhileActive) {
    startSteam();
    Str malformed("abc");
    Str zero("0");
    Str all("18446744073709551615");
    Str empty("");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_is_installed(malformed),
                        "Services.Workshop.IsInstalled: Steam Workshop item id 'abc' must be an "
                        "integer in 1..18446744073709551615");
    EXPECT_TRAP_MESSAGE(
        rt_services_workshop_subscribe(zero),
        "Services.Workshop.Subscribe: Steam Workshop item id '0' must be an integer "
        "in 1..18446744073709551615");
    Str mixed("3000000001,abc");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_query_items(mixed),
                        "Services.Workshop.QueryItems: Steam Workshop item id 'abc' must be an "
                        "integer in 1..18446744073709551615");
    EXPECT_TRAP_MESSAGE(rt_services_workshop_set_title(all, empty),
                        "Services.Workshop.SetTitle: Steam Workshop update id "
                        "'18446744073709551615' must be an integer in 0..18446744073709551614");
    rt_services_platform_shutdown();
}

TEST(ServicesWorkshop, Sdk161RedistributableUsesVersion020Signatures) {
    const FakeSteam &fake = fake164();
    useFake(fake);
    fake.setScripted(0);
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_WORKSHOP), 1);
    ASSERT_EQ(rt_services_workshop_get_subscribed_count(), 2);
    EXPECT_EQ(take(rt_services_workshop_subscribed_id_at(1)), std::string(kParks));
    rt_services_platform_shutdown();
}

TEST(ServicesWorkshop, MissingFromCoreOnlyRedistributable) {
    useFake(fakeCore());
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_WORKSHOP), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: interface SteamAPI_SteamUGC_v021 or SteamAPI_SteamUGC_v020 "
                              "unavailable; Workshop disabled"));
    EXPECT_EQ(rt_services_workshop_get_subscribed_count(), 0);
    Str empty("");
    void *request = rt_services_workshop_query(RT_SERVICES_WORKSHOP_QUERY_POPULAR, 1, empty, empty);
    EXPECT_EQ(rt_services_request_get_is_done(request), 1);
    EXPECT_EQ(take(rt_services_request_get_error(request)),
              std::string("Steam: the Workshop is unavailable (see Platform.Diagnostics)"));
    release(request);
    rt_services_platform_shutdown();
}

TEST(ServicesWorkshop, WorkshopMembersRequireMainThread) {
    rt_services_platform_shutdown();
    g_trap_jump = false;
    g_last_trap.clear();
    int64_t count = -1;
    std::thread worker([&count]() { count = rt_services_workshop_get_subscribed_count(); });
    worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: Workshop.SubscribedCount must be called on the main thread"));
    EXPECT_EQ(count, 0);
    g_last_trap.clear();
    int64_t order = -1;
    std::thread constant_worker([&order]() { order = rt_services_workshop_query_trending(); });
    constant_worker.join();
    EXPECT_EQ(order, 3);
    EXPECT_EQ(g_last_trap, std::string(""));
}

int main(int argc, char **argv) {
    rt_set_main_thread();
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
