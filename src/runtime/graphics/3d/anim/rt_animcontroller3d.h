//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_animcontroller3d.h
// Purpose: Public API for AnimController3D — a high-level skeletal animation
//   state machine layered over AnimPlayer3D. Exposes named states with
//   per-state speed/looping, blended transitions, time-scheduled events,
//   root-motion extraction, and masked overlay layers.
//
// Key invariants:
//   - All entry points take opaque GC-managed controller handles (void *).
//   - Layer 0 is the base layer; higher layers are masked replace or additive overlays.
//   - Times are in seconds, blend weights in 0..1; bone_index -1 disables
//     root motion.
//
// Links: rt_animcontroller3d.c (implementation),
//   rt_skeleton3d.h (underlying AnimPlayer3D), docs/zannalib/graphics/animation.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for layered skeletal-animation state controllers.
/// @details An AnimController3D owns four internal animation players over one
///          retained Skeleton3D. Named states retain their Animation3D clips;
///          optional BlendTree3D and IKSolver3D inputs are retained while
///          attached. Layer zero is the full-weight base, while layers one
///          through three can replace or add bind-pose deltas over selected
///          bone subtrees.
///
///          Unless a function explicitly returns a borrowed pointer, returned
///          Vec3, Quat, Mat4, and string handles are newly created
///          GC-managed runtime objects. Invalid opaque handles are rejected by
///          the runtime's class checks; mutators then return failure or make no
///          change as documented.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an animation state controller bound to a Skeleton3D.
/// @details Retains the skeleton, allocates current and previous skinning
///          palettes, creates one AnimPlayer3D per layer, and initializes the
///          output to the bind pose. Layer zero starts at weight 1; overlay
///          layers start at weight 0.
/// @param[in] skeleton Borrowed Skeleton3D handle to retain for the
///                     controller's lifetime.
/// @return New GC-managed AnimController3D handle, or `NULL` after a runtime
///         trap if the handle is invalid or allocation fails.
void *rt_anim_controller3d_new(void *skeleton);

/// @brief Register a named Animation3D state.
/// @details State names are copied into bounded internal storage. An existing
///          canonical name returns its current index without replacing its
///          clip. New clips must have channels compatible with the bound
///          skeleton and are retained until controller finalization.
/// @param[in,out] controller AnimController3D to modify.
/// @param[in] name Nonempty runtime string used as the lookup key.
/// @param[in] animation Borrowed Animation3D handle to retain on success.
/// @return Existing or newly appended zero-based state index, or `-1` for an
///         invalid argument, incompatible clip, empty name, or allocation
///         failure.
int64_t rt_anim_controller3d_add_state(void *controller, rt_string name, void *animation);
/// @brief Define or update a directional transition between two named states.
/// @details A non-finite or negative duration becomes zero. A later
///          @ref rt_anim_controller3d_play uses this duration when switching
///          from the matching source state to the matching destination.
/// @param[in,out] controller AnimController3D whose transition table to edit.
/// @param[in] from_state Registered source-state name.
/// @param[in] to_state Registered destination-state name.
/// @param[in] blend_seconds Requested nonnegative blend duration in seconds.
/// @return `1` when the transition was inserted or updated; `0` for an
///         invalid controller, unknown state, or allocation failure.
int8_t rt_anim_controller3d_add_transition(void *controller,
                                           rt_string from_state,
                                           rt_string to_state,
                                           double blend_seconds);

/// @brief Play a named state on the base layer.
/// @details Uses a registered transition duration for the active source and
///          requested destination; otherwise switches immediately. The new
///          state starts from time zero, receives its configured speed and
///          looping policy, and fires any time-zero events.
/// @param[in,out] controller AnimController3D to play.
/// @param[in] state_name Registered state name.
/// @return `1` when playback starts; `0` for an invalid controller, unknown
///         state, or unavailable internal player or clip.
int8_t rt_anim_controller3d_play(void *controller, rt_string state_name);
/// @brief Smoothly crossfade into the named state over @p blend_seconds.
/// @details Ignores registered transition defaults. Negative and non-finite
///          durations become zero; a fade also becomes an immediate switch
///          when there is no different playing source state.
/// @param[in,out] controller AnimController3D to play.
/// @param[in] state_name Registered destination-state name.
/// @param[in] blend_seconds Requested blend duration in seconds.
/// @return `1` when playback starts; `0` for an invalid controller, unknown
///         state, or unavailable internal player or clip.
int8_t rt_anim_controller3d_crossfade(void *controller, rt_string state_name, double blend_seconds);
/// @brief Cross-fade to a state, entering its clip at `start_seconds` (ADR 0300).
int8_t rt_anim_controller3d_crossfade_at(void *controller,
                                         rt_string state_name,
                                         double blend_seconds,
                                         double start_seconds);
