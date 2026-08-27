//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_soundsource3d.h
// Purpose: Gameplay-facing spatial source object for 3D audio.
// Key invariants:
//   - Retained position, velocity, attenuation, and mixer state are finite and bounded.
//   - Floating-origin rebases update current and synchronization positions together.
// Ownership/Lifetime:
//   - Sources retain their Sound and optional SceneNode3D binding.
//   - Each source owns at most one transient mixer voice identifier.
// Links: rt_sound3d.h, rt_soundlistener3d.h, rt_scene3d.h
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares a graphics-bound spatial Sound source object.
/// @details Sources retain a Sound and optional SceneNode3D binding, combine
///          user pitch with Doppler, and apply reference/max-distance falloff,
///          pan, volume, occlusion, looping, and mix-group routing to owned
///          transient voice IDs.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a 3D audio source bound to a Sound asset.
/// @param sound Sound handle retained by the source; may be NULL.
/// @return Caller-owned source object, or NULL on allocation failure.
void *rt_soundsource3d_new(void *sound);

/// @brief Get the source's world-space position as a Vec3.
/// @param source Source object.
/// @return New Vec3 snapshot, or NULL for an invalid object/allocation failure.
void *rt_soundsource3d_get_position(void *source);
/// @brief Read the source position into caller-owned scalar outputs without allocating a Vec3.
/// @param source Source object.
/// @param x Output receiving world X.
/// @param y Output receiving world Y.
/// @param z Output receiving world Z.
/// @return One for a valid source and outputs, otherwise zero.
int8_t rt_soundsource3d_get_position_components(void *source, double *x, double *y, double *z);
/// @brief Set the source's position from a Vec3 handle.
/// @param source Source object.
/// @param position Vec3 world position; NULL/invalid handles are ignored.
void rt_soundsource3d_set_position(void *source, void *position);
/// @brief Set the source's position from raw scalar coordinates.
/// @param source Source object.
/// @param x World X coordinate.
/// @param y World Y coordinate.
/// @param z World Z coordinate.
void rt_soundsource3d_set_position_vec(void *source, double x, double y, double z);
/// @brief Shift an unbound source's position by a floating-origin rebase delta (subtracts it).
/// @param source Source object.
/// @param dx Origin X displacement.
/// @param dy Origin Y displacement.
/// @param dz Origin Z displacement.
void rt_soundsource3d_rebase_origin(void *source, double dx, double dy, double dz);

/// @brief Get the source's velocity as a Vec3 (used for Doppler shift).
/// @param source Source object.
/// @return New Vec3 velocity snapshot, or NULL on failure.
void *rt_soundsource3d_get_velocity(void *source);
/// @brief Set the source's velocity vector.
/// @param source Source object.
/// @param velocity Vec3 world velocity; NULL/invalid handles are ignored.
void rt_soundsource3d_set_velocity(void *source, void *velocity);
/// @brief Get the latest Doppler pitch factor computed from listener/source velocity.
/// @param source Source object.
/// @return Factor in `[0.5, 2]`, or `1` for an invalid source.
double rt_soundsource3d_get_doppler_factor(void *source);

/// @brief Get the falloff radius beyond which the source is inaudible.
/// @param source Source object.
/// @return Non-negative maximum distance, or zero for invalid input.
double rt_soundsource3d_get_max_distance(void *source);
/// @brief Set the falloff radius (linear attenuation between source and listener).
/// @param source Source object.
/// @param max_distance Requested non-negative zero-volume distance.
void rt_soundsource3d_set_max_distance(void *source, double max_distance);
/// @brief Get the full-volume reference distance used before linear falloff begins.
/// @param source Source object.
/// @return Non-negative reference distance, or zero for invalid input.
double rt_soundsource3d_get_ref_distance(void *source);
/// @brief Set the full-volume reference distance used before linear falloff begins.
/// @param source Source object.
/// @param ref_distance Requested positive full-volume radius.
void rt_soundsource3d_set_ref_distance(void *source, double ref_distance);

/// @brief Get the source's pre-attenuation volume (0–100).
/// @param source Source object.
/// @return Logical volume, or zero for invalid input.
int64_t rt_soundsource3d_get_volume(void *source);
/// @brief Set the source's pre-attenuation volume (clamped to 0–100).
/// @param source Source object.
/// @param volume Requested logical volume.
void rt_soundsource3d_set_volume(void *source, int64_t volume);

/// @brief Get the user playback-rate multiplier (1.0 default; composes with Doppler).
/// @param source Source object.
/// @return Bounded multiplier in `[0.25, 4]`, or `1` for invalid input.
double rt_soundsource3d_get_pitch(void *source);
/// @brief Set the user playback-rate multiplier (applies immediately to a live voice).
/// @param source Source object.
/// @param pitch Positive multiplier clamped to `[0.25, 4]`; invalid input resets to `1`.
void rt_soundsource3d_set_pitch(void *source, double pitch);
/// @brief Get the occlusion amount (0 open .. 1 fully occluded).
/// @param source Source object.
/// @return Stored fraction, or zero for invalid input.
double rt_soundsource3d_get_occlusion(void *source);
/// @brief Set the occlusion amount (game-driven; the mixer smooths ~80 ms).
/// @param source Source object.
/// @param amount Requested fraction, clamped to `[0, 1]`.
void rt_soundsource3d_set_occlusion(void *source, double amount);
/// @brief Route future playback voices to a mix group (applies from next play).
/// @param source Source object.
/// @param group Numeric mix-group identifier; invalid values fall back to SFX.
void rt_soundsource3d_set_mix_group(void *source, int64_t group);
/// @brief Get the mix group future playback voices route to.
/// @param source Source object.
/// @return Stored group, or SFX for invalid input.
int64_t rt_soundsource3d_get_mix_group(void *source);

/// @brief True if the source loops automatically when its sound finishes.
/// @param source Source object.
/// @return `1` when future playback loops, otherwise `0`.
int8_t rt_soundsource3d_get_looping(void *source);
/// @brief Toggle looping playback.
/// @details Takes effect on the next play and does not mutate a live voice.
/// @param source Source object.
/// @param looping Non-zero to loop future playback.
void rt_soundsource3d_set_looping(void *source, int8_t looping);

/// @brief True if the source is currently producing audio.
/// @param source Source object.
/// @return `1` while its voice is live, otherwise `0`.
int8_t rt_soundsource3d_get_is_playing(void *source);
/// @brief Get the underlying voice ID for direct mixer control (0 if not playing).
/// @param source Source object.
/// @return Positive live voice ID, or zero when inactive/invalid.
int64_t rt_soundsource3d_get_voice_id(void *source);

/// @brief Start playback, replacing any voice currently owned by the source.
/// @param source Source object with a playable Sound.
/// @return Positive assigned voice ID, or `-1` on failure.
int64_t rt_soundsource3d_play(void *source);
/// @brief Stop playback (releases the voice slot).
/// @param source Source object; inactive/invalid sources are ignored.
void rt_soundsource3d_stop(void *source);

/// @brief Bind the source to a SceneNode3D so its position follows the node's transform each frame.
/// @param source Source object.
/// @param node SceneNode3D to retain, or NULL to clear.
void rt_soundsource3d_bind_node(void *source, void *node);
/// @brief Detach the source from any bound node.
/// @param source Source object; last synchronized position is preserved.
void rt_soundsource3d_clear_node_binding(void *source);

#ifdef __cplusplus
}
#endif
