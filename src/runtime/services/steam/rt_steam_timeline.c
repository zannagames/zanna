//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_timeline.c
// Purpose: Binds ISteamTimeline (Steam game recording) for the Steam provider's
//          Zanna.Services.Timeline operations and recording queries.
// Key invariants:
//   - The group is usable only when its accessor and every export resolved;
//     otherwise one diagnostic names what is missing.
//   - Event ids are decimal TimelineEventHandle_t values; a malformed id traps
//     while Steam is the active provider.
//   - Times Steam cannot represent (outside float, ranges longer than 600
//     seconds, phase ids of 64 bytes or more) return false with a diagnostic
//     before calling Steam.
// Ownership/Lifetime:
//   - Strings passed to Steam are borrowed for the call; query results copy the
//     id they name into the completed request.
// Links: src/runtime/services/steam/rt_steam_internal.h,
//        src/runtime/services/rt_services_timeline.h,
//        docs/adr/0362-platform-services-timeline.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_timeline.c
 * @brief Implements Steam timeline markers, phases, and recording queries.
 */

#include "rt_platform.h"
#include "rt_services.h"
#include "rt_services_provider.h"
#include "rt_services_timeline.h"
#include "rt_steam_abi.h"
#include "rt_steam_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

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

/// @brief Open and bind ISteamTimeline.
void rt_services_steam_bind_timeline(void) {
    steam_timeline_api api;
    memset(&api, 0, sizeof(api));
    const char *missing = NULL;

    void *accessor = rt_services_steam_symbol(RT_STEAM_SYMBOL_TIMELINE_V004);
    if (accessor)
        api.self = (RT_FN_PTR_CAST((rt_steam_accessor_fn)accessor))();
    if (!api.self) {
        rt_services_steam_report_missing(RT_STEAM_SYMBOL_TIMELINE_V004, "timeline");
        return;
    }
    STEAM_BIND(api.set_tooltip,
               rt_steam_timeline_tooltip_fn,
               RT_STEAM_SYMBOL_TIMELINE_SET_TOOLTIP,
               missing);
    STEAM_BIND(api.clear_tooltip,
               rt_steam_timeline_clear_tooltip_fn,
               RT_STEAM_SYMBOL_TIMELINE_CLEAR_TOOLTIP,
               missing);
    STEAM_BIND(api.set_game_mode,
               rt_steam_timeline_game_mode_fn,
               RT_STEAM_SYMBOL_TIMELINE_SET_GAME_MODE,
               missing);
    STEAM_BIND(api.add_instant,
               rt_steam_timeline_add_instant_fn,
               RT_STEAM_SYMBOL_TIMELINE_ADD_INSTANT,
               missing);
    STEAM_BIND(
        api.add_range, rt_steam_timeline_add_range_fn, RT_STEAM_SYMBOL_TIMELINE_ADD_RANGE, missing);
    STEAM_BIND(api.start_range,
               rt_steam_timeline_start_range_fn,
               RT_STEAM_SYMBOL_TIMELINE_START_RANGE,
               missing);
    STEAM_BIND(api.update_range,
               rt_steam_timeline_update_range_fn,
               RT_STEAM_SYMBOL_TIMELINE_UPDATE_RANGE,
               missing);
    STEAM_BIND(
        api.end_range, rt_steam_timeline_end_range_fn, RT_STEAM_SYMBOL_TIMELINE_END_RANGE, missing);
    STEAM_BIND(api.remove_event,
               rt_steam_self_u64_void_fn,
               RT_STEAM_SYMBOL_TIMELINE_REMOVE_EVENT,
               missing);
    STEAM_BIND(api.event_recording,
               rt_steam_self_u64_call_fn,
               RT_STEAM_SYMBOL_TIMELINE_EVENT_RECORDING,
               missing);
    STEAM_BIND(
        api.start_phase, rt_steam_self_void_fn, RT_STEAM_SYMBOL_TIMELINE_START_PHASE, missing);
    STEAM_BIND(api.end_phase, rt_steam_self_void_fn, RT_STEAM_SYMBOL_TIMELINE_END_PHASE, missing);
    STEAM_BIND(api.set_phase_id,
               rt_steam_self_str_void_fn,
               RT_STEAM_SYMBOL_TIMELINE_SET_PHASE_ID,
               missing);
    STEAM_BIND(api.phase_recording,
               rt_steam_self_str_call_fn,
               RT_STEAM_SYMBOL_TIMELINE_PHASE_RECORDING,
               missing);
    STEAM_BIND(api.add_phase_tag,
               rt_steam_timeline_phase_tag_fn,
               RT_STEAM_SYMBOL_TIMELINE_ADD_PHASE_TAG,
               missing);
    STEAM_BIND(api.set_attribute,
               rt_steam_timeline_phase_attribute_fn,
               RT_STEAM_SYMBOL_TIMELINE_SET_PHASE_ATTRIBUTE,
               missing);
    STEAM_BIND(api.overlay_to_phase,
               rt_steam_self_str_void_fn,
               RT_STEAM_SYMBOL_TIMELINE_OVERLAY_TO_PHASE,
               missing);
    STEAM_BIND(api.overlay_to_event,
               rt_steam_self_u64_void_fn,
               RT_STEAM_SYMBOL_TIMELINE_OVERLAY_TO_EVENT,
               missing);
    if (missing) {
        rt_services_steam_report_missing(missing, "timeline");
        return;
    }
    api.ready = 1;
    g_steam.timeline = api;
}