/// @brief Stop playback on every layer.
/// @details Pauses the internal players, cancels transitions, invalidates the
///          previous-frame palette, and recomputes the current sampled pose.
///          State selections remain recorded so a later play operation can
///          transition from the selected state.
/// @param[in,out] controller AnimController3D to stop; invalid handles are
///                           ignored.
void rt_anim_controller3d_stop(void *controller);
/// @brief Advance the controller's clock by @p delta_time seconds (drives blends, events, root
/// motion).
/// @details Applies the update-rate LOD gate, snapshots the previous palette,
///          advances all active layers and the blend tree, queues crossed
///          events, evaluates layers and IK, and accumulates root-bone
///          translation and rotation. Negative or non-finite deltas are
///          ignored.
/// @param[in,out] controller AnimController3D to advance.
/// @param[in] delta_time Nonnegative elapsed time in seconds.
void rt_anim_controller3d_update(void *controller, double delta_time);

/// @brief Get the name of the currently-active state.
/// @param[in] controller AnimController3D to inspect.
/// @return Owned runtime-string copy of the base-layer state name, or an empty
///         string when no valid state is selected.
rt_string rt_anim_controller3d_get_current_state(void *controller);
/// @brief Get the name of the state we're transitioning from (empty string if not blending).
/// @details The previous selection can remain available after a transition
///          finishes; callers that specifically need active blend status
///          should also query @ref rt_anim_controller3d_get_is_transitioning.
/// @param[in] controller AnimController3D to inspect.
/// @return Owned runtime-string copy of the previous base-layer state name, or
///         an empty string when no previous state is recorded.
rt_string rt_anim_controller3d_get_previous_state(void *controller);
/// @brief True if the controller is currently in a crossfade.
/// @param[in] controller AnimController3D to inspect.
/// @return `1` while the base layer is transitioning; otherwise `0`.
int8_t rt_anim_controller3d_get_is_transitioning(void *controller);
/// @brief Number of registered states.
/// @param[in] controller AnimController3D to inspect.
/// @return Sanitized state count, or zero for an invalid controller.
int64_t rt_anim_controller3d_get_state_count(void *controller);
/// @brief Current base-layer playback time in seconds.
/// @param[in] controller AnimController3D to inspect.
/// @return Player time for a valid selected base state, or `0.0`.
double rt_anim_controller3d_get_state_time(void *controller);
/// @brief True if the named state is the active, playing base-layer state.
/// @param[in] controller AnimController3D to inspect.
/// @param[in] state_name Registered state name to compare.
/// @return `1` only when the name resolves to the base layer's current state
///         and its player is running; otherwise `0`.
int8_t rt_anim_controller3d_is_state_playing(void *controller, rt_string state_name);
/// @brief C-internal fast path of is_state_playing taking a plain C string (no
///   transient rt_string allocation); not part of the script-visible registry.
/// @param[in] controller AnimController3D to inspect.
/// @param[in] state_name Borrowed NUL-terminated state name.
/// @return `1` only when the name resolves to the base layer's current state
///         and its player is running; otherwise `0`.
int8_t rt_anim_controller3d_is_state_playing_cstr(void *controller, const char *state_name);

