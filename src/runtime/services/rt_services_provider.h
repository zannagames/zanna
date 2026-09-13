//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_provider.h
// Purpose: Internal contract between the provider-neutral Zanna.Services core
//          and the platform providers compiled into it (Steam today; other
//          stores later).
// Key invariants:
//   - A provider is a static, immutable rt_services_provider table. The core
//     calls its callbacks only on the main thread and only in this order:
//     start, then any number of pump/query/begin_request calls, then stop.
//   - No callback other than start is invoked while the provider is stopped.
//   - Query callbacks may be NULL; the core then reports neutral values.
//   - Providers report back exclusively through the rt_services_provider_*
//     functions declared here, which are main-thread only.
// Ownership/Lifetime:
//   - Provider tables have static storage duration.
//   - String query results are new caller-owned references (or NULL for empty).
//   - Text passed to the report functions is borrowed and copied immediately.
// Links: src/runtime/services/rt_services.c,
//        src/runtime/services/steam/rt_steam_provider.c,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_provider.h
 * @brief Declares the platform services provider interface and core callbacks.
 * @details Adding a store means implementing one @ref rt_services_provider
 *          table, listing it in the core's registry (rt_services.c), and
 *          adding optional Zanna.Services.<Provider> extension classes. The
 *          neutral Zanna.Services.Platform surface does not change.
 */

#pragma once

#include "rt_platform.h"
#include "rt_string.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Capacity, in bytes including the terminator, of a provider message buffer.
#define RT_SERVICES_MESSAGE_CAPACITY 2048

/// @brief Static callback table describing one platform services provider.
/// @details All callbacks run on the main thread. @ref start, @ref pump and
///          @ref stop are required; every other callback may be NULL, in which
///          case the core reports the neutral value for that query.
typedef struct rt_services_provider {
    /// @brief Canonical lowercase provider id accepted by Platform.Init (for example "steam").
    const char *id;
    /// @brief Human-readable provider name used in documentation and diagnostics.
    const char *display_name;

    /// @brief Start the provider for a provider-defined application id.
    /// @details Must validate @p app_id first (trapping on a malformed id) and
    ///          leave no provider state behind on failure. On failure the
    ///          provider writes one diagnostic sentence into @p message.
    /// @param app_id Borrowed, non-NULL application id.
    /// @param message Buffer receiving the failure message; untouched on success.
    /// @param message_capacity Size of @p message in bytes.
    /// @return RT_SERVICES_STATUS_OK or a failure RT_SERVICES_STATUS_* value.
    int64_t (*start)(rt_string app_id, char *message, size_t message_capacity);

    /// @brief Deliver pending platform callbacks as events and request completions.
    void (*pump)(void);

    /// @brief Stop the provider. Pending requests are cancelled by the core afterwards.
    void (*stop)(void);

    /// @brief Report whether a started provider supports a feature now.
    /// @param feature RT_SERVICES_FEATURE_* value.
    /// @return 1 when supported, otherwise 0.
    int8_t (*has_feature)(int64_t feature);

    /// @brief Read the provider's application id. @return Owned string or NULL.
    rt_string (*app_id)(void);
    /// @brief Read the signed-in user's id. @return Owned string or NULL.
    rt_string (*user_id)(void);
    /// @brief Read the signed-in user's display name. @return Owned string or NULL.
    rt_string (*user_name)(void);
    /// @brief Read the user's game language code. @return Owned string or NULL.
    rt_string (*language)(void);
    /// @brief Report whether the running application is licensed. @return 1 or 0.
    int8_t (*is_licensed)(void);
    /// @brief Report whether the client is connected online. @return 1 or 0.
    int8_t (*is_online)(void);

    /// @brief Report whether a DLC is owned and installed.
    /// @details Must trap on a malformed provider-defined id.
    /// @param dlc_id Borrowed, non-NULL DLC id.
    /// @return 1 when installed, otherwise 0.
    int8_t (*is_dlc_installed)(rt_string dlc_id);

    /// @brief Begin an asynchronous request of @p kind.
    /// @details On success stores a nonzero provider handle that the provider
    ///          later passes to @ref rt_services_provider_complete_request. On
    ///          failure writes one sentence into @p message.
    /// @param kind RT_SERVICES_REQUEST_* value.
    /// @param out_handle Receives the nonzero provider handle on success.
    /// @param message Buffer receiving the failure message.
    /// @param message_capacity Size of @p message in bytes.
    /// @return 1 when the request started, otherwise 0.
    int8_t (*begin_request)(int64_t kind,
                            uint64_t *out_handle,
                            char *message,
                            size_t message_capacity);
} rt_services_provider;

//===----------------------------------------------------------------------===//
// Built-in providers
//===----------------------------------------------------------------------===//

/// @brief Steamworks provider backed by the runtime-loaded steam_api redistributable.
extern const rt_services_provider rt_services_steam_provider;

//===----------------------------------------------------------------------===//
// Core functions available to providers (main thread only)
//===----------------------------------------------------------------------===//

/// @brief Trap unless the caller runs on the main thread.
/// @details Provider extension classes (for example Zanna.Services.Steam) use
///          this so every services entry point reports the same diagnostic:
///          "Services: <member> must be called on the main thread".
/// @param member Class-qualified member name, for example "Steam.SteamId".
/// @return 1 on the main thread, 0 after reporting the trap.
int rt_services_provider_require_main_thread(const char *member);

/// @brief Queue a platform event for Platform.PollEvent.
/// @details When the queue is full the oldest event is discarded and the
///          dropped-event counter increments.
/// @param kind RT_SERVICES_EVENT_* value; RT_SERVICES_EVENT_NONE is ignored.
/// @param result_code Provider-defined result code, or 0.
/// @param value Event-specific integer payload.
/// @param flag Event-specific boolean payload (nonzero means true).
/// @param text Borrowed event text, truncated to the event text capacity; NULL is empty.
void rt_services_provider_emit_event(
    int64_t kind, int64_t result_code, int64_t value, int8_t flag, const char *text);

/// @brief Record a non-fatal diagnostic readable through Platform.Diagnostics.
/// @details Formats with printf semantics and truncates to the diagnostic
///          capacity. The oldest message is discarded when the ring is full.
/// @param format printf-style format string.
void rt_services_provider_add_diagnostic(const char *format, ...) RT_PRINTF_FORMAT(1, 2);

/// @brief Look up the kind of a pending request by provider handle.
/// @param handle Provider handle from begin_request.
/// @return RT_SERVICES_REQUEST_* value, or 0 when no request is pending for @p handle.
int64_t rt_services_provider_pending_request_kind(uint64_t handle);

/// @brief Complete a pending request and release the core's reference to it.
/// @details Unknown handles are ignored, which makes late or duplicate
///          completions harmless.
/// @param handle Provider handle from begin_request.
/// @param succeeded Nonzero when the request succeeded.
/// @param result_code Provider-defined result code.
/// @param value Kind-specific integer result.
/// @param error Borrowed error text for failures; NULL or empty for success.
void rt_services_provider_complete_request(
    uint64_t handle, int8_t succeeded, int64_t result_code, int64_t value, const char *error);

#ifdef __cplusplus
}
#endif
