//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_animstate.c
// Purpose: Animation state machine that combines state tracking with frame-based
//          animation playback. Each registered state maps to a clip (frame range,
//          duration, loop flag). On transition the internal animation is
//          reconfigured and reset automatically.
//
// Key invariants:
//   - Clip table is a fixed-size array (max 32 states). States are looked up by
//     linear scan; the table is small enough that this is faster than hashing.
//   - Each clip has up to 8 frame events and one update exposes up to 8 fired IDs.
//   - Animation frame counter and state frame counter are both advanced by
//     Update(). The state counter increments unconditionally; the animation
//     counter follows the SpriteAnimation frame-duration convention.
//   - Edge flags (just_entered, just_exited) latch until ClearFlags().
//   - Frame ranges may run forward or reverse and may loop.
//
// Ownership/Lifetime:
//   - GC-managed (rt_obj_new_i64). No retained references — no finalizer.
//   - Named states copy at most 31 C-string bytes into fixed clip storage.
//   - PollEvents returns an independent owned immutable batch snapshot.
//
// Links: rt_animstate.h (public API)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Fixed-capacity animation state machine, playback, and frame events.

#include "rt_animstate.h"
#include "rt_animation_events.h"
#include "rt_object.h"
#include "rt_string.h"

#include <limits.h>
#include <string.h>

#include "rt_trap.h"

//=============================================================================
// Internal Types
//=============================================================================

#define ANIMSTATE_MAX_CLIPS 32
#define ANIMSTATE_MAX_EVENTS_PER_STATE 8
#define ANIMSTATE_MAX_FIRED_EVENTS 8

/// @brief One state-to-clip binding and its per-loop event definitions.
typedef struct {
    int64_t state_id;
    int64_t start_frame;
    int64_t end_frame;
    int64_t frame_duration; // frames per animation frame
    int8_t loop;
    int8_t valid;
    char name[32]; // String name for named state lookup (null-terminated)
    int64_t event_frames[ANIMSTATE_MAX_EVENTS_PER_STATE];
    int64_t event_ids[ANIMSTATE_MAX_EVENTS_PER_STATE];
    int8_t event_count;
    uint16_t event_fired_mask;
} anim_clip_t;

/// @brief Native payload backing a Zanna.Game.AnimStateMachine object.
typedef struct {
    anim_clip_t clips[ANIMSTATE_MAX_CLIPS];
    int32_t clip_count;

    // State tracking (mirrors StateMachine)
    int64_t current_state;
    int64_t previous_state;
    int8_t just_entered;
    int8_t just_exited;
    int64_t frames_in_state;

    // Animation playback (mirrors SpriteAnimation)
    int64_t current_frame;
    int64_t frame_counter; // counts up to frame_duration
    int8_t anim_finished;
    int8_t playing;

    // Current clip cache
    int32_t active_clip_idx; // -1 if none

    // Frame event (fires when animation reaches a specific frame)
    int64_t event_frame;      // Frame to trigger on (-1 = disabled)
    int64_t last_event_frame; // Last frame that fired, prevents same-frame repeats.
    int8_t event_triggered;   // Set to 1 when event_frame is reached
    int64_t events_fired_ids[ANIMSTATE_MAX_FIRED_EVENTS];
    int64_t events_fired_count;
} animstate_impl;

//=============================================================================
// Helpers
//=============================================================================

/// @brief Reinterpret an opaque handle without validation.
/// @param asm_ Opaque pointer; may be NULL.
/// @return Same pointer cast to implementation type.
static animstate_impl *get(void *asm_) {
    return (animstate_impl *)asm_;
}

