//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_statemachine.c
/// @file
/// @brief Implements a bounded integer state machine with registered targets,
///        transition edges, and saturating per-state frame counts.
// Purpose: Finite state machine implementation for Zanna game and application
//   state management. States are integers registered before use and the machine
//   tracks current/previous state, enter/exit edge flags, and a per-state frame
//   counter. Designed for NPC AI (idle/patrol/attack), menus, and any other
//   logic that follows a discrete set of named modes.
//
// Key invariants:
//   - State IDs are non-negative integers in [0, RT_STATE_MAX-1]. The states
//     array is a flat bitset of 256 bytes (1 byte per ID), so registration and
//     lookup are O(1) with no allocations.
//   - A state must be registered with rt_statemachine_add_state() before it
//     can be used as a transition target or initial state. Registering the same
//     ID twice is a no-op (returns 0).
//   - just_entered and just_exited are edge flags: they are set to 1 on the
//     frame a transition occurs and remain 1 until rt_statemachine_clear_flags()
//     is called. Callers are responsible for clearing them each frame.
//   - rt_statemachine_update() increments frames_in_state by 1. It must be
//     called exactly once per frame while the machine is in a valid state.
//   - Transitioning to the current state is a no-op (returns 1, no flag set).
//
// Ownership/Lifetime:
//   - StateMachine objects are GC-managed via rt_obj_new_i64. The GC reclaims
//     them automatically; rt_statemachine_destroy() releases one reference
//     and frees the object when it was the last reference.
//
// Links: src/runtime/game/rt_statemachine.h (public API, with full
//        per-function documentation), docs/zannalib/game.md (StateMachine)
//
//===----------------------------------------------------------------------===//

#include "rt_statemachine.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/// @brief Mutable state owned by a StateMachine runtime object.
struct rt_statemachine_impl {
    int64_t current_state;       ///< Current state ID (-1 if none).
    int64_t previous_state;      ///< Previous state ID (-1 if none).
    int64_t frames_in_state;     ///< Frames since entering current state.
    int8_t just_entered;         ///< Flag: just entered new state.
    int8_t just_exited;          ///< Flag: just exited previous state.
    int8_t states[RT_STATE_MAX]; ///< Registered states (1 = exists).
    int64_t state_count;         ///< Number of registered states.
};