/// @brief Override the per-state playback speed multiplier.
/// @details Negative speeds enable reverse playback. A non-finite value
///          becomes `1.0`; finite values are narrowed safely to float range.
///          Every layer currently using the state is updated immediately.
/// @param[in,out] controller AnimController3D containing the state.
/// @param[in] state_name Registered state to update.
/// @param[in] speed Playback multiplier.
void rt_anim_controller3d_set_state_speed(void *controller, rt_string state_name, double speed);
/// @brief Override whether a state loops at the end of its clip duration.
/// @details Any nonzero @p loop value enables looping. Every layer currently
///          using the state receives the override immediately.
/// @param[in,out] controller AnimController3D containing the state.
/// @param[in] state_name Registered state to update.
/// @param[in] loop Zero for one-shot playback; nonzero for looping.
void rt_anim_controller3d_set_state_looping(void *controller, rt_string state_name, int8_t loop);
/// @brief Configure deterministic update-rate LOD for distant / low-priority controllers.
/// @details Positive finite distance and rate values enable batched updates at
///          @p rate_hz. Nonpositive or non-finite input disables throttling;
///          excessive finite values are capped to internal safety limits.
///          The controller stores distance as policy metadata but does not
///          itself measure camera distance.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] distance Positive caller-computed distance threshold or metric.
/// @param[in] rate_hz Positive evaluation frequency in updates per second.
void rt_anim_controller3d_set_animation_lod(void *controller, double distance, double rate_hz);
/// @brief Configure bone-count LOD: freeze bones at/after `max_bones` to bind-local (<=0 disables).
/// @details Frozen bones still inherit animated ancestors. Positive values are
///          saturated to the signed 32-bit range; values beyond the actual
///          skeleton size simply leave all bones animated.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] max_bones Number of leading, parent-first bones to animate.
void rt_anim_controller3d_set_bone_lod(void *controller, int64_t max_bones);
/// @brief Use a BlendTree3D as the base pose source; pass NULL to clear it.
/// @details A non-null tree must expose local transforms for the exact
///          Skeleton3D and bone count bound to this controller. The controller
///          retains a successfully attached tree and releases any prior tree.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] blend_tree Borrowed compatible BlendTree3D handle, or `NULL`.
/// @return `1` when attached or cleared; `0` for an invalid controller, wrong
///         class, or incompatible skeleton data.
int8_t rt_anim_controller3d_set_blend_tree(void *controller, void *blend_tree);
/// @brief Apply an IKSolver3D after controller layers and before skinning; pass NULL to clear it.
/// @details A non-null solver must reference the controller's exact skeleton.
///          The controller retains a successfully attached solver and releases
///          any prior solver.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] ik_solver Borrowed compatible IKSolver3D handle, or `NULL`.
/// @return `1` when attached or cleared; `0` for an invalid controller, wrong
///         class, or mismatched skeleton.
int8_t rt_anim_controller3d_set_ik_solver(void *controller, void *ik_solver);
/// @brief Append an IKSolver3D to the bounded ordered post-pose stack.
/// @details Duplicate handles are idempotent. The solver must reference the
///          controller's exact skeleton; at most four constraints are retained.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] ik_solver Borrowed compatible IKSolver3D handle.
/// @return `1` when present/appended; `0` for invalid input or a full stack.
int8_t rt_anim_controller3d_add_ik_solver(void *controller, void *ik_solver);

/// @brief Schedule an event to fire when @p state_name reaches @p time_seconds during playback.
/// @details The nonempty event name is copied into bounded storage. Negative
///          times clamp to zero, times beyond a positive clip duration clamp to
///          that duration, and non-finite times are ignored. Multiple events
///          may share a state or timestamp.
/// @param[in,out] controller AnimController3D whose schedule to extend.
/// @param[in] state_name Registered state on whose timeline to fire.
/// @param[in] time_seconds Requested event time in seconds.
/// @param[in] event_name Nonempty name copied into the schedule.
void rt_anim_controller3d_add_event(void *controller,
                                    rt_string state_name,
                                    double time_seconds,
                                    rt_string event_name);
/// @brief Pop the next pending event name (empty string when none queued).
/// @details Events are returned FIFO. Pending names are bounded to 63 bytes;
///          when the ring is full, enqueueing a fresh event drops the oldest.
/// @param[in,out] controller AnimController3D whose queue to drain.
/// @return Owned runtime-string copy of the oldest event name, or an empty
///         string when the controller or queue is empty.
rt_string rt_anim_controller3d_poll_event(void *controller);

