//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_animstate.h
// Purpose: Animation state machine combining StateMachine state tracking with
//          SpriteAnimation frame playback. Each state maps to an animation clip
//          (frame range + duration + loop flag). Transitions automatically
//          reconfigure the animation to play the new state's clip.
//
// Key invariants:
//   - Maximum 32 animation states per machine.
//   - Maximum 8 frame events per state and 8 fired IDs per update.
//   - State IDs must be non-negative integers.
//   - Update() must be called once per frame to advance both state and animation.
//   - JustEntered/JustExited flags latch until ClearFlags() is called.
//   - Frame-based timing (not millisecond-based) matching existing game helpers.
//   - Inclusive clips support forward, reverse, looped, and one-shot playback.
//
// Ownership/Lifetime:
//   - GC-managed via rt_obj_new_i64; no finalizer needed (no retained refs).
//   - Named-state text is copied into a fixed 31-byte C-string field.
//   - PollEvents returns an owned immutable copy.
//
// Links: rt_animstate.c (implementation),
//        rt_statemachine.h (state tracking pattern),
//        rt_spriteanim.h (animation playback pattern)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Fixed-capacity frame-based animation state machine API.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for AnimStateMachine objects.
#define RT_ANIMSTATE_CLASS_ID INT64_C(-0x51020C)

//=========================================================================
// AnimStateMachine Creation
//=========================================================================

/// @brief Create a new animation state machine.
/// @return Owned empty AnimStateMachine object, or NULL on allocation.
void *rt_animstate_new(void);

//=========================================================================
// State/Clip Definition
//=========================================================================

/// @brief Register a state with its animation clip parameters.
/// @details Redefinition clears the state's frame events and restarts playback
///          when that clip is active. A 33rd distinct state traps.
/// @param asm_ Borrowed AnimStateMachine object.
/// @param state_id Non-negative integer state identifier.
/// @param start_frame Inclusive first frame; negative clamps to zero.
/// @param end_frame Inclusive last frame; negative clamps to zero and values
///        below start select reverse playback.
/// @param frame_duration Updates per animation frame; non-positive becomes one.
/// @param loop 1 to loop the clip, 0 for one-shot.
void rt_animstate_add_state(void *asm_,
                            int64_t state_id,
                            int64_t start_frame,
                            int64_t end_frame,
                            int64_t frame_duration,
                            int8_t loop);

/// @brief Set the initial state (must have been added).
/// @param asm_ Borrowed machine.
/// @param state_id Registered state ID.
/// @return 1 on success, 0 if state not found.
int8_t rt_animstate_set_initial(void *asm_, int64_t state_id);

//=========================================================================
// Transitions & Update
//=========================================================================

/// @brief Transition to a new state, reconfiguring the animation clip.
/// @param asm_ Borrowed machine.
/// @param state_id Registered target state ID.
/// @return 1 on success. Transitioning to the current state is a successful no-op.
///         Returns 0 only if the state is not registered.
int8_t rt_animstate_transition(void *asm_, int64_t state_id);

/// @brief Advance one frame. Call once per game loop iteration.
/// @details Clears the previous update's events, saturating-increments state
///          age, advances playback by duration, and detects crossed events.
/// @param asm_ Borrowed machine.
void rt_animstate_update(void *asm_);

/// @brief Clear JustEntered / JustExited edge flags.
/// @param asm_ Borrowed machine.
void rt_animstate_clear_flags(void *asm_);

//=========================================================================
// Properties
//=========================================================================

/// @brief Current state ID.
/// @param asm_ Borrowed machine.
/// @return Current state ID, or -1 when absent/invalid.
int64_t rt_animstate_current_state(void *asm_);

/// @brief Previous state ID (-1 if no transition has occurred).
/// @param asm_ Borrowed machine.
/// @return Previous state ID, or -1 when absent/invalid.
int64_t rt_animstate_previous_state(void *asm_);

/// @brief 1 if a transition occurred since last ClearFlags().
/// @param asm_ Borrowed machine.
/// @return Latched entered flag, or zero on invalid input.
int8_t rt_animstate_just_entered(void *asm_);