#undef STEAM_BIND

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Report whether the timeline group can be called.
/// @return 1 when started and bound, otherwise 0.
static int steam_timeline_ready(void) {
    return g_steam.started && g_steam.timeline.ready;
}

/// @brief Convert a time in seconds to the float Steam takes.
/// @param seconds Finite time in seconds.
/// @param out Receives the converted value.
/// @return 1 when representable, otherwise 0 after recording a diagnostic.
static int steam_timeline_seconds(double seconds, float *out) {
    if (seconds > FLT_MAX || seconds < -FLT_MAX) {
        rt_services_provider_add_diagnostic(
            "Steam: timeline time %g seconds is outside the float range", seconds);
        return 0;
    }
    *out = (float)seconds;
    return 1;
}

/// @brief Parse a timeline event id, trapping on a malformed one.
/// @param member Class-qualified member name for the trap.
/// @param event_id Decimal TimelineEventHandle_t text.
/// @param out Receives the handle.
/// @return 1 when parsed, 0 after reporting the trap.
static int steam_timeline_event_handle(const char *member, const char *event_id, uint64_t *out) {
    if (rt_services_steam_parse_u64(event_id, out))
        return 1;
    char message[320];
    snprintf(message,
             sizeof(message),
             "Services.%s: Steam timeline event id '%s' must be an integer in "
             "1..18446744073709551615",
             member,
             event_id);
    rt_trap(message);
    return 0;
}

/// @brief Report whether a phase id fits Steam's phase id buffer.
/// @param phase_id Phase id.
/// @return 1 when it fits, otherwise 0 after recording a diagnostic.
static int steam_timeline_phase_fits(const char *phase_id) {
    if (strlen(phase_id) < RT_STEAM_TIMELINE_PHASE_ID_CAPACITY)
        return 1;
    rt_services_provider_add_diagnostic("Steam: timeline phase id '%s' is longer than %d bytes",
                                        phase_id,
                                        RT_STEAM_TIMELINE_PHASE_ID_CAPACITY - 1);
    return 0;
}

/// @brief Write a timeline event handle as decimal text.
/// @param handle TimelineEventHandle_t.
/// @param out Destination buffer.
/// @param capacity Size of @p out in bytes.
/// @return 1 when the handle is valid and was written, otherwise 0.
static int steam_timeline_write_handle(uint64_t handle, char *out, size_t capacity) {
    if (handle == 0)
        return 0;
    snprintf(out, capacity, "%llu", (unsigned long long)handle);
    return 1;
}

//===----------------------------------------------------------------------===//
// Operations
//===----------------------------------------------------------------------===//

/// @brief Set the timeline game mode.
/// @param mode TimelineMode value (same ordinals as ETimelineGameMode).
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_set_game_mode(int64_t mode) {
    if (!steam_timeline_ready())
        return 0;
    g_steam.timeline.set_game_mode(g_steam.timeline.self, (int)mode);
    return 1;
}

/// @brief Set the timeline tooltip.
/// @param text Description.
/// @param offset_seconds Time relative to now.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_set_tooltip(const char *text, double offset_seconds) {
    float offset = 0.0f;
    if (!steam_timeline_ready() || !steam_timeline_seconds(offset_seconds, &offset))
        return 0;
    g_steam.timeline.set_tooltip(g_steam.timeline.self, text, offset);
    return 1;
}

