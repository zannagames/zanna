//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services.c
// Purpose: Implements the provider-neutral Zanna.Services core: the provider
//          registry, session lifecycle and status, the platform event queue,
//          asynchronous request objects and their results, bounded
//          diagnostics, the helpers shared with the feature classes, and the
//          core constant classes.
// Key invariants:
//   - All session state is owned by the main thread. Public stateful entry
//     points trap off the main thread; the frame-pump hook silently skips
//     non-main threads instead.
//   - At most one provider is active. Provider callbacks run only between a
//     successful start and the matching stop.
//   - The event queue holds RT_SERVICES_EVENT_CAPACITY events and drops the
//     oldest when full. The pending-request table holds one retained reference
//     per request until the provider completes it or Shutdown cancels it.
//   - A request completes at most once; its result fields never change after.
//   - With no active provider every query returns a neutral value.
// Ownership/Lifetime:
//   - Session state is static and lasts for the process lifetime.
//   - Request objects are runtime heap objects; the pending table owns one
//     reference while a request is outstanding, callers own theirs. A request
//     owns its error and text strings and its entry array, released by its
//     finalizer.
//   - Diagnostics and event text are fixed-capacity copies.
// Links: src/runtime/services/rt_services.h,
//        src/runtime/services/rt_services_internal.h,
//        src/runtime/services/rt_services_provider.h,
//        docs/adr/0352-platform-services-runtime-loaded-providers.md,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services.c
 * @brief Implements the Zanna.Services platform layer shared by all providers.
 * @details The core owns everything that must behave identically for every
 *          store: status transitions, the frame-pump installation, event
 *          queuing, request bookkeeping and cancellation, and diagnostics.
 *          Providers (see rt_services_provider.h) only translate platform
 *          callbacks and queries into these shared structures.
 */

#include "rt_services.h"

#include "rt_object.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_service_hooks.h"
#include "rt_services_internal.h"
#include "rt_services_provider.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Capacity, including the terminator, of one event's text payload.
#define SERVICES_EVENT_TEXT_CAPACITY 256
/// @brief Capacity, including the terminator, of one retained diagnostic.
#define SERVICES_DIAGNOSTIC_TEXT_CAPACITY 1536

/// @brief One queued platform event.
typedef struct services_event {
    int64_t kind;                            ///< RT_SERVICES_EVENT_* value.
    int64_t result_code;                     ///< Provider-defined result code.
    int64_t value;                           ///< Event-specific integer payload.
    int64_t total;                           ///< Total that @ref value counts toward, or 0.
    int8_t flag;                             ///< Event-specific boolean payload.
    char text[SERVICES_EVENT_TEXT_CAPACITY]; ///< Event-specific text payload.
} services_event;

/// @brief Native payload of a Zanna.Services.Request object.
typedef struct rt_services_request_impl {
    int64_t kind;                           ///< RT_SERVICES_REQUEST_* value.
    int8_t done;                            ///< Nonzero once completed.
    int8_t succeeded;                       ///< Nonzero when completed successfully.
    int8_t flag;                            ///< Kind-specific boolean result.
    int64_t result_code;                    ///< Provider-defined result code.
    int64_t value;                          ///< Kind-specific integer result.
    rt_string error;                        ///< Owned failure message, or NULL.
    rt_string text;                         ///< Owned kind-specific text, or NULL.
    const rt_services_provider *provider;   ///< Provider that served the request, or NULL.
    rt_services_leaderboard_entry *entries; ///< Owned entry array, or NULL.
    int64_t entry_count;                    ///< Number of @ref entries.
    int64_t details[RT_SERVICES_REQUEST_DETAIL_CAPACITY]; ///< Kind-specific integer details.
    int64_t detail_count;                                 ///< Number of @ref details.
    void **items;       ///< Owned references to Zanna.Services.WorkshopItem objects, or NULL.
    int64_t item_count; ///< Number of @ref items.
} rt_services_request_impl;

/// @brief One outstanding request awaiting provider completion.
typedef struct services_pending_request {
    uint64_t handle;                   ///< Provider handle from begin_request.
    int64_t kind;                      ///< Request kind.
    rt_services_request_impl *request; ///< Retained request object.
} services_pending_request;

/// @brief Process-global platform services session state (main thread only).
typedef struct services_state {
    const rt_services_provider *active;               ///< Active provider, or NULL.
    services_event queue[RT_SERVICES_EVENT_CAPACITY]; ///< Circular event queue.
    int queue_head;                                   ///< Index of the oldest event.
    int queue_count;                                  ///< Number of queued events.
    int64_t dropped_events;                           ///< Events dropped since Init/Shutdown.
    int8_t overflow_reported; ///< Diagnostic already recorded for this overflow.
    services_event current;   ///< Last polled event.
    services_pending_request
        pending[RT_SERVICES_PENDING_REQUEST_CAPACITY]; ///< Outstanding requests.
    int pending_count;                                 ///< Number of outstanding requests.
    char diagnostics[RT_SERVICES_DIAGNOSTIC_CAPACITY]
                    [SERVICES_DIAGNOSTIC_TEXT_CAPACITY]; ///< Diagnostic ring.
    int diagnostic_head;                                 ///< Index of the oldest diagnostic.
    int diagnostic_count;                                ///< Number of retained diagnostics.
} services_state;

/// @brief Zero-initialized session state; kept separate from the status so it stays in BSS.
static services_state g_services;

/// @brief Current RT_SERVICES_STATUS_* value.
static int64_t g_services_status = RT_SERVICES_STATUS_NOT_STARTED;

/// @brief Providers compiled into this runtime, in Platform.Init lookup order.
static const rt_services_provider *const g_services_providers[] = {
    &rt_services_steam_provider,
};

/// @brief Number of entries in @ref g_services_providers.
#define SERVICES_PROVIDER_COUNT (sizeof(g_services_providers) / sizeof(g_services_providers[0]))

//===----------------------------------------------------------------------===//
// Small helpers
//===----------------------------------------------------------------------===//

/// @brief Trap unless the caller is on the main thread.
/// @param member Class-qualified member name used in the diagnostic, for
///        example "Platform.Init".
/// @return 1 on the main thread, 0 after reporting the trap.
static int services_require_main_thread(const char *member) {
    if (rt_is_main_thread())
        return 1;
    char message[192];
    snprintf(message, sizeof(message), "Services: %s must be called on the main thread", member);
    rt_trap(message);
    return 0;
}

/// @brief Trap unless the caller is on the main thread (provider-facing wrapper).
/// @param member Class-qualified member name used in the diagnostic.
/// @return 1 on the main thread, 0 after reporting the trap.
int rt_services_provider_require_main_thread(const char *member) {
    return services_require_main_thread(member);
}

/// @brief Borrow the bytes of a possibly-NULL runtime string.
/// @param text Runtime string or NULL.
/// @return NUL-terminated bytes; the empty string for NULL.
static const char *services_cstr(rt_string text) {
    return text ? rt_string_cstr(text) : "";
}

/// @brief Borrow the bytes of a possibly-NULL runtime string (feature-class wrapper).
/// @param text Runtime string or NULL.
/// @return NUL-terminated bytes; the empty string for NULL.
const char *rt_services_internal_cstr(rt_string text) {
    return services_cstr(text);
}

/// @brief Return the active provider without a thread check.
/// @return Active provider, or NULL.
const rt_services_provider *rt_services_internal_active_provider(void) {
    return g_services.active;
}

