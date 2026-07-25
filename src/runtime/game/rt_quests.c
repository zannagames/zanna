//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_quests.c
/// @file
/// @brief Implements a bounded quest state machine, polled event queue, and
///        SaveData persistence.
//
// Purpose: Zanna.Game.Quests implementation — quest/stage/objective state
//   machine, polled event ring, one-shot completion flags, and SaveData
//   round-trip. Pure state; no clock or randomness (deterministic).
// Key invariants:
//   - Stage auto-advances when all of its objectives complete; the quest
//     completes after its final stage; every transition emits one event.
//   - Mutations on Hidden/Complete/Failed quests are safe no-op false.
// Ownership/Lifetime:
//   - GC handle; retained id/title/text strings released by the finalizer.
// Links: rt_quests.h, misc/plans/thirdpersonupgrade/29-quest-tracker.md
//
//===----------------------------------------------------------------------===//

#include "rt_quests.h"

#include "rt_platform.h"
#include "rt_savedata.h"
#include "rt_trap.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Allocate a runtime object with the requested class and byte size.
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);

/// @brief Attach a teardown callback to a runtime object.
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));

/// @brief Read a runtime object's class identifier.
extern int64_t rt_obj_class_id(void *obj);

/// @brief Maximum quests retained by one tracker.
#define QUESTS_MAX_QUESTS 32

/// @brief Maximum stages retained by one quest.
#define QUESTS_MAX_STAGES 16

/// @brief Maximum objectives retained by one stage.
#define QUESTS_MAX_OBJECTIVES 8

/// @brief Maximum queued events; overflow discards the oldest event.
#define QUESTS_MAX_EVENTS 64

/// @brief Maximum registration-ID byte length.
#define QUESTS_MAX_ID 64

/// @brief SaveData key containing the version-one serialized state blob.
#define QUESTS_SAVE_KEY "zanna.quests.v1"

/// @brief One flag or counter objective owned inline by a stage.
typedef struct quest_objective {
    rt_string id;
    rt_string text;
    int8_t is_counter; /* 0 = flag, 1 = counter */
    int64_t target;    /* counter target (flags: 1) */
    int64_t progress;  /* current progress (flags: 0/1) */
} quest_objective;

/// @brief One ordered quest stage with an inline objective array.
typedef struct quest_stage {
    rt_string id;
    rt_string text;
    quest_objective objectives[QUESTS_MAX_OBJECTIVES];
    int32_t objective_count;
} quest_stage;

/// @brief One registered quest and its mutable lifecycle state.
typedef struct quest_entry {
    rt_string id;
    rt_string title;
    quest_stage stages[QUESTS_MAX_STAGES];
    int32_t stage_count;
    int64_t state; /* RT_QUEST_STATE_* */
    int32_t current_stage;
    int8_t just_completed; /* one-shot */
} quest_entry;

/// @brief One queued transition/progress notification.
typedef struct quest_event {
    rt_string quest_id; /* borrowed from the quest entry (not retained) */
    int64_t kind;
} quest_event;

/// @brief Private fixed-budget quest and event storage.
typedef struct rt_quests_impl {
    quest_entry quests[QUESTS_MAX_QUESTS];
    int32_t quest_count;
    quest_event events[QUESTS_MAX_EVENTS];
    int32_t event_count;
} rt_quests_impl;

/// @brief Release and clear one retained runtime string slot.
/// @param slot Address of a retained-string field; null/empty is a no-op.
static void quests_release_string(rt_string *slot) {
    /// @brief Release one runtime string reference.
    extern void rt_string_unref(rt_string s);
    if (slot && *slot) {
        rt_string_unref(*slot);
        *slot = NULL;
    }
}

/// @brief Release every retained registration/display string at finalization.
/// @param obj Quest tracker supplied by the runtime finalizer system.
/// @details Event quest IDs are borrowed aliases of quest IDs and therefore
///          require no independent release.
static void quests_finalize(void *obj) {
    rt_quests_impl *tracker = (rt_quests_impl *)obj;
    if (!tracker)
        return;
    for (int32_t q = 0; q < tracker->quest_count; ++q) {
        quest_entry *quest = &tracker->quests[q];
        quests_release_string(&quest->id);
        quests_release_string(&quest->title);
        for (int32_t s = 0; s < quest->stage_count; ++s) {
            quest_stage *stage = &quest->stages[s];
            quests_release_string(&stage->id);
            quests_release_string(&stage->text);
            for (int32_t o = 0; o < stage->objective_count; ++o) {
                quests_release_string(&stage->objectives[o].id);
                quests_release_string(&stage->objectives[o].text);
            }
        }
    }
    tracker->quest_count = 0;
    tracker->event_count = 0;
}

/// @brief Validate and cast an opaque Quests tracker.
/// @param obj Candidate tracker; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return Tracker implementation when valid, otherwise `NULL`.
/// @details A class mismatch raises a runtime trap.
static rt_quests_impl *quests_checked(void *obj, const char *api) {
    if (!obj)
        return NULL;
    if (rt_obj_class_id(obj) != RT_QUESTS_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_quests_impl *)obj;
}