/// @brief Safe-cast a handle to the StateMachine impl, trapping @p api on a
///        class-id mismatch.
/// @param sm Borrowed candidate StateMachine handle.
/// @param api Trap message identifying the calling API.
/// @return Borrowed implementation pointer, or `NULL` when @p sm is `NULL`.
static rt_statemachine checked_statemachine(rt_statemachine sm, const char *api) {
    if (!sm)
        return NULL;
    if (rt_obj_class_id(sm) != RT_STATEMACHINE_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return sm;
}

/// @brief Create an uninitialized state machine with no registered states.
/// @return Owned StateMachine handle, or `NULL` if allocation fails.
rt_statemachine rt_statemachine_new(void) {
    rt_statemachine sm =
        rt_obj_new_i64(RT_STATEMACHINE_CLASS_ID, sizeof(struct rt_statemachine_impl));
    if (!sm)
        return NULL;

    sm->current_state = -1;
    sm->previous_state = -1;
    sm->frames_in_state = 0;
    sm->just_entered = 0;
    sm->just_exited = 0;
    sm->state_count = 0;
    memset(sm->states, 0, sizeof(sm->states));

    return sm;
}

/// @brief Release one owned StateMachine reference.
/// @param sm Owned handle to release; `NULL` is ignored.
void rt_statemachine_destroy(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.Destroy: expected Zanna.Game.StateMachine");
    if (sm && rt_obj_release_check0(sm))
        rt_obj_free(sm);
}

/// @brief Register a state ID as valid; the machine can only transition to known states.
/// @details Out-of-range IDs trap, while registering an existing ID is a
///          harmless false result.
/// @param sm Borrowed StateMachine handle.
/// @param state_id Candidate ID in `[0, RT_STATE_MAX)`.
/// @return `1` when newly registered; `0` for null or duplicate input.
int8_t rt_statemachine_add_state(rt_statemachine sm, int64_t state_id) {
    sm = checked_statemachine(sm, "StateMachine.AddState: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    if (state_id < 0 || state_id >= RT_STATE_MAX) {
        rt_trap("StateMachine.AddState: state_id out of range [0, RT_STATE_MAX-1]");
        return 0;
    }
    if (sm->states[state_id])
        return 0; // Already exists

    sm->states[state_id] = 1;
    sm->state_count++;
    return 1;
}

/// @brief Set the initial state and reset all transition tracking flags.
/// @details The target must already be registered. Success clears the previous
///          state, resets its frame counter, raises the entered edge, and
///          clears the exited edge.
/// @param sm Borrowed StateMachine handle.
/// @param state_id Registered initial state ID.
/// @return `1` on success; `0` for null, out-of-range, or unregistered input.
int8_t rt_statemachine_set_initial(rt_statemachine sm, int64_t state_id) {
    sm = checked_statemachine(sm, "StateMachine.SetInitial: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    if (state_id < 0 || state_id >= RT_STATE_MAX)
        return 0;
    if (!sm->states[state_id])
        return 0;

    sm->current_state = state_id;
    sm->previous_state = -1;
    sm->frames_in_state = 0;
    sm->just_entered = 1;
    sm->just_exited = 0;
    return 1;
}

/// @brief Return the ID of the currently active state, or -1 if uninitialized.
/// @param sm Borrowed StateMachine handle.
/// @return Current state ID, or `-1` for null/uninitialized state.
int64_t rt_statemachine_current(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.Current: expected Zanna.Game.StateMachine");
    if (!sm)
        return -1;
    return sm->current_state;
}

/// @brief Return the ID of the state that was active before the last transition.
/// @param sm Borrowed StateMachine handle.
/// @return Previous state ID, or `-1` when none exists.
int64_t rt_statemachine_previous(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.Previous: expected Zanna.Game.StateMachine");
    if (!sm)
        return -1;
    return sm->previous_state;
}

/// @brief Check whether the machine is currently in the given state.
/// @details This compares directly with the stored state sentinel, so `-1`
///          matches a newly created or otherwise uninitialized machine.
/// @param sm Borrowed StateMachine handle.
/// @param state_id Candidate state ID or sentinel.
/// @return `1` for an exact match; otherwise `0`.
int8_t rt_statemachine_is_state(rt_statemachine sm, int64_t state_id) {
    sm = checked_statemachine(sm, "StateMachine.IsState: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    return sm->current_state == state_id ? 1 : 0;
}

/// @brief Transition to a new state, updating previous/entered/exited flags.
/// @details The target must be registered. A transition to the already-current
///          state succeeds as a no-op and leaves existing flags/counters
///          untouched. Entering from the uninitialized sentinel does not raise
///          the exited edge.
/// @param sm Borrowed StateMachine handle.
/// @param state_id Registered target state ID.
/// @return `1` for a successful transition or same-state no-op; `0` for an
///         invalid or unregistered target.
int8_t rt_statemachine_transition(rt_statemachine sm, int64_t state_id) {
    sm = checked_statemachine(sm, "StateMachine.Transition: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    if (state_id < 0 || state_id >= RT_STATE_MAX)
        return 0;
    if (!sm->states[state_id])
        return 0;
    if (sm->current_state == state_id)
        return 1; // Already in this state, no-op

    sm->previous_state = sm->current_state;
    sm->current_state = state_id;
    sm->frames_in_state = 0;
    sm->just_entered = 1;
    sm->just_exited = (sm->previous_state >= 0) ? 1 : 0;
    return 1;
}

/// @brief Check whether the current state was entered this frame (one-shot flag).
/// @param sm Borrowed StateMachine handle.
/// @return `1` until the caller clears a pending entered edge; otherwise `0`.
int8_t rt_statemachine_just_entered(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.JustEntered: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    return sm->just_entered;
}

/// @brief Check whether a state was exited this frame (one-shot flag).
/// @param sm Borrowed StateMachine handle.
/// @return `1` until the caller clears a pending exited edge; otherwise `0`.
int8_t rt_statemachine_just_exited(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.JustExited: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    return sm->just_exited;
}

/// @brief Reset the just_entered and just_exited one-shot flags after checking them.
/// @param sm Borrowed StateMachine handle.
void rt_statemachine_clear_flags(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.ClearFlags: expected Zanna.Game.StateMachine");
    if (!sm)
        return;
    sm->just_entered = 0;
    sm->just_exited = 0;
}

/// @brief Return how many frames have elapsed since entering the current state.
/// @param sm Borrowed StateMachine handle.
/// @return Saturating update count for the current state, or `0` for null.
int64_t rt_statemachine_frames_in_state(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.FramesInState: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    return sm->frames_in_state;
}

/// @brief Increment the current state's frame counter by one tick.
/// @details Uninitialized machines do nothing and the counter saturates at
///          @ref INT64_MAX rather than overflowing.
/// @param sm Borrowed StateMachine handle.
void rt_statemachine_update(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.Update: expected Zanna.Game.StateMachine");
    if (!sm)
        return;
    if (sm->current_state >= 0) {
        if (sm->frames_in_state < INT64_MAX)
            sm->frames_in_state++;
    }
}

/// @brief Check whether a state ID has been registered with add_state.
/// @param sm Borrowed StateMachine handle.
/// @param state_id Candidate ID.
/// @return `1` for a registered in-range ID; otherwise `0`.
int8_t rt_statemachine_has_state(rt_statemachine sm, int64_t state_id) {
    sm = checked_statemachine(sm, "StateMachine.HasState: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    if (state_id < 0 || state_id >= RT_STATE_MAX)
        return 0;
    return sm->states[state_id];
}

/// @brief Return the count of elements in the statemachine.
/// @param sm Borrowed StateMachine handle.
/// @return Number of distinct registered IDs, or `0` for null.
int64_t rt_statemachine_state_count(rt_statemachine sm) {
    sm = checked_statemachine(sm, "StateMachine.StateCount: expected Zanna.Game.StateMachine");
    if (!sm)
        return 0;
    return sm->state_count;
}