/// @brief Trap with "Services.<member>: <detail>".
/// @param member Class-qualified member name.
/// @param format printf-style detail format.
void rt_services_internal_trap_argument(const char *member, const char *format, ...) {
    char detail[512];
    char message[640];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    snprintf(message, sizeof(message), "Services.%s: %s", member, detail);
    rt_trap(message);
}

/// @brief Require a non-empty identifier argument.
/// @param text Candidate identifier.
/// @param member Class-qualified member name.
/// @param what Argument description.
/// @return Borrowed identifier bytes, or NULL after reporting the trap.
const char *rt_services_internal_require_name(rt_string text,
                                              const char *member,
                                              const char *what) {
    const char *bytes = services_cstr(text);
    if (bytes[0])
        return bytes;
    rt_services_internal_trap_argument(member, "%s must not be empty", what);
    return NULL;
}

/// @brief Compare two ASCII identifiers case-insensitively.
/// @param a First NUL-terminated identifier.
/// @param b Second NUL-terminated identifier.
/// @return 1 when equal ignoring ASCII case, otherwise 0.
static int services_ascii_iequals(const char *a, const char *b) {
    for (;; ++a, ++b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        if (ca == '\0')
            return 1;
    }
}

/// @brief Find a compiled-in provider by id.
/// @param id Requested provider id, compared case-insensitively.
/// @return Provider table, or NULL when no provider matches.
static const rt_services_provider *services_find_provider(const char *id) {
    if (!id || !*id)
        return NULL;
    for (size_t i = 0; i < SERVICES_PROVIDER_COUNT; ++i) {
        if (services_ascii_iequals(g_services_providers[i]->id, id))
            return g_services_providers[i];
    }
    return NULL;
}

/// @brief Format the comma-separated list of compiled-in provider ids.
/// @param out Destination buffer.
/// @param capacity Size of @p out in bytes.
static void services_format_provider_list(char *out, size_t capacity) {
    size_t used = 0;
    if (capacity == 0)
        return;
    out[0] = '\0';
    for (size_t i = 0; i < SERVICES_PROVIDER_COUNT && used < capacity; ++i) {
        int written = snprintf(
            out + used, capacity - used, "%s%s", i ? ", " : "", g_services_providers[i]->id);
        if (written < 0)
            break;
        used += (size_t)written;
    }
}

/// @brief Create a caller-owned runtime string from C text.
/// @param text NUL-terminated text or NULL.
/// @return Owned copy, or the immortal empty string for NULL/empty input.
static rt_string services_owned_text(const char *text) {
    return (text && *text) ? rt_const_cstr(text) : rt_str_empty();
}

/// @brief Create a caller-owned runtime string from C text (feature-class wrapper).
/// @param text NUL-terminated text or NULL.
/// @return Owned copy, or the immortal empty string.
rt_string rt_services_internal_owned_text(const char *text) {
    return services_owned_text(text);
}

/// @brief Normalize a provider string query result.
/// @param value Owned provider result or NULL.
/// @return @p value, or the immortal empty string when NULL.
static rt_string services_owned_or_empty(rt_string value) {
    return value ? value : rt_str_empty();
}

/// @brief Normalize a provider string result (feature-class wrapper).
/// @param value Owned provider result or NULL.
/// @return @p value, or the immortal empty string.
rt_string rt_services_internal_owned_or_empty(rt_string value) {
    return services_owned_or_empty(value);
}

/// @brief Build a caller-owned Ok(string) Result.
/// @param text Success payload.
/// @return Caller-owned Zanna.Result.
static void *services_ok_result(const char *text) {
    rt_string payload = services_owned_text(text);
    void *result = rt_result_ok_str(payload);
    rt_string_unref(payload);
    return result;
}

/// @brief Build a caller-owned Err(string) Result.
/// @param text Error message.
/// @return Caller-owned Zanna.Result.
static void *services_err_result(const char *text) {
    rt_string payload = services_owned_text(text);
    void *result = rt_result_err_str(payload);
    rt_string_unref(payload);
    return result;
}

/// @brief Build a caller-owned Err(string) Result (feature-class wrapper).
/// @param text Error message.
/// @return Caller-owned Zanna.Result.
void *rt_services_internal_err_result(const char *text) {
    return services_err_result(text);
}

//===----------------------------------------------------------------------===//
// Diagnostics
//===----------------------------------------------------------------------===//

/// @brief Append one diagnostic, discarding the oldest when the ring is full.
/// @details A message identical to the newest retained diagnostic is skipped,
///          so a member that fails every frame cannot flush older entries.
/// @param text NUL-terminated message; truncated to the diagnostic capacity.
static void services_record_diagnostic(const char *text) {
    char truncated[SERVICES_DIAGNOSTIC_TEXT_CAPACITY];
    snprintf(truncated, sizeof(truncated), "%s", text ? text : "");
    if (g_services.diagnostic_count > 0) {
        int newest = (g_services.diagnostic_head + g_services.diagnostic_count - 1) %
                     RT_SERVICES_DIAGNOSTIC_CAPACITY;
        if (strcmp(g_services.diagnostics[newest], truncated) == 0)
            return;
    }
    int slot;
    if (g_services.diagnostic_count == RT_SERVICES_DIAGNOSTIC_CAPACITY) {
        slot = g_services.diagnostic_head;
        g_services.diagnostic_head =
            (g_services.diagnostic_head + 1) % RT_SERVICES_DIAGNOSTIC_CAPACITY;
    } else {
        slot = (g_services.diagnostic_head + g_services.diagnostic_count) %
               RT_SERVICES_DIAGNOSTIC_CAPACITY;
        g_services.diagnostic_count++;
    }
    memcpy(g_services.diagnostics[slot], truncated, sizeof(truncated));
}

