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
//   - Constant ordinals are stable public values documented in ADR 0352.
// Ownership/Lifetime:
//   - String results are new caller-owned references.
//   - Init returns a caller-owned Zanna.Result; request methods return a
//     caller-owned Zanna.Services.Request; Diagnostics returns a caller-owned
//     Seq of caller-owned strings.
//   - Provider state is process-global and lives until Shutdown.
// Links: src/runtime/services/rt_services.c,
//        src/runtime/services/rt_services_provider.h,
//        docs/zannalib/services.md,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md
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
/// @brief The game was relaunched with new launch parameters while running.
#define RT_SERVICES_EVENT_LAUNCH_PARAMETERS_CHANGED INT64_C(6)
/// @brief The platform client is shutting down; the game should save and stop the provider.
#define RT_SERVICES_EVENT_SERVICE_SHUTDOWN INT64_C(7)

/// @brief User identity queries (UserId, UserName, IsOnline).
#define RT_SERVICES_FEATURE_IDENTITY INT64_C(1)
/// @brief License and DLC ownership queries (IsLicensed, IsDlcInstalled).
#define RT_SERVICES_FEATURE_LICENSING INT64_C(2)
/// @brief The user's selected game language.
#define RT_SERVICES_FEATURE_LANGUAGE INT64_C(3)
/// @brief Current player-count requests.
#define RT_SERVICES_FEATURE_PLAYER_COUNT INT64_C(4)

/// @brief Request kind for Platform.RequestPlayerCount.
#define RT_SERVICES_REQUEST_PLAYER_COUNT INT64_C(1)

/// @brief Capacity of the platform event queue.
#define RT_SERVICES_EVENT_CAPACITY 256
/// @brief Maximum number of requests that may be pending at once.
#define RT_SERVICES_PENDING_REQUEST_CAPACITY 64
/// @brief Maximum number of retained diagnostic messages.
#define RT_SERVICES_DIAGNOSTIC_CAPACITY 32

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
/// @return Kind-specific value (player count for PlayerCount), or 0.
int64_t rt_services_request_get_value(void *request);

/// @brief Read a failed request's error message.
/// @param request Borrowed Zanna.Services.Request.
/// @return Caller-owned message, or the empty string when none exists.
rt_string rt_services_request_get_error(void *request);

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

/// @brief Return `Zanna.Services.RequestKind.PlayerCount`.
/// @return Stable ordinal 1.
int64_t rt_services_request_kind_player_count(void);

#ifdef __cplusplus
}
#endif
