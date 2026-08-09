//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_sound3d.h
// Purpose: Spatial audio — distance attenuation and stereo panning based on
//   3D listener and source positions. Wraps the existing 2D audio API.
// Key invariants:
//   - Published listener snapshots have bounded velocity and an orthonormal basis.
//   - Voice outputs are initialized and bounded even when inputs are invalid.
// Ownership/Lifetime:
//   - Listener snapshots are copied; no caller-owned array is retained.
//   - Per-voice metadata is process-owned and bounded by the runtime hard cap.
// Links: rt_audio.h, rt_vec3.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares listener snapshots and voice-level 3D audio spatialization.
/// @details The API converts world positions and velocities into the existing
///          2D voice controls: logical volume, stereo pan, and Doppler pitch.
///          A process-wide active listener overrides a configurable fallback,
///          while a bounded metadata table preserves per-voice falloff settings
///          for later source-position updates.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Snapshot of a listener's spatial pose for voice-parameter math: world
///   position, orthonormal forward/right/up basis, velocity (Doppler), and a `valid` flag.
/// @details Array members use world-space XYZ coordinates. Setter helpers
///          sanitize non-finite values and orthonormalize the orientation basis.
typedef struct rt_sound3d_listener_state {
    double position[3];
    double forward[3];
    double right[3];
    double up[3];
    double velocity[3];
    int8_t valid;
} rt_sound3d_listener_state;

/// @brief Reset @p state to the canonical identity (origin, -Z forward, +X right, +Y up).
/// @param state Listener snapshot to initialize; NULL is ignored.
void rt_sound3d_listener_state_identity(rt_sound3d_listener_state *state);
/// @brief Populate @p state from explicit position/forward/velocity arrays (any may be NULL).
/// @param state Listener snapshot to populate.
/// @param position Optional world-space XYZ position.
/// @param forward Optional facing vector; defaults to -Z.
/// @param velocity Optional world-space XYZ velocity.
void rt_sound3d_listener_state_set(rt_sound3d_listener_state *state,
                                   const double *position,
                                   const double *forward,
                                   const double *velocity);
/// @brief Populate @p state from explicit position/orientation/velocity arrays.
/// @param state Listener snapshot to populate.
/// @param position Optional world-space XYZ position.
/// @param forward Optional facing vector; defaults to -Z.
/// @param up Optional up vector; defaults to +Y and preserves listener roll.
/// @param velocity Optional world-space XYZ velocity.
void rt_sound3d_listener_state_set_pose(rt_sound3d_listener_state *state,
                                        const double *position,
                                        const double *forward,
                                        const double *up,
                                        const double *velocity);
/// @brief Read the listener-state currently driving spatial audio (active or fallback).
/// @param out_state Receives a copied listener snapshot; NULL is ignored.
void rt_sound3d_get_effective_listener_state(rt_sound3d_listener_state *out_state);
/// @brief Promote a listener-state snapshot to the active spatial-audio listener.
/// @details NULL or invalid input clears the active listener. Valid input is copied,
///          bounded, and orthonormalized before publication.
/// @param state Snapshot to copy.
void rt_sound3d_set_active_listener_state(const rt_sound3d_listener_state *state);
/// @brief Detach the active SoundListener3D and revert to the fallback listener.
/// @details Must be called on the runtime main thread.
void rt_sound3d_clear_active_listener_state(void);
/// @brief Compute distance-attenuated volume + stereo pan for a 3D source given a listener state.
/// @param listener Listener snapshot, or NULL for the fallback listener.
/// @param source_position Source world-space XYZ position.
/// @param max_distance Zero-volume falloff distance; zero disables attenuation.
/// @param base_volume Logical pre-attenuation volume in `[0, 100]`.
/// @param out_volume Receives attenuated volume in `[0, 100]`.
/// @param out_pan Receives stereo pan in `[-100, 100]`.
/// @details Valid output pointers are cleared before input validation.
void rt_sound3d_compute_voice_params(const rt_sound3d_listener_state *listener,
                                     const double *source_position,
                                     double max_distance,
                                     int64_t base_volume,
                                     int64_t *out_volume,
                                     int64_t *out_pan);
