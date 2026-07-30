//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_skeleton3d.h
// Purpose: Skeleton3D, Animation3D, and AnimPlayer3D — skeletal animation
//   with bone hierarchies, keyframe interpolation (SLERP for rotation),
//   CPU skinning, and animation crossfade.
//
// Key invariants:
//   - Publicly appended bones use parent-before-child order; pose builders also
//     defend against non-topological imported/private storage and cycles.
//   - Max 1024 bones per skeleton (VGFX3D_MAX_SKELETON_BONES); draw palettes
//     stay capped at 256 (VGFX3D_MAX_BONES) via per-mesh bone maps.
//   - Bone palette = global_transform * inverse_bind_pose per bone.
//   - Keyframe rotation uses SLERP; position/scale use linear interpolation.
//
// Ownership/Lifetime:
//   - Skeleton/animation/player objects are GC-managed runtime values.
//   - Skeletons retain exact bone-name strings; players retain bound skeletons/clips.
//
// Links: plans/3d/14-skeletal-animation.md, vgfx3d_skinning.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for skeletons, animation clips, players, and pose blending.
/// @details Skeleton3D owns a bounded hierarchy and exact retained bone names;
///          Animation3D owns time-sorted per-bone keyframes; AnimPlayer3D
///          samples one clip with optional crossfade; AnimBlend3D combines up
///          to its fixed state limit. Binding a skeleton to an animation
///          runtime freezes further bone insertion. Bone globals returned by
///          this API are skeleton/model-space matrices, not scene-world
///          transforms.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @name Skeleton3D
/// @{

/// @brief Create an empty skeleton (no bones yet).
/// @return New GC-managed mutable Skeleton3D, or `NULL` after a runtime trap on
///         allocation failure.
void *rt_skeleton3d_new(void);
/// @brief Append a bone with an exact retained name, parent index, and bind-pose matrix.
/// @details The skeleton retains the complete runtime string without a fixed-length name limit.
///          A parent index of -1 creates a root; other bones must reference an
///          already appended parent. A missing, wrong-class, or non-finite
///          Mat4 falls back to identity. Insertion is rejected after the
///          skeleton is frozen or at the 1024-bone limit.
/// @param[in,out] skel Mutable Skeleton3D to extend.
/// @param[in] name Borrowed runtime string retained exactly; `NULL` becomes
///                 the empty singleton.
/// @param[in] parent_index `-1` for a root or an existing lower bone index.
/// @param[in] bind_mat4 Borrowed parent-relative bind Mat4; invalid input uses
///                     identity.
/// @return The new zero-based bone index, or -1 when validation/allocation fails.
int64_t rt_skeleton3d_add_bone(void *skel, rt_string name, int64_t parent_index, void *bind_mat4);

/// @brief Register (or remove, with empty local) an external bone-name alias
///        consulted by animation retargeting (e.g. "mixamorig:Hips" -> "hips").
/// @details Alias sides use a legacy 63-byte canonical form. Re-registering an
///          external name replaces its mapping; a null/empty local removes it.
///          Alias editing remains allowed after the bone table is frozen.
/// @param[in,out] skeleton Skeleton3D whose alias table to edit.
/// @param[in] external Borrowed nonempty external rig name.
/// @param[in] local Borrowed destination bone name, or null/empty to remove.
void rt_skeleton3d_set_bone_alias(void *skeleton, rt_string external, rt_string local);