/// @brief Designate which bone provides root motion (-1 disables root motion).
/// @details A valid change resets both accumulated translation and rotation.
///          Values other than `-1` must name an existing bone; other invalid
///          indexes leave the current selection and accumulators unchanged.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] bone_index Zero-based skeleton bone index, or `-1` to disable.
void rt_anim_controller3d_set_root_motion_bone(void *controller, int64_t bone_index);
/// @brief Peek at the accumulated root-motion translation since the last consume.
/// @param[in,out] controller AnimController3D to inspect; non-finite stored
///                           lanes are sanitized to zero.
/// @return New Vec3 containing the accumulated model-space translation, or a
///         zero vector for an invalid controller.
void *rt_anim_controller3d_get_root_motion_delta(void *controller);
/// @brief Consume the accumulated root-motion delta and reset the accumulator.
/// @param[in,out] controller AnimController3D whose translation to consume.
/// @return New Vec3 containing the pre-reset model-space delta, or a zero
///         vector for an invalid controller.
void *rt_anim_controller3d_consume_root_motion(void *controller);

/// @brief Set the blend weight (0..1) for an animation layer (layer 0 = base).
/// @details Overlay weights clamp to `[0,1]`, with non-finite input becoming
///          zero. Layer zero is always forced to full weight.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] layer Zero-based layer index.
/// @param[in] weight Requested overlay contribution.
void rt_anim_controller3d_set_layer_weight(void *controller, int64_t layer, double weight);
/// @brief Restrict a layer to @p root_bone and its descendants.
/// @details Layer zero and a `-1` overlay root cover the full skeleton.
///          Out-of-range overlay roots likewise rebuild as a full-skeleton
///          mask; values below `-1` or above `INT32_MAX` normalize to `-1`.
/// @param[in,out] controller AnimController3D to configure.
/// @param[in] layer Zero-based layer index.
/// @param[in] root_bone Skeleton subtree root, or `-1` for every bone.
void rt_anim_controller3d_set_layer_mask(void *controller, int64_t layer, int64_t root_bone);
/// @brief Hard-cut a specific layer to the named state.
/// @details Enables replace-mode composition and accepts both the base and
///          overlay layer indexes.
/// @param[in,out] controller AnimController3D to play.
/// @param[in] layer Zero-based layer index.
/// @param[in] state_name Registered state name.
/// @return `1` when playback starts; `0` for an invalid layer, unknown state,
///         or unavailable internal player or clip.
int8_t rt_anim_controller3d_play_layer(void *controller, int64_t layer, rt_string state_name);
/// @brief Hard-cut a specific overlay layer to a true additive bind-pose delta state.
/// @param[in,out] controller AnimController3D to play.
/// @param[in] layer Overlay layer index in the range 1 through 3.
/// @param[in] state_name Registered state name.
/// @return `1` when playback starts; `0` for the base or another invalid
///         layer, an unknown state, or unavailable player or clip.
int8_t rt_anim_controller3d_play_layer_additive(void *controller,
                                                int64_t layer,
                                                rt_string state_name);
/// @brief Crossfade a specific layer to the named state over @p blend_seconds.
/// @details Enables replace-mode composition. Negative and non-finite
///          durations become zero; the base layer is accepted.
/// @param[in,out] controller AnimController3D to play.
/// @param[in] layer Zero-based layer index.
/// @param[in] state_name Registered destination-state name.
/// @param[in] blend_seconds Requested blend duration in seconds.
/// @return `1` when playback starts; `0` for an invalid layer, unknown state,
///         or unavailable internal player or clip.
int8_t rt_anim_controller3d_crossfade_layer(void *controller,
                                            int64_t layer,
                                            rt_string state_name,
                                            double blend_seconds);
/// @brief Crossfade a specific overlay layer to a true additive bind-pose delta state.
/// @details Negative and non-finite durations become zero.
/// @param[in,out] controller AnimController3D to play.
/// @param[in] layer Overlay layer index in the range 1 through 3.
/// @param[in] state_name Registered destination-state name.
/// @param[in] blend_seconds Requested blend duration in seconds.
/// @return `1` when playback starts; `0` for the base or another invalid
///         layer, an unknown state, or unavailable player or clip.
int8_t rt_anim_controller3d_crossfade_layer_additive(void *controller,
                                                     int64_t layer,
                                                     rt_string state_name,
                                                     double blend_seconds);
/// @brief Stop playback on one layer.
/// @details Cancels its transition and restores replace mode. Overlay layers
///          also clear their current state so they no longer composite; the
///          base layer keeps its selection. The configured layer weight is not
///          changed.
/// @param[in,out] controller AnimController3D to modify.
/// @param[in] layer Zero-based layer index.
void rt_anim_controller3d_stop_layer(void *controller, int64_t layer);