/// @brief Validate a registration id: 1..64 chars of [A-Za-z0-9._-].
/// @param id Runtime string to validate.
/// @return `1` when the explicit bytes satisfy the registration grammar,
///         otherwise `0`.
/// @details Validates the runtime string's explicit byte length and rejects any
///          embedded NUL. `strlen` stops at the first NUL, so a NUL-bearing id
///          would otherwise pass the content/length checks on only its prefix and
///          then alias other ids under `strcmp`/`%s` serialization (VDOC-247).
static int quests_valid_id(rt_string id) {
    if (!id)
        return 0;
    const char *cstr = rt_string_cstr(id);
    if (!cstr)
        return 0;
    int64_t len = rt_str_len(id);
    if (len <= 0 || len > QUESTS_MAX_ID || (size_t)len != strlen(cstr))
        return 0;
    for (int64_t i = 0; i < len; ++i) {
        char c = cstr[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.';
        if (!ok)
            return 0;
    }
    return 1;
}

/// @brief Return an id's C string only when it has no embedded NUL.
/// @param id Runtime string used for lookup.
/// @return Borrowed C-string pointer, or `NULL` for null/invalid input.
/// @details Used for lookups so a NUL-bearing runtime string cannot address a
///          registered id by its pre-NUL prefix; returns NULL (treated as
///          not-found) for a NUL-bearing or empty id (VDOC-247).
static const char *quests_lookup_cstr(rt_string id) {
    if (!id)
        return NULL;
    const char *cstr = rt_string_cstr(id);
    if (!cstr)
        return NULL;
    int64_t len = rt_str_len(id);
    if (len < 0 || (size_t)len != strlen(cstr))
        return NULL;
    return cstr;
}

/// @brief Find a registered quest by exact C-string ID.
/// @param tracker Tracker to scan.
/// @param quest_id Non-null NUL-terminated ID.
/// @return Matching mutable entry, or `NULL`.
static quest_entry *quests_find(rt_quests_impl *tracker, const char *quest_id) {
    for (int32_t q = 0; q < tracker->quest_count; ++q) {
        const char *have = tracker->quests[q].id ? rt_string_cstr(tracker->quests[q].id) : NULL;
        if (have && strcmp(have, quest_id) == 0)
            return &tracker->quests[q];
    }
    return NULL;
}

/// @brief Find a registered stage by exact C-string ID.
/// @param quest Quest to scan.
/// @param stage_id Non-null NUL-terminated ID.
/// @return Matching mutable stage, or `NULL`.
static quest_stage *quests_find_stage(quest_entry *quest, const char *stage_id) {
    for (int32_t s = 0; s < quest->stage_count; ++s) {
        const char *have = quest->stages[s].id ? rt_string_cstr(quest->stages[s].id) : NULL;
        if (have && strcmp(have, stage_id) == 0)
            return &quest->stages[s];
    }
    return NULL;
}

/// @brief Find a registered objective by exact C-string ID.
/// @param stage Stage to scan.
/// @param obj_id Non-null NUL-terminated ID.
/// @return Matching mutable objective, or `NULL`.
static quest_objective *quests_find_objective(quest_stage *stage, const char *obj_id) {
    for (int32_t o = 0; o < stage->objective_count; ++o) {
        const char *have = stage->objectives[o].id ? rt_string_cstr(stage->objectives[o].id) : NULL;
        if (have && strcmp(have, obj_id) == 0)
            return &stage->objectives[o];
    }
    return NULL;
}

/// @brief Append a quest event, dropping the oldest event at capacity.
/// @param tracker Tracker whose FIFO event array is modified.
/// @param quest Quest supplying the borrowed stable ID.
/// @param kind RT_QUEST_EVENT_* value.
/// @details Overflow shifts the remaining 63 records left before appending.
static void quests_emit(rt_quests_impl *tracker, quest_entry *quest, int64_t kind) {
    if (tracker->event_count >= QUESTS_MAX_EVENTS) {
        /* Drop the oldest event; the ring is generous for per-frame polling. */
        memmove(&tracker->events[0],
                &tracker->events[1],
                (QUESTS_MAX_EVENTS - 1) * sizeof(quest_event));
        tracker->event_count = QUESTS_MAX_EVENTS - 1;
    }
    tracker->events[tracker->event_count].quest_id = quest->id;
    tracker->events[tracker->event_count].kind = kind;
    tracker->event_count += 1;
}

/// @brief Allocate an empty quest tracker.
/// @return New runtime-managed tracker, or `NULL` after raising an allocation
///         trap.
/// @details Registration and event arrays are zero-initialized; the finalizer
///          owns all strings retained later.
void *rt_quests_new(void) {
    rt_quests_impl *tracker =
        (rt_quests_impl *)rt_obj_new_i64(RT_QUESTS_CLASS_ID, (int64_t)sizeof(*tracker));
    if (!tracker) {
        rt_trap("Game.Quests.New: allocation failed");
        return NULL;
    }
    memset(tracker, 0, sizeof(*tracker));
    rt_obj_set_finalizer(tracker, quests_finalize);
    return tracker;
}

/// @brief Register a quest or replace an existing quest's display title.
/// @param obj Tracker to modify.
/// @param quest_id Stable ID of 1–64 ASCII letters, digits, dot, underscore, or
///        hyphen.
/// @param title Optional display title retained by the tracker.
/// @return The original @p obj for fluent chaining.
/// @details A new quest starts Hidden with no stages. Re-registration preserves
///          stages and lifecycle state while replacing the retained title.
///          Invalid IDs, capacity overflow, and non-null wrong-class trackers
///          raise traps without registration.
void *rt_quests_add_quest(void *obj, rt_string quest_id, rt_string title) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.AddQuest: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    if (!tracker)
        return obj;
    if (!quests_valid_id(quest_id)) {
        rt_trap("Game.Quests.AddQuest: id must be 1..64 chars of [A-Za-z0-9._-]");
        return obj;
    }
    quest_entry *quest = quests_find(tracker, cid);
    if (!quest) {
        if (tracker->quest_count >= QUESTS_MAX_QUESTS) {
            rt_trap("Game.Quests.AddQuest: quest budget (32) exceeded");
            return obj;
        }
        quest = &tracker->quests[tracker->quest_count++];
        memset(quest, 0, sizeof(*quest));
        quest->id = rt_string_ref(quest_id);
        quest->state = RT_QUEST_STATE_HIDDEN;
    }
    quests_release_string(&quest->title);
    quest->title = title ? rt_string_ref(title) : NULL;
    return obj;
}

