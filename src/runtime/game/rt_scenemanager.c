//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_scenemanager.c
/// @file
/// @brief Implements bounded named-scene registration, immediate switching,
///        timed transitions, and one-update edge flags.
// Purpose: Multi-scene manager — named scenes, switch, transitions, edge flags.
// Key invariants:
//   - Scene names are unique within one manager and switches never target unknown scenes.
//   - Timed transitions publish edge flags only when the active scene changes.
// Ownership/Lifetime:
//   - Each manager owns its bounded inline scene registry for the manager lifetime.
//   - Returned scene names are runtime-owned immutable strings.
// Links: src/runtime/game/rt_scenemanager.h,
//        src/tests/unit/runtime/TestSceneManager.cpp
//
//===----------------------------------------------------------------------===//

#include "rt_scenemanager.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <string.h>

/// @brief Maximum number of named scenes held by one manager.
#define SM_MAX_SCENES 64
/// @brief Fixed storage size for each NUL-terminated scene name.
#define SM_SCENE_NAME_MAX 128

/// @brief One registered scene name and its retained activation metadata.
typedef struct {
    /// @brief NUL-terminated UTF-8 scene name.
    char name[SM_SCENE_NAME_MAX];
    /// @brief Reserved activation marker initialized for registered scenes.
    int8_t active;
} sm_scene_t;

/// @brief Inline state owned by the SceneManager runtime object.
typedef struct {
    /// @brief Fixed-capacity scene registry.
    sm_scene_t scenes[SM_MAX_SCENES];
    /// @brief Number of occupied entries in @ref scenes.
    int32_t count;
    int32_t current;  // Index into scenes[] (-1 = none)
    int32_t previous; // Previous scene index
    int8_t just_entered;
    int8_t just_exited;
    // Transition
    int8_t transitioning;
    int32_t next_scene;     // Target scene during transition
    int64_t trans_timer;    // Countdown ms
    int64_t trans_duration; // Total duration ms
    int8_t transition_completed;
} scenemanager_impl;

/// @brief Safe-cast a handle to the SceneManager impl, trapping @p api on a
///        class-id mismatch.
/// @param mgr Borrowed candidate SceneManager handle.
/// @param api Trap message identifying the calling API.
/// @return Borrowed implementation pointer, or `NULL` if @p mgr is `NULL`.
static scenemanager_impl *checked_scenemanager(void *mgr, const char *api) {
    if (!mgr)
        return NULL;
    if (rt_obj_class_id(mgr) != RT_SCENEMANAGER_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (scenemanager_impl *)mgr;
}

/// @brief Copy a runtime-string scene name into a fixed @p out buffer.
/// @details Rejects a name that does not fit the fixed buffer instead of
///          truncating it, so two distinct long names sharing a 127-byte prefix
///          cannot alias under the strcmp-based lookup (VDOC-243). Also rejects an
///          embedded NUL, which would corrupt the comparison.
/// @param name Borrowed runtime string handle.
/// @param out Destination with capacity @ref SM_SCENE_NAME_MAX.
/// @return `1` after a complete copy; `0` for null, malformed, embedded-NUL,
///         or overlong input.
static int scene_name_from_handle(void *name, char out[SM_SCENE_NAME_MAX]) {
    if (!name || !out)
        return 0;
    rt_string s = (rt_string)name;
    const char *cname = rt_string_cstr(s);
    if (!cname)
        return 0;
    int64_t len = rt_str_len(s);
    if (len < 0 || (size_t)len >= SM_SCENE_NAME_MAX || strlen(cname) != (size_t)len)
        return 0;
    memcpy(out, cname, (size_t)len);
    out[len] = '\0';
    return 1;
}

/// @brief Linear-search registered scenes by name.
/// @param sm Borrowed manager implementation.
/// @param name NUL-terminated exact scene name.
/// @return The scene's zero-based index, or `-1` for absent or invalid input.
static int find_scene(scenemanager_impl *sm, const char *name) {
    if (!sm || !name)
        return -1;
    for (int32_t i = 0; i < sm->count; i++) {
        if (strcmp(sm->scenes[i].name, name) == 0)
            return i;
    }
    return -1;
}

/// @brief Create a new scene manager for named game state transitions.
/// @details Manages a flat list of named scenes (e.g., "menu", "gameplay", "pause").
///          Supports instant switching and timed transitions with progress tracking.
/// @return Owned SceneManager runtime object, or `NULL` if allocation fails.
void *rt_scenemanager_new(void) {
    scenemanager_impl *sm = (scenemanager_impl *)rt_obj_new_i64(RT_SCENEMANAGER_CLASS_ID,
                                                                (int64_t)sizeof(scenemanager_impl));
    if (!sm)
        return NULL;
    memset(sm, 0, sizeof(scenemanager_impl));
    sm->current = -1;
    sm->previous = -1;
    sm->next_scene = -1;
    return sm;
}

/// @brief Register a uniquely named scene.
/// @details The first successfully added scene becomes current and raises the
///          entered edge. Null, malformed, duplicate, overlong, and
///          over-capacity additions are ignored.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed runtime scene-name string.
void rt_scenemanager_add(void *mgr, void *name) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.Add: expected Zanna.Game.SceneManager");
    if (!sm || !name)
        return;
    if (sm->count >= SM_MAX_SCENES)
        return;
    char cname[SM_SCENE_NAME_MAX];
    if (!scene_name_from_handle(name, cname))
        return;
    if (find_scene(sm, cname) >= 0)
        return;
    sm_scene_t *s = &sm->scenes[sm->count++];
    strncpy(s->name, cname, SM_SCENE_NAME_MAX - 1);
    s->name[SM_SCENE_NAME_MAX - 1] = '\0';
    s->active = 1;
    // Auto-set first scene as current if none
    if (sm->current < 0) {
        sm->current = sm->count - 1;
        sm->just_entered = 1;
    }
}

