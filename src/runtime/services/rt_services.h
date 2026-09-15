//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services.h
// Purpose: Public C ABI for Zanna.Services: the provider-neutral platform
//          services layer (lifecycle, status, identity, licensing, events,
//          asynchronous requests, diagnostics) and its constant classes.
// Key invariants:
//   - At most one provider is active per process; the neutral surface never
//     names a specific store.
//   - Every stateful entry point must run on the main thread and traps with
//     "Services: <Class>.<Member> must be called on the main thread" otherwise.
//     Constant getters may run on any thread.
//   - Absence is normal: with no started provider every query returns a
//     neutral value (0, false, or the empty string) and never traps.
//   - Constant ordinals are stable public values documented in ADR 0352 and
//     ADR 0353; later phases only append values.
// Ownership/Lifetime:
//   - String results are new caller-owned references.
//   - Init returns a caller-owned Zanna.Result; request methods return a
//     caller-owned Zanna.Services.Request; Diagnostics returns a caller-owned
//     Seq of caller-owned strings.
//   - A Request owns copies of its result text and leaderboard entries.
//   - Provider state is process-global and lives until Shutdown.
// Links: src/runtime/services/rt_services.c,
//        src/runtime/services/rt_services_provider.h,
//        src/runtime/services/rt_services_progress.h,
//        src/runtime/services/rt_services_social.h,
//        src/runtime/services/rt_services_cloud.h,
//        docs/zannalib/services.md,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services.h
 * @brief Declares the provider-neutral Zanna.Services runtime surface.
 * @details A game starts one provider (for example "steam") with
 *          @ref rt_services_platform_init, pumps it once per frame (Canvas and
 *          Canvas3D polls do this automatically while a provider is started),
 *          reads identity and licensing state, drains platform events with
 *          @ref rt_services_platform_poll_event, and starts non-blocking
 *          requests whose progress is observed through
 *          Zanna.Services.Request objects.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Stable constants (mirrored by the Zanna.Services constant classes)
//===----------------------------------------------------------------------===//

/// @brief Runtime heap class id for Zanna.Services.Request objects.
#define RT_SERVICES_REQUEST_CLASS_ID INT64_C(-0x5E0101)
/// @brief Runtime heap class id for Zanna.Services.WorkshopItem objects.
#define RT_SERVICES_WORKSHOP_ITEM_CLASS_ID INT64_C(-0x5E0102)

/// @brief A provider is started and serving requests.
#define RT_SERVICES_STATUS_OK INT64_C(0)
/// @brief No provider has been started, or the last one was shut down.
#define RT_SERVICES_STATUS_NOT_STARTED INT64_C(1)
/// @brief Init named a provider that is not compiled into this runtime.
#define RT_SERVICES_STATUS_UNKNOWN_PROVIDER INT64_C(2)
/// @brief The provider's redistributable library file does not exist.
#define RT_SERVICES_STATUS_LIBRARY_NOT_FOUND INT64_C(3)
/// @brief The library exists but could not be loaded or lacks required exports.
#define RT_SERVICES_STATUS_LIBRARY_INCOMPATIBLE INT64_C(4)
/// @brief The provider has no redistributable for this operating system and architecture.
#define RT_SERVICES_STATUS_UNSUPPORTED_PLATFORM INT64_C(5)
/// @brief The platform client (for example the Steam client) is not running.
#define RT_SERVICES_STATUS_CLIENT_NOT_RUNNING INT64_C(6)
/// @brief The platform client is older than the redistributable requires.
#define RT_SERVICES_STATUS_VERSION_MISMATCH INT64_C(7)
/// @brief The provider failed to initialize for another reason.
#define RT_SERVICES_STATUS_INIT_FAILED INT64_C(8)

