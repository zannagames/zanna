//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_quests.h
/// @file
/// @brief Declares a bounded quest state machine, polled event queue, and
///        SaveData persistence API.
//
// Purpose: Zanna.Game.Quests — quest/stage/objective tracker with polled
//   progress events and SaveData persistence (plan 29). 2D/3D-agnostic:
//   a pure state machine with no clock, canvas, or scene dependencies.
// Key invariants:
//   - String ids are the stable identity (registration is idempotent on id);
//     bounded shapes trap at registration (32 quests, 16 stages, 8
//     objectives per stage).
//   - Saved state excludes registration data: games re-register at boot,
//     then Load applies id-matched state (unknown saved ids are tolerated).
// Ownership/Lifetime:
//   - Runtime-managed tracker retains registration/display strings; its
//     finalizer releases them.
//   - Returned nonempty strings are retained references for the caller.
// Links: ADR 0099, src/runtime/game/rt_achievement.h (structural template)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate Quests tracker handles.
#define RT_QUESTS_CLASS_ID INT64_C(-0x510220)

/// @brief Registered but not yet activated.
#define RT_QUEST_STATE_HIDDEN 0

/// @brief Currently accepting objective mutations.
#define RT_QUEST_STATE_ACTIVE 1

/// @brief Successfully advanced beyond the final registered stage.
#define RT_QUEST_STATE_COMPLETE 2

/// @brief Explicitly failed while Active.
#define RT_QUEST_STATE_FAILED 3

/// @brief Hidden-to-Active transition event.
#define RT_QUEST_EVENT_ACTIVATED 0

/// @brief Counter changed without reaching its target.
#define RT_QUEST_EVENT_OBJECTIVE_PROGRESS 1

/// @brief Flag/counter newly reached its target.
#define RT_QUEST_EVENT_OBJECTIVE_COMPLETE 2

/// @brief Current stage completed and advanced.
#define RT_QUEST_EVENT_STAGE_COMPLETE 3

/// @brief Quest advanced beyond its final stage.
#define RT_QUEST_EVENT_QUEST_COMPLETE 4

/// @brief Active quest transitioned to Failed.
#define RT_QUEST_EVENT_QUEST_FAILED 5

/// @brief Create an empty quest tracker.
/// @return New runtime-managed tracker, or `NULL` after raising an allocation
///         trap.
void *rt_quests_new(void);

/* Registration (fluent; idempotent on ids). */

/// @brief Register a quest with @p quest_id and display @p title (fluent).
/// @param tracker Tracker to modify.
/// @param quest_id Stable 1–64-byte ID containing only ASCII letters, digits,
///        dot, underscore, or hyphen.
/// @param title Optional display title retained by the tracker.
/// @return The original @p tracker.
/// @details New quests start Hidden. Re-registration replaces title while
///          preserving stages/state. Invalid IDs, the 32-quest budget, and a
///          non-null wrong-class tracker raise runtime traps.
void *rt_quests_add_quest(void *tracker, rt_string quest_id, rt_string title);

/// @brief Append a stage to @p quest_id with journal @p text (fluent).
/// @param tracker Tracker to modify.
/// @param quest_id Existing quest ID.
/// @param stage_id Stable ID using the registration grammar.
/// @param text Optional retained journal text.
/// @return The original @p tracker.
/// @details Re-registration replaces text while preserving objectives. Unknown
///          quests, invalid IDs, the 16-stage budget, and wrong-class trackers
///          raise runtime traps.
void *rt_quests_add_stage(void *tracker, rt_string quest_id, rt_string stage_id, rt_string text);

/// @brief Add a boolean (flag) objective to a stage (fluent).
/// @param tracker Tracker to modify.
/// @param quest_id Existing quest ID.
/// @param stage_id Existing stage ID.
/// @param obj_id Stable ID using the registration grammar.
/// @param text Optional retained display text.
/// @return The original @p tracker.
/// @details Flags have target one. Re-registration preserves/clamps progress
///          and may advance an Active current stage. Unknown parents, invalid
///          IDs, the eight-objective budget, and wrong-class trackers trap.
void *rt_quests_add_flag(
    void *tracker, rt_string quest_id, rt_string stage_id, rt_string obj_id, rt_string text);

/// @brief Add a counting objective completing at @p target (fluent).
/// @param tracker Tracker to modify.
/// @param quest_id Existing quest ID.
/// @param stage_id Existing stage ID.
/// @param obj_id Stable ID using the registration grammar.
/// @param text Optional retained display text.
/// @param target Completion target; nonpositive values become one.
/// @return The original @p tracker.
/// @details Re-registration preserves/clamps progress and may advance an
///          Active current stage. Validation and budget failures raise traps.
void *rt_quests_add_counter(void *tracker,
                            rt_string quest_id,
                            rt_string stage_id,
                            rt_string obj_id,
                            rt_string text,
                            int64_t target);