/// @brief 1 if the previous state was exited since last ClearFlags().
/// @param asm_ Borrowed machine.
/// @return Latched exited flag, or zero on invalid input.
int8_t rt_animstate_just_exited(void *asm_);

/// @brief Frames spent in the current state.
/// @param asm_ Borrowed machine.
/// @return Saturating update count, or zero on invalid input.
int64_t rt_animstate_frames_in_state(void *asm_);

/// @brief Current animation frame index.
/// @param asm_ Borrowed machine.
/// @return Absolute sprite-frame index, or zero on invalid input.
int64_t rt_animstate_current_frame(void *asm_);

/// @brief 1 if the current clip is a one-shot that has finished.
/// @param asm_ Borrowed machine.
/// @return Completion flag, or zero on invalid input.
int8_t rt_animstate_is_anim_finished(void *asm_);

/// @brief Animation progress 0-100 within the current clip.
/// @param asm_ Borrowed machine.
/// @return Truncated absolute frame progress; zero without an active clip.
int64_t rt_animstate_progress(void *asm_);

/// @brief Add a named animation state (auto-assigns integer ID).
/// @details Uses the lowest unused ID and copies at most 31 C-string-visible
///          name bytes.
/// @param asm_ Borrowed machine.
/// @param name Borrowed runtime string handle.
/// @param start Inclusive start frame.
/// @param end Inclusive end frame.
/// @param dur Updates per frame.
/// @param loop Nonzero to loop.
void rt_animstate_add_named(
    void *asm_, void *name, int64_t start, int64_t end, int64_t dur, int8_t loop);

/// @brief Transition to a state by name.
/// @param asm_ Borrowed machine.
/// @param name Borrowed runtime string matched case-sensitively.
void rt_animstate_play(void *asm_, void *name);

/// @brief Get the name of the current state. Returns "" if no name.
/// @param asm_ Borrowed machine.
/// @return Owned copied runtime string.
void *rt_animstate_current_name(void *asm_);

/// @brief Set a frame event (fires when animation reaches this frame).
/// @param asm_ Borrowed machine.
/// @param frame Absolute frame, or negative to disable.
void rt_animstate_set_event_frame(void *asm_, int64_t frame);

/// @brief Check and clear the event flag. Returns 1 if event fired since last check.
/// @param asm_ Borrowed machine.
/// @return One when consumed; otherwise zero.
int8_t rt_animstate_event_fired(void *asm_);

/// @brief Add a frame-keyed event to a state. Returns 1 on success, 0 if the state is missing or
/// full.
/// @param asm_ Borrowed machine.
/// @param state_id Registered state ID.
/// @param frame Frame within the inclusive clip range.
/// @param event_id Application-defined signed ID.
/// @return `1` when the event was added; `0` when the state is missing or full.
int8_t rt_animstate_add_event(void *asm_, int64_t state_id, int64_t frame, int64_t event_id);

/// @brief Remove all frame-keyed events from a state.
/// @param asm_ Borrowed machine.
/// @param state_id Registered state ID.
void rt_animstate_clear_events(void *asm_, int64_t state_id);

/// @brief Return how many multi-events fired during the most recent Update().
/// @param asm_ Borrowed machine.
/// @return Count from zero through eight.
int64_t rt_animstate_events_fired_count(void *asm_);

/// @brief Return the event id at index from the most recent Update(), or 0 if invalid.
/// @param asm_ Borrowed machine.
/// @param index Zero-based fired-event index.
/// @return Stored ID, or zero for invalid input/index.
int64_t rt_animstate_event_fired_id(void *asm_, int64_t index);

/// @brief Snapshot event IDs fired by the most recent Update().
/// @details Returns an immutable Zanna.Game.AnimationEventBatch whose contents
///          are independent from subsequent calls to Update(), Transition(), or
///          ClearEvents(). This is the composable replacement for reading
///          EventsFiredCount and EventFiredId from mutable state.
/// @param asm_ AnimStateMachine object.
/// @return New Zanna.Game.AnimationEventBatch object, or NULL on allocation failure.
void *rt_animstate_poll_events(void *asm_);

#ifdef __cplusplus
}
#endif