/// @brief Record a formatted non-fatal diagnostic on behalf of a provider.
/// @param format printf-style format string; NULL records an empty message.
void rt_services_provider_add_diagnostic(const char *format, ...) {
    char message[SERVICES_DIAGNOSTIC_TEXT_CAPACITY];
    if (!format) {
        services_record_diagnostic("");
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    services_record_diagnostic(message);
}

//===----------------------------------------------------------------------===//
// Events
//===----------------------------------------------------------------------===//

/// @brief Discard all queued events and the last polled event.
static void services_reset_events(void) {
    g_services.queue_head = 0;
    g_services.queue_count = 0;
    g_services.dropped_events = 0;
    g_services.overflow_reported = 0;
    memset(&g_services.current, 0, sizeof(g_services.current));
}

/// @brief Queue a progress event, dropping the oldest when the queue is full.
/// @param kind RT_SERVICES_EVENT_* value; RT_SERVICES_EVENT_NONE is ignored.
/// @param result_code Provider-defined result code.
/// @param value Event-specific integer payload.
/// @param total Total that @p value counts toward, or 0.
/// @param flag Event-specific boolean payload.
/// @param text Borrowed event text; NULL is empty.
void rt_services_provider_emit_progress_event(int64_t kind,
                                              int64_t result_code,
                                              int64_t value,
                                              int64_t total,
                                              int8_t flag,
                                              const char *text) {
    if (kind == RT_SERVICES_EVENT_NONE)
        return;
    if (g_services.queue_count == RT_SERVICES_EVENT_CAPACITY) {
        g_services.queue_head = (g_services.queue_head + 1) % RT_SERVICES_EVENT_CAPACITY;
        g_services.queue_count--;
        g_services.dropped_events++;
        if (!g_services.overflow_reported) {
            services_record_diagnostic("Services: event queue full; dropped oldest event");
            g_services.overflow_reported = 1;
        }
    }
    int index = (g_services.queue_head + g_services.queue_count) % RT_SERVICES_EVENT_CAPACITY;
    services_event *event = &g_services.queue[index];
    event->kind = kind;
    event->result_code = result_code;
    event->value = value;
    event->total = total;
    event->flag = flag ? 1 : 0;
    snprintf(event->text, sizeof(event->text), "%s", text ? text : "");
    g_services.queue_count++;
}

/// @brief Queue a platform event without a progress total.
/// @param kind RT_SERVICES_EVENT_* value; RT_SERVICES_EVENT_NONE is ignored.
/// @param result_code Provider-defined result code.
/// @param value Event-specific integer payload.
/// @param flag Event-specific boolean payload.
/// @param text Borrowed event text; NULL is empty.
void rt_services_provider_emit_event(
    int64_t kind, int64_t result_code, int64_t value, int8_t flag, const char *text) {
    rt_services_provider_emit_progress_event(kind, result_code, value, 0, flag, text);
}

//===----------------------------------------------------------------------===//
// Requests
//===----------------------------------------------------------------------===//

/// @brief Finalizer releasing a request's owned strings and entries.
/// @param obj Zero-reference request payload.
static void services_request_finalize(void *obj) {
    rt_services_request_impl *request = (rt_services_request_impl *)obj;
    if (!request)
        return;
    if (request->error) {
        rt_string_unref(request->error);
        request->error = NULL;
    }
    if (request->text) {
        rt_string_unref(request->text);
        request->text = NULL;
    }
    free(request->entries);
    request->entries = NULL;
    request->entry_count = 0;
    for (int64_t i = 0; i < request->item_count; ++i) {
        if (request->items[i] && rt_obj_release_known_check0(request->items[i]))
            rt_obj_free(request->items[i]);
    }
    free(request->items);
    request->items = NULL;
    request->item_count = 0;
}

/// @brief Allocate a pending request object.
/// @param kind RT_SERVICES_REQUEST_* value.
/// @return Caller-owned request with one reference, or NULL after an allocation trap.
static rt_services_request_impl *services_request_new(int64_t kind) {
    rt_services_request_impl *request = (rt_services_request_impl *)rt_obj_new_i64(
        RT_SERVICES_REQUEST_CLASS_ID, (int64_t)sizeof(rt_services_request_impl));
    if (!request)
        return NULL;
    memset(request, 0, sizeof(*request));
    request->kind = kind;
    rt_obj_set_finalizer(request, services_request_finalize);
    return request;
}

/// @brief Complete a request once from a full result; later completions are ignored.
/// @details Copies the text, error, and up to RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY
///          entries. The error is kept only for failures. When the entry copy
///          cannot be allocated the request fails instead of reporting a
///          truncated list.
/// @param request Request to complete; NULL is ignored.
/// @param result Borrowed completion record.
static void services_request_finish_result(rt_services_request_impl *request,
                                           const rt_services_request_result *result) {
    if (!request || request->done || !result)
        return;
    int8_t succeeded = result->succeeded ? 1 : 0;
    const char *error = result->error;
    int64_t count = result->entries ? result->entry_count : 0;
    if (count < 0)
        count = 0;
    if (count > RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY)
        count = RT_SERVICES_LEADERBOARD_ENTRY_CAPACITY;
    if (count > 0) {
        request->entries = (rt_services_leaderboard_entry *)malloc(
            (size_t)count * sizeof(rt_services_leaderboard_entry));
        if (request->entries) {
            memcpy(request->entries,
                   result->entries,
                   (size_t)count * sizeof(rt_services_leaderboard_entry));
            for (int64_t i = 0; i < count; ++i) {
                request->entries[i].user_id[RT_SERVICES_USER_ID_CAPACITY - 1] = '\0';
                request->entries[i].user_name[RT_SERVICES_USER_NAME_CAPACITY - 1] = '\0';
            }
            request->entry_count = count;
        } else {
            succeeded = 0;
            error = "Services: out of memory storing leaderboard entries";
        }
    }
    int64_t items = (succeeded && result->items) ? result->item_count : 0;
    if (items > RT_SERVICES_REQUEST_ITEM_CAPACITY)
        items = RT_SERVICES_REQUEST_ITEM_CAPACITY;
    if (items > 0) {
        request->items = (void **)calloc((size_t)items, sizeof(void *));
        for (int64_t i = 0; request->items && i < items; ++i) {
            request->items[i] = rt_services_internal_workshop_item_new(&result->items[i]);
            if (!request->items[i])
                break;
            request->item_count = i + 1;
        }
        if (request->item_count != items) {
            succeeded = 0;
            error = "Services: out of memory storing Workshop items";
        }
    }
    if (succeeded && result->details && result->detail_count > 0) {
        int64_t details = result->detail_count;
        if (details > RT_SERVICES_REQUEST_DETAIL_CAPACITY)
            details = RT_SERVICES_REQUEST_DETAIL_CAPACITY;
        memcpy(request->details, result->details, (size_t)details * sizeof(int64_t));
        request->detail_count = details;
    }
    request->done = 1;
    request->succeeded = succeeded;
    request->flag = succeeded && result->flag ? 1 : 0;
    request->result_code = result->result_code;
    request->value = result->value;
    if (result->text && *result->text)
        request->text = rt_const_cstr(result->text);
    if (!succeeded && error && *error)
        request->error = rt_const_cstr(error);
}

/// @brief Complete a request once with a status, value, and error.
/// @param request Request to complete; NULL is ignored.
/// @param succeeded Nonzero for success.
/// @param result_code Provider-defined result code.
/// @param value Kind-specific integer result.
/// @param error Borrowed failure message; ignored for success.
static void services_request_finish(rt_services_request_impl *request,
                                    int8_t succeeded,
                                    int64_t result_code,
                                    int64_t value,
                                    const char *error) {
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.succeeded = succeeded;
    result.result_code = result_code;
    result.value = value;
    result.error = error;
    services_request_finish_result(request, &result);
}

/// @brief Drop one reference to a request, freeing it at zero.
/// @param request Request whose reference the caller owns.
static void services_request_release(rt_services_request_impl *request) {
    if (request && rt_obj_release_known_check0(request))
        rt_obj_free(request);
}

/// @brief Validate a request handle passed from Zia.
/// @param request Candidate handle; NULL is returned unchanged.
/// @param member Class-qualified member name for diagnostics.
/// @return Request payload, or NULL for NULL or after a trap for a foreign handle.
static rt_services_request_impl *services_request_checked(void *request, const char *member) {
    if (!services_require_main_thread(member) || !request)
        return NULL;
    if (!rt_obj_is_instance(
            request, RT_SERVICES_REQUEST_CLASS_ID, sizeof(rt_services_request_impl))) {
        char message[192];
        snprintf(message, sizeof(message), "Services: %s: expected Zanna.Services.Request", member);
        rt_trap(message);
        return NULL;
    }
    return (rt_services_request_impl *)request;
}

/// @brief Find the pending-table slot for a provider handle.
/// @param handle Provider handle.
/// @return Slot index, or -1 when the handle is not pending.
static int services_pending_index(uint64_t handle) {
    for (int i = 0; i < g_services.pending_count; ++i) {
        if (g_services.pending[i].handle == handle)
            return i;
    }
    return -1;
}

/// @brief Look up the kind of a pending request.
/// @param handle Provider handle from begin_request.
/// @return RT_SERVICES_REQUEST_* value, or 0 when not pending.
int64_t rt_services_provider_pending_request_kind(uint64_t handle) {
    int index = services_pending_index(handle);
    return index < 0 ? 0 : g_services.pending[index].kind;
}

/// @brief Complete a pending request from a full result and release the table's reference.
/// @param handle Provider handle from begin_request; unknown handles are ignored.
/// @param result Borrowed completion record; NULL is ignored.
void rt_services_provider_finish_request(uint64_t handle,
                                         const rt_services_request_result *result) {
    int index = services_pending_index(handle);
    if (index < 0 || !result)
        return;
    rt_services_request_impl *request = g_services.pending[index].request;
    g_services.pending[index] = g_services.pending[g_services.pending_count - 1];
    g_services.pending_count--;
    services_request_finish_result(request, result);
    services_request_release(request);
}

/// @brief Complete a pending request and release the table's reference.
/// @param handle Provider handle from begin_request; unknown handles are ignored.
/// @param succeeded Nonzero for success.
/// @param result_code Provider-defined result code.
/// @param value Kind-specific integer result.
/// @param error Borrowed failure message.
void rt_services_provider_complete_request(
    uint64_t handle, int8_t succeeded, int64_t result_code, int64_t value, const char *error) {
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.succeeded = succeeded;
    result.result_code = result_code;
    result.value = value;
    result.error = error;
    rt_services_provider_finish_request(handle, &result);
}

/// @brief Fail and release every pending request.
/// @param reason Failure message stored on each cancelled request.
static void services_cancel_pending_requests(const char *reason) {
    while (g_services.pending_count > 0) {
        rt_services_request_impl *request =
            g_services.pending[g_services.pending_count - 1].request;
        g_services.pending_count--;
        services_request_finish(request, 0, 0, 0, reason);
        services_request_release(request);
    }
}

/// @brief Start a request against the active provider.
/// @param args Validated request kind and arguments.
/// @param member Class-qualified member name for diagnostics.
/// @return Caller-owned request, already failed when it cannot start.
void *rt_services_internal_begin_request(const rt_services_request_args *args, const char *member) {
    if (!services_require_main_thread(member))
        return NULL;
    rt_services_request_impl *request = services_request_new(args->kind);
    if (!request)
        return NULL;
    const rt_services_provider *provider = g_services.active;
    request->provider = provider;
    if (!provider) {
        services_request_finish(
            request, 0, 0, 0, "Services: no platform services provider is started");
        return request;
    }
    if (!provider->begin_request) {
        char message[192];
        snprintf(message,
                 sizeof(message),
                 "Services: provider '%s' does not support this request",
                 provider->id);
        services_request_finish(request, 0, 0, 0, message);
        return request;
    }
    if (g_services.pending_count >= RT_SERVICES_PENDING_REQUEST_CAPACITY) {
        services_request_finish(request, 0, 0, 0, "Services: too many pending requests (limit 64)");
        return request;
    }
    char message[RT_SERVICES_MESSAGE_CAPACITY];
    uint64_t handle = 0;
    message[0] = '\0';
    if (!provider->begin_request(args, &handle, message, sizeof(message)) || handle == 0) {
        services_request_finish(
            request, 0, 0, 0, message[0] ? message : "Services: request could not be started");
        return request;
    }
    rt_obj_retain_known(request);
    g_services.pending[g_services.pending_count].handle = handle;
    g_services.pending[g_services.pending_count].kind = args->kind;
    g_services.pending[g_services.pending_count].request = request;
    g_services.pending_count++;
    return request;
}

//===----------------------------------------------------------------------===//
// Lifecycle
//===----------------------------------------------------------------------===//

/// @brief Pump the active provider, if any.
static void services_pump_active(void) {
    const rt_services_provider *provider = g_services.active;
    if (provider)
        provider->pump();
}

/// @brief Frame-pump hook installed while a provider is started.
/// @details Called from Canvas.Poll and Canvas3D.Poll. Never traps: frame
///          loops on other threads are simply not pumped.
static void services_frame_pump(void) {
    if (rt_is_main_thread())
        services_pump_active();
}

/// @brief Start a platform services provider.
/// @param provider Provider id such as "steam".
/// @param app_id Provider-defined application id.
/// @return Caller-owned Zanna.Result: Ok(provider id) or Err(message).
void *rt_services_platform_init(rt_string provider, rt_string app_id) {
    if (!services_require_main_thread("Platform.Init"))
        return services_err_result("Services: Platform.Init must be called on the main thread");

    const char *requested = services_cstr(provider);
    const rt_services_provider *selected = services_find_provider(requested);
    char message[RT_SERVICES_MESSAGE_CAPACITY];
    message[0] = '\0';

    if (!selected) {
        char available[256];
        services_format_provider_list(available, sizeof(available));
        snprintf(message,
                 sizeof(message),
                 "Services: unknown provider '%s' (available: %s)",
                 requested,
                 available);
        if (!g_services.active)
            g_services_status = RT_SERVICES_STATUS_UNKNOWN_PROVIDER;
        services_record_diagnostic(message);
        return services_err_result(message);
    }

    if (g_services.active) {
        if (g_services.active == selected)
            return services_ok_result(selected->id);
        snprintf(message,
                 sizeof(message),
                 "Services: provider '%s' is already active; call Platform.Shutdown() before "
                 "starting '%s'",
                 g_services.active->id,
                 selected->id);
        services_record_diagnostic(message);
        return services_err_result(message);
    }

    int64_t status = selected->start(app_id ? app_id : rt_str_empty(), message, sizeof(message));
    if (status != RT_SERVICES_STATUS_OK) {
        if (!message[0])
            snprintf(
                message, sizeof(message), "Services: provider '%s' failed to start", selected->id);
        g_services_status = status;
        services_record_diagnostic(message);
        return services_err_result(message);
    }

    services_reset_events();
    g_services.active = selected;
    g_services_status = RT_SERVICES_STATUS_OK;
    rt_service_hooks_set_frame_pump(services_frame_pump);
    return services_ok_result(selected->id);
}

/// @brief Pump the active provider once; a no-op when none is started.
void rt_services_platform_update(void) {
    if (!services_require_main_thread("Platform.Update"))
        return;
    services_pump_active();
}

/// @brief Stop the active provider and reset session state.
void rt_services_platform_shutdown(void) {
    if (!services_require_main_thread("Platform.Shutdown"))
        return;
    rt_service_hooks_set_frame_pump(NULL);
    const rt_services_provider *provider = g_services.active;
    g_services.active = NULL;
    if (provider)
        provider->stop();
    services_cancel_pending_requests("Services: request cancelled by Platform.Shutdown()");
    services_reset_events();
    g_services_status = RT_SERVICES_STATUS_NOT_STARTED;
}

/// @brief Dequeue the next platform event into the last-polled slot.
/// @return Event kind, or RT_SERVICES_EVENT_NONE when the queue is empty.
int64_t rt_services_platform_poll_event(void) {
    if (!services_require_main_thread("Platform.PollEvent"))
        return RT_SERVICES_EVENT_NONE;
    memset(&g_services.current, 0, sizeof(g_services.current));
    if (g_services.queue_count == 0)
        return RT_SERVICES_EVENT_NONE;
    g_services.current = g_services.queue[g_services.queue_head];
    g_services.queue_head = (g_services.queue_head + 1) % RT_SERVICES_EVENT_CAPACITY;
    g_services.queue_count--;
    g_services.overflow_reported = 0;
    return g_services.current.kind;
}

/// @brief Report whether a provider id is compiled into this runtime.
/// @param name Provider id.
/// @return 1 when available, otherwise 0.
int8_t rt_services_platform_has_provider(rt_string name) {
    return services_find_provider(services_cstr(name)) ? 1 : 0;
}

/// @brief Report whether the active provider supports a feature.
/// @param feature RT_SERVICES_FEATURE_* value.
/// @return 1 when supported now, otherwise 0.
int8_t rt_services_platform_has_feature(int64_t feature) {
    if (!services_require_main_thread("Platform.HasFeature"))
        return 0;
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->has_feature && provider->has_feature(feature)) ? 1 : 0;
}