/// @brief No event is available.
#define RT_SERVICES_EVENT_NONE INT64_C(0)
/// @brief The platform client connected to its online service.
#define RT_SERVICES_EVENT_SERVICE_CONNECTED INT64_C(1)
/// @brief The platform client lost its online service connection (code = provider result).
#define RT_SERVICES_EVENT_SERVICE_DISCONNECTED INT64_C(2)
/// @brief A connection attempt failed (code = provider result, flag = still retrying).
#define RT_SERVICES_EVENT_CONNECT_FAILED INT64_C(3)
/// @brief The platform overlay opened or closed (flag = open, value = 1 when user-initiated).
#define RT_SERVICES_EVENT_OVERLAY_CHANGED INT64_C(4)
/// @brief A DLC finished installing (text = provider DLC id, value = numeric id when known).
#define RT_SERVICES_EVENT_DLC_INSTALLED INT64_C(5)
/// @brief The game was relaunched with new launch parameters while running; read them again
///        with Platform.LaunchCommandLine and Platform.LaunchParameter.
#define RT_SERVICES_EVENT_LAUNCH_PARAMETERS_CHANGED INT64_C(6)
/// @brief The platform client is shutting down; the game should save and stop the provider.
#define RT_SERVICES_EVENT_SERVICE_SHUTDOWN INT64_C(7)
/// @brief Stats.Store finished (code = provider result, flag = stored successfully).
#define RT_SERVICES_EVENT_STATS_STORED INT64_C(8)
/// @brief An achievement unlock or progress report was stored (text = achievement id,
///        flag = unlocked, value = progress, total = progress maximum).
#define RT_SERVICES_EVENT_ACHIEVEMENT_STORED INT64_C(9)
/// @brief The floating on-screen keyboard opened by OnScreenKeyboard.ShowFloating closed.
#define RT_SERVICES_EVENT_TEXT_INPUT_DISMISSED INT64_C(10)
/// @brief An achievement icon finished loading (text = achievement id, flag = unlocked variant,
///        value = 1 when the achievement has an icon for that state).
#define RT_SERVICES_EVENT_ACHIEVEMENT_ICON_READY INT64_C(11)
/// @brief A controller connected while ActionInput is started (text = controller id).
#define RT_SERVICES_EVENT_CONTROLLER_CONNECTED INT64_C(12)
/// @brief A controller disconnected while ActionInput is started (text = controller id).
#define RT_SERVICES_EVENT_CONTROLLER_DISCONNECTED INT64_C(13)
/// @brief A controller's binding configuration loaded (text = controller id, flag = the
///        configuration binds actions, value = major binding revision).
#define RT_SERVICES_EVENT_CONTROLLER_CONFIGURED INT64_C(14)
/// @brief A Workshop item was installed or updated on disk (text = item id).
#define RT_SERVICES_EVENT_WORKSHOP_ITEM_INSTALLED INT64_C(15)
/// @brief A Workshop.Download finished (text = item id, code = provider result, flag = succeeded).
#define RT_SERVICES_EVENT_WORKSHOP_ITEM_DOWNLOADED INT64_C(16)
/// @brief The player subscribed to or unsubscribed from a Workshop item (text = item id,
///        flag = subscribed).
#define RT_SERVICES_EVENT_WORKSHOP_SUBSCRIPTION_CHANGED INT64_C(17)

/// @brief User identity queries (UserId, UserName, IsOnline).
#define RT_SERVICES_FEATURE_IDENTITY INT64_C(1)
/// @brief License and DLC ownership queries (IsLicensed, IsDlcInstalled).
#define RT_SERVICES_FEATURE_LICENSING INT64_C(2)
/// @brief The user's selected game language.
#define RT_SERVICES_FEATURE_LANGUAGE INT64_C(3)
/// @brief Current player-count requests.
#define RT_SERVICES_FEATURE_PLAYER_COUNT INT64_C(4)
/// @brief Zanna.Services.Achievements.
#define RT_SERVICES_FEATURE_ACHIEVEMENTS INT64_C(5)
/// @brief Zanna.Services.Stats.
#define RT_SERVICES_FEATURE_STATS INT64_C(6)
/// @brief Zanna.Services.Leaderboards.
#define RT_SERVICES_FEATURE_LEADERBOARDS INT64_C(7)
/// @brief Zanna.Services.Presence.
#define RT_SERVICES_FEATURE_PRESENCE INT64_C(8)
/// @brief Zanna.Services.Overlay.
#define RT_SERVICES_FEATURE_OVERLAY INT64_C(9)
/// @brief Zanna.Services.OnScreenKeyboard.
#define RT_SERVICES_FEATURE_TEXT_INPUT INT64_C(10)
/// @brief Zanna.Services.Cloud.
#define RT_SERVICES_FEATURE_CLOUD INT64_C(11)
/// @brief Launch parameters (Platform.LaunchCommandLine, Platform.LaunchParameter).
#define RT_SERVICES_FEATURE_LAUNCH_PARAMETERS INT64_C(12)
/// @brief Zanna.Services.Timeline (game recording timeline markers and phases).
#define RT_SERVICES_FEATURE_TIMELINE INT64_C(13)
/// @brief Application details (DlcCount, DlcIdAt, DlcNameAt, DlcAvailableAt, BuildId, BranchName).
#define RT_SERVICES_FEATURE_APP_DETAILS INT64_C(14)
/// @brief Achievement icons (Achievements.IconWidth, IconHeight, IconRgba).
#define RT_SERVICES_FEATURE_ACHIEVEMENT_ICONS INT64_C(15)
/// @brief Global unlock percentages (Achievements.RequestGlobalPercentages, GlobalPercent).
#define RT_SERVICES_FEATURE_ACHIEVEMENT_PERCENTAGES INT64_C(16)
/// @brief Zanna.Services.ActionInput (platform action-based controller input).
#define RT_SERVICES_FEATURE_ACTION_INPUT INT64_C(17)
/// @brief Zanna.Services.Workshop (user-generated content).
#define RT_SERVICES_FEATURE_WORKSHOP INT64_C(18)

