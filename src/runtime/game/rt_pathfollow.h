//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_pathfollow.h
/// @file
/// @brief Declares fixed-point waypoint traversal with once, loop, and
///        ping-pong modes.
//
// Purpose: Path follower for moving objects along waypoint paths with linear interpolation,
// supporting one-shot, looping, and ping-pong traversal modes.
//
// Key invariants:
//   - Maximum waypoints per path is RT_PATHFOLLOW_MAX_POINTS (64).
//   - At least two waypoints must be added before calling rt_pathfollow_start.
//   - Progress is in range [0, 1000] representing the full path length.
//   - Fixed-point convention: 1000 = 1.0 for coordinates, speed, and progress.
//
// Ownership/Lifetime:
//   - PathFollower handles are reference-counted GC objects. The object owns a
//     lazily allocated segment-length cache freed by its finalizer.
//   - rt_pathfollow_destroy releases the caller's reference.
//
// Links: src/runtime/game/rt_pathfollow.c (implementation),
// src/runtime/game/rt_tween.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate PathFollower handles.
#define RT_PATHFOLLOW_CLASS_ID INT64_C(-0x510208)

/// @brief Maximum number of inline waypoints in one follower.
#define RT_PATHFOLLOW_MAX_POINTS 64

/// @brief Endpoint behavior used when traversal reaches either path end.
typedef enum {
    RT_PATHFOLLOW_ONCE = 0,     ///< Play once and stop at end.
    RT_PATHFOLLOW_LOOP = 1,     ///< Loop back to start.
    RT_PATHFOLLOW_PINGPONG = 2, ///< Reverse at endpoints.
} rt_pathfollow_mode_t;

/// @brief Opaque handle to a runtime reference-counted PathFollower.
typedef struct rt_pathfollow_impl *rt_pathfollow;

/// @brief Allocate an empty, inactive PathFollower.
/// @return A new PathFollower reference, or `NULL` on allocation failure.
/// @details Defaults are ONCE mode, position `(0,0)`, and speed 100000
///          fixed-point units per second (100 world units per second).
rt_pathfollow rt_pathfollow_new(void);

/// @brief Release one runtime reference to a PathFollower.
/// @param path Handle to release; `NULL` is a no-op.
/// @details The lazy segment-length cache is reclaimed with the final object
///          reference. A non-null wrong-class handle raises a runtime trap.
void rt_pathfollow_destroy(rt_pathfollow path);

/// @brief Remove all waypoints and reset traversal state.
/// @param path Follower to clear; `NULL` is a no-op.
/// @details Position becomes `(0,0)` and cached lengths are freed. Speed and
///          mode are preserved. A non-null wrong-class handle raises a trap.
void rt_pathfollow_clear(rt_pathfollow path);

/// @brief Append one fixed-point waypoint.
/// @param path Follower to modify.
/// @param x X coordinate where 1000 equals one world unit.
/// @param y Y coordinate where 1000 equals one world unit.
/// @return `1` when appended, or `0` for a null/full follower.
/// @details The first point becomes current position. Length recomputation is
///          deferred until needed. A non-null wrong-class handle traps.
int8_t rt_pathfollow_add_point(rt_pathfollow path, int64_t x, int64_t y);

/// @brief Count stored waypoints.
/// @param path Follower to query.
/// @return Count in `[0, RT_PATHFOLLOW_MAX_POINTS]`, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_point_count(rt_pathfollow path);

/// @brief Set endpoint traversal behavior.
/// @param path Follower to modify; `NULL` is a no-op.
/// @param mode RT_PATHFOLLOW_ONCE, RT_PATHFOLLOW_LOOP, or
///        RT_PATHFOLLOW_PINGPONG. Other values are ignored.
/// @details Current position and state are preserved. A non-null wrong-class
///          handle raises a runtime trap.
void rt_pathfollow_set_mode(rt_pathfollow path, int64_t mode);

/// @brief Read endpoint traversal behavior.
/// @param path Follower to query.
/// @return An rt_pathfollow_mode_t integer, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_mode(rt_pathfollow path);

/// @brief Set path traversal speed.
/// @param path Follower to modify.
/// @param speed Positive fixed-point units per second, where 1000 equals one
///        world unit per second. Nonpositive values are ignored.
/// @details A non-null wrong-class handle raises a runtime trap.
void rt_pathfollow_set_speed(rt_pathfollow path, int64_t speed);