/// @brief Report whether a DLC is owned and installed.
/// @param dlc_id Provider-defined DLC id.
/// @return 1 when installed, otherwise 0.
int8_t rt_services_platform_is_dlc_installed(rt_string dlc_id) {
    if (!services_require_main_thread("Platform.IsDlcInstalled"))
        return 0;
    const rt_services_provider *provider = g_services.active;
    if (!provider || !provider->is_dlc_installed)
        return 0;
    return provider->is_dlc_installed(dlc_id ? dlc_id : rt_str_empty()) ? 1 : 0;
}

/// @brief Start a player-count request.
/// @return Caller-owned Zanna.Services.Request.
void *rt_services_platform_request_player_count(void) {
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = RT_SERVICES_REQUEST_PLAYER_COUNT;
    args.name = "";
    args.text = "";
    return rt_services_internal_begin_request(&args, "Platform.RequestPlayerCount");
}

/// @brief Copy the retained diagnostics, oldest first.
/// @return Caller-owned Seq of caller-owned strings.
void *rt_services_platform_diagnostics(void) {
    if (!services_require_main_thread("Platform.Diagnostics"))
        return NULL;
    void *seq = rt_seq_new_owned();
    if (!seq)
        return NULL;
    for (int i = 0; i < g_services.diagnostic_count; ++i) {
        int index = (g_services.diagnostic_head + i) % RT_SERVICES_DIAGNOSTIC_CAPACITY;
        rt_seq_push_raw(seq, services_owned_text(g_services.diagnostics[index]));
    }
    return seq;
}