/// @brief Request kind for Platform.RequestPlayerCount.
#define RT_SERVICES_REQUEST_PLAYER_COUNT INT64_C(1)
/// @brief Request kind for Leaderboards.Find and Leaderboards.FindOrCreate.
#define RT_SERVICES_REQUEST_LEADERBOARD_FIND INT64_C(2)
/// @brief Request kind for Leaderboards.Upload.
#define RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD INT64_C(3)
/// @brief Request kind for Leaderboards.Download.
#define RT_SERVICES_REQUEST_LEADERBOARD_DOWNLOAD INT64_C(4)
/// @brief Request kind for OnScreenKeyboard.RequestText.
#define RT_SERVICES_REQUEST_TEXT_INPUT INT64_C(5)
/// @brief Request kind for Timeline.RequestEventRecording.
#define RT_SERVICES_REQUEST_TIMELINE_EVENT_RECORDING INT64_C(6)
/// @brief Request kind for Timeline.RequestPhaseRecording.
#define RT_SERVICES_REQUEST_TIMELINE_PHASE_RECORDING INT64_C(7)
/// @brief Request kind for Achievements.RequestGlobalPercentages.
#define RT_SERVICES_REQUEST_ACHIEVEMENT_PERCENTAGES INT64_C(8)
/// @brief Request kind for Workshop.Query, Workshop.QueryUser, and Workshop.QueryItems.
#define RT_SERVICES_REQUEST_WORKSHOP_QUERY INT64_C(9)
/// @brief Request kind for Workshop.Subscribe.
#define RT_SERVICES_REQUEST_WORKSHOP_SUBSCRIBE INT64_C(10)
/// @brief Request kind for Workshop.Unsubscribe.
#define RT_SERVICES_REQUEST_WORKSHOP_UNSUBSCRIBE INT64_C(11)
/// @brief Request kind for Workshop.CreateItem.
#define RT_SERVICES_REQUEST_WORKSHOP_CREATE INT64_C(12)
/// @brief Request kind for Workshop.SubmitUpdate.
#define RT_SERVICES_REQUEST_WORKSHOP_SUBMIT INT64_C(13)
/// @brief Request kind for Workshop.DeleteItem.
#define RT_SERVICES_REQUEST_WORKSHOP_DELETE INT64_C(14)

/// @brief Capacity of the platform event queue.
#define RT_SERVICES_EVENT_CAPACITY 256
/// @brief Maximum number of requests that may be pending at once.
#define RT_SERVICES_PENDING_REQUEST_CAPACITY 64
/// @brief Maximum number of retained diagnostic messages.
#define RT_SERVICES_DIAGNOSTIC_CAPACITY 32
/// @brief Maximum number of leaderboard entries one request holds.
#define RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY 100
/// @brief Maximum number of integer details one request holds.
#define RT_SERVICES_REQUEST_DETAIL_CAPACITY 8
/// @brief Maximum number of Workshop items one request holds.
#define RT_SERVICES_REQUEST_ITEM_CAPACITY 100

//===----------------------------------------------------------------------===//
// Zanna.Services.Platform
//===----------------------------------------------------------------------===//

/// @brief Start a platform services provider.
/// @details Resolves @p provider case-insensitively against the providers
///          compiled into this runtime and starts it with the provider-defined
///          @p app_id. Starting the already-active provider succeeds without
///          restarting it. Starting a different provider while one is active
///          fails and leaves the active provider untouched. On success the
///          frame pump is installed so Canvas and Canvas3D polls pump the
///          provider automatically. Failure messages are also recorded in
///          @ref rt_services_platform_diagnostics.
/// @param provider Provider id such as "steam"; NULL is treated as empty.
/// @param app_id Provider-defined application id; a malformed id traps.
/// @return Caller-owned Zanna.Result: Ok(provider id) or Err(message).
void *rt_services_platform_init(rt_string provider, rt_string app_id);

