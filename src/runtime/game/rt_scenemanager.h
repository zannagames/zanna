//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_scenemanager.h
/// @file
/// @brief Declares the C ABI for bounded named-scene switching and timed
///        transition state.
// Purpose: Multi-scene manager with named scenes and transitions.
//
// Key invariants:
//   - A manager stores at most 64 unique names of at most 127 bytes each.
//   - Scene changes raise entered/exited edges until the next update call.
//   - Timed transitions track state only; rendering remains caller-owned.
//
// Ownership/Lifetime:
//   - rt_scenemanager_new() returns an owned runtime object.
//   - Manager and name arguments are borrowed for the duration of each call.
//   - Current/previous names are runtime-owned immutable string handles.
//
// Links: src/runtime/game/rt_scenemanager.c
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class identifier used to validate SceneManager handles.
#define RT_SCENEMANAGER_CLASS_ID INT64_C(-0x510218)

/// @brief Create a multi-scene manager with no active scene.
/// @return Owned SceneManager runtime object, or `NULL` if allocation fails.
void *rt_scenemanager_new(void);

/// @brief Register a uniquely named scene.
/// @details The first accepted scene becomes current. Null, malformed,
///          duplicate, overlong, and over-capacity additions are ignored.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed runtime string handle.
void rt_scenemanager_add(void *mgr, void *name);

/// @brief Switch immediately to a registered named scene.
/// @details A successful switch raises entered/exited edges and cancels any
///          active timed transition.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed target scene-name string.
void rt_scenemanager_switch(void *mgr, void *name);

/// @brief Begin or retarget a timed scene transition.
/// @details The target becomes current only after updates consume the duration;
///          nonpositive durations are normalized to one millisecond. Rendering
///          and visual effects remain caller-owned.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed target scene-name string.
/// @param duration_ms Requested duration in milliseconds.
void rt_scenemanager_switch_transition(void *mgr, void *name, int64_t duration_ms);

/// @brief Advance transition state and clear previous one-update edges.
/// @param mgr Borrowed SceneManager handle.
/// @param dt Elapsed milliseconds; nonpositive values do not advance time.
void rt_scenemanager_update(void *mgr, int64_t dt);

/// @brief Get the currently active scene name.
/// @param mgr Borrowed SceneManager handle.
/// @return Runtime-owned immutable name string, or an immutable empty string
///         when no current scene exists.
void *rt_scenemanager_current(void *mgr);

/// @brief Get the scene name active before the latest completed switch.
/// @param mgr Borrowed SceneManager handle.
/// @return Runtime-owned immutable name string, or an immutable empty string
///         when no previous scene exists.
void *rt_scenemanager_previous(void *mgr);

/// @brief Test whether a name exactly matches the current scene.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed runtime scene-name string.
/// @return `1` for an exact match; otherwise `0`.
int8_t rt_scenemanager_is_scene(void *mgr, void *name);

/// @brief Query the one-update scene-entered edge.
/// @param mgr Borrowed SceneManager handle.
/// @return `1` after a scene becomes active and before the next update;
///         otherwise `0`.
int8_t rt_scenemanager_just_entered(void *mgr);

/// @brief Query the one-update scene-exited edge.
/// @param mgr Borrowed SceneManager handle.
/// @return `1` after leaving a scene and before the next update; otherwise `0`.
int8_t rt_scenemanager_just_exited(void *mgr);

/// @brief Test whether a timed scene transition is active.
/// @param mgr Borrowed SceneManager handle.
/// @return `1` while a transition countdown is active; otherwise `0`.
int8_t rt_scenemanager_is_transitioning(void *mgr);

/// @brief Return normalized timed-transition progress.
/// @details Completion reports `1.0` until the next update clears its edge;
///          idle state otherwise reports `0.0`.
/// @param mgr Borrowed SceneManager handle.
/// @return Clamped progress in `[0.0, 1.0]`.
double rt_scenemanager_transition_progress(void *mgr);

#ifdef __cplusplus
}
#endif