/// @brief Read one launch parameter passed by the platform's launch URL.
/// @param key Parameter name; an empty name traps.
/// @return Caller-owned value, or the empty string.
rt_string rt_services_platform_launch_parameter(rt_string key) {
    static const char member[] = "Platform.LaunchParameter";
    if (!services_require_main_thread(member))
        return rt_str_empty();
    const char *name = rt_services_internal_require_name(key, member, "key");
    if (!name)
        return rt_str_empty();
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->launch_parameter)
               ? services_owned_or_empty(provider->launch_parameter(name))
               : rt_str_empty();
}

//===----------------------------------------------------------------------===//
// Platform properties
//===----------------------------------------------------------------------===//

/// @brief Report whether a provider is started.
/// @return 1 while active, otherwise 0.
int8_t rt_services_platform_get_is_available(void) {
    if (!services_require_main_thread("Platform.IsAvailable"))
        return 0;
    return g_services.active ? 1 : 0;
}

/// @brief Read the platform services status.
/// @return RT_SERVICES_STATUS_* value.
int64_t rt_services_platform_get_status(void) {
    if (!services_require_main_thread("Platform.Status"))
        return RT_SERVICES_STATUS_NOT_STARTED;
    return g_services_status;
}

/// @brief Read the active provider id.
/// @return Caller-owned id, or the empty string.
rt_string rt_services_platform_get_provider(void) {
    if (!services_require_main_thread("Platform.Provider") || !g_services.active)
        return rt_str_empty();
    return services_owned_text(g_services.active->id);
}

/// @brief Read the provider's application id.
/// @return Caller-owned id, or the empty string.
rt_string rt_services_platform_get_app_id(void) {
    if (!services_require_main_thread("Platform.AppId"))
        return rt_str_empty();
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->app_id) ? services_owned_or_empty(provider->app_id())
                                          : rt_str_empty();
}

/// @brief Read the signed-in user's id.
/// @return Caller-owned id, or the empty string.
rt_string rt_services_platform_get_user_id(void) {
    if (!services_require_main_thread("Platform.UserId"))
        return rt_str_empty();
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->user_id) ? services_owned_or_empty(provider->user_id())
                                           : rt_str_empty();
}

/// @brief Read the signed-in user's display name.
/// @return Caller-owned name, or the empty string.
rt_string rt_services_platform_get_user_name(void) {
    if (!services_require_main_thread("Platform.UserName"))
        return rt_str_empty();
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->user_name) ? services_owned_or_empty(provider->user_name())
                                             : rt_str_empty();
}

/// @brief Read the user's game language code.
/// @return Caller-owned language code, or the empty string.
rt_string rt_services_platform_get_language(void) {
    if (!services_require_main_thread("Platform.Language"))
        return rt_str_empty();
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->language) ? services_owned_or_empty(provider->language())
                                            : rt_str_empty();
}

/// @brief Report whether the running application is licensed.
/// @return 1 when licensed, otherwise 0.
int8_t rt_services_platform_get_is_licensed(void) {
    if (!services_require_main_thread("Platform.IsLicensed"))
        return 0;
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->is_licensed && provider->is_licensed()) ? 1 : 0;
}

/// @brief Report whether the platform client is online.
/// @return 1 when online, otherwise 0.
int8_t rt_services_platform_get_is_online(void) {
    if (!services_require_main_thread("Platform.IsOnline"))
        return 0;
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->is_online && provider->is_online()) ? 1 : 0;
}

/// @brief Read the command line passed by the platform's launch URL.
/// @return Caller-owned command line, or the empty string.
rt_string rt_services_platform_get_launch_command_line(void) {
    if (!services_require_main_thread("Platform.LaunchCommandLine"))
        return rt_str_empty();
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->launch_command_line)
               ? services_owned_or_empty(provider->launch_command_line())
               : rt_str_empty();
}

/// @brief Capacity, including the terminator, of a DLC id read from a provider.
#define SERVICES_DLC_ID_CAPACITY 32
/// @brief Capacity, including the terminator, of a DLC name read from a provider.
#define SERVICES_DLC_NAME_CAPACITY 256

/// @brief One DLC record read from the active provider.
typedef struct services_dlc {
    char id[SERVICES_DLC_ID_CAPACITY];     ///< Provider-defined DLC id.
    char name[SERVICES_DLC_NAME_CAPACITY]; ///< Display name.
    int8_t available;                      ///< Nonzero when the DLC can be bought.
} services_dlc;

/// @brief Read the DLC at an index from the active provider.
/// @param member Class-qualified member name for the thread check.
/// @param index DLC index.
/// @param out Receives the record.
/// @return 1 when @p index names a DLC, otherwise 0.
static int services_read_dlc(const char *member, int64_t index, services_dlc *out) {
    if (!services_require_main_thread(member) || index < 0)
        return 0;
    const rt_services_provider *provider = g_services.active;
    if (!provider || !provider->dlc_at)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!provider->dlc_at(
            index, out->id, sizeof(out->id), out->name, sizeof(out->name), &out->available))
        return 0;
    out->id[sizeof(out->id) - 1] = '\0';
    out->name[sizeof(out->name) - 1] = '\0';
    return 1;
}

/// @brief Count the application's DLC.
/// @return Count, or 0.
int64_t rt_services_platform_get_dlc_count(void) {
    if (!services_require_main_thread("Platform.DlcCount"))
        return 0;
    const rt_services_provider *provider = g_services.active;
    const int64_t count = (provider && provider->dlc_count) ? provider->dlc_count() : 0;
    return count > 0 ? count : 0;
}

/// @brief Read a DLC id by index.
/// @param index DLC index.
/// @return Caller-owned id, or the empty string.
rt_string rt_services_platform_dlc_id_at(int64_t index) {
    services_dlc dlc;
    return services_read_dlc("Platform.DlcIdAt", index, &dlc) ? services_owned_text(dlc.id)
                                                              : rt_str_empty();
}