/// @brief Clear the timeline tooltip.
/// @param offset_seconds Time relative to now.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_clear_tooltip(double offset_seconds) {
    float offset = 0.0f;
    if (!steam_timeline_ready() || !steam_timeline_seconds(offset_seconds, &offset))
        return 0;
    g_steam.timeline.clear_tooltip(g_steam.timeline.self, offset);
    return 1;
}

/// @brief Add an instantaneous or closed range event.
/// @param event Validated event.
/// @param range Nonzero for a range event.
/// @param out_id Receives the event id.
/// @param id_capacity Size of @p out_id in bytes.
/// @return 1 when added, otherwise 0.
static int8_t steam_timeline_add_event(const rt_services_timeline_event *event,
                                       int8_t range,
                                       char *out_id,
                                       size_t id_capacity) {
    float offset = 0.0f;
    if (!steam_timeline_ready() || !steam_timeline_seconds(event->offset_seconds, &offset))
        return 0;
    uint64_t handle = 0;
    if (range) {
        if (event->duration_seconds > RT_STEAM_TIMELINE_MAX_EVENT_DURATION) {
            rt_services_provider_add_diagnostic(
                "Steam: timeline range events last at most 600 seconds (got %g)",
                event->duration_seconds);
            return 0;
        }
        handle = g_steam.timeline.add_range(g_steam.timeline.self,
                                            event->title,
                                            event->description,
                                            event->icon,
                                            (uint32_t)event->priority,
                                            offset,
                                            (float)event->duration_seconds,
                                            (int)event->clip);
    } else {
        handle = g_steam.timeline.add_instant(g_steam.timeline.self,
                                              event->title,
                                              event->description,
                                              event->icon,
                                              (uint32_t)event->priority,
                                              offset,
                                              (int)event->clip);
    }
    if (steam_timeline_write_handle(handle, out_id, id_capacity))
        return 1;
    rt_services_provider_add_diagnostic("Steam: %s returned no timeline event handle",
                                        range ? "AddRangeTimelineEvent"
                                              : "AddInstantaneousTimelineEvent");
    return 0;
}

/// @brief Start an open range event.
/// @param event Validated event.
/// @param out_id Receives the event id.
/// @param id_capacity Size of @p out_id in bytes.
/// @return 1 when started, otherwise 0.
static int8_t steam_timeline_start_range_event(const rt_services_timeline_event *event,
                                               char *out_id,
                                               size_t id_capacity) {
    float offset = 0.0f;
    if (!steam_timeline_ready() || !steam_timeline_seconds(event->offset_seconds, &offset))
        return 0;
    uint64_t handle = g_steam.timeline.start_range(g_steam.timeline.self,
                                                   event->title,
                                                   event->description,
                                                   event->icon,
                                                   (uint32_t)event->priority,
                                                   offset,
                                                   (int)event->clip);
    if (steam_timeline_write_handle(handle, out_id, id_capacity))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: StartRangeTimelineEvent returned no timeline event handle");
    return 0;
}

/// @brief Update an open range event.
/// @param event_id Decimal event id; a malformed id traps.
/// @param event Validated event (priority may be the keep-current value).
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_update_range_event(const char *event_id,
                                                const rt_services_timeline_event *event) {
    uint64_t handle = 0;
    if (!steam_timeline_event_handle("Timeline.UpdateRangeEvent", event_id, &handle) ||
        !steam_timeline_ready())
        return 0;
    const uint32_t priority = event->priority == RT_SERVICES_TIMELINE_KEEP_PRIORITY
                                  ? RT_STEAM_TIMELINE_PRIORITY_KEEP_CURRENT
                                  : (uint32_t)event->priority;
    g_steam.timeline.update_range(g_steam.timeline.self,
                                  handle,
                                  event->title,
                                  event->description,
                                  event->icon,
                                  priority,
                                  (int)event->clip);
    return 1;
}

/// @brief End an open range event.
/// @param event_id Decimal event id; a malformed id traps.
/// @param offset_seconds End time relative to now.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_end_range_event(const char *event_id, double offset_seconds) {
    uint64_t handle = 0;
    float offset = 0.0f;
    if (!steam_timeline_event_handle("Timeline.EndRangeEvent", event_id, &handle) ||
        !steam_timeline_ready() || !steam_timeline_seconds(offset_seconds, &offset))
        return 0;
    g_steam.timeline.end_range(g_steam.timeline.self, handle, offset);
    return 1;
}