/// @brief Register a stage or replace an existing stage's journal text.
/// @param obj Tracker to modify.
/// @param quest_id Existing quest ID.
/// @param stage_id Stable stage ID using the registration grammar.
/// @param text Optional display text retained by the tracker.
/// @return The original @p obj for fluent chaining.
/// @details New stages append in traversal order. Re-registration preserves
///          objectives while replacing text. Unknown quests, invalid IDs,
///          budget overflow, and non-null wrong-class trackers raise traps.
void *rt_quests_add_stage(void *obj, rt_string quest_id, rt_string stage_id, rt_string text) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.AddStage: invalid tracker");
    const char *cq = quests_lookup_cstr(quest_id);
    const char *cs = quests_lookup_cstr(stage_id);
    if (!tracker)
        return obj;
    quest_entry *quest = cq ? quests_find(tracker, cq) : NULL;
    if (!quest) {
        rt_trap("Game.Quests.AddStage: unknown quest id (AddQuest first)");
        return obj;
    }
    if (!quests_valid_id(stage_id)) {
        rt_trap("Game.Quests.AddStage: id must be 1..64 chars of [A-Za-z0-9._-]");
        return obj;
    }
    quest_stage *stage = quests_find_stage(quest, cs);
    if (!stage) {
        if (quest->stage_count >= QUESTS_MAX_STAGES) {
            rt_trap("Game.Quests.AddStage: stage budget (16) exceeded");
            return obj;
        }
        stage = &quest->stages[quest->stage_count++];
        memset(stage, 0, sizeof(*stage));
        stage->id = rt_string_ref(stage_id);
    }
    quests_release_string(&stage->text);
    stage->text = text ? rt_string_ref(text) : NULL;
    return obj;
}

/// @brief Forward declaration for automatic active-stage advancement.
static void quests_check_advance(rt_quests_impl *tracker, quest_entry *quest);

/// @brief Register or reconfigure one flag/counter objective.
/// @param obj Tracker to modify.
/// @param quest_id Existing quest ID.
/// @param stage_id Existing stage ID.
/// @param obj_id Stable objective ID using the registration grammar.
/// @param text Optional retained display text.
/// @param is_counter Nonzero selects counter semantics; zero selects a flag.
/// @param target Requested counter target; nonpositive values become one.
/// @param api_unknown Trap message for invalid tracker or unknown quest/stage.
/// @param api_id Trap message for an invalid objective ID.
/// @param api_budget Trap message for objective-capacity overflow.
/// @return The original @p obj for fluent chaining.
/// @details Re-registration preserves progress, clamps it to the new target,
///          replaces text/type/target, and may immediately advance an active
///          quest whose current stage has become complete.
static void *quests_add_objective(void *obj,
                                  rt_string quest_id,
                                  rt_string stage_id,
                                  rt_string obj_id,
                                  rt_string text,
                                  int8_t is_counter,
                                  int64_t target,
                                  const char *api_unknown,
                                  const char *api_id,
                                  const char *api_budget) {
    rt_quests_impl *tracker = quests_checked(obj, api_unknown);
    const char *cq = quests_lookup_cstr(quest_id);
    const char *cs = quests_lookup_cstr(stage_id);
    const char *co = quests_lookup_cstr(obj_id);
    if (!tracker)
        return obj;
    quest_entry *quest = cq ? quests_find(tracker, cq) : NULL;
    quest_stage *stage = quest && cs ? quests_find_stage(quest, cs) : NULL;
    if (!stage) {
        rt_trap(api_unknown);
        return obj;
    }
    if (!quests_valid_id(obj_id)) {
        rt_trap(api_id);
        return obj;
    }
    quest_objective *objective = quests_find_objective(stage, co);
    if (!objective) {
        if (stage->objective_count >= QUESTS_MAX_OBJECTIVES) {
            rt_trap(api_budget);
            return obj;
        }
        objective = &stage->objectives[stage->objective_count++];
        memset(objective, 0, sizeof(*objective));
        objective->id = rt_string_ref(obj_id);
    }
    quests_release_string(&objective->text);
    objective->text = text ? rt_string_ref(text) : NULL;
    objective->is_counter = is_counter;
    objective->target = is_counter ? (target > 0 ? target : 1) : 1;
    // Re-registration migration: retained progress may now meet or exceed a lower
    // target, so clamp it and re-evaluate advancement. The mutation path would
    // otherwise skip this and could strand an active quest on a stage whose every
    // objective is already complete (VDOC-253). check_advance is a no-op while the
    // quest is not active, so ordinary hidden-quest setup is unaffected.
    if (objective->progress > objective->target)
        objective->progress = objective->target;
    quests_check_advance(tracker, quest);
    return obj;
}

/// @brief Register or reconfigure a boolean flag objective.
/// @param obj Tracker to modify.
/// @param quest_id Existing quest ID.
/// @param stage_id Existing stage ID.
/// @param obj_id Stable objective ID.
/// @param text Optional retained display text.
/// @return The original @p obj for fluent chaining.
/// @details Flags have target one. Validation/trap behavior is provided by the
///          shared objective-registration helper.
void *rt_quests_add_flag(
    void *obj, rt_string quest_id, rt_string stage_id, rt_string obj_id, rt_string text) {
    return quests_add_objective(obj,
                                quest_id,
                                stage_id,
                                obj_id,
                                text,
                                0,
                                1,
                                "Game.Quests.AddFlag: unknown quest/stage id",
                                "Game.Quests.AddFlag: id must be 1..64 chars of [A-Za-z0-9._-]",
                                "Game.Quests.AddFlag: objective budget (8) exceeded");
}