/// @brief Read a DLC name by index.
/// @param index DLC index.
/// @return Caller-owned name, or the empty string.
rt_string rt_services_platform_dlc_name_at(int64_t index) {
    services_dlc dlc;
    return services_read_dlc("Platform.DlcNameAt", index, &dlc) ? services_owned_text(dlc.name)
                                                                : rt_str_empty();
}

/// @brief Report whether a DLC can be bought now.
/// @param index DLC index.
/// @return 1 when available, otherwise 0.
int8_t rt_services_platform_dlc_available_at(int64_t index) {
    services_dlc dlc;
    return (services_read_dlc("Platform.DlcAvailableAt", index, &dlc) && dlc.available) ? 1 : 0;
}

/// @brief Read the installed build's id.
/// @return Build id, or 0.
int64_t rt_services_platform_get_build_id(void) {
    if (!services_require_main_thread("Platform.BuildId"))
        return 0;
    const rt_services_provider *provider = g_services.active;
    const int64_t build = (provider && provider->build_id) ? provider->build_id() : 0;
    return build > 0 ? build : 0;
}

/// @brief Read the installed build's branch.
/// @return Caller-owned branch name, or the empty string.
rt_string rt_services_platform_get_branch_name(void) {
    if (!services_require_main_thread("Platform.BranchName"))
        return rt_str_empty();
    const rt_services_provider *provider = g_services.active;
    return (provider && provider->branch_name) ? services_owned_or_empty(provider->branch_name())
                                               : rt_str_empty();
}

/// @brief Read the last polled event's result code.
/// @return Provider-defined code, or 0.
int64_t rt_services_platform_get_event_result_code(void) {
    if (!services_require_main_thread("Platform.EventResultCode"))
        return 0;
    return g_services.current.result_code;
}

/// @brief Read the last polled event's text.
/// @return Caller-owned text, or the empty string.
rt_string rt_services_platform_get_event_text(void) {
    if (!services_require_main_thread("Platform.EventText"))
        return rt_str_empty();
    return services_owned_text(g_services.current.text);
}

/// @brief Read the last polled event's integer payload.
/// @return Event-specific value, or 0.
int64_t rt_services_platform_get_event_value(void) {
    if (!services_require_main_thread("Platform.EventValue"))
        return 0;
    return g_services.current.value;
}

/// @brief Read the last polled event's boolean payload.
/// @return Event-specific flag, or 0.
int8_t rt_services_platform_get_event_flag(void) {
    if (!services_require_main_thread("Platform.EventFlag"))
        return 0;
    return g_services.current.flag;
}

/// @brief Read the total that the last polled event's value counts toward.
/// @return Event-specific total, or 0.
int64_t rt_services_platform_get_event_total(void) {
    if (!services_require_main_thread("Platform.EventTotal"))
        return 0;
    return g_services.current.total;
}

/// @brief Read the dropped-event counter.
/// @return Events dropped since the last Init or Shutdown.
int64_t rt_services_platform_get_dropped_events(void) {
    if (!services_require_main_thread("Platform.DroppedEvents"))
        return 0;
    return g_services.dropped_events;
}

//===----------------------------------------------------------------------===//
// Request properties
//===----------------------------------------------------------------------===//

/// @brief Read a request's kind.
/// @param request Borrowed request handle.
/// @return RT_SERVICES_REQUEST_* value, or 0.
int64_t rt_services_request_get_kind(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.Kind");
    return impl ? impl->kind : 0;
}

/// @brief Report whether a request completed.
/// @param request Borrowed request handle.
/// @return 1 when completed, otherwise 0.
int8_t rt_services_request_get_is_done(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.IsDone");
    return impl ? impl->done : 0;
}

/// @brief Report whether a request completed successfully.
/// @param request Borrowed request handle.
/// @return 1 when succeeded, otherwise 0.
int8_t rt_services_request_get_succeeded(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.Succeeded");
    return impl ? impl->succeeded : 0;
}

/// @brief Read a request's provider result code.
/// @param request Borrowed request handle.
/// @return Provider-defined code, or 0.
int64_t rt_services_request_get_result_code(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.ResultCode");
    return impl ? impl->result_code : 0;
}

/// @brief Read a request's integer result.
/// @param request Borrowed request handle.
/// @return Kind-specific value, or 0.
int64_t rt_services_request_get_value(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.Value");
    return impl ? impl->value : 0;
}

/// @brief Read a request's boolean result.
/// @param request Borrowed request handle.
/// @return Kind-specific flag, or 0.
int8_t rt_services_request_get_flag(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.Flag");
    return impl ? impl->flag : 0;
}

/// @brief Read a request's text result.
/// @param request Borrowed request handle.
/// @return Caller-owned text, or the empty string.
rt_string rt_services_request_get_text(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.Text");
    if (!impl || !impl->text)
        return rt_str_empty();
    return rt_string_ref(impl->text);
}

/// @brief Read a request's failure message.
/// @param request Borrowed request handle.
/// @return Caller-owned message, or the empty string.
rt_string rt_services_request_get_error(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.Error");
    if (!impl || !impl->error)
        return rt_str_empty();
    return rt_string_ref(impl->error);
}

/// @brief Read how many leaderboard entries a request holds.
/// @param request Borrowed request handle.
/// @return Entry count, or 0.
int64_t rt_services_request_get_entry_count(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.EntryCount");
    return impl ? impl->entry_count : 0;
}

/// @brief Validate a request handle and entry index.
/// @details Traps with "Services.Request.<member>: index <i> is outside
///          0..<count - 1>" for an out-of-range index.
/// @param request Candidate request handle.
/// @param index Entry index.
/// @param member Request member name, for example "EntryRank".
/// @return Entry, or NULL for a NULL request or after a trap.
static const rt_services_leaderboard_entry *services_request_entry(void *request,
                                                                   int64_t index,
                                                                   const char *member) {
    char qualified[64];
    snprintf(qualified, sizeof(qualified), "Request.%s", member);
    rt_services_request_impl *impl = services_request_checked(request, qualified);
    if (!impl)
        return NULL;
    if (index < 0 || index >= impl->entry_count) {
        if (impl->entry_count == 0) {
            rt_services_internal_trap_argument(
                qualified,
                "index %lld is out of range; the request holds no entries",
                (long long)index);
        } else {
            rt_services_internal_trap_argument(qualified,
                                               "index %lld is outside 0..%lld",
                                               (long long)index,
                                               (long long)(impl->entry_count - 1));
        }
        return NULL;
    }
    return &impl->entries[index];
}

/// @brief Read one entry's global rank.
/// @param request Borrowed request handle.
/// @param index Entry index.
/// @return Rank, or 0 after a trap.
int64_t rt_services_request_entry_rank(void *request, int64_t index) {
    const rt_services_leaderboard_entry *entry =
        services_request_entry(request, index, "EntryRank");
    return entry ? entry->rank : 0;
}

/// @brief Read one entry's score.
/// @param request Borrowed request handle.
/// @param index Entry index.
/// @return Score, or 0 after a trap.
int64_t rt_services_request_entry_score(void *request, int64_t index) {
    const rt_services_leaderboard_entry *entry =
        services_request_entry(request, index, "EntryScore");
    return entry ? entry->score : 0;
}

/// @brief Read one entry's user id.
/// @param request Borrowed request handle.
/// @param index Entry index.
/// @return Caller-owned user id, or the empty string after a trap.
rt_string rt_services_request_entry_user_id(void *request, int64_t index) {
    const rt_services_leaderboard_entry *entry =
        services_request_entry(request, index, "EntryUserId");
    return entry ? services_owned_text(entry->user_id) : rt_str_empty();
}