/// @brief Number of registered bone-name aliases.
/// @param[in] skeleton Skeleton3D to inspect.
/// @return Stored alias count, or zero for an invalid handle.
int64_t rt_skeleton3d_get_alias_count(void *skeleton);
/// @brief Pre-compute inverse bind-pose matrices used during skinning. Call after all bones added.
/// @details Builds model-space bind globals defensively even for malformed
///          ordering/cycles, then stores each inverse. Singular matrices use
///          identity; scratch-allocation failure leaves existing inverses.
/// @param[in,out] skel Skeleton3D whose inverse-bind matrices to refresh.
void rt_skeleton3d_compute_inverse_bind(void *skel);
/// @brief Total number of bones in the skeleton.
/// @param[in,out] skel Skeleton3D to inspect; private count metadata is
///                     sanitized in place.
/// @return Safe bone count, or zero for an invalid handle.
int64_t rt_skeleton3d_get_bone_count(void *skel);
/// @brief Look up a bone by its complete byte-exact name (-1 if not found).
/// @param[in,out] skel Skeleton3D whose safe bone range to search.
/// @param[in] name Borrowed non-null runtime string.
/// @return First matching zero-based bone index, or `-1`.
int64_t rt_skeleton3d_find_bone(void *skel, rt_string name);
/// @brief Look up a bone index by name as Some(index), or None when absent.
/// @param[in] skel Skeleton3D to search.
/// @param[in] name Borrowed exact runtime name.
/// @return New Option containing a boxed matching index, or `None`.
void *rt_skeleton3d_find_bone_option(void *skel, rt_string name);
/// @brief Get a retained exact name for the bone at @p index.
/// @details The caller owns the returned reference. Out-of-range input returns the shared
///          empty string.
/// @param[in,out] skel Skeleton3D to inspect.
/// @param[in] index Zero-based bone index.
/// @return Retained runtime-string reference to the full name, or an owned
///         empty-string result.
rt_string rt_skeleton3d_get_bone_name(void *skel, int64_t index);
/// @brief Get the bind-pose matrix (Mat4) of the bone at @p index.
/// @param[in,out] skel Skeleton3D to inspect.
/// @param[in] index Zero-based bone index.
/// @return New Mat4 copy of the parent-relative bind matrix, or `NULL` for
///         invalid input/index.
void *rt_skeleton3d_get_bone_bind_pose(void *skel, int64_t index);
/// @brief Internal: parent index of the bone at @p index (-1 for roots/out of range).
/// @param[in,out] skel Skeleton3D to inspect.
/// @param[in] index Zero-based bone index.
/// @return Valid direct-parent index, or `-1` for a root, malformed parent,
///         invalid handle, or invalid index.
int64_t rt_skeleton3d_get_bone_parent_raw(void *skel, int64_t index);
/// @brief `Skeleton3D.GetBoneParent(index)` — public rig-topology readback (ADR 0227).
/// @param[in,out] skel Skeleton3D to inspect.
/// @param[in] index Zero-based bone index.
/// @return Valid direct-parent index, or `-1` for roots and invalid input.
int64_t rt_skeleton3d_get_bone_parent(void *skel, int64_t index);
/// @brief Internal: copy the bone's parent-relative bind matrix (row-major 16
///        doubles) into @p out16; returns 0 on out-of-range.
/// @param[in,out] skel Skeleton3D to inspect.
/// @param[in] index Zero-based bone index.
/// @param[out] out16 Writable array of at least 16 doubles.
/// @return `1` after copying; `0` for invalid arguments/index.
int8_t rt_skeleton3d_get_bone_bind_local_raw(void *skel, int64_t index, double *out16);

/// @}

/// @name Animation3D
/// @{
/// @brief Create a named animation clip with the given duration in seconds.
/// @details The display name is copied and truncated to 63 bytes. Duration is
///          safely narrowed to positive float; nonpositive or non-finite input
///          defaults to one second. New clips are non-looping.
/// @param[in] name Borrowed runtime display name, or `NULL` for empty.
/// @param[in] duration Requested clip duration in seconds.
/// @return New GC-managed Animation3D, or `NULL` after a runtime trap on
///         allocation failure.
void *rt_animation3d_new(rt_string name, double duration);
/// @brief Add a keyframe for one bone at @p time seconds (any of position/rotation/scale may be
/// NULL).
/// @details Bone indexes range from 0 through 1023. Negative time clamps to
///          zero; non-finite or non-float-range time rejects the operation.
///          Supplied values must be Vec3/Quat/Vec3 respectively. Components
///          outside finite float range are treated as absent and fall back to
///          bind pose during sampling. Keys remain time-sorted, and an
///          equal-time key replaces the prior key. Failure is silent in this
///          compatibility API.
/// @param[in,out] anim Animation3D to modify.
/// @param[in] bone_index Target skeleton-bone index.
/// @param[in] time Key time in seconds.
/// @param[in] position Optional borrowed Vec3 translation.
/// @param[in] rotation Optional borrowed Quat rotation.
/// @param[in] scale Optional borrowed Vec3 scale.
void rt_animation3d_add_keyframe(
    void *anim, int64_t bone_index, double time, void *position, void *rotation, void *scale);