/// @brief Safe-cast a handle to the AnimStateMachine impl, trapping @p api on
///        a class-id mismatch.
/// @param asm_ Borrowed candidate handle; may be NULL.
/// @param api Borrowed mismatch diagnostic.
/// @return Valid implementation pointer, or NULL for null/mismatch.
static animstate_impl *checked_animstate(void *asm_, const char *api) {
    if (!asm_)
        return NULL;
    if (rt_obj_class_id(asm_) != RT_ANIMSTATE_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return get(asm_);
}

/// @brief Find the index of the valid clip bound to @p state_id, or -1.
/// @param a Borrowed valid machine.
/// @param state_id Application-defined state ID.
/// @return Clip-table index, or -1 when absent.
static int find_clip(animstate_impl *a, int64_t state_id) {
    for (int i = 0; i < a->clip_count; i++) {
        if (a->clips[i].valid && a->clips[i].state_id == state_id)
            return i;
    }
    return -1;
}

/// @brief True if @p frame is inside a clip's inclusive frame range.
/// @param c Borrowed clip; may be NULL.
/// @param frame Absolute sprite-frame index.
/// @return One when within forward or reverse inclusive endpoints.
static int8_t animstate_frame_in_clip(const anim_clip_t *c, int64_t frame) {
    if (!c)
        return 0;
    if (c->start_frame <= c->end_frame)
        return frame >= c->start_frame && frame <= c->end_frame;
    return frame <= c->start_frame && frame >= c->end_frame;
}

/// @brief Make @p clip_idx the active clip, resetting frame/event playback
///        state. A negative index stops playback.
/// @param a Mutable machine.
/// @param clip_idx Valid clip index, or negative to stop.
static void apply_clip(animstate_impl *a, int clip_idx) {
    a->active_clip_idx = clip_idx;
    if (clip_idx < 0) {
        a->playing = 0;
        return;
    }
    anim_clip_t *c = &a->clips[clip_idx];
    a->current_frame = c->start_frame;
    a->frame_counter = 0;
    a->anim_finished = 0;
    a->playing = 1;
    a->event_triggered = 0;
    a->last_event_frame = -1;
    a->events_fired_count = 0;
    memset(a->events_fired_ids, 0, sizeof(a->events_fired_ids));
    c->event_fired_mask = 0;
}

/// @brief Test whether playback crossed @p event_frame between @p prev_frame
///        and @p current_frame this step.
/// @details Handles exact landing, plus loop wrap-around for both
///          forward (start<=end) and reverse (start>end) clips.
/// @param start Inclusive clip start.
/// @param end Inclusive clip end.
/// @param loop Nonzero when wraparound is enabled.
/// @param prev_frame Frame before the update.
/// @param current_frame Frame after the update.
/// @param event_frame Candidate event frame.
/// @return One when the update crossed or newly landed on the event.
static int8_t animstate_crossed_frame(int64_t start,
                                      int64_t end,
                                      int8_t loop,
                                      int64_t prev_frame,
                                      int64_t current_frame,
                                      int64_t event_frame) {
    if (current_frame == event_frame && prev_frame != current_frame)
        return 1;
    if (!loop)
        return 0;
    if (start <= end && current_frame < prev_frame)
        return (event_frame > prev_frame && event_frame <= end) ||
               (event_frame >= start && event_frame <= current_frame);
    if (start > end && current_frame > prev_frame)
        return (event_frame < prev_frame && event_frame >= end) ||
               (event_frame <= start && event_frame >= current_frame);
    return 0;
}

/// @brief Legacy single-event check: set event_triggered when the current
///        frame first lands on the clip's event_frame (debounced).
/// @param a Mutable machine.
/// @param prev_frame Frame before the update.
static void animstate_maybe_fire_event(animstate_impl *a, int64_t prev_frame) {
    if (a->event_frame < 0)
        return;
    if (a->current_frame != a->event_frame) {
        a->last_event_frame = -1;
        return;
    }
    if (prev_frame == a->current_frame || a->last_event_frame == a->current_frame)
        return;
    a->event_triggered = 1;
    a->last_event_frame = a->current_frame;
}

/// @brief Multi-event check: collect all of clip @p c's event ids whose frames
///        were crossed this step into a->events_fired_ids.
/// @details Per-clip event_fired_mask debounces repeats; the mask resets on
///          loop wrap so events fire again each cycle.
/// @param a Mutable machine.
/// @param c Mutable active clip.
/// @param prev_frame Frame before the update.
static void animstate_maybe_fire_events(animstate_impl *a, anim_clip_t *c, int64_t prev_frame) {
    if (!a || !c)
        return;
    a->events_fired_count = 0;
    memset(a->events_fired_ids, 0, sizeof(a->events_fired_ids));
    if ((c->start_frame <= c->end_frame && a->current_frame < prev_frame) ||
        (c->start_frame > c->end_frame && a->current_frame > prev_frame))
        c->event_fired_mask = 0;

    for (int8_t i = 0; i < c->event_count && a->events_fired_count < ANIMSTATE_MAX_FIRED_EVENTS;
         i++) {
        uint16_t mask = (uint16_t)(1u << (uint8_t)i);
        if ((c->event_fired_mask & mask) != 0)
            continue;
        if (!animstate_crossed_frame(c->start_frame,
                                     c->end_frame,
                                     c->loop,
                                     prev_frame,
                                     a->current_frame,
                                     c->event_frames[i]))
            continue;
        c->event_fired_mask |= mask;
        a->events_fired_ids[a->events_fired_count++] = c->event_ids[i];
    }
}

/// @brief Integer percentage (value*100/total) clamped to [0, INT64_MAX];
///        returns 0 for non-positive inputs.
/// @param value Completed units.
/// @param total Total units.
/// @return Truncated percentage clamped to 0–100.
static int64_t animstate_percent_i64(int64_t value, int64_t total) {
    if (value <= 0 || total <= 0)
        return 0;
    long double scaled = ((long double)value * 100.0L) / (long double)total;
    int64_t pct = scaled >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scaled;
    return pct > 100 ? 100 : pct;
}

//=============================================================================
// Creation
//=============================================================================

/// @brief Create an empty animation state machine.
/// @details Initializes all sentinel state IDs/clip indices/event frames to -1;
///          fixed tables and counters begin zeroed.
/// @return Owned runtime object, or NULL on allocation failure.
void *rt_animstate_new(void) {
    animstate_impl *a =
        (animstate_impl *)rt_obj_new_i64(RT_ANIMSTATE_CLASS_ID, (int64_t)sizeof(animstate_impl));
    if (!a)
        return NULL;
    memset(a, 0, sizeof(animstate_impl));
    a->current_state = -1;
    a->previous_state = -1;
    a->active_clip_idx = -1;
    a->event_frame = -1;
    a->last_event_frame = -1;
    return a;
}

//=============================================================================
// State/Clip Definition
//=============================================================================

/// @brief Add or redefine a numeric state and its playback clip.
/// @details Rejects negative state IDs, clamps negative frame endpoints to
///          zero, and normalizes non-positive frame duration to one update per
///          animation frame. A new 33rd state traps. Redefinition clears that
///          clip's events; redefining the active clip restarts playback without
///          changing state IDs or transition-edge flags.
/// @param asm_ Borrowed machine.
/// @param state_id Nonnegative application-defined state ID.
/// @param start_frame Inclusive start frame.
/// @param end_frame Inclusive end frame; may precede start for reverse playback.
/// @param frame_duration Updates per animation-frame step.
/// @param loop Nonzero to wrap at the end.
void rt_animstate_add_state(void *asm_,
                            int64_t state_id,
                            int64_t start_frame,
                            int64_t end_frame,
                            int64_t frame_duration,
                            int8_t loop) {
    animstate_impl *a =
        checked_animstate(asm_, "AnimStateMachine.AddState: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return;
    if (state_id < 0)
        return;
    if (start_frame < 0)
        start_frame = 0;
    if (end_frame < 0)
        end_frame = 0;

    // Overwrite existing
    int idx = find_clip(a, state_id);
    if (idx < 0) {
        if (a->clip_count >= ANIMSTATE_MAX_CLIPS) {
            rt_trap("AnimStateMachine.AddState: limit exceeded (32)");
            return;
        }
        idx = a->clip_count++;
    }

    anim_clip_t *c = &a->clips[idx];
    c->state_id = state_id;
    c->start_frame = start_frame;
    c->end_frame = end_frame;
    c->frame_duration = frame_duration > 0 ? frame_duration : 1;
    c->loop = loop;
    c->valid = 1;
    c->event_count = 0;
    c->event_fired_mask = 0;
    memset(c->event_frames, 0, sizeof(c->event_frames));
    memset(c->event_ids, 0, sizeof(c->event_ids));

    // Redefining the currently active clip (e.g. a hot-reload) must restart
    // playback so current_frame, timing, direction, and events track the new
    // range. Otherwise the machine keeps running from a frame outside the new
    // clip and reports incoherent progress until it happens to re-enter the
    // range (VDOC-275). This is a redefinition, not a transition, so the
    // state/edge flags are intentionally left unchanged.
    if (idx == a->active_clip_idx)
        apply_clip(a, idx);
}

/// @brief Set the starting state and reset all transition flags to initial values.
/// @details Requires a previously defined nonnegative state, resets playback
///          and event buffers, sets previous state to -1, and latches
///          `just_entered`.
/// @param asm_ Borrowed machine.
/// @param state_id Defined state ID.
/// @return One on success; zero for invalid machine/state.
int8_t rt_animstate_set_initial(void *asm_, int64_t state_id) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.SetInitial: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return 0;
    if (state_id < 0)
        return 0;
    int idx = find_clip(a, state_id);
    if (idx < 0)
        return 0;

    a->current_state = state_id;
    a->previous_state = -1;
    a->just_entered = 1;
    a->just_exited = 0;
    a->frames_in_state = 0;
    a->event_triggered = 0;
    a->last_event_frame = -1;
    apply_clip(a, idx);
    return 1;
}

//=============================================================================
// Transitions & Update
//=============================================================================

/// @brief Transition to a new state, setting entered/exited flags and resetting the frame counter.
/// @details A transition to the already-current state succeeds without changing
///          flags or playback. A different defined state becomes current,
///          previous records the old ID, edge flags latch, and clip/event
///          playback restarts.
/// @param asm_ Borrowed machine.
/// @param state_id Defined target state ID.
/// @return One on success/no-op same-state transition; zero when unavailable.
int8_t rt_animstate_transition(void *asm_, int64_t state_id) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.Transition: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return 0;
    if (state_id < 0)
        return 0;

    // No-op if already in this state
    if (a->current_state == state_id)
        return 1;

    int idx = find_clip(a, state_id);
    if (idx < 0)
        return 0;

    a->previous_state = a->current_state;
    a->current_state = state_id;
    a->just_entered = 1;
    a->just_exited = a->previous_state >= 0 ? 1 : 0;
    a->frames_in_state = 0;
    apply_clip(a, idx);
    return 1;
}