/// @brief Get the model-space (skeleton-root-relative) global matrix for bone
/// @p bone_index after all layers + transitions are evaluated. Compose with the
/// owning node's world transform to reach world space.
/// @param[in] controller AnimController3D to inspect.
/// @param[in] bone_index Zero-based skeleton bone index.
/// @return New Mat4 snapshot of the composited global transform, or `NULL` for
///         an invalid controller, missing palette, or out-of-range bone.
void *rt_anim_controller3d_get_bone_matrix(void *controller, int64_t bone_index);

/// @brief Extract a bone's model-space position + rotation quaternion from the
/// final pose. Returns 1 on success, 0 on missing pose/invalid bone.
/// @details Both outputs are initialized to zero position and identity
///          rotation when their pointers are non-null, even on failure.
/// @param[in] controller AnimController3D to inspect.
/// @param[in] bone_index Zero-based skeleton bone index.
/// @param[out] out_pos Writable array of at least three doubles.
/// @param[out] out_quat Writable array of at least four doubles in `(x,y,z,w)`
///                      order.
/// @return `1` when both outputs were populated from a valid bone; otherwise
///         `0`.
int rt_anim_controller3d_get_bone_pose(void *controller,
                                       int64_t bone_index,
                                       double *out_pos,
                                       double *out_quat);

/// @name Scene3D runtime integration
/// @{

/// @brief Borrow the Skeleton3D bound to this controller.
/// @param[in] controller AnimController3D to inspect.
/// @return Borrowed Skeleton3D handle, or `NULL` for an invalid controller or
///         corrupted backing reference. The caller must not release it.
void *rt_anim_controller3d_get_skeleton(void *controller);
/// @brief Internal (ragdoll): overwrite masked bones' model-space globals after
///        evaluation, propagate deltas to unmasked descendants, refresh palette.
/// @details Input arrays must cover the controller's complete bone count.
///          Every nonzero mask lane copies the corresponding row-major
///          4-by-4 matrix; unmasked descendants inherit changed-parent deltas.
///          Allocation failure or invalid input leaves without reporting an
///          error and may leave no changes.
/// @param[in,out] controller AnimController3D whose evaluated pose to modify.
/// @param[in] mask Borrowed array of one byte per skeleton bone.
/// @param[in] globals Borrowed array of `bone_count * 16` model-space floats.
void rt_anim_controller3d_apply_pose_override(void *controller,
                                              const int8_t *mask,
                                              const float *globals);
/// @brief Borrow the flat float array of bone matrices for GPU upload (length = bone_count*16).
/// @details The pointer remains owned by the controller and can be overwritten
///          by later pose evaluation or invalidated at controller finalization.
/// @param[in] controller AnimController3D to inspect.
/// @param[out] bone_count Optional destination for the number of matrices;
///                        initialized to zero on failure.
/// @return Borrowed pointer to `*bone_count * 16` row-major floats, or `NULL`
///         when the controller, skeleton, or palette is unavailable.
const float *rt_anim_controller3d_get_final_palette_data(void *controller, int32_t *bone_count);
/// @brief Borrow the previous frame's bone palette (used for motion vectors / TAA).
/// @details No previous palette exists until an update snapshots one. The
///          pointer remains controller-owned and is overwritten by later
///          updates or invalidated by stop/finalization.
/// @param[in] controller AnimController3D to inspect.
/// @param[out] bone_count Optional destination for the number of matrices;
///                        initialized to zero on failure.
/// @return Borrowed pointer to `*bone_count * 16` row-major floats, or `NULL`
///         until a valid previous palette is available.
const float *rt_anim_controller3d_get_previous_palette_data(void *controller, int32_t *bone_count);
/// @brief Internal runtime helper: consume the accumulated root-motion rotation delta and reset it.
/// @details Normalizes the accumulator before returning it and resets the
///          stored value to identity.
/// @param[in,out] controller AnimController3D whose rotation to consume.
/// @return New Quat containing the pre-reset delta, or an identity quaternion
///         for an invalid controller.
void *rt_anim_controller3d_consume_root_motion_rotation(void *controller);

/// @}

#ifdef __cplusplus
}
#endif