/// @brief Register or reconfigure a counter objective.
/// @param obj Tracker to modify.
/// @param quest_id Existing quest ID.
/// @param stage_id Existing stage ID.
/// @param obj_id Stable objective ID.
/// @param text Optional retained display text.
/// @param target Positive completion target; nonpositive input becomes one.
/// @return The original @p obj for fluent chaining.
void *rt_quests_add_counter(void *obj,
                            rt_string quest_id,
                            rt_string stage_id,
                            rt_string obj_id,
                            rt_string text,
                            int64_t target) {
    return quests_add_objective(obj,
                                quest_id,
                                stage_id,
                                obj_id,
                                text,
                                1,
                                target,
                                "Game.Quests.AddCounter: unknown quest/stage id",
                                "Game.Quests.AddCounter: id must be 1..64 chars of [A-Za-z0-9._-]",
                                "Game.Quests.AddCounter: objective budget (8) exceeded");
}

/// @brief Test whether an objective has reached its target.
/// @param objective Objective to inspect.
/// @return Nonzero when `progress >= target`.
static int quests_objective_done(const quest_objective *objective) {
    return objective->progress >= objective->target;
}

/// @brief Test whether every objective in a stage is complete.
/// @param stage Stage to inspect.
/// @return `1` when all objectives are done, including an empty stage.
static int quests_stage_done(const quest_stage *stage) {
    for (int32_t o = 0; o < stage->objective_count; ++o)
        if (!quests_objective_done(&stage->objectives[o]))
            return 0;
    return 1;
}

/// @brief Advance through consecutively complete active stages.
/// @param tracker Tracker receiving transition events.
/// @param quest Quest to reevaluate.
/// @details Each completed stage emits StageComplete. Crossing beyond the last
///          registered stage sets Complete, arms the one-shot flag, and emits
///          QuestComplete. A zero-stage active quest is unchanged because
///          there is no current stage to evaluate.
static void quests_check_advance(rt_quests_impl *tracker, quest_entry *quest) {
    while (quest->state == RT_QUEST_STATE_ACTIVE && quest->current_stage < quest->stage_count &&
           quests_stage_done(&quest->stages[quest->current_stage])) {
        quests_emit(tracker, quest, RT_QUEST_EVENT_STAGE_COMPLETE);
        quest->current_stage += 1;
        if (quest->current_stage >= quest->stage_count) {
            quest->state = RT_QUEST_STATE_COMPLETE;
            quest->just_completed = 1;
            quests_emit(tracker, quest, RT_QUEST_EVENT_QUEST_COMPLETE);
        }
    }
}

/// @brief Transition a Hidden quest to Active.
/// @param obj Tracker to modify.
/// @param quest_id Registered quest ID.
/// @return `1` on transition, or `0` for null/unknown/malformed ID or a quest
///         not currently Hidden.
/// @details Activation resets the current-stage index to zero, emits Activated,
///          and immediately advances any consecutively complete registered
///          stages. Existing objective progress is preserved. A non-null
///          wrong-class tracker raises a runtime trap.
int8_t rt_quests_activate(void *obj, rt_string quest_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.Activate: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_entry *quest = tracker && cid ? quests_find(tracker, cid) : NULL;
    if (!quest || quest->state != RT_QUEST_STATE_HIDDEN)
        return 0;
    quest->state = RT_QUEST_STATE_ACTIVE;
    quest->current_stage = 0;
    quests_emit(tracker, quest, RT_QUEST_EVENT_ACTIVATED);
    /* Re-evaluate any registered stages already complete at activation time. */
    quests_check_advance(tracker, quest);
    return 1;
}

/// @brief Transition an Active quest to Failed.
/// @param obj Tracker to modify.
/// @param quest_id Registered quest ID.
/// @return `1` on transition, or `0` for null/unknown/malformed ID or a quest
///         not currently Active.
/// @details Success emits QuestFailed. Objective state is preserved. A
///          non-null wrong-class tracker raises a runtime trap.
int8_t rt_quests_fail(void *obj, rt_string quest_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.Fail: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_entry *quest = tracker && cid ? quests_find(tracker, cid) : NULL;
    if (!quest || quest->state != RT_QUEST_STATE_ACTIVE)
        return 0;
    quest->state = RT_QUEST_STATE_FAILED;
    quests_emit(tracker, quest, RT_QUEST_EVENT_QUEST_FAILED);
    return 1;
}

/// @brief Find an objective only in an Active quest's current stage.
/// @param quest Quest to inspect.
/// @param obj_id Non-null objective ID.
/// @return Matching mutable objective, or `NULL` when inactive, beyond the
///         final stage, or unknown.
static quest_objective *quests_active_objective(quest_entry *quest, const char *obj_id) {
    if (quest->state != RT_QUEST_STATE_ACTIVE || quest->current_stage >= quest->stage_count)
        return NULL;
    return quests_find_objective(&quest->stages[quest->current_stage], obj_id);
}

/// @brief Complete a flag objective in the Active quest's current stage.
/// @param obj Tracker to modify.
/// @param quest_id Registered quest ID.
/// @param obj_id Flag-objective ID in the current stage.
/// @return `1` when newly completed, otherwise `0`.
/// @details Counters, already-complete flags, inactive/unknown quests, malformed
///          IDs, and missing objectives are safe failures. Success emits
///          ObjectiveComplete and may emit stage/quest transitions. A non-null
///          wrong-class tracker raises a runtime trap.
int8_t rt_quests_set_flag(void *obj, rt_string quest_id, rt_string obj_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.SetFlag: invalid tracker");
    const char *cq = quests_lookup_cstr(quest_id);
    const char *co = quests_lookup_cstr(obj_id);
    quest_entry *quest = tracker && cq ? quests_find(tracker, cq) : NULL;
    quest_objective *objective = quest && co ? quests_active_objective(quest, co) : NULL;
    // SetFlag only applies to flag objectives; applying it to a counter would
    // silently jump it to its target. Reject the wrong kind as a no-op (VDOC-248).
    if (!objective || objective->is_counter || quests_objective_done(objective))
        return 0;
    objective->progress = objective->target;
    quests_emit(tracker, quest, RT_QUEST_EVENT_OBJECTIVE_COMPLETE);
    quests_check_advance(tracker, quest);
    return 1;
}