/// @brief Instantly switch to a registered named scene.
/// @details A successful switch updates current/previous indices, raises both
///          edge flags, and cancels any timed transition. Unknown names and
///          requests for the already-current scene are ignored.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed runtime scene-name string.
void rt_scenemanager_switch(void *mgr, void *name) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.Switch: expected Zanna.Game.SceneManager");
    if (!sm || !name)
        return;
    char cname[SM_SCENE_NAME_MAX];
    if (!scene_name_from_handle(name, cname))
        return;
    int idx = find_scene(sm, cname);
    if (idx < 0 || idx == sm->current)
        return;
    sm->previous = sm->current;
    sm->current = idx;
    sm->just_entered = 1;
    sm->just_exited = 1;
    sm->transitioning = 0;
    sm->transition_completed = 0;
    sm->next_scene = -1;
    sm->trans_timer = 0;
    sm->trans_duration = 0;
}

/// @brief Begin or retarget a timed transition to a registered scene.
/// @details Nonpositive durations are normalized to one millisecond. Requests
///          for an unknown/current scene or the active transition target are
///          ignored.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed target scene-name string.
/// @param duration_ms Requested transition duration in milliseconds.
void rt_scenemanager_switch_transition(void *mgr, void *name, int64_t duration_ms) {
    scenemanager_impl *sm = checked_scenemanager(
        mgr, "SceneManager.SwitchTransition: expected Zanna.Game.SceneManager");
    if (!sm || !name)
        return;
    char cname[SM_SCENE_NAME_MAX];
    if (!scene_name_from_handle(name, cname))
        return;
    int idx = find_scene(sm, cname);
    if (idx < 0 || idx == sm->current || (sm->transitioning && idx == sm->next_scene))
        return;
    sm->transitioning = 1;
    sm->transition_completed = 0;
    sm->next_scene = idx;
    sm->trans_duration = duration_ms > 0 ? duration_ms : 1;
    sm->trans_timer = sm->trans_duration;
}

/// @brief Advance transition state by a number of milliseconds.
/// @details Clears all one-update edge flags before processing. Positive time
///          decrements an active countdown; completion commits the target,
///          updates the previous scene, and raises entered, exited, and
///          transition-completed state for the current update.
/// @param mgr Borrowed SceneManager handle.
/// @param dt Elapsed milliseconds; nonpositive values do not advance time.
void rt_scenemanager_update(void *mgr, int64_t dt) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.Update: expected Zanna.Game.SceneManager");
    if (!sm)
        return;
    sm->transition_completed = 0;
    sm->just_entered = 0;
    sm->just_exited = 0;

    if (sm->transitioning && dt > 0) {
        sm->trans_timer -= dt;
        if (sm->trans_timer <= 0) {
            sm->transitioning = 0;
            sm->transition_completed = 1;
            sm->previous = sm->current;
            sm->current = sm->next_scene;
            sm->next_scene = -1;
            sm->trans_timer = 0;
            sm->just_entered = 1;
            sm->just_exited = 1;
        }
    }
}