/// @brief Update the animstate state (called per frame/tick).
/// @details Clears per-update event outputs, saturating-increments
///          frames-in-state, and advances the active frame once its duration
///          counter expires. Supports inclusive forward/reverse ranges,
///          looping, non-loop completion, legacy single events, and up to eight
///          clip events crossed during the step.
/// @param asm_ Borrowed machine.
void rt_animstate_update(void *asm_) {
    animstate_impl *a =
        checked_animstate(asm_, "AnimStateMachine.Update: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return;
    if (a->current_state < 0)
        return;
    a->event_triggered = 0;
    a->events_fired_count = 0;
    memset(a->events_fired_ids, 0, sizeof(a->events_fired_ids));

    // Advance state frame counter
    if (a->frames_in_state < INT64_MAX)
        a->frames_in_state++;

    // Advance animation
    if (!a->playing || a->anim_finished || a->active_clip_idx < 0)
        return;

    anim_clip_t *c = &a->clips[a->active_clip_idx];
    int64_t prev_frame = a->current_frame;
    a->frame_counter++;
    if (a->frame_counter >= c->frame_duration) {
        a->frame_counter = 0;

        if (c->start_frame <= c->end_frame) {
            // Forward clip
            if (a->current_frame < c->end_frame) {
                a->current_frame++;
            } else if (c->loop) {
                a->current_frame = c->start_frame;
            } else {
                a->anim_finished = 1;
            }
        } else {
            // Reverse clip (start > end)
            if (a->current_frame > c->end_frame) {
                a->current_frame--;
            } else if (c->loop) {
                a->current_frame = c->start_frame;
            } else {
                a->anim_finished = 1;
            }
        }
    }

    animstate_maybe_fire_event(a, prev_frame);
    animstate_maybe_fire_events(a, c, prev_frame);
}

/// @brief Reset the just_entered and just_exited one-shot flags (call once per frame after
/// checking).
/// @param asm_ Borrowed machine.
void rt_animstate_clear_flags(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.ClearFlags: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return;
    a->just_entered = 0;
    a->just_exited = 0;
}

//=============================================================================
// Properties
//=============================================================================

/// @brief Return the ID of the currently active state.
/// @param asm_ Borrowed machine.
/// @return Current ID, or -1 for no state/invalid input.
int64_t rt_animstate_current_state(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.CurrentState: expected Zanna.Game.AnimStateMachine");
    return a ? a->current_state : -1;
}

/// @brief Return the ID of the state that was active before the last transition.
/// @param asm_ Borrowed machine.
/// @return Previous ID, or -1 before a transition/for invalid input.
int64_t rt_animstate_previous_state(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.PreviousState: expected Zanna.Game.AnimStateMachine");
    return a ? a->previous_state : -1;
}

/// @brief Check whether the current state was entered this frame (one-shot flag).
/// @param asm_ Borrowed machine.
/// @return Latched flag, or zero for invalid input.
int8_t rt_animstate_just_entered(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.JustEntered: expected Zanna.Game.AnimStateMachine");
    return a ? a->just_entered : 0;
}

/// @brief Check whether the previous state was exited this frame (one-shot flag).
/// @param asm_ Borrowed machine.
/// @return Latched flag, or zero for invalid input.
int8_t rt_animstate_just_exited(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.JustExited: expected Zanna.Game.AnimStateMachine");
    return a ? a->just_exited : 0;
}

/// @brief Return how many frames have elapsed since entering the current state.
/// @param asm_ Borrowed machine.
/// @return Saturating update count, or zero for invalid input.
int64_t rt_animstate_frames_in_state(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.FramesInState: expected Zanna.Game.AnimStateMachine");
    return a ? a->frames_in_state : 0;
}

/// @brief Return the current animation frame index within the active clip.
/// @param asm_ Borrowed machine.
/// @return Absolute sprite-frame index, or zero for invalid input.
int64_t rt_animstate_current_frame(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.CurrentFrame: expected Zanna.Game.AnimStateMachine");
    return a ? a->current_frame : 0;
}

/// @brief Check whether the current clip has reached its last frame (non-looping only).
/// @param asm_ Borrowed machine.
/// @return One after non-loop playback completes; otherwise zero.
int8_t rt_animstate_is_anim_finished(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.IsAnimFinished: expected Zanna.Game.AnimStateMachine");
    return a ? a->anim_finished : 0;
}

/// @brief Return active clip progress as an integer percentage.
/// @details Measures absolute frame displacement between inclusive endpoints;
///          zero-length clips report 100 and absent clips report zero.
/// @param asm_ Borrowed machine.
/// @return Truncated value from zero through 100.
int64_t rt_animstate_progress(void *asm_) {
    animstate_impl *a =
        checked_animstate(asm_, "AnimStateMachine.Progress: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return 0;
    if (a->active_clip_idx < 0)
        return 0;

    anim_clip_t *c = &a->clips[a->active_clip_idx];
    int64_t total = c->end_frame - c->start_frame;
    if (total == 0)
        return 100;
    int64_t elapsed = a->current_frame - c->start_frame;
    if (total < 0) {
        total = -total;
        elapsed = -elapsed;
    }
    return animstate_percent_i64(elapsed, total);
}

//=============================================================================
// Named State API
//=============================================================================

/// @brief Add a state addressable by a copied short name.
/// @details Chooses the lowest unused nonnegative numeric ID, delegates clip
///          validation/creation to @ref rt_animstate_add_state, then copies the
///          C-string-visible name prefix into a 32-byte field. Names therefore
///          truncate to 31 bytes and embedded NUL terminates early.
/// @param asm_ Borrowed machine.
/// @param name_str Borrowed runtime string handle.
/// @param start Inclusive start frame.
/// @param end Inclusive end frame.
/// @param dur Updates per animation-frame step.
/// @param loop Nonzero to loop.
void rt_animstate_add_named(
    void *asm_, void *name_str, int64_t start, int64_t end, int64_t dur, int8_t loop) {
    animstate_impl *a =
        checked_animstate(asm_, "AnimStateMachine.AddNamed: expected Zanna.Game.AnimStateMachine");
    if (!a || !name_str)
        return;
    // Allocate a genuinely unused state ID. The old code reused clip_count as the
    // id, which collided when a numeric state already owned that id: add_state then
    // overwrote that clip in place (no count bump) and the name was stranded in
    // clips[clip_count], a slot outside the searched prefix (VDOC-274). With at most
    // ANIMSTATE_MAX_CLIPS valid clips this scan always finds a free id within
    // [0, ANIMSTATE_MAX_CLIPS].
    int64_t id = 0;
    while (find_clip(a, id) >= 0)
        id++;

    rt_animstate_add_state(asm_, id, start, end, dur, loop);

    // Capture the actual clip index add_state used (it appends, so the new clip is
    // wherever find_clip locates it) and store the name there. A negative index
    // means add_state hit the 32-clip limit and added nothing.
    int idx = find_clip(a, id);
    if (idx < 0)
        return;
    const char *cname = rt_string_cstr((rt_string)name_str);
    if (cname) {
        strncpy(a->clips[idx].name, cname, 31);
        a->clips[idx].name[31] = '\0';
    }
}

/// @brief Transition to the first state with an equal stored C-string name.
/// @details Comparison is case-sensitive and scans only registered clip order.
///          Missing/null names are no-ops.
/// @param asm_ Borrowed machine.
/// @param name_str Borrowed runtime string handle.
void rt_animstate_play(void *asm_, void *name_str) {
    animstate_impl *a =
        checked_animstate(asm_, "AnimStateMachine.Play: expected Zanna.Game.AnimStateMachine");
    if (!a || !name_str)
        return;
    const char *cname = rt_string_cstr((rt_string)name_str);
    if (!cname)
        return;
    for (int32_t i = 0; i < a->clip_count; i++) {
        if (a->clips[i].valid && strcmp(a->clips[i].name, cname) == 0) {
            rt_animstate_transition(asm_, a->clips[i].state_id);
            return;
        }
    }
}

/// @brief Copy the active clip's stored short name to a runtime string.
/// @param asm_ Borrowed machine.
/// @return Owned copied name or shared empty string when no active clip exists.
void *rt_animstate_current_name(void *asm_) {
    animstate_impl *a =
        checked_animstate(asm_, "AnimStateMachine.StateName: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return (void *)rt_const_cstr("");
    if (a->active_clip_idx >= 0 && a->active_clip_idx < a->clip_count)
        return (void *)rt_const_cstr(a->clips[a->active_clip_idx].name);
    return (void *)rt_const_cstr("");
}

/// @brief Configure the legacy single event frame.
/// @details Any signed frame is stored; negative values disable firing. Existing
///          trigger/debounce state is cleared.
/// @param asm_ Borrowed machine.
/// @param frame Absolute frame index, or negative to disable.
void rt_animstate_set_event_frame(void *asm_, int64_t frame) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.SetEventFrame: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return;
    a->event_frame = frame;
    a->event_triggered = 0;
    a->last_event_frame = -1;
}

/// @brief Consume the legacy event-fired latch.
/// @param asm_ Borrowed machine.
/// @return One once after a trigger; otherwise zero.
int8_t rt_animstate_event_fired(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.EventFired: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return 0;
    if (a->event_triggered) {
        a->event_triggered = 0; // auto-clear
        return 1;
    }
    return 0;
}

/// @brief Add a multi-event marker to a defined clip.
/// @details The frame must lie within inclusive forward/reverse endpoints.
///          Appends in registration order up to eight events per state and
///          clears that slot's fired bit.
/// @param asm_ Borrowed machine.
/// @param state_id Defined state ID.
/// @param frame Absolute event frame.
/// @param event_id Application-defined event ID.
/// @return One when added; zero for invalid state/frame or full event table.
int8_t rt_animstate_add_event(void *asm_, int64_t state_id, int64_t frame, int64_t event_id) {
    animstate_impl *a =
        checked_animstate(asm_, "AnimStateMachine.AddEvent: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return 0;
    if (state_id < 0)
        return 0;
    int idx = find_clip(a, state_id);
    if (idx < 0)
        return 0;
    anim_clip_t *c = &a->clips[idx];
    if (!animstate_frame_in_clip(c, frame))
        return 0;
    if (c->event_count >= ANIMSTATE_MAX_EVENTS_PER_STATE)
        return 0;
    int8_t slot = c->event_count++;
    c->event_frames[slot] = frame;
    c->event_ids[slot] = event_id;
    c->event_fired_mask &= (uint16_t)~(1u << (uint8_t)slot);
    return 1;
}

/// @brief Remove every multi-event marker from a state.
/// @details Also clears current-update fired IDs when clearing the active clip.
///          The legacy single event frame is unaffected.
/// @param asm_ Borrowed machine.
/// @param state_id Defined state ID.
void rt_animstate_clear_events(void *asm_, int64_t state_id) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.ClearEvents: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return;
    if (state_id < 0)
        return;
    int idx = find_clip(a, state_id);
    if (idx < 0)
        return;
    anim_clip_t *c = &a->clips[idx];
    c->event_count = 0;
    c->event_fired_mask = 0;
    memset(c->event_frames, 0, sizeof(c->event_frames));
    memset(c->event_ids, 0, sizeof(c->event_ids));
    if (a->active_clip_idx == idx) {
        a->events_fired_count = 0;
        memset(a->events_fired_ids, 0, sizeof(a->events_fired_ids));
    }
}

/// @brief Return how many multi-events fired during the latest update.
/// @param asm_ Borrowed machine.
/// @return Count from zero through eight.
int64_t rt_animstate_events_fired_count(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.EventsFiredCount: expected Zanna.Game.AnimStateMachine");
    return a ? a->events_fired_count : 0;
}

/// @brief Read a fired multi-event ID by latest-update order.
/// @param asm_ Borrowed machine.
/// @param index Zero-based fired-event index.
/// @return Event ID, or zero for invalid input/index.
int64_t rt_animstate_event_fired_id(void *asm_, int64_t index) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.EventFiredId: expected Zanna.Game.AnimStateMachine");
    if (!a || index < 0 || index >= a->events_fired_count)
        return 0;
    return a->events_fired_ids[index];
}

/// @brief Snapshot multi-event IDs fired by the latest update.
/// @details Copies current fired IDs into an immutable independent batch.
///          Null/invalid machine yields an owned empty batch.
/// @param asm_ Borrowed machine.
/// @return Owned AnimationEventBatch, or NULL on snapshot allocation failure.
void *rt_animstate_poll_events(void *asm_) {
    animstate_impl *a = checked_animstate(
        asm_, "AnimStateMachine.PollEvents: expected Zanna.Game.AnimStateMachine");
    if (!a)
        return rt_animation_event_batch_from_ids(NULL, 0);
    return rt_animation_event_batch_from_ids(a->events_fired_ids, a->events_fired_count);
}