/// @brief Pump the active provider once.
/// @details Delivers pending platform callbacks as events and completes
///          finished requests. A no-op when no provider is started.
void rt_services_platform_update(void);

/// @brief Stop the active provider and reset session state.
/// @details Completes pending requests as failed, clears the event queue and
///          the last polled event, detaches the frame pump, and returns
///          Status to NotStarted. Idempotent. Diagnostics are retained.
void rt_services_platform_shutdown(void);

/// @brief Dequeue the next platform event.
/// @details The dequeued event becomes the "last polled event" read by the
///          EventResultCode/EventText/EventValue/EventFlag getters. An empty
///          queue clears the last polled event.
/// @return The event kind (RT_SERVICES_EVENT_*), or RT_SERVICES_EVENT_NONE.
int64_t rt_services_platform_poll_event(void);

/// @brief Report whether a provider id is compiled into this runtime.
/// @param name Provider id, compared ASCII case-insensitively; NULL is empty.
/// @return 1 when the provider exists, otherwise 0.
int8_t rt_services_platform_has_provider(rt_string name);

/// @brief Report whether the active provider currently supports a feature.
/// @param feature RT_SERVICES_FEATURE_* value; unknown values report 0.
/// @return 1 when a started provider supports @p feature, otherwise 0.
int8_t rt_services_platform_has_feature(int64_t feature);

/// @brief Report whether a DLC is owned and installed.
/// @details The id format is provider-defined; the Steam provider requires a
///          decimal app id and traps on a malformed id.
/// @param dlc_id Provider-defined DLC id.
/// @return 1 when installed, otherwise 0 (including when no provider is started).
int8_t rt_services_platform_is_dlc_installed(rt_string dlc_id);

/// @brief Start a non-blocking request for the game's current player count.
/// @details The returned request completes during a later pump. When no
///          provider supports the feature, or the pending-request limit is
///          reached, the request is returned already completed as failed.
/// @return Caller-owned Zanna.Services.Request (never NULL unless allocation traps).
void *rt_services_platform_request_player_count(void);

/// @brief Copy the retained diagnostic messages, oldest first.
/// @details Holds at most RT_SERVICES_DIAGNOSTIC_CAPACITY entries; older
///          entries are discarded first. Covers Init failures, disabled
///          provider features, rejected callback payloads, and event overflow.
/// @return Caller-owned Seq of caller-owned strings.
void *rt_services_platform_diagnostics(void);

/// @brief Read one launch parameter passed by the platform's launch URL.
/// @details Steam passes parameters as `steam://run/<appid>//?key=value&...`;
///          names starting with `@` are reserved and always read as empty.
///          Read them again after EventKind.LaunchParametersChanged. An empty
///          key traps with "Services.Platform.LaunchParameter: key must not be
///          empty", whether or not a provider is started.
/// @param key Parameter name.
/// @return Caller-owned value, or the empty string when absent or unavailable.
rt_string rt_services_platform_launch_parameter(rt_string key);

/// @brief Report whether a provider is started.
/// @return 1 while a provider is active, otherwise 0.
int8_t rt_services_platform_get_is_available(void);

/// @brief Read the platform services status.
/// @return RT_SERVICES_STATUS_* value.
int64_t rt_services_platform_get_status(void);

/// @brief Read the active provider id.
/// @return Caller-owned provider id (for example "steam"), or the empty string.
rt_string rt_services_platform_get_provider(void);

/// @brief Read the application id reported by the active provider.
/// @return Caller-owned id string, or the empty string when unavailable.
rt_string rt_services_platform_get_app_id(void);

/// @brief Read the signed-in user's provider-defined id.
/// @return Caller-owned id string (decimal SteamID64 for Steam), or the empty string.
rt_string rt_services_platform_get_user_id(void);

/// @brief Read the signed-in user's display name.
/// @return Caller-owned name, or the empty string when unavailable.
rt_string rt_services_platform_get_user_name(void);

/// @brief Read the user's selected game language code.
/// @return Caller-owned provider-defined language code (Steam: "english",
///         "german", ...), or the empty string when unavailable.
rt_string rt_services_platform_get_language(void);

/// @brief Report whether the user holds a license for the running application.
/// @return 1 when licensed, otherwise 0.
int8_t rt_services_platform_get_is_licensed(void);

/// @brief Report whether the platform client is connected to its online service.
/// @return 1 when online, otherwise 0.
int8_t rt_services_platform_get_is_online(void);