/// @brief Tolerance-based keyframe reduction; returns the number of dropped keys.
/// @details Keeps channel endpoints, cubic/Hermite keys, presence-mask
///          changes, and any key not reconstructible by neighbor interpolation.
///          Tolerances are narrowed directly to float and callers should pass
///          finite nonnegative values.
/// @param[in,out] anim Animation3D whose channels to reduce in place.
/// @param[in] pos_tolerance Maximum allowed translation-lane error.
/// @param[in] rot_tolerance Maximum allowed `1 - abs(quaternion dot)` error.
/// @param[in] scl_tolerance Maximum allowed scale-lane error.
/// @return Number of removed keyframes, or zero for an invalid clip.
int64_t rt_animation3d_compress_keyframes(void *anim,
                                          double pos_tolerance,
                                          double rot_tolerance,
                                          double scl_tolerance);

/// @brief Attach CUBICSPLINE Hermite tangents (value units per second) to the
///   keyframe at (@p bone_index, @p time); NULL pairs leave that channel linear.
/// @details A channel becomes cubic only when both its in/out pointers are
///          present. Arrays are copied as three position, four rotation, or
///          three scale floats. Missing channels/keys and non-finite time are
///          ignored; lane finiteness is the importer's responsibility.
/// @param[in,out] anim Animation3D containing the existing key.
/// @param[in] bone_index Target channel's bone index.
/// @param[in] time Existing key time matched with runtime time tolerance.
/// @param[in] pos_in Optional borrowed three-float incoming position tangent.
/// @param[in] pos_out Optional borrowed three-float outgoing position tangent.
/// @param[in] rot_in Optional borrowed four-float incoming rotation tangent.
/// @param[in] rot_out Optional borrowed four-float outgoing rotation tangent.
/// @param[in] scl_in Optional borrowed three-float incoming scale tangent.
/// @param[in] scl_out Optional borrowed three-float outgoing scale tangent.
void rt_animation3d_set_keyframe_tangents(void *anim,
                                          int64_t bone_index,
                                          double time,
                                          const float *pos_in,
                                          const float *pos_out,
                                          const float *rot_in,
                                          const float *rot_out,
                                          const float *scl_in,
                                          const float *scl_out);
/// @brief Set whether the animation loops automatically when it reaches its duration.
/// @param[in,out] anim Animation3D to configure.
/// @param[in] loop Zero for one-shot playback; nonzero for looping.
void rt_animation3d_set_looping(void *anim, int8_t loop);
/// @brief Get the looping flag.
/// @param[in] anim Animation3D to inspect.
/// @return `1` for looping; `0` for one-shot or invalid input.
int8_t rt_animation3d_get_looping(void *anim);
/// @brief Get the animation duration in seconds.
/// @param[in] anim Animation3D to inspect.
/// @return Positive finite duration, or `0.0` for invalid/corrupt input.
double rt_animation3d_get_duration(void *anim);
/// @brief Get the animation's name.
/// @param[in] anim Animation3D to inspect.
/// @return Owned runtime-string copy of the bounded display name, or an empty
///         string for invalid input.
rt_string rt_animation3d_get_name(void *anim);
/// @brief Copy animation channels from @p src_skeleton to matching bones in @p dst_skeleton.
/// @details Produces an independent clip preserving name, duration, looping,
///          key times, rotations, scales, masks, and cubic tangent data.
///          Mapping uses explicit destination aliases and humanoid-role/name
///          inference with deterministic fallbacks; unmapped channels are
///          skipped. Translation values are scaled by source/destination bone
///          length ratio for differing proportions.
/// @param[in] anim Source Animation3D.
/// @param[in] src_skeleton Skeleton3D against which source channel indexes are
///                         interpreted.
/// @param[in] dst_skeleton Destination Skeleton3D.
/// @return New GC-managed retargeted Animation3D, or `NULL` for invalid input
///         or allocation/copy failure.
void *rt_animation3d_retarget(void *anim, void *src_skeleton, void *dst_skeleton);