/// @brief Add positive progress to a current-stage counter objective.
/// @param obj Tracker to modify.
/// @param quest_id Registered quest ID.
/// @param obj_id Counter-objective ID in the current stage.
/// @param amount Positive increment.
/// @return `1` when progress changes, otherwise `0`.
/// @details Progress saturates at target without signed overflow. Success emits
///          ObjectiveProgress or ObjectiveComplete and may emit stage/quest
///          transitions. Flags, completed counters, nonpositive amounts, and
///          inactive/unknown/malformed input fail safely. A non-null
///          wrong-class tracker raises a runtime trap.
int8_t rt_quests_progress(void *obj, rt_string quest_id, rt_string obj_id, int64_t amount) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.Progress: invalid tracker");
    const char *cq = quests_lookup_cstr(quest_id);
    const char *co = quests_lookup_cstr(obj_id);
    quest_entry *quest = tracker && cq ? quests_find(tracker, cq) : NULL;
    quest_objective *objective = quest && co ? quests_active_objective(quest, co) : NULL;
    // Progress only applies to counter objectives; incrementing a flag would let
    // it complete outside its intended SetFlag path. Reject the wrong kind (VDOC-248).
    if (!objective || !objective->is_counter || amount <= 0 || quests_objective_done(objective))
        return 0;
    // Saturate without evaluating an overflowing sum: progress is in [0, target]
    // and not yet done, so `target - progress` is a non-negative headroom. Only add
    // when it fits; otherwise clamp straight to the target (VDOC-249).
    if (amount >= objective->target - objective->progress)
        objective->progress = objective->target;
    else
        objective->progress += amount;
    quests_emit(tracker,
                quest,
                quests_objective_done(objective) ? RT_QUEST_EVENT_OBJECTIVE_COMPLETE
                                                 : RT_QUEST_EVENT_OBJECTIVE_PROGRESS);
    quests_check_advance(tracker, quest);
    return 1;
}

/// @brief Count quests currently in the Active state.
/// @param obj Tracker to inspect.
/// @return Active count, or zero for `NULL`.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_active_count(void *obj) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.get_ActiveCount: invalid tracker");
    int64_t count = 0;
    if (tracker)
        for (int32_t q = 0; q < tracker->quest_count; ++q)
            if (tracker->quests[q].state == RT_QUEST_STATE_ACTIVE)
                count++;
    return count;
}

/// @brief Read the ID of the Nth Active quest in registration order.
/// @param obj Tracker to inspect.
/// @param index Zero-based Active-quest ordinal.
/// @return Retained quest-ID string, or the canonical empty string when out of
///         range or unavailable.
/// @details Negative indices never match. A non-null wrong-class tracker traps.
rt_string rt_quests_active_quest(void *obj, int64_t index) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.ActiveQuest: invalid tracker");
    if (tracker) {
        int64_t seen = 0;
        for (int32_t q = 0; q < tracker->quest_count; ++q) {
            if (tracker->quests[q].state != RT_QUEST_STATE_ACTIVE)
                continue;
            if (seen == index)
                return rt_string_ref(tracker->quests[q].id);
            seen++;
        }
    }
    return rt_str_empty();
}

/// @brief Read a registered quest's display title.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @return Retained title, or canonical empty string when unknown/unset.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_quest_title(void *obj, rt_string quest_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.QuestTitle: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_entry *quest = tracker && cid ? quests_find(tracker, cid) : NULL;
    return quest && quest->title ? rt_string_ref(quest->title) : rt_str_empty();
}

/// @brief Read a quest lifecycle state for safe polling.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @return RT_QUEST_STATE_* value, defaulting to Hidden when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_quest_state(void *obj, rt_string quest_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.QuestState: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_entry *quest = tracker && cid ? quests_find(tracker, cid) : NULL;
    return quest ? quest->state : RT_QUEST_STATE_HIDDEN;
}

/// @brief Read an Active quest's current-stage journal text.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @return Retained stage text, or canonical empty string when unavailable,
///         unset, inactive, or beyond the final stage.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_current_stage_text(void *obj, rt_string quest_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.CurrentStageText: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_entry *quest = tracker && cid ? quests_find(tracker, cid) : NULL;
    if (!quest || quest->state != RT_QUEST_STATE_ACTIVE ||
        quest->current_stage >= quest->stage_count)
        return rt_str_empty();
    quest_stage *stage = &quest->stages[quest->current_stage];
    return stage->text ? rt_string_ref(stage->text) : rt_str_empty();
}

/// @brief Resolve an Active quest's current stage.
/// @param quest Quest to inspect; may be `NULL`.
/// @return Current mutable stage, or `NULL` when inactive/unavailable.
static quest_stage *quests_current_stage(quest_entry *quest) {
    if (!quest || quest->state != RT_QUEST_STATE_ACTIVE ||
        quest->current_stage >= quest->stage_count)
        return NULL;
    return &quest->stages[quest->current_stage];
}

/// @brief Count objectives in an Active quest's current stage.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @return Objective count, or zero when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_objective_count(void *obj, rt_string quest_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.ObjectiveCount: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_stage *stage = tracker && cid ? quests_current_stage(quests_find(tracker, cid)) : NULL;
    return stage ? stage->objective_count : 0;
}

/// @brief Read current-stage objective display text by index.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based objective index.
/// @return Retained text, or canonical empty string when unavailable/unset.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_objective_text(void *obj, rt_string quest_id, int64_t index) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.ObjectiveText: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_stage *stage = tracker && cid ? quests_current_stage(quests_find(tracker, cid)) : NULL;
    if (!stage || index < 0 || index >= stage->objective_count)
        return rt_str_empty();
    quest_objective *objective = &stage->objectives[index];
    return objective->text ? rt_string_ref(objective->text) : rt_str_empty();
}