/// @brief Read the command line passed by the platform's launch URL.
/// @details Steam passes it as `steam://run/<appid>//<command line>/`, for
///          example when a friend joins through rich presence. This is not the
///          operating system command line (see Zanna.System.Environment). Read
///          it again after EventKind.LaunchParametersChanged.
/// @return Caller-owned command line, or the empty string when there is none.
rt_string rt_services_platform_get_launch_command_line(void);

/// @brief Count the DLC the application defines, owned or not.
/// @return Count (Steam reports at most 64), or 0 when unavailable.
int64_t rt_services_platform_get_dlc_count(void);

/// @brief Read the id of the DLC at @p index.
/// @param index DLC index in 0..DlcCount-1.
/// @return Caller-owned provider-defined DLC id, or the empty string outside the range.
rt_string rt_services_platform_dlc_id_at(int64_t index);

/// @brief Read the display name of the DLC at @p index.
/// @param index DLC index in 0..DlcCount-1.
/// @return Caller-owned name, or the empty string outside the range.
rt_string rt_services_platform_dlc_name_at(int64_t index);

/// @brief Report whether the DLC at @p index can be bought now.
/// @param index DLC index in 0..DlcCount-1.
/// @return 1 when it is available in the store, otherwise 0.
int8_t rt_services_platform_dlc_available_at(int64_t index);

/// @brief Read the installed build's id.
/// @return Build id (0 when the build did not come from the platform), or 0 when unavailable.
int64_t rt_services_platform_get_build_id(void);

/// @brief Read the branch the installed build comes from.
/// @return Caller-owned branch name (Steam: the beta name), or the empty string on the default
///         branch or when unavailable.
rt_string rt_services_platform_get_branch_name(void);

/// @brief Read the provider result code of the last polled event.
/// @return Provider-defined code (Steam: EResult), or 0.
int64_t rt_services_platform_get_event_result_code(void);

/// @brief Read the text payload of the last polled event.
/// @return Caller-owned text, or the empty string.
rt_string rt_services_platform_get_event_text(void);

/// @brief Read the integer payload of the last polled event.
/// @return Event-specific value, or 0.
int64_t rt_services_platform_get_event_value(void);

/// @brief Read the boolean payload of the last polled event.
/// @return Event-specific flag, or 0.
int8_t rt_services_platform_get_event_flag(void);

/// @brief Read the total that the last polled event's value counts toward.
/// @details Progress events such as AchievementStored report a value out of a
///          total; every other event reports 0.
/// @return Event-specific total, or 0.
int64_t rt_services_platform_get_event_total(void);

/// @brief Read how many events were discarded because the queue was full.
/// @return Count since the last Init or Shutdown.
int64_t rt_services_platform_get_dropped_events(void);

//===----------------------------------------------------------------------===//
// Zanna.Services.Request
//===----------------------------------------------------------------------===//

/// @brief Read a request's kind.
/// @param request Borrowed Zanna.Services.Request; an invalid handle traps.
/// @return RT_SERVICES_REQUEST_* value, or 0 for NULL.
int64_t rt_services_request_get_kind(void *request);

/// @brief Report whether a request has completed (successfully or not).
/// @param request Borrowed Zanna.Services.Request.
/// @return 1 when completed, otherwise 0.
int8_t rt_services_request_get_is_done(void *request);

/// @brief Report whether a completed request succeeded.
/// @param request Borrowed Zanna.Services.Request.
/// @return 1 when completed successfully, otherwise 0.
int8_t rt_services_request_get_succeeded(void *request);

/// @brief Read a completed request's provider result code.
/// @param request Borrowed Zanna.Services.Request.
/// @return Provider-defined code (Steam: 1 for success), or 0 when none exists.
int64_t rt_services_request_get_result_code(void *request);

/// @brief Read a completed request's integer result.
/// @param request Borrowed Zanna.Services.Request.
/// @return Kind-specific value (player count for PlayerCount, total board
///         entries for LeaderboardFind and LeaderboardDownload, the new global
///         rank for LeaderboardUpload), or 0.
int64_t rt_services_request_get_value(void *request);

/// @brief Read a completed request's boolean result.
/// @param request Borrowed Zanna.Services.Request.
/// @return Kind-specific flag (LeaderboardUpload: the stored score changed), or 0.
int8_t rt_services_request_get_flag(void *request);

/// @brief Read a completed request's text result.
/// @param request Borrowed Zanna.Services.Request.
/// @return Caller-owned kind-specific text (the leaderboard name for leaderboard
///         kinds, the entered text for TextInput), or the empty string.
rt_string rt_services_request_get_text(void *request);

