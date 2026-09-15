//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_timeline.h
// Purpose: Public C ABI for Zanna.Services.Timeline, the provider-neutral
//          game recording timeline: the game mode shown on the timeline bar,
//          state tooltips, instantaneous and range events, game phases with
//          tags and attributes, recording queries, and overlay navigation,
//          plus the TimelineMode and TimelineClip constants.
// Key invariants:
//   - Stateful entry points must run on the main thread.
//   - Arguments malformed for every provider (empty titles, ids, or tag names,
//     unknown constants, priorities outside their range) trap even without a
//     started provider; non-finite or negative times return false (or an
//     empty event id) with a diagnostic; provider limits do the same.
//   - Without a provider that supports the feature every member returns false
//     or "" and recording requests complete as failed.
//   - Constant ordinals are stable public values documented in ADR 0362.
// Ownership/Lifetime:
//   - Event ids are caller-owned strings; recording queries return
//     caller-owned Zanna.Services.Request objects.
// Links: src/runtime/services/rt_services_timeline.c,
//        src/runtime/services/rt_services_provider.h,
//        docs/zannalib/services.md,
//        docs/adr/0362-platform-services-timeline.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_timeline.h
 * @brief Declares Zanna.Services.Timeline and its constant classes.
 * @details Platforms that record gameplay in the background (Steam game
 *          recording) show a timeline of the session. Games mark it with
 *          events (a home run, a boss fight), describe the current state, and
 *          divide sessions into phases (a match, a chapter) that players can
 *          find and clip later.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Stable constants
//===----------------------------------------------------------------------===//

/// @brief The player is playing.
#define RT_SERVICES_TIMELINE_MODE_PLAYING INT64_C(1)
/// @brief The game is setting up play, such as a lobby or character selection.
#define RT_SERVICES_TIMELINE_MODE_STAGING INT64_C(2)
/// @brief The player is in menus.
#define RT_SERVICES_TIMELINE_MODE_MENUS INT64_C(3)
/// @brief A loading screen is shown.
#define RT_SERVICES_TIMELINE_MODE_LOADING_SCREEN INT64_C(4)

/// @brief The event is not offered as a clip.
#define RT_SERVICES_TIMELINE_CLIP_NONE INT64_C(1)
/// @brief The event may be offered as a clip.
#define RT_SERVICES_TIMELINE_CLIP_STANDARD INT64_C(2)
/// @brief The event is offered as a clip before standard events.
#define RT_SERVICES_TIMELINE_CLIP_FEATURED INT64_C(3)

/// @brief Highest event, tag, and attribute priority.
#define RT_SERVICES_TIMELINE_MAX_PRIORITY INT64_C(1000)
/// @brief Timeline.UpdateRangeEvent priority that keeps the current priority.
#define RT_SERVICES_TIMELINE_KEEP_PRIORITY INT64_C(-1)

//===----------------------------------------------------------------------===//
// Zanna.Services.Timeline
//===----------------------------------------------------------------------===//

/// @brief Set the game mode that colors the timeline bar.
/// @param mode TimelineMode value; other values trap.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_game_mode(int64_t mode);

/// @brief Describe the current game state (for example the score) on the timeline.
/// @param text Non-empty description; an empty text traps (use ClearTooltip).
/// @param offset_seconds Time relative to now; negative values are in the past.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_tooltip(rt_string text, double offset_seconds);

/// @brief Remove the state description.
/// @param offset_seconds Time relative to now.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_clear_tooltip(double offset_seconds);

/// @brief Mark an instantaneous event.
/// @param title Non-empty title.
/// @param description Description, possibly empty.
/// @param icon Provider icon name, possibly empty.
/// @param priority 0..1000; other values trap.
/// @param offset_seconds Time relative to now.
/// @param clip TimelineClip value; other values trap.
/// @return Caller-owned event id, or the empty string when not added.
rt_string rt_services_timeline_add_event(rt_string title,
                                         rt_string description,
                                         rt_string icon,
                                         int64_t priority,
                                         double offset_seconds,
                                         int64_t clip);

/// @brief Mark a range event that is already over.
/// @param title Non-empty title.
/// @param description Description, possibly empty.
/// @param icon Provider icon name, possibly empty.
/// @param priority 0..1000; other values trap.
/// @param offset_seconds Start time relative to now.
/// @param duration_seconds Length of the range; must be finite and not negative.
/// @param clip TimelineClip value; other values trap.
/// @return Caller-owned event id, or the empty string when not added.
rt_string rt_services_timeline_add_range_event(rt_string title,
                                               rt_string description,
                                               rt_string icon,
                                               int64_t priority,
                                               double offset_seconds,
                                               double duration_seconds,
                                               int64_t clip);

