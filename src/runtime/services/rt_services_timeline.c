//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_timeline.c
// Purpose: Implements the provider-neutral Zanna.Services.Timeline class on
//          top of the active provider's timeline operation table, and the
//          TimelineMode and TimelineClip constant classes.
// Key invariants:
//   - Every member checks the main thread first, then the argument rules that
//     hold for every provider (traps), then run-time values (false with a
//     diagnostic), and only then delegates to the provider.
//   - Without an active provider exposing the timeline, members return false
//     or "" and recording requests complete as failed.
// Ownership/Lifetime:
//   - Event ids are copied from a fixed buffer into caller-owned strings.
//   - Recording queries return caller-owned Zanna.Services.Request objects.
// Links: src/runtime/services/rt_services_timeline.h,
//        src/runtime/services/rt_services_internal.h,
//        docs/adr/0362-platform-services-timeline.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_timeline.c
 * @brief Implements Zanna.Services.Timeline.
 */

#include "rt_services_timeline.h"

#include "rt_services.h"
#include "rt_services_internal.h"
#include "rt_services_provider.h"
#include "rt_string.h"

#include <math.h>
#include <string.h>

/// @brief Capacity, including the terminator, of an event id returned by a provider.
#define TIMELINE_EVENT_ID_CAPACITY 64

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Read the active provider's timeline operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_timeline_ops *timeline_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->timeline : NULL;
}

/// @brief Trap unless @p priority is a valid timeline priority.
/// @param member Class-qualified member name.
/// @param priority Candidate priority.
/// @param allow_keep Nonzero when RT_SERVICES_TIMELINE_KEEP_PRIORITY is accepted.
/// @return 1 when valid, 0 after reporting the trap.
static int timeline_require_priority(const char *member, int64_t priority, int allow_keep) {
    if (priority >= 0 && priority <= RT_SERVICES_TIMELINE_MAX_PRIORITY)
        return 1;
    if (allow_keep && priority == RT_SERVICES_TIMELINE_KEEP_PRIORITY)
        return 1;
    rt_services_internal_trap_argument(member,
                                       allow_keep ? "priority must be in 0..1000 or -1 (got %lld)"
                                                  : "priority must be in 0..1000 (got %lld)",
                                       (long long)priority);
    return 0;
}

/// @brief Trap unless @p clip is a TimelineClip value.
/// @param member Class-qualified member name.
/// @param clip Candidate value.
/// @return 1 when valid, 0 after reporting the trap.
static int timeline_require_clip(const char *member, int64_t clip) {
    if (clip >= RT_SERVICES_TIMELINE_CLIP_NONE && clip <= RT_SERVICES_TIMELINE_CLIP_FEATURED)
        return 1;
    rt_services_internal_trap_argument(
        member, "clip must be a TimelineClip value (got %lld)", (long long)clip);
    return 0;
}

/// @brief Report whether a time offset is finite, recording a diagnostic otherwise.
/// @param member Class-qualified member name.
/// @param offset_seconds Candidate offset.
/// @return 1 when finite, otherwise 0.
static int timeline_finite_offset(const char *member, double offset_seconds) {
    if (isfinite(offset_seconds))
        return 1;
    rt_services_provider_add_diagnostic(
        "Services: %s needs a finite time offset (got %g)", member, offset_seconds);
    return 0;
}