/// @brief Remove an event.
/// @param event_id Decimal event id; a malformed id traps.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_remove_event(const char *event_id) {
    uint64_t handle = 0;
    if (!steam_timeline_event_handle("Timeline.RemoveEvent", event_id, &handle) ||
        !steam_timeline_ready())
        return 0;
    g_steam.timeline.remove_event(g_steam.timeline.self, handle);
    return 1;
}

/// @brief Start a game phase.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_start_phase(void) {
    if (!steam_timeline_ready())
        return 0;
    g_steam.timeline.start_phase(g_steam.timeline.self);
    return 1;
}

/// @brief End the current game phase.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_end_phase(void) {
    if (!steam_timeline_ready())
        return 0;
    g_steam.timeline.end_phase(g_steam.timeline.self);
    return 1;
}

/// @brief Set the current phase id.
/// @param phase_id Non-empty phase id.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_set_phase_id(const char *phase_id) {
    if (!steam_timeline_ready() || !steam_timeline_phase_fits(phase_id))
        return 0;
    g_steam.timeline.set_phase_id(g_steam.timeline.self, phase_id);
    return 1;
}

/// @brief Tag the current phase.
/// @param name Tag name.
/// @param icon Icon name.
/// @param group Tag group.
/// @param priority 0..1000.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_add_phase_tag(const char *name,
                                           const char *icon,
                                           const char *group,
                                           int64_t priority) {
    if (!steam_timeline_ready())
        return 0;
    g_steam.timeline.add_phase_tag(g_steam.timeline.self, name, icon, group, (uint32_t)priority);
    return 1;
}

/// @brief Set a phase attribute.
/// @param group Attribute group.
/// @param value Attribute value.
/// @param priority 0..1000.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_set_phase_attribute(const char *group,
                                                 const char *value,
                                                 int64_t priority) {
    if (!steam_timeline_ready())
        return 0;
    g_steam.timeline.set_attribute(g_steam.timeline.self, group, value, (uint32_t)priority);
    return 1;
}

/// @brief Open the overlay at a phase.
/// @param phase_id Phase id.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_open_overlay_to_phase(const char *phase_id) {
    if (!steam_timeline_ready() || !steam_timeline_phase_fits(phase_id))
        return 0;
    g_steam.timeline.overlay_to_phase(g_steam.timeline.self, phase_id);
    return 1;
}

/// @brief Open the overlay at an event.
/// @param event_id Decimal event id; a malformed id traps.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_timeline_open_overlay_to_event(const char *event_id) {
    uint64_t handle = 0;
    if (!steam_timeline_event_handle("Timeline.OpenOverlayToEvent", event_id, &handle) ||
        !steam_timeline_ready())
        return 0;
    g_steam.timeline.overlay_to_event(g_steam.timeline.self, handle);
    return 1;
}

/// @brief Steam timeline operations.
const rt_services_timeline_ops rt_services_steam_timeline_ops = {
    .set_game_mode = steam_timeline_set_game_mode,
    .set_tooltip = steam_timeline_set_tooltip,
    .clear_tooltip = steam_timeline_clear_tooltip,
    .add_event = steam_timeline_add_event,
    .start_range_event = steam_timeline_start_range_event,
    .update_range_event = steam_timeline_update_range_event,
    .end_range_event = steam_timeline_end_range_event,
    .remove_event = steam_timeline_remove_event,
    .start_phase = steam_timeline_start_phase,
    .end_phase = steam_timeline_end_phase,
    .set_phase_id = steam_timeline_set_phase_id,
    .add_phase_tag = steam_timeline_add_phase_tag,
    .set_phase_attribute = steam_timeline_set_phase_attribute,
    .open_overlay_to_phase = steam_timeline_open_overlay_to_phase,
    .open_overlay_to_event = steam_timeline_open_overlay_to_event,
};

//===----------------------------------------------------------------------===//
// Recording queries
//===----------------------------------------------------------------------===//