/// @}

/// @name AnimPlayer3D
/// @{
/// @brief Create an animation player bound to a skeleton (drives bone palette during draw).
/// @details Retains and freezes the skeleton, allocates current/previous/local/
///          global pose buffers sized to its safe bone count, and initializes
///          bind pose at speed one with playback stopped.
/// @param[in,out] skeleton Borrowed Skeleton3D to retain and freeze.
/// @return New GC-managed AnimPlayer3D, or `NULL` after a runtime trap for an
///         invalid skeleton or allocation failure.
void *rt_anim_player3d_new(void *skeleton);
/// @brief Begin playing @p animation immediately from time 0.
/// @details Retains the new clip, releases prior current/fade clips, clears
///          temporal history, and evaluates time zero. Passing `NULL` clears
///          playback and evaluates bind pose; a wrong-class non-null input is
///          ignored.
/// @param[in,out] player AnimPlayer3D to control.
/// @param[in] animation Borrowed Animation3D, or `NULL`.
void rt_anim_player3d_play(void *player, void *animation);
/// @brief Crossfade from the current animation to @p animation over @p duration seconds.
/// @details Retains both source and destination during the fade, advances them
///          in parallel, blends local TRS, and clears temporal history.
///          Nonpositive or non-finite duration degenerates to an immediate
///          play.
/// @param[in,out] player AnimPlayer3D to control.
/// @param[in] animation Borrowed non-null Animation3D destination.
/// @param[in] duration Requested blend time in seconds.
void rt_anim_player3d_crossfade(void *player, void *animation, double duration);
/// @brief Stop the player; subsequent draws use the bind pose.
/// @details Cancels any fade and resets temporal palette history without
///          releasing the selected current clip.
/// @param[in,out] player AnimPlayer3D to stop.
void rt_anim_player3d_stop(void *player);
/// @brief Advance the player's clock by @p delta_time seconds.
/// @details Ignores negative/non-finite deltas and stopped players. Applies
///          signed speed, wrapping looping clips and clamping/stopping
///          one-shots at either endpoint, advances the fade source, then
///          refreshes local, global, and skinning matrices.
/// @param[in,out] player AnimPlayer3D to advance.
/// @param[in] delta_time Nonnegative elapsed time in seconds.
void rt_anim_player3d_update(void *player, double delta_time);
/// @brief Multiply the playback speed (1.0 = normal, 2.0 = double, -1.0 = reverse).
/// @details Non-finite input becomes one and finite input saturates safely to
///          float range.
/// @param[in,out] player AnimPlayer3D to configure.
/// @param[in] speed Signed playback multiplier.
void rt_anim_player3d_set_speed(void *player, double speed);
/// @brief Get the playback speed multiplier.
/// @param[in] player AnimPlayer3D to inspect.
/// @return Finite stored speed, or `1.0` for invalid/corrupt input.
double rt_anim_player3d_get_speed(void *player);
/// @brief True if the player is actively driving an animation.
/// @param[in] player AnimPlayer3D to inspect.
/// @return `1` while playback is active; otherwise `0`.
int8_t rt_anim_player3d_is_playing(void *player);
/// @brief Get the current playback time in seconds.
/// @param[in] player AnimPlayer3D to inspect.
/// @return Finite current clip time, or `0.0`.
double rt_anim_player3d_get_time(void *player);
/// @brief Seek to a specific clip time in seconds.
/// @details Looping clips wrap the finite requested value, including negative
///          time; non-looping clips clamp to `[0,duration]`. Non-finite input
///          becomes zero. Seeking clears temporal history and immediately
///          reevaluates the pose.
/// @param[in,out] player AnimPlayer3D to seek.
/// @param[in] time Requested absolute clip time.
void rt_anim_player3d_set_time(void *player, double time);
/// @brief Get the skeleton/model-space global matrix for a bone at the current time.
/// @param[in] player AnimPlayer3D to inspect.
/// @param[in] bone_index Zero-based skeleton bone index.
/// @return New Mat4 snapshot, or `NULL` for invalid input/index or unavailable
///         pose data.
void *rt_anim_player3d_get_bone_matrix(void *player, int64_t bone_index);