/// @brief Read path traversal speed.
/// @param path Follower to query.
/// @return Fixed-point units per second, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_speed(rt_pathfollow path);

/// @brief Start or resume traversal.
/// @param path Follower to start.
/// @details At least two waypoints are required. A paused follower resumes;
///          a previously finished follower rewinds before restarting. Null or
///          underspecified paths are no-ops. A non-null wrong-class handle
///          raises a runtime trap.
void rt_pathfollow_start(rt_pathfollow path);

/// @brief Pause traversal at the current position.
/// @param path Follower to pause; `NULL` is a no-op.
/// @details Segment progress is retained for rt_pathfollow_start(). A non-null
///          wrong-class handle raises a runtime trap.
void rt_pathfollow_pause(rt_pathfollow path);

/// @brief Stop traversal and rewind to the first waypoint.
/// @param path Follower to stop; `NULL` is a no-op.
/// @details Waypoints, speed, and mode are preserved; active, finished,
///          reverse, progress, and carried fractions are cleared. A non-null
///          wrong-class handle raises a runtime trap.
void rt_pathfollow_stop(rt_pathfollow path);

/// @brief Test whether traversal updates are enabled.
/// @param path Follower to query.
/// @return `1` when active, or `0` when paused, stopped, finished, or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_pathfollow_is_active(rt_pathfollow path);

/// @brief Test whether an ONCE traversal reached its last waypoint.
/// @param path Follower to query.
/// @return Stored finished flag, or zero for `NULL`.
/// @details LOOP and PINGPONG traversal do not normally set this flag. Stop,
///          clear, and restart clear it. A non-null wrong-class handle traps.
int8_t rt_pathfollow_is_finished(rt_pathfollow path);

/// @brief Advance traversal by an elapsed time interval.
/// @param path Follower to update.
/// @param dt Elapsed milliseconds; nonpositive values do nothing.
/// @details Movement may cross multiple segments. Sub-unit distance and
///          sub-per-mille progress are carried across calls. Endpoint behavior
///          follows the selected mode, zero-length segments are skipped, and a
///          fully degenerate path becomes inactive. Cache-allocation failure
///          leaves the follower active for retry. A non-null wrong-class handle
///          raises a runtime trap.
void rt_pathfollow_update(rt_pathfollow path, int64_t dt);

/// @brief Read the current interpolated X coordinate.
/// @param path Follower to query.
/// @return Fixed-point X where 1000 equals one world unit, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_x(rt_pathfollow path);

/// @brief Read the current interpolated Y coordinate.
/// @param path Follower to query.
/// @return Fixed-point Y where 1000 equals one world unit, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_y(rt_pathfollow path);

/// @brief Measure current positional distance along the entire path.
/// @param path Follower to query.
/// @return Per-mille distance from the first waypoint in `[0,1000]`, or zero
///         for an empty, degenerate, unavailable, or null path.
/// @details This value decreases during reverse ping-pong motion and resets
///          after a loop wrap. Lengths are rebuilt lazily. A non-null
///          wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_progress(rt_pathfollow path);

/// @brief Teleport to a fractional distance along the path.
/// @param path Follower to reposition.
/// @param progress Requested per-mille distance, clamped to `[0,1000]`.
/// @details Carried fractions are cleared, while active, finished, mode, and
///          reverse state are preserved. Null, fewer-than-two-point, and
///          zero-length paths are no-ops. A non-null wrong-class handle traps.
void rt_pathfollow_set_progress(rt_pathfollow path, int64_t progress);

/// @brief Read the current segment index.
/// @param path Follower to query.
/// @return Index connecting `waypoint[i]` to `waypoint[i+1]`, or zero for
///         `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_segment(rt_pathfollow path);

/// @brief Quantize the current direction of travel to eight compass headings.
/// @param path Follower to query.
/// @return One of `0, 45000, ..., 315000` fixed-point degrees, or zero when
///         unavailable or the segment endpoints coincide.
/// @details Zero is right and 90000 is down in screen coordinates. Reverse
///          traversal flips the direction. A non-null wrong-class handle
///          raises a runtime trap.
int64_t rt_pathfollow_get_angle(rt_pathfollow path);

#ifdef __cplusplus
}
#endif