/// @brief Read a failed request's error message.
/// @param request Borrowed Zanna.Services.Request.
/// @return Caller-owned message, or the empty string when none exists.
rt_string rt_services_request_get_error(void *request);

/// @brief Read how many leaderboard entries a completed request holds.
/// @param request Borrowed Zanna.Services.Request.
/// @return Entry count (LeaderboardDownload only; at most
///         RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY), or 0.
int64_t rt_services_request_get_entry_count(void *request);

/// @brief Read one leaderboard entry's global rank.
/// @param request Borrowed Zanna.Services.Request.
/// @param index Entry index in 0..EntryCount-1; other values trap.
/// @return Global rank, starting at 1.
int64_t rt_services_request_entry_rank(void *request, int64_t index);

/// @brief Read one leaderboard entry's score.
/// @param request Borrowed Zanna.Services.Request.
/// @param index Entry index in 0..EntryCount-1; other values trap.
/// @return Score as stored on the board.
int64_t rt_services_request_entry_score(void *request, int64_t index);

/// @brief Read one leaderboard entry's user id.
/// @param request Borrowed Zanna.Services.Request.
/// @param index Entry index in 0..EntryCount-1; other values trap.
/// @return Caller-owned provider-defined user id (decimal SteamID64 on Steam).
rt_string rt_services_request_entry_user_id(void *request, int64_t index);

/// @brief Read one leaderboard entry's user display name.
/// @details While the provider that produced the request is still active the
///          name is looked up live, because platforms can learn names after the
///          entries arrive; otherwise the name captured at completion is used.
/// @param request Borrowed Zanna.Services.Request.
/// @param index Entry index in 0..EntryCount-1; other values trap.
/// @return Caller-owned display name, or the empty string while it is unknown.
rt_string rt_services_request_entry_user_name(void *request, int64_t index);

/// @brief Read how many integer details a completed request holds.
/// @param request Borrowed Zanna.Services.Request.
/// @return Detail count (kind-specific; at most RT_SERVICES_REQUEST_DETAIL_CAPACITY), or 0.
int64_t rt_services_request_get_detail_count(void *request);

/// @brief Read one integer detail of a completed request.
/// @details The meaning of each index depends on the request kind (for
///          TimelinePhaseRecording: 0 recorded milliseconds, 1 longest clip
///          milliseconds, 2 clip count, 3 screenshot count).
/// @param request Borrowed Zanna.Services.Request.
/// @param index Detail index in 0..DetailCount-1; other values trap.
/// @return Detail value.
int64_t rt_services_request_detail(void *request, int64_t index);

/// @brief Count the Workshop items a completed query holds.
/// @param request Borrowed Zanna.Services.Request.
/// @return Item count (at most RT_SERVICES_REQUEST_ITEM_CAPACITY), or 0.
int64_t rt_services_request_get_item_count(void *request);

/// @brief Read one Workshop item of a completed query.
/// @param request Borrowed Zanna.Services.Request.
/// @param index Item index in 0..ItemCount-1; other values trap.
/// @return Caller-owned Zanna.Services.WorkshopItem, or NULL after a trap.
void *rt_services_request_item_at(void *request, int64_t index);

//===----------------------------------------------------------------------===//
// Constant classes: Zanna.Services.Status / EventKind / Feature / RequestKind
//===----------------------------------------------------------------------===//

/// @brief Return `Zanna.Services.Status.Ok`.
/// @return Stable ordinal 0.
int64_t rt_services_status_ok(void);
/// @brief Return `Zanna.Services.Status.NotStarted`.
/// @return Stable ordinal 1.
int64_t rt_services_status_not_started(void);
/// @brief Return `Zanna.Services.Status.UnknownProvider`.
/// @return Stable ordinal 2.
int64_t rt_services_status_unknown_provider(void);
/// @brief Return `Zanna.Services.Status.LibraryNotFound`.
/// @return Stable ordinal 3.
int64_t rt_services_status_library_not_found(void);
/// @brief Return `Zanna.Services.Status.LibraryIncompatible`.
/// @return Stable ordinal 4.
int64_t rt_services_status_library_incompatible(void);
/// @brief Return `Zanna.Services.Status.UnsupportedPlatform`.
/// @return Stable ordinal 5.
int64_t rt_services_status_unsupported_platform(void);
/// @brief Return `Zanna.Services.Status.ClientNotRunning`.
/// @return Stable ordinal 6.
int64_t rt_services_status_client_not_running(void);
/// @brief Return `Zanna.Services.Status.VersionMismatch`.
/// @return Stable ordinal 7.
int64_t rt_services_status_version_mismatch(void);
/// @brief Return `Zanna.Services.Status.InitFailed`.
/// @return Stable ordinal 8.
int64_t rt_services_status_init_failed(void);