/// @}

/// @name Mesh3D skinning extensions
/// @{
/// @brief Bind a Skeleton3D to the mesh so subsequent skinned draws use its bone palette.
/// @details A successful non-null binding retains and freezes the skeleton,
///          releases the prior binding, updates the non-partitioned palette
///          count up to 256 bones, and marks geometry changed. Pass `NULL` to
///          detach; a wrong-class non-null handle is ignored.
/// @param[in,out] mesh Mesh3D to configure.
/// @param[in,out] skeleton Borrowed Skeleton3D to retain/freeze, or `NULL`.
void rt_mesh3d_set_skeleton(void *mesh, void *skeleton);
/// @brief Set and normalize up to four bone influences for one mesh vertex.
/// @details Bone indexes outside `[0,255]` are replaced with index/weight zero.
///          Only positive finite weights contribute and are normalized to sum
///          one; an empty contribution leaves all four weights zero. The mesh
///          palette count and geometry generation are updated.
/// @param[in,out] mesh Mesh3D to modify.
/// @param[in] vertex_index Zero-based vertex index.
/// @param[in] b0 First palette-bone index.
/// @param[in] w0 First requested weight.
/// @param[in] b1 Second palette-bone index.
/// @param[in] w1 Second requested weight.
/// @param[in] b2 Third palette-bone index.
/// @param[in] w2 Third requested weight.
/// @param[in] b3 Fourth palette-bone index.
/// @param[in] w3 Fourth requested weight.
void rt_mesh3d_set_bone_weights(void *mesh,
                                int64_t vertex_index,
                                int64_t b0,
                                double w0,
                                int64_t b1,
                                double w1,
                                int64_t b2,
                                double w2,
                                int64_t b3,
                                double w3);

/// @}

/// @name Canvas3D skeletal drawing
/// @{

/// @brief Draw a mesh with skinning applied via the player's current bone palette.
/// @details Accepts either AnimPlayer3D or AnimController3D. The transform must
///          be a live Mat4 and serves as the temporal motion key. The animator
///          object is retained through frame completion so current/previous
///          palettes remain valid; unsupported palette sizes fall through the
///          lower drawing path's CPU/partition logic.
/// @param[in,out] canvas Canvas3D receiving the deferred draw.
/// @param[in] mesh Borrowed nonempty Mesh3D.
/// @param[in] transform Borrowed Mat4 model transform.
/// @param[in] material Borrowed material handle.
/// @param[in] anim_player Borrowed AnimPlayer3D or AnimController3D.
void rt_canvas3d_draw_mesh_skinned(
    void *canvas, void *mesh, void *transform, void *material, void *anim_player);
/// @brief Draw a mesh with multi-state blending via an AnimBlend3D handle.
/// @details Also accepts a BlendTree3D and borrows its underlying AnimBlend3D.
///          The live Mat4 transform doubles as the temporal key, and the
///          supplied blend/tree object is retained through frame completion.
/// @param[in,out] canvas Canvas3D receiving the deferred draw.
/// @param[in] mesh Borrowed nonempty Mesh3D.
/// @param[in] transform Borrowed Mat4 model transform.
/// @param[in] material Borrowed material handle.
/// @param[in] blend Borrowed AnimBlend3D or BlendTree3D.
void rt_canvas3d_draw_mesh_blended(
    void *canvas, void *mesh, void *transform, void *material, void *blend);

/// @}

/// @name AnimBlend3D
/// @{