/// @brief Read one entry's user display name, live while its provider is active.
/// @param request Borrowed request handle.
/// @param index Entry index.
/// @return Caller-owned name, or the empty string while unknown.
rt_string rt_services_request_entry_user_name(void *request, int64_t index) {
    const rt_services_leaderboard_entry *entry =
        services_request_entry(request, index, "EntryUserName");
    if (!entry)
        return rt_str_empty();
    const rt_services_request_impl *impl = (const rt_services_request_impl *)request;
    const rt_services_provider *provider = g_services.active;
    if (provider && provider == impl->provider && provider->leaderboards &&
        provider->leaderboards->user_name) {
        rt_string live = provider->leaderboards->user_name(entry->user_id);
        if (live)
            return live;
    }
    return services_owned_text(entry->user_name);
}

/// @brief Read how many integer details a request holds.
/// @param request Borrowed request handle.
/// @return Detail count, or 0.
int64_t rt_services_request_get_detail_count(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.DetailCount");
    return impl ? impl->detail_count : 0;
}

/// @brief Read one integer detail of a request.
/// @details Traps with "Services.Request.Detail: index <i> is outside 0..<n>"
///          for an out-of-range index.
/// @param request Borrowed request handle.
/// @param index Detail index.
/// @return Detail value, or 0 after a trap.
int64_t rt_services_request_detail(void *request, int64_t index) {
    static const char member[] = "Request.Detail";
    rt_services_request_impl *impl = services_request_checked(request, member);
    if (!impl)
        return 0;
    if (index < 0 || index >= impl->detail_count) {
        if (impl->detail_count == 0) {
            rt_services_internal_trap_argument(
                member,
                "index %lld is out of range; the request holds no details",
                (long long)index);
        } else {
            rt_services_internal_trap_argument(member,
                                               "index %lld is outside 0..%lld",
                                               (long long)index,
                                               (long long)(impl->detail_count - 1));
        }
        return 0;
    }
    return impl->details[index];
}

/// @brief Count the Workshop items a completed query holds.
/// @param request Borrowed request handle.
/// @return Item count, or 0.
int64_t rt_services_request_get_item_count(void *request) {
    rt_services_request_impl *impl = services_request_checked(request, "Request.ItemCount");
    return impl ? impl->item_count : 0;
}

/// @brief Read one Workshop item of a completed query.
/// @param request Borrowed request handle.
/// @param index Item index.
/// @return Caller-owned WorkshopItem, or NULL after a trap.
void *rt_services_request_item_at(void *request, int64_t index) {
    static const char member[] = "Request.ItemAt";
    rt_services_request_impl *impl = services_request_checked(request, member);
    if (!impl)
        return NULL;
    if (index < 0 || index >= impl->item_count) {
        if (impl->item_count == 0) {
            rt_services_internal_trap_argument(
                member, "index %lld is out of range; the request holds no items", (long long)index);
        } else {
            rt_services_internal_trap_argument(member,
                                               "index %lld is outside 0..%lld",
                                               (long long)index,
                                               (long long)(impl->item_count - 1));
        }
        return NULL;
    }
    rt_obj_retain_known(impl->items[index]);
    return impl->items[index];
}

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return Status.Ok. @return 0.
int64_t rt_services_status_ok(void) {
    return RT_SERVICES_STATUS_OK;
}

/// @brief Return Status.NotStarted. @return 1.
int64_t rt_services_status_not_started(void) {
    return RT_SERVICES_STATUS_NOT_STARTED;
}

/// @brief Return Status.UnknownProvider. @return 2.
int64_t rt_services_status_unknown_provider(void) {
    return RT_SERVICES_STATUS_UNKNOWN_PROVIDER;
}

/// @brief Return Status.LibraryNotFound. @return 3.
int64_t rt_services_status_library_not_found(void) {
    return RT_SERVICES_STATUS_LIBRARY_NOT_FOUND;
}

/// @brief Return Status.LibraryIncompatible. @return 4.
int64_t rt_services_status_library_incompatible(void) {
    return RT_SERVICES_STATUS_LIBRARY_INCOMPATIBLE;
}

/// @brief Return Status.UnsupportedPlatform. @return 5.
int64_t rt_services_status_unsupported_platform(void) {
    return RT_SERVICES_STATUS_UNSUPPORTED_PLATFORM;
}

/// @brief Return Status.ClientNotRunning. @return 6.
int64_t rt_services_status_client_not_running(void) {
    return RT_SERVICES_STATUS_CLIENT_NOT_RUNNING;
}

/// @brief Return Status.VersionMismatch. @return 7.
int64_t rt_services_status_version_mismatch(void) {
    return RT_SERVICES_STATUS_VERSION_MISMATCH;
}

/// @brief Return Status.InitFailed. @return 8.
int64_t rt_services_status_init_failed(void) {
    return RT_SERVICES_STATUS_INIT_FAILED;
}

/// @brief Return EventKind.None. @return 0.
int64_t rt_services_event_kind_none(void) {
    return RT_SERVICES_EVENT_NONE;
}

/// @brief Return EventKind.ServiceConnected. @return 1.
int64_t rt_services_event_kind_service_connected(void) {
    return RT_SERVICES_EVENT_SERVICE_CONNECTED;
}

/// @brief Return EventKind.ServiceDisconnected. @return 2.
int64_t rt_services_event_kind_service_disconnected(void) {
    return RT_SERVICES_EVENT_SERVICE_DISCONNECTED;
}

/// @brief Return EventKind.ConnectFailed. @return 3.
int64_t rt_services_event_kind_connect_failed(void) {
    return RT_SERVICES_EVENT_CONNECT_FAILED;
}

/// @brief Return EventKind.OverlayChanged. @return 4.
int64_t rt_services_event_kind_overlay_changed(void) {
    return RT_SERVICES_EVENT_OVERLAY_CHANGED;
}

/// @brief Return EventKind.DlcInstalled. @return 5.
int64_t rt_services_event_kind_dlc_installed(void) {
    return RT_SERVICES_EVENT_DLC_INSTALLED;
}

/// @brief Return EventKind.LaunchParametersChanged. @return 6.
int64_t rt_services_event_kind_launch_parameters_changed(void) {
    return RT_SERVICES_EVENT_LAUNCH_PARAMETERS_CHANGED;
}

/// @brief Return EventKind.ServiceShutdown. @return 7.
int64_t rt_services_event_kind_service_shutdown(void) {
    return RT_SERVICES_EVENT_SERVICE_SHUTDOWN;
}

/// @brief Return EventKind.StatsStored. @return 8.
int64_t rt_services_event_kind_stats_stored(void) {
    return RT_SERVICES_EVENT_STATS_STORED;
}

/// @brief Return EventKind.AchievementStored. @return 9.
int64_t rt_services_event_kind_achievement_stored(void) {
    return RT_SERVICES_EVENT_ACHIEVEMENT_STORED;
}

/// @brief Return EventKind.TextInputDismissed. @return 10.
int64_t rt_services_event_kind_text_input_dismissed(void) {
    return RT_SERVICES_EVENT_TEXT_INPUT_DISMISSED;
}

/// @brief Return EventKind.AchievementIconReady. @return 11.
int64_t rt_services_event_kind_achievement_icon_ready(void) {
    return RT_SERVICES_EVENT_ACHIEVEMENT_ICON_READY;
}

/// @brief Return EventKind.ControllerConnected. @return 12.
int64_t rt_services_event_kind_controller_connected(void) {
    return RT_SERVICES_EVENT_CONTROLLER_CONNECTED;
}

