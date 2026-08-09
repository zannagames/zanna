//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_soundlistener3d.h
// Purpose: Gameplay-facing active-listener object for 3D audio.
// Key invariants:
//   - Exactly one live listener identity is active at a time.
//   - Stored pose and velocity are repaired before observation or mixer publication.
// Ownership/Lifetime:
//   - Listeners retain optional SceneNode3D and Camera3D bindings.
//   - The active-listener global is weak and cleared during finalization.
// Links: rt_sound3d.h, rt_soundsource3d.h, rt_scene3d.h, rt_canvas3d.h
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the graphics-bound active-listener object for spatial audio.
/// @details Listener objects store explicit pose/velocity or retain a SceneNode3D
///          or Camera3D binding synchronized each frame. Exactly one object may
///          feed the low-level active-listener slot; clearing it restores the
///          independently configurable fallback listener.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a 3D audio listener (positions audio relative to this listener while active).
/// @return Caller-owned listener object, or NULL on allocation failure.
void *rt_soundlistener3d_new(void);

/// @brief Get the listener's world position as a Vec3.
/// @param listener Listener object.
/// @return New Vec3 snapshot, or NULL for an invalid object/allocation failure.
void *rt_soundlistener3d_get_position(void *listener);
/// @brief Set the listener's position from a Vec3 handle.
/// @param listener Listener object.
/// @param position Vec3 world position; NULL/invalid handles are ignored.
void rt_soundlistener3d_set_position(void *listener, void *position);
/// @brief Set the listener's position from raw scalar coordinates.
/// @param listener Listener object.
/// @param x World X coordinate.
/// @param y World Y coordinate.
/// @param z World Z coordinate.
void rt_soundlistener3d_set_position_vec(void *listener, double x, double y, double z);

/// @brief Get the listener's forward direction as a Vec3.
/// @param listener Listener object.
/// @return New normalized Vec3 snapshot, or NULL on failure.
void *rt_soundlistener3d_get_forward(void *listener);
/// @brief Set the listener's forward direction.
/// @param listener Listener object.
/// @param forward Vec3 direction; the spatial core normalizes it. NULL is ignored.
void rt_soundlistener3d_set_forward(void *listener, void *forward);

/// @brief Get the listener's up direction as a Vec3.
/// @param listener Listener object.
/// @return New normalized Vec3 snapshot, or NULL on failure.
void *rt_soundlistener3d_get_up(void *listener);
/// @brief Set the listener's up direction.
/// @param listener Listener object.
/// @param up Vec3 direction orthonormalized against forward; NULL is ignored.
void rt_soundlistener3d_set_up(void *listener, void *up);

/// @brief Get the listener's velocity (used for Doppler shift).
/// @param listener Listener object.
/// @return New Vec3 velocity snapshot, or NULL on failure.
void *rt_soundlistener3d_get_velocity(void *listener);
/// @brief Set the listener's velocity.
/// @param listener Listener object.
/// @param velocity Vec3 world velocity; NULL is ignored.
void rt_soundlistener3d_set_velocity(void *listener, void *velocity);

/// @brief True if this listener is the currently-active spatial-audio listener.
/// @param listener Listener object.
/// @return `1` when active, otherwise `0`.
int8_t rt_soundlistener3d_get_is_active(void *listener);
/// @brief Promote/demote this listener to/from active. Only one active listener at a time.
/// @param listener Listener object.
/// @param active Non-zero to activate; zero to demote.
void rt_soundlistener3d_set_is_active(void *listener, int8_t active);

/// @brief Bind to a SceneNode3D; listener follows the node's transform each frame.
/// @details Replaces and releases any prior node/camera binding.
/// @param listener Listener object.
/// @param node SceneNode3D to retain, or NULL to clear.
void rt_soundlistener3d_bind_node(void *listener, void *node);
/// @brief Detach from any bound scene node.
/// @param listener Listener object; the last synchronized pose is preserved.
void rt_soundlistener3d_clear_node_binding(void *listener);
/// @brief Bind to a Camera3D; listener follows the complete camera view pose each frame.
/// @details Position, forward, and rolled up direction are synchronized. Zero-delta
/// binding refreshes update pose without consuming the next velocity sample.
/// Replaces and releases any prior camera/node binding.
/// @param listener Listener object.
/// @param camera Camera3D to retain, or NULL to clear.
void rt_soundlistener3d_bind_camera(void *listener, void *camera);
/// @brief Detach from any bound camera.
/// @param listener Listener object; the last synchronized pose is preserved.
void rt_soundlistener3d_clear_camera_binding(void *listener);

#ifdef __cplusplus
}
#endif