/// @brief Start a timeline recording query.
/// @param args Validated query arguments; args->name holds the event or phase id.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when started, otherwise 0.
int8_t rt_services_steam_begin_timeline_request(const rt_services_request_args *args,
                                                uint64_t *out_handle,
                                                char *message,
                                                size_t message_capacity) {
    const int event_query = args->kind == RT_SERVICES_REQUEST_TIMELINE_EVENT_RECORDING;
    uint64_t event_handle = 0;
    if (event_query &&
        !steam_timeline_event_handle("Timeline.RequestEventRecording", args->name, &event_handle))
        return 0;
    if (!steam_timeline_ready()) {
        snprintf(message,
                 message_capacity,
                 "Steam: the timeline is unavailable (see Platform.Diagnostics)");
        return 0;
    }
    if (!event_query && strlen(args->name) >= RT_STEAM_TIMELINE_PHASE_ID_CAPACITY) {
        snprintf(message,
                 message_capacity,
                 "Steam: timeline phase id '%s' is longer than %d bytes",
                 args->name,
                 RT_STEAM_TIMELINE_PHASE_ID_CAPACITY - 1);
        return 0;
    }
    steam_request_op *op = rt_services_steam_op_alloc(
        event_query ? STEAM_OP_TIMELINE_EVENT_RECORDING : STEAM_OP_TIMELINE_PHASE_RECORDING);
    if (!op) {
        snprintf(message, message_capacity, "Steam: too many pending requests");
        return 0;
    }
    snprintf(op->name, sizeof(op->name), "%s", args->name);
    rt_steam_api_call call =
        event_query ? g_steam.timeline.event_recording(g_steam.timeline.self, event_handle)
                    : g_steam.timeline.phase_recording(g_steam.timeline.self, args->name);
    if (call == 0) {
        rt_services_steam_op_free(op);
        snprintf(message,
                 message_capacity,
                 "Steam: %s returned an invalid call handle",
                 event_query ? "DoesEventRecordingExist" : "DoesGamePhaseRecordingExist");
        return 0;
    }
    op->call = call;
    *out_handle = op->token;
    return 1;
}

/// @brief Clamp an unsigned 64-bit count to int64.
/// @param value Count.
/// @return @p value, or INT64_MAX when it does not fit.
static int64_t steam_timeline_clamp(uint64_t value) {
    return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

/// @brief Complete a timeline recording query.
/// @param op Operation in a timeline stage.
/// @param completed Decoded SteamAPICallCompleted_t.
void rt_services_steam_timeline_call_completed(steam_request_op *op,
                                               const rt_steam_api_call_completed *completed) {
    char error[RT_SERVICES_MESSAGE_CAPACITY];
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.succeeded = 1;
    result.result_code = RT_STEAM_RESULT_OK;
    result.text = op->name;

    if (op->stage == STEAM_OP_TIMELINE_EVENT_RECORDING) {
        rt_steam_timeline_event_recording_exists exists;
        if (!rt_services_steam_fetch_call_result(completed,
                                                 RT_STEAM_CB_TIMELINE_EVENT_RECORDING_EXISTS,
                                                 &exists,
                                                 sizeof(exists),
                                                 "DoesEventRecordingExist",
                                                 error,
                                                 sizeof(error))) {
            rt_services_steam_op_fail(op, error);
            return;
        }
        result.flag = exists.recording_exists ? 1 : 0;
        // Finish before freeing the slot: result.text points into op->name.
        rt_services_provider_finish_request(op->token, &result);
        rt_services_steam_op_free(op);
        return;
    }

    rt_steam_timeline_phase_recording_exists phase;
    if (!rt_services_steam_fetch_call_result(completed,
                                             RT_STEAM_CB_TIMELINE_PHASE_RECORDING_EXISTS,
                                             &phase,
                                             sizeof(phase),
                                             "DoesGamePhaseRecordingExist",
                                             error,
                                             sizeof(error))) {
        rt_services_steam_op_fail(op, error);
        return;
    }
    const int64_t details[4] = {
        steam_timeline_clamp(phase.recording_ms),
        steam_timeline_clamp(phase.longest_clip_ms),
        (int64_t)phase.clip_count,
        (int64_t)phase.screenshot_count,
    };
    result.value = details[0];
    result.flag =
        (phase.recording_ms > 0 || phase.clip_count > 0 || phase.screenshot_count > 0) ? 1 : 0;
    result.details = details;
    result.detail_count = 4;
    // Finish before freeing the slot: result.text points into op->name.
    rt_services_provider_finish_request(op->token, &result);
    rt_services_steam_op_free(op);
}