/// @brief Read current-stage objective progress.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based objective index.
/// @return Progress in `[0,target]`, or zero when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_objective_progress(void *obj, rt_string quest_id, int64_t index) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.ObjectiveProgress: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_stage *stage = tracker && cid ? quests_current_stage(quests_find(tracker, cid)) : NULL;
    if (!stage || index < 0 || index >= stage->objective_count)
        return 0;
    return stage->objectives[index].progress;
}

/// @brief Read a current-stage objective's completion target.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based objective index.
/// @return Positive target, or zero when unavailable.
/// @details Flags have target one. A non-null wrong-class tracker traps.
int64_t rt_quests_objective_target(void *obj, rt_string quest_id, int64_t index) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.ObjectiveTarget: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_stage *stage = tracker && cid ? quests_current_stage(quests_find(tracker, cid)) : NULL;
    if (!stage || index < 0 || index >= stage->objective_count)
        return 0;
    return stage->objectives[index].target;
}

/// @brief Test current-stage objective completion.
/// @param obj Tracker to inspect.
/// @param quest_id Quest ID.
/// @param index Zero-based objective index.
/// @return `1` when progress reached target, otherwise `0`.
/// @details A non-null wrong-class tracker raises a runtime trap.
int8_t rt_quests_objective_complete(void *obj, rt_string quest_id, int64_t index) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.ObjectiveComplete: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_stage *stage = tracker && cid ? quests_current_stage(quests_find(tracker, cid)) : NULL;
    if (!stage || index < 0 || index >= stage->objective_count)
        return 0;
    return quests_objective_done(&stage->objectives[index]) ? 1 : 0;
}

/// @brief Count currently queued quest events.
/// @param obj Tracker to inspect.
/// @return Queue count in `[0,64]`, or zero for `NULL`.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_event_count(void *obj) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.EventCount: invalid tracker");
    return tracker ? tracker->event_count : 0;
}

/// @brief Read the quest ID attached to a queued event.
/// @param obj Tracker to inspect.
/// @param index Zero-based FIFO event index.
/// @return Retained quest ID, or canonical empty string when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
rt_string rt_quests_event_quest_id(void *obj, int64_t index) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.EventQuestId: invalid tracker");
    if (!tracker || index < 0 || index >= tracker->event_count || !tracker->events[index].quest_id)
        return rt_str_empty();
    return rt_string_ref(tracker->events[index].quest_id);
}

/// @brief Read a queued event's RT_QUEST_EVENT_* kind.
/// @param obj Tracker to inspect.
/// @param index Zero-based FIFO event index.
/// @return Event kind, or `-1` when unavailable.
/// @details A non-null wrong-class tracker raises a runtime trap.
int64_t rt_quests_event_kind(void *obj, int64_t index) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.EventKind: invalid tracker");
    if (!tracker || index < 0 || index >= tracker->event_count)
        return -1;
    return tracker->events[index].kind;
}

/// @brief Discard all queued events.
/// @param obj Tracker to modify; `NULL` is a no-op.
/// @details This does not clear per-quest just-completed flags. A non-null
///          wrong-class tracker raises a runtime trap.
void rt_quests_clear_events(void *obj) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.ClearEvents: invalid tracker");
    if (tracker)
        tracker->event_count = 0;
}

/// @brief Consume a quest's one-shot completion flag.
/// @param obj Tracker to inspect and mutate.
/// @param quest_id Quest ID.
/// @return `1` once after runtime stage advancement completes the quest,
///         otherwise `0`.
/// @details Reading success clears the flag independently of the event queue.
///          Loading Complete state does not arm it. A non-null wrong-class
///          tracker raises a runtime trap.
int8_t rt_quests_just_completed(void *obj, rt_string quest_id) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.JustCompleted: invalid tracker");
    const char *cid = quests_lookup_cstr(quest_id);
    quest_entry *quest = tracker && cid ? quests_find(tracker, cid) : NULL;
    if (!quest || !quest->just_completed)
        return 0;
    quest->just_completed = 0;
    return 1;
}

/*==========================================================================
 * SaveData persistence
 *
 * One string value under "zanna.quests.v1". Ids are [A-Za-z0-9._-] by
 * registration contract, so the compact format needs no escaping:
 *   q=<id>;s=<state>;g=<stage>{;o=<objId>:<progress>}*  records joined by '|'
 *=========================================================================*/

/// @brief Append printf-formatted text to a growable heap buffer.
/// @param buf Address of the heap-buffer pointer.
/// @param used Address of current payload length.
/// @param cap Address of allocated byte capacity.
/// @param fmt printf-compatible format followed by its arguments.
/// @details Grows @p buf as needed so a serialized record is never bounded by a
///          fixed stack chunk — a legal maximum quest (16 stages x 8 objectives x
///          64-byte ids) far exceeds any single fixed buffer (VDOC-250). Keeps the
///          buffer NUL-terminated. @return 1 on success, 0 on format/alloc failure.
static int quests_save_appendf(char **buf, size_t *used, size_t *cap, const char *fmt, ...)
    RT_PRINTF_FORMAT(4, 5);
/// @copydoc quests_save_appendf
static int quests_save_appendf(char **buf, size_t *used, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_measure;
    va_copy(ap_measure, ap);
    int need = vsnprintf(NULL, 0, fmt, ap_measure);
    va_end(ap_measure);
    if (need < 0) {
        va_end(ap);
        return 0;
    }
    if (*used + (size_t)need + 1 > *cap) {
        size_t newcap = *cap ? *cap : 256;
        while (*used + (size_t)need + 1 > newcap) {
            if (newcap > SIZE_MAX / 2) {
                va_end(ap);
                return 0;
            }
            newcap *= 2;
        }
        char *grown = (char *)realloc(*buf, newcap);
        if (!grown) {
            va_end(ap);
            return 0;
        }
        *buf = grown;
        *cap = newcap;
    }
    int wrote = vsnprintf(*buf + *used, *cap - *used, fmt, ap);
    va_end(ap);
    if (wrote < 0)
        return 0;
    *used += (size_t)wrote;
    return 1;
}