/// @brief Create a multi-state animation blender bound to a skeleton.
/// @details Retains and freezes the skeleton, allocates bind-pose current and
///          temporal buffers, and starts with no states. At most eight states
///          may be registered.
/// @param[in,out] skeleton Borrowed Skeleton3D to retain and freeze.
/// @return New GC-managed AnimBlend3D, or `NULL` for an invalid skeleton or
///         allocation failure.
void *rt_anim_blend3d_new(void *skeleton);
/// @brief Register a named animation as a blend state. Returns the new state index.
/// @details The clip must have channels fitting the bound skeleton and is
///          retained on success. The name is copied and truncated to 63 bytes;
///          duplicate/empty names are accepted. New states start at weight
///          zero, time zero, speed one, and inherit clip looping.
/// @param[in,out] blend AnimBlend3D to extend.
/// @param[in] name Borrowed runtime state name, or `NULL`.
/// @param[in] animation Borrowed compatible Animation3D.
/// @return Newly appended zero-based state index, or `-1` for invalid input,
///         incompatibility, or the eight-state limit.
int64_t rt_anim_blend3d_add_state(void *blend, rt_string name, void *animation);
/// @brief Set the blend weight for the @p state-th state by index (typically 0..1).
/// @details Values clamp to `[0,1]` and non-finite input becomes zero. Positive
///          state contributions are normalized together during update.
/// @param[in,out] blend AnimBlend3D to configure.
/// @param[in] state Zero-based state index.
/// @param[in] weight Requested nonnegative contribution.
void rt_anim_blend3d_set_weight(void *blend, int64_t state, double weight);
/// @brief Set the blend weight by state name.
/// @details The query is truncated to the same 63-byte canonical form and
///          updates the first exact match; empty/unknown names are ignored.
/// @param[in,out] blend AnimBlend3D to configure.
/// @param[in] name Borrowed nonempty runtime state name.
/// @param[in] weight Requested contribution, clamped to `[0,1]`.
void rt_anim_blend3d_set_weight_by_name(void *blend, rt_string name, double weight);
/// @brief Read the current blend weight of the @p state-th state.
/// @param[in] blend AnimBlend3D to inspect.
/// @param[in] state Zero-based state index.
/// @return Sanitized stored weight in `[0,1]`, or zero.
double rt_anim_blend3d_get_weight(void *blend, int64_t state);
/// @brief Set the per-state playback speed multiplier.
/// @details Negative speed enables reverse playback, non-finite input becomes
///          one, and finite input saturates safely to float range.
/// @param[in,out] blend AnimBlend3D to configure.
/// @param[in] state Zero-based state index.
/// @param[in] speed Signed playback multiplier.
void rt_anim_blend3d_set_speed(void *blend, int64_t state, double speed);
/// @brief Tick the blender forward by @p dt seconds (advances all enabled states).
/// @details All valid state timers advance, regardless of weight. Looping clips
///          wrap and non-looping clips clamp at both ends. The resulting pose
///          is a normalized TRS blend of positive-weight states, or bind pose
///          when none contribute. Negative/non-finite time is ignored.
/// @param[in,out] blend AnimBlend3D to advance and evaluate.
/// @param[in] dt Nonnegative elapsed time in seconds.
void rt_anim_blend3d_update(void *blend, double dt);
/// @brief Number of registered blend states.
/// @param[in] blend AnimBlend3D to inspect.
/// @return Sanitized state count in `[0,8]`, or zero.
int64_t rt_anim_blend3d_state_count(void *blend);
/// @brief Internal: borrowed skeleton handle used for controller compatibility checks.
/// @param[in,out] blend AnimBlend3D to inspect; a stale class slot may be
///                      cleared defensively.
/// @return Borrowed Skeleton3D handle, or `NULL`; the caller must not release it.
void *rt_anim_blend3d_get_skeleton(void *blend);
/// @brief Internal: borrowed local-transform buffer (with @p bone_count) for controller
/// composition.
/// @details The buffer contains one row-major local matrix per bone, remains
///          owned by the blender, and is overwritten by later updates.
/// @param[in,out] blend AnimBlend3D to inspect; pose storage may grow.
/// @param[out] bone_count Optional output initialized to zero, then set to the
///                        matrix count on success.
/// @return Borrowed `*bone_count * 16` float buffer, or `NULL`.
const float *rt_anim_blend3d_get_local_transform_data(void *blend, int32_t *bone_count);

/// @}

#ifdef __cplusplus
}
#endif
