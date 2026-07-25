//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_statemachine.h
/// @file
/// @brief Declares a bounded integer state machine with registration,
///        transition edges, and saturating per-state tick counts.
// Purpose: Finite state machine with up to RT_STATE_MAX registered states, tracking current
// state, frame count, and one-frame enter/exit transition flags.
//
// Key invariants:
//   - State IDs must be in [0, RT_STATE_MAX-1]; each ID may be registered once.
//   - Transition flags (just_entered/just_exited) are set on transition and persist until cleared.
//   - rt_statemachine_update must be called once per frame to advance the frame counter.
//   - Frame counters saturate at INT64_MAX instead of overflowing.
//   - Updates before initialization are no-ops; a registered state may be
//     entered with either set_initial or transition.
//
// Ownership/Lifetime:
//   - GC-managed via rt_obj_new_i64; explicit destroy releases the caller's reference.
//
// Links: src/runtime/game/rt_statemachine.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Maximum state capacity and exclusive upper bound for state IDs.
/// @details State IDs directly index the fixed registration table.
#define RT_STATE_MAX 256

/// @brief Runtime class identifier used to validate StateMachine handles.
#define RT_STATEMACHINE_CLASS_ID INT64_C(-0x51020D)

/// @brief Opaque handle to a StateMachine instance.
typedef struct rt_statemachine_impl *rt_statemachine;

/// @brief Allocates and initializes a new StateMachine with no registered
///   states.
/// @return Owned uninitialized StateMachine handle, or `NULL` if allocation
///         fails.
rt_statemachine rt_statemachine_new(void);

/// @brief Release one owned StateMachine reference.
/// @param sm Owned handle to release; `NULL` is ignored.
void rt_statemachine_destroy(rt_statemachine sm);

/// @brief Registers a new state in the state machine.
///
/// A state must be added before it can be used as a transition target or
/// set as the initial state.
/// Out-of-range IDs trigger a runtime trap; duplicate IDs are harmless no-ops.
/// @param sm Borrowed state machine handle.
/// @param state_id Unique integer identifier for the state, in the range
///   [0, RT_STATE_MAX - 1].
/// @return `1` when newly registered; `0` for null or duplicate input.
int8_t rt_statemachine_add_state(rt_statemachine sm, int64_t state_id);

/// @brief Designates which state the machine starts in.
///
/// The state must have been previously registered. Success resets the frame
/// counter and previous-state sentinel, raises entered, and clears exited.
/// @param sm The state machine to configure.
/// @param state_id The ID of the initial state.
/// @return `1` on success; `0` for null, out-of-range, or unregistered input.
int8_t rt_statemachine_set_initial(rt_statemachine sm, int64_t state_id);

/// @brief Retrieves the ID of the currently active state.
/// @param sm The state machine to query.
/// @return The current state ID, or -1 if no state has been set yet.
int64_t rt_statemachine_current(rt_statemachine sm);

/// @brief Retrieves the ID of the state that was active before the most
///   recent transition.
/// @param sm The state machine to query.
/// @return The previous state ID, or -1 if no transition has occurred.
int64_t rt_statemachine_previous(rt_statemachine sm);

/// @brief Tests whether the machine is currently in a specific state.
/// @details The comparison includes the `-1` uninitialized sentinel.
/// @param sm The state machine to query.
/// @param state_id The state ID or sentinel to compare against.
/// @return 1 if the current state matches @p state_id, 0 otherwise.
int8_t rt_statemachine_is_state(rt_statemachine sm, int64_t state_id);

/// @brief Transitions the machine to a new state.
///
/// Sets the just_entered and just_exited flags, updates the previous-state
/// record, and resets the frames-in-state counter to zero. The target state
/// must have been registered with rt_statemachine_add_state().
/// Transitioning to the current state returns success without changing flags
/// or counters. Entering from the uninitialized sentinel does not raise exited.
/// @param sm The state machine to modify.
/// @param state_id The ID of the state to transition into.
/// @return `1` on transition or same-state no-op; `0` for invalid targets.
int8_t rt_statemachine_transition(rt_statemachine sm, int64_t state_id);

/// @brief Queries whether a transition into the current state just occurred.
///
/// Returns 1 on the frame a transition was made, and continues to return 1
/// until rt_statemachine_clear_flags() is called.
/// @param sm The state machine to query.
/// @return 1 if the machine just entered the current state, 0 otherwise.
int8_t rt_statemachine_just_entered(rt_statemachine sm);

/// @brief Queries whether the machine just exited its previous state.
///
/// Returns 1 on the frame a transition was made, and continues to return 1
/// until rt_statemachine_clear_flags() is called.
/// @param sm The state machine to query.
/// @return 1 if the machine just left its previous state, 0 otherwise.
int8_t rt_statemachine_just_exited(rt_statemachine sm);

/// @brief Resets the just_entered and just_exited transition flags.
///
/// Should be called at the end of each frame to ensure edge flags are only
/// active for one frame cycle.
/// @param sm The state machine to modify.
void rt_statemachine_clear_flags(rt_statemachine sm);

/// @brief Retrieves the number of frames spent in the current state.
///
/// Incremented by rt_statemachine_update() each frame. Reset to zero on
/// every actual transition and saturated at `INT64_MAX`.
/// @param sm The state machine to query.
/// @return Number of frames since the last transition (or initial state
///   set).
int64_t rt_statemachine_frames_in_state(rt_statemachine sm);

/// @brief Advances the state machine by one frame, incrementing the
///   frames-in-state counter.
///
/// Call once per game frame when frame-based timing is desired. An
/// uninitialized machine is unchanged and the counter never overflows.
/// @param sm The state machine to update.
void rt_statemachine_update(rt_statemachine sm);

/// @brief Tests whether a state with the given ID has been registered.
/// @param sm The state machine to query.
/// @param state_id The state ID to look up.
/// @return 1 if a state with this ID exists, 0 otherwise.
int8_t rt_statemachine_has_state(rt_statemachine sm, int64_t state_id);

/// @brief Retrieves the total number of states registered in the machine.
/// @param sm The state machine to query.
/// @return The count of registered states, in [0, RT_STATE_MAX].
int64_t rt_statemachine_state_count(rt_statemachine sm);

#ifdef __cplusplus
}
#endif