/// @brief Serialize registered quest state into SaveData.
/// @param obj Tracker to serialize.
/// @param savedata Destination SaveData handle.
/// @return `1` after storing the blob, or `0` for invalid input or local
///         serialization allocation/format failure.
/// @details The version-one blob stores quest state/current stage plus only
///          nonzero objective progress. IDs provide structure; display and
///          registration metadata are omitted. A non-null wrong-class tracker
///          raises a runtime trap.
int8_t rt_quests_save(void *obj, void *savedata) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.Save: invalid tracker");
    if (!tracker || !savedata)
        return 0;
    size_t capacity = 256;
    size_t used = 0;
    char *out = (char *)malloc(capacity);
    if (!out)
        return 0;
    out[0] = '\0';
    for (int32_t q = 0; q < tracker->quest_count; ++q) {
        quest_entry *quest = &tracker->quests[q];
        // Append each field directly to the growable buffer; no fixed per-quest
        // chunk, so a full-budget quest with max-length ids always serializes.
        if (!quests_save_appendf(&out,
                                 &used,
                                 &capacity,
                                 "%sq=%s;s=%lld;g=%d",
                                 q > 0 ? "|" : "",
                                 rt_string_cstr(quest->id),
                                 (long long)quest->state,
                                 quest->current_stage)) {
            free(out);
            return 0;
        }
        for (int32_t s = 0; s < quest->stage_count; ++s) {
            for (int32_t o = 0; o < quest->stages[s].objective_count; ++o) {
                quest_objective *objective = &quest->stages[s].objectives[o];
                if (objective->progress <= 0)
                    continue;
                if (!quests_save_appendf(&out,
                                         &used,
                                         &capacity,
                                         ";o=%s.%s:%lld",
                                         rt_string_cstr(quest->stages[s].id),
                                         rt_string_cstr(objective->id),
                                         (long long)objective->progress)) {
                    free(out);
                    return 0;
                }
            }
        }
    }
    out[used] = '\0';
    // rt_const_cstr allocates a fresh owned string, and SetString retains its own
    // reference, so release both temporaries after the call to avoid leaking one
    // key and one value per save (VDOC-251).
    rt_string key = rt_const_cstr(QUESTS_SAVE_KEY);
    rt_string value = rt_const_cstr(out);
    rt_savedata_set_string(savedata, key, value);
    rt_string_unref(key);
    rt_string_unref(value);
    free(out);
    return 1;
}

/// @brief Strict signed 64-bit parse of a whole token (VDOC-252).
/// @param s Non-null decimal token.
/// @param out Receives the parsed value on success.
/// @details The entire non-empty token must be a valid base-10 integer with no
///          trailing characters and no overflow — unlike `atoll`, which silently
///          returns 0 or a truncated value for malformed input.
/// @return 1 with @p out set on success, 0 on empty/garbage/overflow input.
static int quests_parse_i64(const char *s, int64_t *out) {
    if (!s || !*s)
        return 0;
    errno = 0;
    char *end = NULL;
    long long value = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return 0;
    *out = (int64_t)value;
    return 1;
}

/// @brief One structural pass over a mutable blob copy (VDOC-252).
/// @param tracker Registered tracker used for ID matching and optional writes.
/// @param text Mutable NUL-terminated blob destructively tokenized in place.
/// @param apply Zero validates only; nonzero applies recognized values.
/// @details Parses `q=<id>;s=<int>;g=<int>{;o=<sid>.<oid>:<int>}*` records joined by
///          `|`. Returns 1 only when every record is well-formed: it must lead with
///          `q=`, every field must use a recognized prefix, and every integer must
///          parse strictly. Unknown quest/stage/objective ids remain tolerated (they
///          are skipped, keeping forward-compatible data patches safe). When @p apply
///          is nonzero the tracker is mutated; the caller validates first (apply=0)
///          and commits only on a fully well-formed blob, so a corrupt save never
///          leaves a hybrid of decoded and stale state.
/// @return 1 if the blob is well-formed, 0 if malformed.
static int quests_load_process(rt_quests_impl *tracker, char *text, int apply) {
    char *cursor = text;
    while (cursor && *cursor) {
        char *next = strchr(cursor, '|');
        if (next)
            *next++ = '\0';
        quest_entry *quest = NULL;
        int first_field = 1;
        char *field = cursor;
        while (field && *field) {
            char *field_next = strchr(field, ';');
            if (field_next)
                *field_next++ = '\0';
            if (strncmp(field, "q=", 2) == 0) {
                if (!first_field)
                    return 0; // a second q= mid-record is malformed
                quest = quests_find(tracker, field + 2);
            } else if (first_field) {
                return 0; // records must lead with q=
            } else if (strncmp(field, "s=", 2) == 0) {
                int64_t state;
                if (!quests_parse_i64(field + 2, &state))
                    return 0;
                if (apply && quest && state >= RT_QUEST_STATE_HIDDEN &&
                    state <= RT_QUEST_STATE_FAILED)
                    quest->state = state;
            } else if (strncmp(field, "g=", 2) == 0) {
                int64_t stage;
                if (!quests_parse_i64(field + 2, &stage))
                    return 0;
                if (apply && quest && stage >= 0 && stage <= quest->stage_count)
                    quest->current_stage = (int32_t)stage;
            } else if (strncmp(field, "o=", 2) == 0) {
                char *sep = strrchr(field + 2, ':');
                char *dot = strchr(field + 2, '.');
                if (!sep || !dot || dot >= sep)
                    return 0;
                *sep = '\0';
                *dot = '\0';
                int64_t progress;
                if (!quests_parse_i64(sep + 1, &progress))
                    return 0;
                if (apply && quest) {
                    quest_stage *stage = quests_find_stage(quest, field + 2);
                    quest_objective *objective =
                        stage ? quests_find_objective(stage, dot + 1) : NULL;
                    if (objective) {
                        if (progress < 0)
                            progress = 0;
                        if (progress > objective->target)
                            progress = objective->target;
                        objective->progress = progress;
                    }
                }
            } else {
                return 0; // unrecognized field prefix
            }
            first_field = 0;
            field = field_next;
        }
        if (first_field)
            return 0; // empty record between separators
        cursor = next;
    }
    return 1;
}