/// @brief Return `Zanna.Services.EventKind.None`.
/// @return Stable ordinal 0.
int64_t rt_services_event_kind_none(void);
/// @brief Return `Zanna.Services.EventKind.ServiceConnected`.
/// @return Stable ordinal 1.
int64_t rt_services_event_kind_service_connected(void);
/// @brief Return `Zanna.Services.EventKind.ServiceDisconnected`.
/// @return Stable ordinal 2.
int64_t rt_services_event_kind_service_disconnected(void);
/// @brief Return `Zanna.Services.EventKind.ConnectFailed`.
/// @return Stable ordinal 3.
int64_t rt_services_event_kind_connect_failed(void);
/// @brief Return `Zanna.Services.EventKind.OverlayChanged`.
/// @return Stable ordinal 4.
int64_t rt_services_event_kind_overlay_changed(void);
/// @brief Return `Zanna.Services.EventKind.DlcInstalled`.
/// @return Stable ordinal 5.
int64_t rt_services_event_kind_dlc_installed(void);
/// @brief Return `Zanna.Services.EventKind.LaunchParametersChanged`.
/// @return Stable ordinal 6.
int64_t rt_services_event_kind_launch_parameters_changed(void);
/// @brief Return `Zanna.Services.EventKind.ServiceShutdown`.
/// @return Stable ordinal 7.
int64_t rt_services_event_kind_service_shutdown(void);
/// @brief Return `Zanna.Services.EventKind.StatsStored`.
/// @return Stable ordinal 8.
int64_t rt_services_event_kind_stats_stored(void);
/// @brief Return `Zanna.Services.EventKind.AchievementStored`.
/// @return Stable ordinal 9.
int64_t rt_services_event_kind_achievement_stored(void);
/// @brief Return `Zanna.Services.EventKind.TextInputDismissed`.
/// @return Stable ordinal 10.
int64_t rt_services_event_kind_text_input_dismissed(void);
/// @brief Return `Zanna.Services.EventKind.AchievementIconReady`.
/// @return Stable ordinal 11.
int64_t rt_services_event_kind_achievement_icon_ready(void);
/// @brief Return `Zanna.Services.EventKind.ControllerConnected`.
/// @return Stable ordinal 12.
int64_t rt_services_event_kind_controller_connected(void);
/// @brief Return `Zanna.Services.EventKind.ControllerDisconnected`.
/// @return Stable ordinal 13.
int64_t rt_services_event_kind_controller_disconnected(void);
/// @brief Return `Zanna.Services.EventKind.ControllerConfigured`.
/// @return Stable ordinal 14.
int64_t rt_services_event_kind_controller_configured(void);
/// @brief Return `Zanna.Services.EventKind.WorkshopItemInstalled`.
/// @return Stable ordinal 15.
int64_t rt_services_event_kind_workshop_item_installed(void);
/// @brief Return `Zanna.Services.EventKind.WorkshopItemDownloaded`.
/// @return Stable ordinal 16.
int64_t rt_services_event_kind_workshop_item_downloaded(void);
/// @brief Return `Zanna.Services.EventKind.WorkshopSubscriptionChanged`.
/// @return Stable ordinal 17.
int64_t rt_services_event_kind_workshop_subscription_changed(void);