/// @brief Validate and collect an event description.
/// @details Traps on an empty title, a bad priority, or a bad clip value, then
///          checks the offset and (for ranges) the duration.
/// @param member Class-qualified member name.
/// @param title Title argument.
/// @param description Description argument.
/// @param icon Icon argument.
/// @param priority Priority argument.
/// @param allow_keep Nonzero when the keep-current priority is accepted.
/// @param offset_seconds Offset argument.
/// @param duration_seconds Duration argument (ignored unless @p check_duration).
/// @param check_duration Nonzero for range events.
/// @param clip Clip argument.
/// @param out Receives the collected event.
/// @return 1 when the event is valid, otherwise 0 (after a trap or diagnostic).
static int timeline_collect_event(const char *member,
                                  rt_string title,
                                  rt_string description,
                                  rt_string icon,
                                  int64_t priority,
                                  int allow_keep,
                                  double offset_seconds,
                                  double duration_seconds,
                                  int check_duration,
                                  int64_t clip,
                                  rt_services_timeline_event *out) {
    const char *title_text = rt_services_internal_require_name(title, member, "title");
    if (!title_text || !timeline_require_priority(member, priority, allow_keep) ||
        !timeline_require_clip(member, clip))
        return 0;
    if (!timeline_finite_offset(member, offset_seconds))
        return 0;
    if (check_duration && (!isfinite(duration_seconds) || duration_seconds < 0.0)) {
        rt_services_provider_add_diagnostic(
            "Services: %s needs a finite duration of 0 seconds or more (got %g)",
            member,
            duration_seconds);
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->title = title_text;
    out->description = rt_services_internal_cstr(description);
    out->icon = rt_services_internal_cstr(icon);
    out->priority = priority;
    out->offset_seconds = offset_seconds;
    out->duration_seconds = check_duration ? duration_seconds : 0.0;
    out->clip = clip;
    return 1;
}

/// @brief Start a recording query request.
/// @param member Class-qualified member name.
/// @param kind RT_SERVICES_REQUEST_TIMELINE_* kind.
/// @param id Id argument.
/// @param what Argument description for the empty-id trap.
/// @return Caller-owned request, or NULL after a trap.
static void *timeline_begin_recording_request(const char *member,
                                              int64_t kind,
                                              rt_string id,
                                              const char *what) {
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    const char *text = rt_services_internal_require_name(id, member, what);
    if (!text)
        return NULL;
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = kind;
    args.name = text;
    args.text = "";
    return rt_services_internal_begin_request(&args, member);
}

//===----------------------------------------------------------------------===//
// Zanna.Services.Timeline
//===----------------------------------------------------------------------===//

/// @brief Set the game mode that colors the timeline bar.
/// @param mode TimelineMode value.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_game_mode(int64_t mode) {
    static const char member[] = "Timeline.SetGameMode";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    if (mode < RT_SERVICES_TIMELINE_MODE_PLAYING ||
        mode > RT_SERVICES_TIMELINE_MODE_LOADING_SCREEN) {
        rt_services_internal_trap_argument(
            member, "mode must be a TimelineMode value (got %lld)", (long long)mode);
        return 0;
    }
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->set_game_mode && ops->set_game_mode(mode)) ? 1 : 0;
}

/// @brief Describe the current game state on the timeline.
/// @param text Non-empty description.
/// @param offset_seconds Time relative to now.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_tooltip(rt_string text, double offset_seconds) {
    static const char member[] = "Timeline.SetTooltip";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *description = rt_services_internal_require_name(text, member, "text");
    if (!description || !timeline_finite_offset(member, offset_seconds))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->set_tooltip && ops->set_tooltip(description, offset_seconds)) ? 1 : 0;
}

/// @brief Remove the state description.
/// @param offset_seconds Time relative to now.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_clear_tooltip(double offset_seconds) {
    static const char member[] = "Timeline.ClearTooltip";
    if (!rt_services_provider_require_main_thread(member) ||
        !timeline_finite_offset(member, offset_seconds))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->clear_tooltip && ops->clear_tooltip(offset_seconds)) ? 1 : 0;
}

/// @brief Mark an instantaneous event.
/// @param title Non-empty title.
/// @param description Description.
/// @param icon Icon name.
/// @param priority 0..1000.
/// @param offset_seconds Time relative to now.
/// @param clip TimelineClip value.
/// @return Caller-owned event id, or the empty string.
rt_string rt_services_timeline_add_event(rt_string title,
                                         rt_string description,
                                         rt_string icon,
                                         int64_t priority,
                                         double offset_seconds,
                                         int64_t clip) {
    static const char member[] = "Timeline.AddEvent";
    if (!rt_services_provider_require_main_thread(member))
        return rt_str_empty();
    rt_services_timeline_event event;
    if (!timeline_collect_event(
            member, title, description, icon, priority, 0, offset_seconds, 0.0, 0, clip, &event))
        return rt_str_empty();
    const rt_services_timeline_ops *ops = timeline_ops();
    char id[TIMELINE_EVENT_ID_CAPACITY];
    id[0] = '\0';
    if (!ops || !ops->add_event || !ops->add_event(&event, 0, id, sizeof(id)) || !id[0])
        return rt_str_empty();
    return rt_services_internal_owned_text(id);
}