/* Lifecycle. */

/// @brief Move a Hidden quest to Active and queue Activated.
/// @param tracker Tracker to modify.
/// @param quest_id Registered quest ID.
/// @return `1` on transition, otherwise `0`.
/// @details Current stage resets to zero and already-complete registered stages
///          advance immediately. A quest with no stages remains Active. Existing
///          progress is preserved. A non-null wrong-class tracker traps.
int8_t rt_quests_activate(void *tracker, rt_string quest_id);

/// @brief Move an active quest to FAILED; queues a QUEST_FAILED event.
/// @param tracker Tracker to modify.
/// @param quest_id Registered quest ID.
/// @return `1` on transition, or `0` when unknown/malformed/not Active.
/// @details A non-null wrong-class tracker raises a runtime trap.
int8_t rt_quests_fail(void *tracker, rt_string quest_id);

/// @brief Complete a flag objective in the current stage (advances stage/quest when all complete).
/// @param tracker Tracker to modify.
/// @param quest_id Active quest ID.
/// @param obj_id Flag ID in the current stage.
/// @return `1` when newly completed, otherwise `0`.
/// @details Counters, completed flags, inactive/unknown quests, and malformed
///          IDs fail safely. Success emits ObjectiveComplete before any stage
///          transitions. A non-null wrong-class tracker traps.
int8_t rt_quests_set_flag(void *tracker, rt_string quest_id, rt_string obj_id);

/// @brief Add @p amount to a counter objective (clamped at its target); advances on completion.
/// @param tracker Tracker to modify.
/// @param quest_id Active quest ID.
/// @param obj_id Counter ID in the current stage.
/// @param amount Positive increment.
/// @return `1` when progress changes, otherwise `0`.
/// @details Saturates without overflow and emits ObjectiveProgress or
///          ObjectiveComplete before stage transitions. Wrong objective kind,
///          nonpositive amount, completed/missing/inactive input fail safely.
int8_t rt_quests_progress(void *tracker, rt_string quest_id, rt_string obj_id, int64_t amount);

/* Queries. */

/// @brief Number of quests currently in the ACTIVE state.
/// @param tracker Tracker to inspect.
/// @return Active count, or zero for `NULL`.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_active_count(void *tracker);

/// @brief Id of the @p index-th active quest (0-based; empty when out of range).
/// @param tracker Tracker to inspect.
/// @param index Zero-based Active-quest ordinal in registration order.
/// @return Retained ID or canonical empty string.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_active_quest(void *tracker, int64_t index);

/// @brief Registered display title for @p quest_id (empty when unknown).
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @return Retained title or canonical empty string.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_quest_title(void *tracker, rt_string quest_id);

/// @brief Current RT_QUEST_STATE_* value for @p quest_id (HIDDEN when unknown).
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @return Stored state, defaulting to RT_QUEST_STATE_HIDDEN.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_quest_state(void *tracker, rt_string quest_id);

/// @brief Journal text of the quest's current stage (empty when not active).
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @return Retained text or canonical empty string.
/// @details Only an Active in-range current stage is visible. A non-null
///          wrong-class tracker raises a runtime trap.
rt_string rt_quests_current_stage_text(void *tracker, rt_string quest_id);

/// @brief Objective count of the quest's current stage.
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @return Current-stage count, or zero when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_objective_count(void *tracker, rt_string quest_id);

/// @brief Display text of the @p index-th objective in the current stage.
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based current-stage objective index.
/// @return Retained text or canonical empty string.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_objective_text(void *tracker, rt_string quest_id, int64_t index);

/// @brief Current progress of the @p index-th objective (0/1 for flags).
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based current-stage objective index.
/// @return Progress, or zero when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_objective_progress(void *tracker, rt_string quest_id, int64_t index);

/// @brief Completion target of the @p index-th objective (1 for flags).
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based current-stage objective index.
/// @return Positive target, or zero when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_objective_target(void *tracker, rt_string quest_id, int64_t index);

/// @brief True when the @p index-th objective has reached its target.
/// @param tracker Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based current-stage objective index.
/// @return `1` when complete, otherwise `0`.
/// @details A non-null wrong-class tracker raises a runtime trap.
int8_t rt_quests_objective_complete(void *tracker, rt_string quest_id, int64_t index);

/* Polled events. */

/// @brief Number of queued progress events since the last clear.
/// @param tracker Tracker to inspect.
/// @return FIFO event count in `[0,64]`, or zero for `NULL`.
/// @details When full, a new event discards the oldest. A non-null wrong-class
///          tracker raises a runtime trap.
int64_t rt_quests_event_count(void *tracker);