/// @brief Return `Zanna.Services.Feature.Identity`.
/// @return Stable ordinal 1.
int64_t rt_services_feature_identity(void);
/// @brief Return `Zanna.Services.Feature.Licensing`.
/// @return Stable ordinal 2.
int64_t rt_services_feature_licensing(void);
/// @brief Return `Zanna.Services.Feature.Language`.
/// @return Stable ordinal 3.
int64_t rt_services_feature_language(void);
/// @brief Return `Zanna.Services.Feature.PlayerCount`.
/// @return Stable ordinal 4.
int64_t rt_services_feature_player_count(void);
/// @brief Return `Zanna.Services.Feature.Achievements`.
/// @return Stable ordinal 5.
int64_t rt_services_feature_achievements(void);
/// @brief Return `Zanna.Services.Feature.Stats`.
/// @return Stable ordinal 6.
int64_t rt_services_feature_stats(void);
/// @brief Return `Zanna.Services.Feature.Leaderboards`.
/// @return Stable ordinal 7.
int64_t rt_services_feature_leaderboards(void);
/// @brief Return `Zanna.Services.Feature.Presence`.
/// @return Stable ordinal 8.
int64_t rt_services_feature_presence(void);
/// @brief Return `Zanna.Services.Feature.Overlay`.
/// @return Stable ordinal 9.
int64_t rt_services_feature_overlay(void);
/// @brief Return `Zanna.Services.Feature.TextInput`.
/// @return Stable ordinal 10.
int64_t rt_services_feature_text_input(void);
/// @brief Return `Zanna.Services.Feature.Cloud`.
/// @return Stable ordinal 11.
int64_t rt_services_feature_cloud(void);
/// @brief Return `Zanna.Services.Feature.LaunchParameters`.
/// @return Stable ordinal 12.
int64_t rt_services_feature_launch_parameters(void);
/// @brief Return `Zanna.Services.Feature.Timeline`.
/// @return Stable ordinal 13.
int64_t rt_services_feature_timeline(void);
/// @brief Return `Zanna.Services.Feature.AppDetails`.
/// @return Stable ordinal 14.
int64_t rt_services_feature_app_details(void);
/// @brief Return `Zanna.Services.Feature.AchievementIcons`.
/// @return Stable ordinal 15.
int64_t rt_services_feature_achievement_icons(void);
/// @brief Return `Zanna.Services.Feature.AchievementPercentages`.
/// @return Stable ordinal 16.
int64_t rt_services_feature_achievement_percentages(void);
/// @brief Return `Zanna.Services.Feature.ActionInput`.
/// @return Stable ordinal 17.
int64_t rt_services_feature_action_input(void);
/// @brief Return `Zanna.Services.Feature.Workshop`.
/// @return Stable ordinal 18.
int64_t rt_services_feature_workshop(void);

/// @brief Return `Zanna.Services.RequestKind.PlayerCount`.
/// @return Stable ordinal 1.
int64_t rt_services_request_kind_player_count(void);
/// @brief Return `Zanna.Services.RequestKind.LeaderboardFind`.
/// @return Stable ordinal 2.
int64_t rt_services_request_kind_leaderboard_find(void);
/// @brief Return `Zanna.Services.RequestKind.LeaderboardUpload`.
/// @return Stable ordinal 3.
int64_t rt_services_request_kind_leaderboard_upload(void);
/// @brief Return `Zanna.Services.RequestKind.LeaderboardDownload`.
/// @return Stable ordinal 4.
int64_t rt_services_request_kind_leaderboard_download(void);
/// @brief Return `Zanna.Services.RequestKind.TextInput`.
/// @return Stable ordinal 5.
int64_t rt_services_request_kind_text_input(void);
/// @brief Return `Zanna.Services.RequestKind.TimelineEventRecording`.
/// @return Stable ordinal 6.
int64_t rt_services_request_kind_timeline_event_recording(void);
/// @brief Return `Zanna.Services.RequestKind.TimelinePhaseRecording`.
/// @return Stable ordinal 7.
int64_t rt_services_request_kind_timeline_phase_recording(void);
/// @brief Return `Zanna.Services.RequestKind.AchievementPercentages`.
/// @return Stable ordinal 8.
int64_t rt_services_request_kind_achievement_percentages(void);
/// @brief Return `Zanna.Services.RequestKind.WorkshopQuery`.
/// @return Stable ordinal 9.
int64_t rt_services_request_kind_workshop_query(void);
/// @brief Return `Zanna.Services.RequestKind.WorkshopSubscribe`.
/// @return Stable ordinal 10.
int64_t rt_services_request_kind_workshop_subscribe(void);
/// @brief Return `Zanna.Services.RequestKind.WorkshopUnsubscribe`.
/// @return Stable ordinal 11.
int64_t rt_services_request_kind_workshop_unsubscribe(void);
/// @brief Return `Zanna.Services.RequestKind.WorkshopCreate`.
/// @return Stable ordinal 12.
int64_t rt_services_request_kind_workshop_create(void);
/// @brief Return `Zanna.Services.RequestKind.WorkshopSubmit`.
/// @return Stable ordinal 13.
int64_t rt_services_request_kind_workshop_submit(void);
/// @brief Return `Zanna.Services.RequestKind.WorkshopDelete`.
/// @return Stable ordinal 14.
int64_t rt_services_request_kind_workshop_delete(void);

#ifdef __cplusplus
}
#endif