/// @brief Mark a range event that is already over.
/// @param title Non-empty title.
/// @param description Description.
/// @param icon Icon name.
/// @param priority 0..1000.
/// @param offset_seconds Start time relative to now.
/// @param duration_seconds Finite, non-negative length.
/// @param clip TimelineClip value.
/// @return Caller-owned event id, or the empty string.
rt_string rt_services_timeline_add_range_event(rt_string title,
                                               rt_string description,
                                               rt_string icon,
                                               int64_t priority,
                                               double offset_seconds,
                                               double duration_seconds,
                                               int64_t clip) {
    static const char member[] = "Timeline.AddRangeEvent";
    if (!rt_services_provider_require_main_thread(member))
        return rt_str_empty();
    rt_services_timeline_event event;
    if (!timeline_collect_event(member,
                                title,
                                description,
                                icon,
                                priority,
                                0,
                                offset_seconds,
                                duration_seconds,
                                1,
                                clip,
                                &event))
        return rt_str_empty();
    const rt_services_timeline_ops *ops = timeline_ops();
    char id[TIMELINE_EVENT_ID_CAPACITY];
    id[0] = '\0';
    if (!ops || !ops->add_event || !ops->add_event(&event, 1, id, sizeof(id)) || !id[0])
        return rt_str_empty();
    return rt_services_internal_owned_text(id);
}

/// @brief Start a range event.
/// @param title Non-empty title.
/// @param description Description.
/// @param icon Icon name.
/// @param priority 0..1000.
/// @param offset_seconds Start time relative to now.
/// @param clip TimelineClip value.
/// @return Caller-owned event id, or the empty string.
rt_string rt_services_timeline_start_range_event(rt_string title,
                                                 rt_string description,
                                                 rt_string icon,
                                                 int64_t priority,
                                                 double offset_seconds,
                                                 int64_t clip) {
    static const char member[] = "Timeline.StartRangeEvent";
    if (!rt_services_provider_require_main_thread(member))
        return rt_str_empty();
    rt_services_timeline_event event;
    if (!timeline_collect_event(
            member, title, description, icon, priority, 0, offset_seconds, 0.0, 0, clip, &event))
        return rt_str_empty();
    const rt_services_timeline_ops *ops = timeline_ops();
    char id[TIMELINE_EVENT_ID_CAPACITY];
    id[0] = '\0';
    if (!ops || !ops->start_range_event || !ops->start_range_event(&event, id, sizeof(id)) ||
        !id[0])
        return rt_str_empty();
    return rt_services_internal_owned_text(id);
}

/// @brief Change an open range event.
/// @param event_id Id from StartRangeEvent.
/// @param title Non-empty title.
/// @param description Description.
/// @param icon Icon name.
/// @param priority 0..1000, or -1 to keep the current priority.
/// @param clip TimelineClip value.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_update_range_event(rt_string event_id,
                                               rt_string title,
                                               rt_string description,
                                               rt_string icon,
                                               int64_t priority,
                                               int64_t clip) {
    static const char member[] = "Timeline.UpdateRangeEvent";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = rt_services_internal_require_name(event_id, member, "event id");
    if (!id)
        return 0;
    rt_services_timeline_event event;
    if (!timeline_collect_event(
            member, title, description, icon, priority, 1, 0.0, 0.0, 0, clip, &event))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->update_range_event && ops->update_range_event(id, &event)) ? 1 : 0;
}

/// @brief Close an open range event.
/// @param event_id Id from StartRangeEvent.
/// @param offset_seconds End time relative to now.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_end_range_event(rt_string event_id, double offset_seconds) {
    static const char member[] = "Timeline.EndRangeEvent";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = rt_services_internal_require_name(event_id, member, "event id");
    if (!id || !timeline_finite_offset(member, offset_seconds))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->end_range_event && ops->end_range_event(id, offset_seconds)) ? 1 : 0;
}

/// @brief Remove an event.
/// @param event_id Event id.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_remove_event(rt_string event_id) {
    static const char member[] = "Timeline.RemoveEvent";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = rt_services_internal_require_name(event_id, member, "event id");
    if (!id)
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->remove_event && ops->remove_event(id)) ? 1 : 0;
}

/// @brief Ask whether the recording still covers an event.
/// @param event_id Event id.
/// @return Caller-owned request.
void *rt_services_timeline_request_event_recording(rt_string event_id) {
    return timeline_begin_recording_request("Timeline.RequestEventRecording",
                                            RT_SERVICES_REQUEST_TIMELINE_EVENT_RECORDING,
                                            event_id,
                                            "event id");
}

/// @brief Start a game phase.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_start_phase(void) {
    if (!rt_services_provider_require_main_thread("Timeline.StartPhase"))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->start_phase && ops->start_phase()) ? 1 : 0;
}