/// @brief Quest id associated with the @p index-th queued event.
/// @param tracker Tracker to inspect.
/// @param index Zero-based FIFO event index.
/// @return Retained quest ID or canonical empty string.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_event_quest_id(void *tracker, int64_t index);

/// @brief RT_QUEST_EVENT_* kind of the @p index-th queued event.
/// @param tracker Tracker to inspect.
/// @param index Zero-based FIFO event index.
/// @return Event kind, or `-1` when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_event_kind(void *tracker, int64_t index);

/// @brief Drop all queued events (typically after a UI poll).
/// @param tracker Tracker to modify; `NULL` is a no-op.
/// @details Per-quest just-completed flags remain armed. A non-null wrong-class
///          tracker raises a runtime trap.
void rt_quests_clear_events(void *tracker);

/// @brief Consume the one-shot completion flag for @p quest_id.
/// @param tracker Tracker to inspect and mutate.
/// @param quest_id Quest ID.
/// @return `1` once after runtime advancement completes the quest, otherwise
///         `0`.
/// @details This flag is independent of queued events. Successful reading
///          clears it; loading Complete state does not arm it. A non-null
///          wrong-class tracker raises a runtime trap.
int8_t rt_quests_just_completed(void *tracker, rt_string quest_id);

/* Constant accessors (QuestState / QuestEventKind). */

/// @brief RT_QUEST_STATE_HIDDEN as a runtime constant.
/// @return RT_QUEST_STATE_HIDDEN.
int64_t rt_quests_state_hidden(void);

/// @brief RT_QUEST_STATE_ACTIVE as a runtime constant.
/// @return RT_QUEST_STATE_ACTIVE.
int64_t rt_quests_state_active(void);

/// @brief RT_QUEST_STATE_COMPLETE as a runtime constant.
/// @return RT_QUEST_STATE_COMPLETE.
int64_t rt_quests_state_complete(void);

/// @brief RT_QUEST_STATE_FAILED as a runtime constant.
/// @return RT_QUEST_STATE_FAILED.
int64_t rt_quests_state_failed(void);

/// @brief RT_QUEST_EVENT_ACTIVATED as a runtime constant.
/// @return RT_QUEST_EVENT_ACTIVATED.
int64_t rt_quests_event_activated(void);

/// @brief RT_QUEST_EVENT_OBJECTIVE_PROGRESS as a runtime constant.
/// @return RT_QUEST_EVENT_OBJECTIVE_PROGRESS.
int64_t rt_quests_event_objective_progress(void);

/// @brief RT_QUEST_EVENT_OBJECTIVE_COMPLETE as a runtime constant.
/// @return RT_QUEST_EVENT_OBJECTIVE_COMPLETE.
int64_t rt_quests_event_objective_complete(void);

/// @brief RT_QUEST_EVENT_STAGE_COMPLETE as a runtime constant.
/// @return RT_QUEST_EVENT_STAGE_COMPLETE.
int64_t rt_quests_event_stage_complete(void);

/// @brief RT_QUEST_EVENT_QUEST_COMPLETE as a runtime constant.
/// @return RT_QUEST_EVENT_QUEST_COMPLETE.
int64_t rt_quests_event_quest_complete(void);

/// @brief RT_QUEST_EVENT_QUEST_FAILED as a runtime constant.
/// @return RT_QUEST_EVENT_QUEST_FAILED.
int64_t rt_quests_event_quest_failed(void);

/* SaveData persistence. */

/// @brief Serialize quest state (not registration data) into @p savedata.
/// @param tracker Tracker to serialize.
/// @param savedata Destination SaveData handle.
/// @return `1` after storing the version-one blob, otherwise `0`.
/// @details Saves state/current stage and nonzero objective progress under
///          `zanna.quests.v1`; display/registration data is omitted. Local
///          allocation/format failure returns zero. A non-null wrong-class
///          tracker raises a runtime trap.
int8_t rt_quests_save(void *tracker, void *savedata);

/// @brief Apply id-matched saved state onto registered quests (unknown ids tolerated).
/// @param tracker Pre-registered tracker to modify.
/// @param savedata Source SaveData handle.
/// @return `1` after validating and applying a nonempty blob, otherwise `0`.
/// @details Loading is two-phase and malformed data makes no changes. Unknown
///          IDs are skipped; progress is clamped. No events, auto-advancement,
///          or just-completed flags are produced. A non-null wrong-class
///          tracker raises a runtime trap.
int8_t rt_quests_load(void *tracker, void *savedata);

#ifdef __cplusplus
}
#endif