/// @brief Get the name of the currently active scene.
/// @param mgr Borrowed SceneManager handle.
/// @return Runtime-owned immutable name string, or an immutable empty string
///         when no current scene is available.
void *rt_scenemanager_current(void *mgr) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.Current: expected Zanna.Game.SceneManager");
    if (!sm)
        return (void *)rt_const_cstr("");
    if (sm->current >= 0 && sm->current < sm->count)
        return (void *)rt_const_cstr(sm->scenes[sm->current].name);
    return (void *)rt_const_cstr("");
}

/// @brief Get the name of the previously active scene (before the last transition).
/// @param mgr Borrowed SceneManager handle.
/// @return Runtime-owned immutable previous-name string, or an immutable empty
///         string when no previous scene is available.
void *rt_scenemanager_previous(void *mgr) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.Previous: expected Zanna.Game.SceneManager");
    if (!sm)
        return (void *)rt_const_cstr("");
    if (sm->previous >= 0 && sm->previous < sm->count)
        return (void *)rt_const_cstr(sm->scenes[sm->previous].name);
    return (void *)rt_const_cstr("");
}

/// @brief Check whether the current scene matches the given name.
/// @param mgr Borrowed SceneManager handle.
/// @param name Borrowed runtime scene-name string.
/// @return `1` for an exact current-name match; otherwise `0`.
int8_t rt_scenemanager_is_scene(void *mgr, void *name) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.IsScene: expected Zanna.Game.SceneManager");
    if (!sm || !name)
        return 0;
    if (sm->current < 0)
        return 0;
    char cname[SM_SCENE_NAME_MAX];
    if (!scene_name_from_handle(name, cname))
        return 0;
    return strcmp(sm->scenes[sm->current].name, cname) == 0;
}

/// @brief Check whether a scene was entered this frame (one-shot, cleared on next update).
/// @param mgr Borrowed SceneManager handle.
/// @return `1` when the entered edge is pending; otherwise `0`.
int8_t rt_scenemanager_just_entered(void *mgr) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.JustEntered: expected Zanna.Game.SceneManager");
    return sm ? sm->just_entered : 0;
}

/// @brief Check whether a scene was exited this frame (one-shot, cleared on next update).
/// @param mgr Borrowed SceneManager handle.
/// @return `1` when the exited edge is pending; otherwise `0`.
int8_t rt_scenemanager_just_exited(void *mgr) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.JustExited: expected Zanna.Game.SceneManager");
    return sm ? sm->just_exited : 0;
}

/// @brief Check whether a timed scene transition is currently in progress.
/// @param mgr Borrowed SceneManager handle.
/// @return `1` while a target countdown is active; otherwise `0`.
int8_t rt_scenemanager_is_transitioning(void *mgr) {
    scenemanager_impl *sm =
        checked_scenemanager(mgr, "SceneManager.IsTransitioning: expected Zanna.Game.SceneManager");
    return sm ? sm->transitioning : 0;
}

/// @brief Get the transition progress as a ratio (0.0 = start, 1.0 = complete).
/// @details Returns exactly `1.0` for the update in which a transition
///          completed, then returns `0.0` after the next update clears that
///          completion edge.
/// @param mgr Borrowed SceneManager handle.
/// @return Clamped transition progress in `[0.0, 1.0]`, or `0.0` when idle.
double rt_scenemanager_transition_progress(void *mgr) {
    scenemanager_impl *sm = checked_scenemanager(
        mgr, "SceneManager.TransitionProgress: expected Zanna.Game.SceneManager");
    if (!sm)
        return 0.0;
    if (sm->transition_completed)
        return 1.0;
    if (!sm->transitioning || sm->trans_duration <= 0)
        return 0.0;
    double elapsed = (double)(sm->trans_duration - sm->trans_timer);
    if (elapsed < 0.0)
        elapsed = 0.0;
    if (elapsed > (double)sm->trans_duration)
        elapsed = (double)sm->trans_duration;
    return elapsed / (double)sm->trans_duration;
}