/// @brief End the current game phase.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_end_phase(void) {
    if (!rt_services_provider_require_main_thread("Timeline.EndPhase"))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->end_phase && ops->end_phase()) ? 1 : 0;
}

/// @brief Give the current phase a persistent id.
/// @param phase_id Non-empty id.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_phase_id(rt_string phase_id) {
    static const char member[] = "Timeline.SetPhaseId";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = rt_services_internal_require_name(phase_id, member, "phase id");
    if (!id)
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->set_phase_id && ops->set_phase_id(id)) ? 1 : 0;
}

/// @brief Tag the current phase.
/// @param name Non-empty tag name.
/// @param icon Icon name.
/// @param group Non-empty tag group.
/// @param priority 0..1000.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_add_phase_tag(rt_string name,
                                          rt_string icon,
                                          rt_string group,
                                          int64_t priority) {
    static const char member[] = "Timeline.AddPhaseTag";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *tag = rt_services_internal_require_name(name, member, "tag name");
    if (!tag)
        return 0;
    const char *tag_group = rt_services_internal_require_name(group, member, "tag group");
    if (!tag_group || !timeline_require_priority(member, priority, 0))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->add_phase_tag &&
            ops->add_phase_tag(tag, rt_services_internal_cstr(icon), tag_group, priority))
               ? 1
               : 0;
}

/// @brief Set a text attribute of the current phase.
/// @param group Non-empty attribute group.
/// @param value Attribute value.
/// @param priority 0..1000.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_phase_attribute(rt_string group,
                                                rt_string value,
                                                int64_t priority) {
    static const char member[] = "Timeline.SetPhaseAttribute";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *attribute_group =
        rt_services_internal_require_name(group, member, "attribute group");
    if (!attribute_group || !timeline_require_priority(member, priority, 0))
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->set_phase_attribute &&
            ops->set_phase_attribute(attribute_group, rt_services_internal_cstr(value), priority))
               ? 1
               : 0;
}

/// @brief Ask what the recording holds for a phase.
/// @param phase_id Phase id.
/// @return Caller-owned request.
void *rt_services_timeline_request_phase_recording(rt_string phase_id) {
    return timeline_begin_recording_request("Timeline.RequestPhaseRecording",
                                            RT_SERVICES_REQUEST_TIMELINE_PHASE_RECORDING,
                                            phase_id,
                                            "phase id");
}

/// @brief Open the platform overlay at a phase.
/// @param phase_id Phase id.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_open_overlay_to_phase(rt_string phase_id) {
    static const char member[] = "Timeline.OpenOverlayToPhase";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = rt_services_internal_require_name(phase_id, member, "phase id");
    if (!id)
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->open_overlay_to_phase && ops->open_overlay_to_phase(id)) ? 1 : 0;
}

/// @brief Open the platform overlay at an event.
/// @param event_id Event id.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_open_overlay_to_event(rt_string event_id) {
    static const char member[] = "Timeline.OpenOverlayToEvent";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *id = rt_services_internal_require_name(event_id, member, "event id");
    if (!id)
        return 0;
    const rt_services_timeline_ops *ops = timeline_ops();
    return (ops && ops->open_overlay_to_event && ops->open_overlay_to_event(id)) ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return TimelineMode.Playing. @return 1.
int64_t rt_services_timeline_mode_playing(void) {
    return RT_SERVICES_TIMELINE_MODE_PLAYING;
}

/// @brief Return TimelineMode.Staging. @return 2.
int64_t rt_services_timeline_mode_staging(void) {
    return RT_SERVICES_TIMELINE_MODE_STAGING;
}

/// @brief Return TimelineMode.Menus. @return 3.
int64_t rt_services_timeline_mode_menus(void) {
    return RT_SERVICES_TIMELINE_MODE_MENUS;
}

/// @brief Return TimelineMode.LoadingScreen. @return 4.
int64_t rt_services_timeline_mode_loading_screen(void) {
    return RT_SERVICES_TIMELINE_MODE_LOADING_SCREEN;
}

/// @brief Return TimelineClip.None. @return 1.
int64_t rt_services_timeline_clip_none(void) {
    return RT_SERVICES_TIMELINE_CLIP_NONE;
}

/// @brief Return TimelineClip.Standard. @return 2.
int64_t rt_services_timeline_clip_standard(void) {
    return RT_SERVICES_TIMELINE_CLIP_STANDARD;
}

/// @brief Return TimelineClip.Featured. @return 3.
int64_t rt_services_timeline_clip_featured(void) {
    return RT_SERVICES_TIMELINE_CLIP_FEATURED;
}