/// @brief Start a range event that EndRangeEvent closes.
/// @param title Non-empty title.
/// @param description Description, possibly empty.
/// @param icon Provider icon name, possibly empty.
/// @param priority 0..1000; other values trap.
/// @param offset_seconds Start time relative to now.
/// @param clip TimelineClip value; other values trap.
/// @return Caller-owned event id, or the empty string when not started.
rt_string rt_services_timeline_start_range_event(rt_string title,
                                                 rt_string description,
                                                 rt_string icon,
                                                 int64_t priority,
                                                 double offset_seconds,
                                                 int64_t clip);

/// @brief Change an open range event.
/// @param event_id Id from StartRangeEvent; an empty id traps.
/// @param title Non-empty title.
/// @param description Description, possibly empty.
/// @param icon Provider icon name, possibly empty.
/// @param priority 0..1000, or -1 to keep the current priority; other values trap.
/// @param clip TimelineClip value; other values trap.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_update_range_event(rt_string event_id,
                                               rt_string title,
                                               rt_string description,
                                               rt_string icon,
                                               int64_t priority,
                                               int64_t clip);

/// @brief Close an open range event.
/// @param event_id Id from StartRangeEvent; an empty id traps.
/// @param offset_seconds End time relative to now.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_end_range_event(rt_string event_id, double offset_seconds);

/// @brief Remove an event added by this process.
/// @param event_id Event id; an empty id traps.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_remove_event(rt_string event_id);

/// @brief Ask whether the recording still covers an event.
/// @details The request completes with Flag set when a recording exists and
///          Text holding the event id.
/// @param event_id Event id; an empty id traps.
/// @return Caller-owned Zanna.Services.Request of kind TimelineEventRecording.
void *rt_services_timeline_request_event_recording(rt_string event_id);

/// @brief Start a game phase, ending the current one.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_start_phase(void);

/// @brief End the current game phase.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_end_phase(void);

/// @brief Give the current phase a persistent id for later queries.
/// @param phase_id Non-empty id, such as a match or season id.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_phase_id(rt_string phase_id);

/// @brief Tag the current phase (for example the opponent).
/// @param name Non-empty tag name.
/// @param icon Provider icon name, possibly empty.
/// @param group Non-empty tag group used to filter phases.
/// @param priority 0..1000; other values trap.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_add_phase_tag(rt_string name,
                                          rt_string icon,
                                          rt_string group,
                                          int64_t priority);

/// @brief Set a text attribute of the current phase (for example the score).
/// @param group Non-empty attribute group.
/// @param value Attribute value, possibly empty.
/// @param priority 0..1000; other values trap.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_set_phase_attribute(rt_string group, rt_string value, int64_t priority);

/// @brief Ask what the recording holds for a phase.
/// @details The request completes with Value holding the recorded
///          milliseconds, Flag set when anything was recorded, Text holding
///          the phase id, and details 0 recorded milliseconds, 1 longest clip
///          milliseconds, 2 clip count, 3 screenshot count.
/// @param phase_id Phase id; an empty id traps.
/// @return Caller-owned Zanna.Services.Request of kind TimelinePhaseRecording.
void *rt_services_timeline_request_phase_recording(rt_string phase_id);

/// @brief Open the platform overlay at a phase.
/// @param phase_id Phase id; an empty id traps.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_open_overlay_to_phase(rt_string phase_id);

/// @brief Open the platform overlay at an event.
/// @param event_id Event id; an empty id traps.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_timeline_open_overlay_to_event(rt_string event_id);

//===----------------------------------------------------------------------===//
// Constant classes: Zanna.Services.TimelineMode / TimelineClip
//===----------------------------------------------------------------------===//

/// @brief Return `Zanna.Services.TimelineMode.Playing`. @return Stable ordinal 1.
int64_t rt_services_timeline_mode_playing(void);
/// @brief Return `Zanna.Services.TimelineMode.Staging`. @return Stable ordinal 2.
int64_t rt_services_timeline_mode_staging(void);
/// @brief Return `Zanna.Services.TimelineMode.Menus`. @return Stable ordinal 3.
int64_t rt_services_timeline_mode_menus(void);
/// @brief Return `Zanna.Services.TimelineMode.LoadingScreen`. @return Stable ordinal 4.
int64_t rt_services_timeline_mode_loading_screen(void);

/// @brief Return `Zanna.Services.TimelineClip.None`. @return Stable ordinal 1.
int64_t rt_services_timeline_clip_none(void);
/// @brief Return `Zanna.Services.TimelineClip.Standard`. @return Stable ordinal 2.
int64_t rt_services_timeline_clip_standard(void);
/// @brief Return `Zanna.Services.TimelineClip.Featured`. @return Stable ordinal 3.
int64_t rt_services_timeline_clip_featured(void);

#ifdef __cplusplus
}
#endif