/// @brief Apply saved state onto already-registered quests. Unknown saved
///   ids are tolerated (data patches stay safe); returns false when the
///   tracker has no registrations, the key is absent, or the blob is malformed.
/// @param obj Registered tracker to modify.
/// @param savedata Source SaveData handle.
/// @return `1` after validating and applying a nonempty blob, otherwise `0`.
/// @details Parsing is transactional with validation and application performed
///          on separate mutable copies. Unknown IDs are skipped. Recognized
///          states/stage indices outside valid ranges are ignored; objective
///          progress is clamped. Loading emits no events, performs no automatic
///          advancement, and does not arm just-completed. A non-null
///          wrong-class tracker raises a runtime trap.
int8_t rt_quests_load(void *obj, void *savedata) {
    rt_quests_impl *tracker = quests_checked(obj, "Game.Quests.Load: invalid tracker");
    if (!tracker || !savedata || tracker->quest_count == 0)
        return 0;
    // rt_const_cstr allocates a fresh owned key, and GetString returns a freshly
    // retained handle; release both on every exit to avoid a per-load leak
    // (VDOC-251). rt_str_empty() is the immortal canonical empty string.
    rt_string key = rt_const_cstr(QUESTS_SAVE_KEY);
    rt_string blob = rt_savedata_get_string(savedata, key, rt_str_empty());
    rt_string_unref(key);
    const char *text = blob ? rt_string_cstr(blob) : NULL;
    if (!text || !*text) {
        if (blob)
            rt_string_unref(blob);
        return 0;
    }
    // Two-phase load: validate a throwaway copy first and reject a malformed blob
    // without touching the tracker, then commit on a second clean copy. The
    // tokenizer is destructive, so each phase needs its own copy (VDOC-252).
    size_t len = strlen(text);
    char *validate_copy = (char *)malloc(len + 1);
    char *apply_copy = (char *)malloc(len + 1);
    if (!validate_copy || !apply_copy) {
        free(validate_copy);
        free(apply_copy);
        rt_string_unref(blob);
        return 0;
    }
    memcpy(validate_copy, text, len + 1);
    memcpy(apply_copy, text, len + 1);
    int8_t result = 0;
    if (quests_load_process(tracker, validate_copy, 0)) {
        quests_load_process(tracker, apply_copy, 1);
        result = 1;
    }
    free(validate_copy);
    free(apply_copy);
    rt_string_unref(blob);
    return result;
}

/*==========================================================================
 * Constant accessors (QuestState / QuestEventKind static classes)
 *=========================================================================*/

/// @brief QuestState.Hidden constant.
/// @return RT_QUEST_STATE_HIDDEN.
int64_t rt_quests_state_hidden(void) {
    return RT_QUEST_STATE_HIDDEN;
}

/// @brief QuestState.Active constant.
/// @return RT_QUEST_STATE_ACTIVE.
int64_t rt_quests_state_active(void) {
    return RT_QUEST_STATE_ACTIVE;
}

/// @brief QuestState.Complete constant.
/// @return RT_QUEST_STATE_COMPLETE.
int64_t rt_quests_state_complete(void) {
    return RT_QUEST_STATE_COMPLETE;
}

/// @brief QuestState.Failed constant.
/// @return RT_QUEST_STATE_FAILED.
int64_t rt_quests_state_failed(void) {
    return RT_QUEST_STATE_FAILED;
}

/// @brief QuestEventKind.Activated constant.
/// @return RT_QUEST_EVENT_ACTIVATED.
int64_t rt_quests_event_activated(void) {
    return RT_QUEST_EVENT_ACTIVATED;
}

/// @brief QuestEventKind.ObjectiveProgress constant.
/// @return RT_QUEST_EVENT_OBJECTIVE_PROGRESS.
int64_t rt_quests_event_objective_progress(void) {
    return RT_QUEST_EVENT_OBJECTIVE_PROGRESS;
}

/// @brief QuestEventKind.ObjectiveComplete constant.
/// @return RT_QUEST_EVENT_OBJECTIVE_COMPLETE.
int64_t rt_quests_event_objective_complete(void) {
    return RT_QUEST_EVENT_OBJECTIVE_COMPLETE;
}

/// @brief QuestEventKind.StageComplete constant.
/// @return RT_QUEST_EVENT_STAGE_COMPLETE.
int64_t rt_quests_event_stage_complete(void) {
    return RT_QUEST_EVENT_STAGE_COMPLETE;
}

/// @brief QuestEventKind.QuestComplete constant.
/// @return RT_QUEST_EVENT_QUEST_COMPLETE.
int64_t rt_quests_event_quest_complete(void) {
    return RT_QUEST_EVENT_QUEST_COMPLETE;
}

/// @brief QuestEventKind.QuestFailed constant.
/// @return RT_QUEST_EVENT_QUEST_FAILED.
int64_t rt_quests_event_quest_failed(void) {
    return RT_QUEST_EVENT_QUEST_FAILED;
}