/// @brief Return EventKind.ControllerDisconnected. @return 13.
int64_t rt_services_event_kind_controller_disconnected(void) {
    return RT_SERVICES_EVENT_CONTROLLER_DISCONNECTED;
}

/// @brief Return EventKind.ControllerConfigured. @return 14.
int64_t rt_services_event_kind_controller_configured(void) {
    return RT_SERVICES_EVENT_CONTROLLER_CONFIGURED;
}

/// @brief Return EventKind.WorkshopItemInstalled. @return 15.
int64_t rt_services_event_kind_workshop_item_installed(void) {
    return RT_SERVICES_EVENT_WORKSHOP_ITEM_INSTALLED;
}

/// @brief Return EventKind.WorkshopItemDownloaded. @return 16.
int64_t rt_services_event_kind_workshop_item_downloaded(void) {
    return RT_SERVICES_EVENT_WORKSHOP_ITEM_DOWNLOADED;
}

/// @brief Return EventKind.WorkshopSubscriptionChanged. @return 17.
int64_t rt_services_event_kind_workshop_subscription_changed(void) {
    return RT_SERVICES_EVENT_WORKSHOP_SUBSCRIPTION_CHANGED;
}

/// @brief Return Feature.Identity. @return 1.
int64_t rt_services_feature_identity(void) {
    return RT_SERVICES_FEATURE_IDENTITY;
}

/// @brief Return Feature.Licensing. @return 2.
int64_t rt_services_feature_licensing(void) {
    return RT_SERVICES_FEATURE_LICENSING;
}

/// @brief Return Feature.Language. @return 3.
int64_t rt_services_feature_language(void) {
    return RT_SERVICES_FEATURE_LANGUAGE;
}

/// @brief Return Feature.PlayerCount. @return 4.
int64_t rt_services_feature_player_count(void) {
    return RT_SERVICES_FEATURE_PLAYER_COUNT;
}

/// @brief Return Feature.Achievements. @return 5.
int64_t rt_services_feature_achievements(void) {
    return RT_SERVICES_FEATURE_ACHIEVEMENTS;
}

/// @brief Return Feature.Stats. @return 6.
int64_t rt_services_feature_stats(void) {
    return RT_SERVICES_FEATURE_STATS;
}

/// @brief Return Feature.Leaderboards. @return 7.
int64_t rt_services_feature_leaderboards(void) {
    return RT_SERVICES_FEATURE_LEADERBOARDS;
}

/// @brief Return Feature.Presence. @return 8.
int64_t rt_services_feature_presence(void) {
    return RT_SERVICES_FEATURE_PRESENCE;
}

/// @brief Return Feature.Overlay. @return 9.
int64_t rt_services_feature_overlay(void) {
    return RT_SERVICES_FEATURE_OVERLAY;
}

/// @brief Return Feature.TextInput. @return 10.
int64_t rt_services_feature_text_input(void) {
    return RT_SERVICES_FEATURE_TEXT_INPUT;
}

/// @brief Return Feature.Cloud. @return 11.
int64_t rt_services_feature_cloud(void) {
    return RT_SERVICES_FEATURE_CLOUD;
}

/// @brief Return Feature.LaunchParameters. @return 12.
int64_t rt_services_feature_launch_parameters(void) {
    return RT_SERVICES_FEATURE_LAUNCH_PARAMETERS;
}

/// @brief Return Feature.Timeline. @return 13.
int64_t rt_services_feature_timeline(void) {
    return RT_SERVICES_FEATURE_TIMELINE;
}

/// @brief Return Feature.AppDetails. @return 14.
int64_t rt_services_feature_app_details(void) {
    return RT_SERVICES_FEATURE_APP_DETAILS;
}

/// @brief Return Feature.AchievementIcons. @return 15.
int64_t rt_services_feature_achievement_icons(void) {
    return RT_SERVICES_FEATURE_ACHIEVEMENT_ICONS;
}

/// @brief Return Feature.AchievementPercentages. @return 16.
int64_t rt_services_feature_achievement_percentages(void) {
    return RT_SERVICES_FEATURE_ACHIEVEMENT_PERCENTAGES;
}

/// @brief Return Feature.ActionInput. @return 17.
int64_t rt_services_feature_action_input(void) {
    return RT_SERVICES_FEATURE_ACTION_INPUT;
}

/// @brief Return Feature.Workshop. @return 18.
int64_t rt_services_feature_workshop(void) {
    return RT_SERVICES_FEATURE_WORKSHOP;
}

/// @brief Return RequestKind.PlayerCount. @return 1.
int64_t rt_services_request_kind_player_count(void) {
    return RT_SERVICES_REQUEST_PLAYER_COUNT;
}

/// @brief Return RequestKind.LeaderboardFind. @return 2.
int64_t rt_services_request_kind_leaderboard_find(void) {
    return RT_SERVICES_REQUEST_LEADERBOARD_FIND;
}

/// @brief Return RequestKind.LeaderboardUpload. @return 3.
int64_t rt_services_request_kind_leaderboard_upload(void) {
    return RT_SERVICES_REQUEST_LEADERBOARD_UPLOAD;
}

/// @brief Return RequestKind.LeaderboardDownload. @return 4.
int64_t rt_services_request_kind_leaderboard_download(void) {
    return RT_SERVICES_REQUEST_LEADERBOARD_DOWNLOAD;
}

/// @brief Return RequestKind.TextInput. @return 5.
int64_t rt_services_request_kind_text_input(void) {
    return RT_SERVICES_REQUEST_TEXT_INPUT;
}

/// @brief Return RequestKind.TimelineEventRecording. @return 6.
int64_t rt_services_request_kind_timeline_event_recording(void) {
    return RT_SERVICES_REQUEST_TIMELINE_EVENT_RECORDING;
}

/// @brief Return RequestKind.TimelinePhaseRecording. @return 7.
int64_t rt_services_request_kind_timeline_phase_recording(void) {
    return RT_SERVICES_REQUEST_TIMELINE_PHASE_RECORDING;
}

/// @brief Return RequestKind.AchievementPercentages. @return 8.
int64_t rt_services_request_kind_achievement_percentages(void) {
    return RT_SERVICES_REQUEST_ACHIEVEMENT_PERCENTAGES;
}

/// @brief Return RequestKind.WorkshopQuery. @return 9.
int64_t rt_services_request_kind_workshop_query(void) {
    return RT_SERVICES_REQUEST_WORKSHOP_QUERY;
}

/// @brief Return RequestKind.WorkshopSubscribe. @return 10.
int64_t rt_services_request_kind_workshop_subscribe(void) {
    return RT_SERVICES_REQUEST_WORKSHOP_SUBSCRIBE;
}

/// @brief Return RequestKind.WorkshopUnsubscribe. @return 11.
int64_t rt_services_request_kind_workshop_unsubscribe(void) {
    return RT_SERVICES_REQUEST_WORKSHOP_UNSUBSCRIBE;
}

/// @brief Return RequestKind.WorkshopCreate. @return 12.
int64_t rt_services_request_kind_workshop_create(void) {
    return RT_SERVICES_REQUEST_WORKSHOP_CREATE;
}

/// @brief Return RequestKind.WorkshopSubmit. @return 13.
int64_t rt_services_request_kind_workshop_submit(void) {
    return RT_SERVICES_REQUEST_WORKSHOP_SUBMIT;
}

/// @brief Return RequestKind.WorkshopDelete. @return 14.
int64_t rt_services_request_kind_workshop_delete(void) {
    return RT_SERVICES_REQUEST_WORKSHOP_DELETE;
}