/// @brief Extended spatial calculation with reference distance and Doppler factor output.
/// @param listener Listener snapshot, or NULL for the fallback listener.
/// @param source_position Source world-space XYZ position.
/// @param source_velocity Optional source world-space XYZ velocity.
/// @param ref_distance Full-volume radius.
/// @param max_distance Zero-volume falloff radius.
/// @param base_volume Logical pre-attenuation volume.
/// @param out_volume Receives attenuated volume.
/// @param out_pan Receives stereo pan.
/// @param out_doppler Optional destination for playback-rate factor in `[0.5, 2]`.
/// @details Valid output pointers are initialized before input validation. Sonic and
///          supersonic radial inputs saturate in the physical shift direction.
void rt_sound3d_compute_voice_params_ex(const rt_sound3d_listener_state *listener,
                                        const double *source_position,
                                        const double *source_velocity,
                                        double ref_distance,
                                        double max_distance,
                                        int64_t base_volume,
                                        int64_t *out_volume,
                                        int64_t *out_pan,
                                        double *out_doppler);
/// @brief Register an existing voice for later SpatialAudio3D.UpdateVoice fallback lookups.
/// @param voice Positive backend voice identifier.
/// @param max_distance Zero-volume falloff distance.
/// @param base_volume Logical unattenuated volume.
void rt_sound3d_register_voice(int64_t voice, double max_distance, int64_t base_volume);
/// @brief Register an existing voice with explicit reference/max falloff distances.
/// @param voice Positive backend voice identifier.
/// @param ref_distance Full-volume radius.
/// @param max_distance Zero-volume radius, normalized not to precede @p ref_distance.
/// @param base_volume Logical unattenuated volume.
void rt_sound3d_register_voice_ex(int64_t voice,
                                  double ref_distance,
                                  double max_distance,
                                  int64_t base_volume);
/// @brief Return the number of occupied entries in Sound3D's growable voice metadata table.
/// @return Occupied entry count, bounded by the hard capacity.
int64_t rt_sound3d_tracked_voice_count(void);
/// @brief Return the current Sound3D voice metadata capacity.
/// @return Allocated capacity, or the initial capacity before first allocation.
int64_t rt_sound3d_tracked_voice_capacity(void);

/// @brief Set the fallback listener position and orientation (Vec3 handles).
/// @param position Listener world position; NULL maps to the origin.
/// @param forward Facing direction; NULL/degenerate input maps to -Z.
void rt_sound3d_set_listener(void *position, void *forward);
/// @brief Play a sound at a 3D position with linear distance attenuation.
/// @param sound Playable Sound handle.
/// @param position Source-position Vec3 handle.
/// @param max_distance Zero-volume falloff distance; zero means infinite range.
/// @param volume Logical pre-attenuation volume.
/// @return Positive voice identifier, or `-1` when playback cannot start.
int64_t rt_sound3d_play_at(void *sound, void *position, double max_distance, int64_t volume);
/// @brief Update a playing voice's volume and pan based on its current 3D position.
/// @param voice Voice identifier returned by @ref rt_sound3d_play_at.
/// @param position Current source-position Vec3 handle.
/// @param max_distance Positive falloff override, or zero to reuse registration.
void rt_sound3d_update_voice(int64_t voice, void *position, double max_distance);
/// @brief Update a playing voice using explicit source velocity and falloff radii.
/// @details Finished voice identifiers are ignored. Positive distance overrides
/// persist in the voice metadata table and become the values reused by later
/// zero-override updates.
/// @param voice Positive backend voice identifier.
/// @param position Current source-position Vec3 handle.
/// @param source_velocity Optional source world-space XYZ velocity.
/// @param ref_distance Positive full-volume override, or zero to reuse.
/// @param max_distance Positive zero-volume override, or zero to reuse.
void rt_sound3d_update_voice_ex(int64_t voice,
                                void *position,
                                const double *source_velocity,
                                double ref_distance,
                                double max_distance);
/// @brief Push positions of any SoundSource3D objects bound to scene nodes (called per frame).
/// @details Graphics-disabled builds provide a no-op implementation.
/// @param dt Frame delta in seconds.
void rt_sound3d_sync_bindings(double dt);

#ifdef __cplusplus
}
#endif
