//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_pathfollow.c
/// @file
/// @brief Implements fixed-point waypoint traversal with once, loop, and
///        ping-pong modes.
//
// Purpose: Waypoint-based path follower for Zanna games. Stores an ordered list
//   of 2D waypoints and advances an entity along the path at a configurable
//   speed. The follower interpolates linearly between consecutive waypoints,
//   computing a per-mille-quantized world-space position and eight-direction
//   heading along the route. Typical uses: enemy patrol routes, projectile
//   trajectories, cutscene camera paths, and conveyor belts.
//
// Key invariants:
//   - IMPORTANT: All coordinates use a fixed-point scale of 1000 units = 1
//     world unit. Pass pixel-scale positions multiplied by 1000. For example,
//     a screen position of (320, 240) is stored as (320000, 240000). Failing
//     to scale results in the follower appearing to advance by a factor of 1000
//     too quickly.
//   - Speed uses the same fixed-point scale per second. Update() accepts elapsed
//     milliseconds and carries the sub-unit distance and sub-per-mille progress
//     remainders across calls, so slow speeds and long segments still accumulate
//     motion instead of truncating every frame's fraction to zero (VDOC-280).
//   - Segment progress is an integer from 0 through 1000. Segment lengths are
//     Euclidean distances between adjacent fixed-point waypoints.
//   - When the follower reaches the last waypoint it either stops (one-shot) or
//     wraps back to the first waypoint (looping), depending on the loop flag.
//   - Up to 64 waypoints are stored inline. A lazily built malloc'd array holds
//     their segment lengths.
//
// Ownership/Lifetime:
//   - PathFollower objects are reference-counted GC objects. The finalizer frees
//     the segment-length cache; rt_pathfollow_destroy() releases a reference.
//
// Links: src/runtime/game/rt_pathfollow.h (public API),
//        docs/zannalib/game.md (PathFollower section — coordinate scale note)
//
//===----------------------------------------------------------------------===//

#include "rt_pathfollow.h"
#include "rt_object.h"
#include "rt_trap.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @brief One inline fixed-point waypoint.
struct waypoint {
    int64_t x;
    int64_t y;
};

/// @brief Private traversal state, waypoint storage, and lazy length cache.
struct rt_pathfollow_impl {
    struct waypoint points[RT_PATHFOLLOW_MAX_POINTS];
    int64_t point_count;       ///< Number of waypoints.
    rt_pathfollow_mode_t mode; ///< Path mode.
    int64_t speed;             ///< Speed (units/sec, fixed-point).
    int8_t active;             ///< Is following active.
    int8_t finished;           ///< Has reached end (ONCE mode).
    int8_t reverse;            ///< Direction for PINGPONG.
    int64_t current_x;         ///< Current X position.
    int64_t current_y;         ///< Current Y position.
    int64_t segment;           ///< Current segment index.
    int64_t segment_progress;  ///< Progress within segment (0-1000).
    int64_t move_remainder;    ///< Sub-unit distance carried between updates (milliunits, 0-999).
    int64_t progress_remainder;///< Sub-per-mille progress carried within the current segment.
    int64_t total_length;      ///< Total path length (cached).
    int64_t *segment_lengths;  ///< Cached segment lengths.
    int8_t lengths_dirty;      ///< 1 if the length cache must be rebuilt before use.
};

/// @brief Validate and cast an opaque PathFollower handle.
/// @param path Candidate handle; `NULL` is accepted.
/// @param api Trap message for a non-null class mismatch.
/// @return @p path when valid, or `NULL` for null or invalid input.
/// @details A class mismatch raises a runtime trap before returning `NULL`.
static rt_pathfollow checked_pathfollow(rt_pathfollow path, const char *api) {
    if (!path)
        return NULL;
    if (rt_obj_class_id(path) != RT_PATHFOLLOW_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return path;
}

/// @brief Add two signed integers without overflow.
/// @param a First addend.
/// @param b Second addend.
/// @return Exact sum when representable, otherwise the nearest int64 endpoint.
static int64_t pathfollow_saturating_add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Compute (value*scale)/divisor in long double, saturating to int64;
///        0 when @p divisor is 0.
/// @param value Multiplicand.
/// @param scale Scale factor.
/// @param divisor Divisor.
/// @return Truncated scaled quotient, a saturated endpoint, or zero for a
///         zero divisor.
static int64_t pathfollow_scaled(int64_t value, int64_t scale, int64_t divisor) {
    if (divisor == 0)
        return 0;
    long double result = ((long double)value * (long double)scale) / (long double)divisor;
    if (result > (long double)INT64_MAX)
        return INT64_MAX;
    if (result < (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)result;
}

/// @brief Linear interpolation between @p a and @p b by @p per_mille / 1000
///        (0..1000), saturating to int64.
/// @param a Segment start coordinate.
/// @param b Segment end coordinate.
/// @param per_mille Interpolation fraction in thousandths.
/// @return Truncated interpolated coordinate, saturated to the int64 range.
static int64_t pathfollow_lerp(int64_t a, int64_t b, int64_t per_mille) {
    long double value =
        (long double)a + ((long double)b - (long double)a) * ((long double)per_mille / 1000.0L);
    if (value > (long double)INT64_MAX)
        return INT64_MAX;
    if (value < (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)value;
}

/// @brief Euclidean distance between two fixed-point points.
/// @param x1 First point X.
/// @param y1 First point Y.
/// @param x2 Second point X.
/// @param y2 Second point Y.
/// @return Distance rounded to the nearest fixed-point integer and saturated
///         at INT64_MAX.
/// Uses long-double arithmetic so very short segments remain non-zero while
/// still avoiding overflow for large coordinates.
static int64_t distance(int64_t x1, int64_t y1, int64_t x2, int64_t y2) {
    long double dx = (long double)x2 - (long double)x1;
    long double dy = (long double)y2 - (long double)y1;
    long double len = sqrtl(dx * dx + dy * dy);
    if (len >= (long double)INT64_MAX)
        return INT64_MAX;
    return (int64_t)(len + 0.5L);
}

/// @brief Reset traversal position and fractional state to the first waypoint.
/// @param path Follower to rewind; `NULL` is a no-op.
/// @details Direction becomes forward and both carried remainders are cleared.
///          An empty follower rewinds to coordinate `(0,0)`.
static void rt_pathfollow_rewind(rt_pathfollow path) {
    if (!path)
        return;

    path->segment = 0;
    path->segment_progress = 0;
    path->move_remainder = 0;
    path->progress_remainder = 0;
    path->reverse = 0;

    if (path->point_count > 0) {
        path->current_x = path->points[0].x;
        path->current_y = path->points[0].y;
    } else {
        path->current_x = 0;
        path->current_y = 0;
    }
}

/// @brief Step to the next/previous path segment when progress reaches an end,
///        applying loop / ping-pong (reverse) / clamp end behavior.
/// @param path Active follower positioned at a segment boundary.
/// @details A mode transition may advance the segment, wrap, reverse direction,
///          or mark an ONCE traversal finished and inactive. Progress remainder
///          is cleared because it was measured against the old segment length.
static void rt_pathfollow_advance_segment_boundary(rt_pathfollow path) {
    int64_t seg = path->segment;

    // A new segment has a different length, so the sub-per-mille progress
    // remainder accumulated against the old segment no longer applies.
    path->progress_remainder = 0;

    if (path->reverse) {
        if (seg > 0) {
            path->segment--;
            path->segment_progress = 1000;
        } else {
            path->segment_progress = 0;
            if (path->mode == RT_PATHFOLLOW_PINGPONG) {
                path->reverse = 0;
            } else if (path->mode == RT_PATHFOLLOW_LOOP) {
                path->segment = path->point_count - 2;
                path->segment_progress = 1000;
            } else {
                path->finished = 1;
                path->active = 0;
            }
        }
    } else {
        if (seg < path->point_count - 2) {
            path->segment++;
            path->segment_progress = 0;
        } else {
            path->segment_progress = 1000;
            if (path->mode == RT_PATHFOLLOW_PINGPONG) {
                path->reverse = 1;
            } else if (path->mode == RT_PATHFOLLOW_LOOP) {
                path->segment = 0;
                path->segment_progress = 0;
            } else {
                path->finished = 1;
                path->active = 0;
            }
        }
    }
}

/// @brief Rebuild the cached `segment_lengths` array and `total_length` from the waypoint list.
/// @param path Follower whose cache is rebuilt.
/// Called lazily by ensure_lengths() after one or more points are added. Frees and re-mallocs the
/// array (no in-place realloc). For paths with fewer than 2 points the cache is cleared.
/// @return 1 on success (cache is current), 0 if the array could not be allocated — in which case
///         the previous valid cache is preserved so a retry can succeed later (VDOC-281).
static int8_t recalculate_lengths(rt_pathfollow path) {
    if (!path)
        return 0;
    if (path->point_count < 2) {
        if (path->segment_lengths) {
            free(path->segment_lengths);
            path->segment_lengths = NULL;
        }
        path->total_length = 0;
        return 1;
    }

    int64_t segments = path->point_count - 1;
    int64_t *new_lengths = malloc((size_t)segments * sizeof(int64_t));
    if (!new_lengths)
        return 0; // preserve the previous cache and total_length; caller retries

    if (path->segment_lengths)
        free(path->segment_lengths);
    path->segment_lengths = new_lengths;

    path->total_length = 0;
    for (int64_t i = 0; i < segments; i++) {
        path->segment_lengths[i] = distance(
            path->points[i].x, path->points[i].y, path->points[i + 1].x, path->points[i + 1].y);
        path->total_length =
            pathfollow_saturating_add(path->total_length, path->segment_lengths[i]);
    }
    return 1;
}

/// @brief Rebuild the length cache only if a point was added since the last build.
/// @param path Follower whose dirty cache may be rebuilt; `NULL` is a no-op.
/// @details Lazy recomputation turns N incremental add_point calls from O(n^2) (a
///          full rebuild on every add) into O(n) (one rebuild on the first query
///          after the adds). Must be called by every reader of segment_lengths /
///          total_length before it touches the cache.
static void ensure_lengths(rt_pathfollow path) {
    if (path && path->lengths_dirty) {
        // Clear the dirty flag only on success; a failed (allocation-starved) build
        // stays dirty so the next reader retries instead of caching a broken state
        // permanently (VDOC-281).
        if (recalculate_lengths(path))
            path->lengths_dirty = 0;
    }
}

/// @brief Free the lazily allocated segment-length cache during finalization.
/// @param obj PathFollower object supplied by the runtime finalizer system.
/// @details Waypoints are inline and need no separate teardown.
static void pathfollow_finalize(void *obj) {
    struct rt_pathfollow_impl *path = (struct rt_pathfollow_impl *)obj;
    if (path && path->segment_lengths)
        free(path->segment_lengths);
}

/// @brief Construct an empty PathFollower with no waypoints, ONCE mode, and a default speed of
/// 100 world units / second (i.e. 100000 in fixed-point). Caller adds points via `add_point`.
/// Returns a GC-managed handle wired to `pathfollow_finalize`; NULL on allocation failure.
/// @return A new runtime-managed PathFollower reference, or `NULL` on
///         allocation failure.
/// @details The follower begins inactive at `(0,0)` with no cached lengths.
rt_pathfollow rt_pathfollow_new(void) {
    struct rt_pathfollow_impl *path = (struct rt_pathfollow_impl *)rt_obj_new_i64(
        RT_PATHFOLLOW_CLASS_ID, (int64_t)sizeof(struct rt_pathfollow_impl));
    if (!path)
        return NULL;

    memset(path, 0, sizeof(struct rt_pathfollow_impl));
    path->speed = 100000; // Default: 100 units/sec
    path->mode = RT_PATHFOLLOW_ONCE;
    rt_obj_set_finalizer(path, pathfollow_finalize);
    return path;
}

/// @brief Release the PathFollower; frees the segment-length cache via the finalizer when the
/// reference count reaches zero.
/// @param path Handle to release; `NULL` is a no-op.
/// @details A non-null handle of another runtime class raises a trap.
void rt_pathfollow_destroy(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Destroy: expected Zanna.Game.PathFollower");
    if (!path)
        return;

    if (rt_obj_release_check0(path))
        rt_obj_free(path);
}

/// @brief Reset the follower to a blank slate: no waypoints, no progress, inactive, freed cache.
/// Useful for re-using a PathFollower handle across multiple level loads.
/// @param path Follower to clear; `NULL` is a no-op.
/// @details Mode and speed are preserved. Current position becomes `(0,0)`;
///          active, finished, and reverse state are cleared. A non-null
///          wrong-class handle raises a runtime trap.
void rt_pathfollow_clear(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Clear: expected Zanna.Game.PathFollower");
    if (!path)
        return;

    path->point_count = 0;
    path->segment = 0;
    path->segment_progress = 0;
    path->move_remainder = 0;
    path->progress_remainder = 0;
    path->current_x = 0;
    path->current_y = 0;
    path->active = 0;
    path->finished = 0;
    path->reverse = 0;
    path->total_length = 0;

    if (path->segment_lengths) {
        free(path->segment_lengths);
        path->segment_lengths = NULL;
    }
}

/// @brief Append a waypoint at fixed-point world coords (x, y). The first added point also becomes
/// the follower's starting position. Returns 0 if the cap (`RT_PATHFOLLOW_MAX_POINTS`) is hit.
/// Marks the segment cache dirty; the next length-dependent operation rebuilds it in O(n).
/// @param path Follower to modify.
/// @param x Fixed-point X coordinate where 1000 equals one world unit.
/// @param y Fixed-point Y coordinate where 1000 equals one world unit.
/// @return `1` when appended, or `0` for a null/full follower.
/// @details Existing traversal state is otherwise preserved. A non-null
///          wrong-class handle raises a runtime trap.
int8_t rt_pathfollow_add_point(rt_pathfollow path, int64_t x, int64_t y) {
    path = checked_pathfollow(path, "PathFollower.AddPoint: expected Zanna.Game.PathFollower");
    if (!path)
        return 0;
    if (path->point_count >= RT_PATHFOLLOW_MAX_POINTS)
        return 0;

    path->points[path->point_count].x = x;
    path->points[path->point_count].y = y;
    path->point_count++;

    // Set initial position if this is the first point
    if (path->point_count == 1) {
        path->current_x = x;
        path->current_y = y;
    }

    // Defer the O(n) length rebuild to the next query (see ensure_lengths) so a
    // path built with N add_point calls costs O(n) total instead of O(n^2).
    path->lengths_dirty = 1;
    return 1;
}

/// @brief Count stored waypoints.
/// @param path Follower to query.
/// @return Count in `[0, RT_PATHFOLLOW_MAX_POINTS]`, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_point_count(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.PointCount: expected Zanna.Game.PathFollower");
    return path ? path->point_count : 0;
}

/// @brief Select traversal mode: ONCE (stops at end), LOOP (wraps to start), PINGPONG (reverses).
/// Out-of-range values are silently ignored (mode is preserved).
/// @param path Follower to modify; `NULL` is a no-op.
/// @param mode One of rt_pathfollow_mode_t's integer values.
/// @details Changing mode does not rewind or change current active/finished
///          state. A non-null wrong-class handle raises a runtime trap.
void rt_pathfollow_set_mode(rt_pathfollow path, int64_t mode) {
    path = checked_pathfollow(path, "PathFollower.SetMode: expected Zanna.Game.PathFollower");
    if (!path)
        return;
    if (mode >= RT_PATHFOLLOW_ONCE && mode <= RT_PATHFOLLOW_PINGPONG)
        path->mode = (rt_pathfollow_mode_t)mode;
}

/// @brief Read the current traversal mode.
/// @param path Follower to query.
/// @return An rt_pathfollow_mode_t integer, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_mode(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Mode: expected Zanna.Game.PathFollower");
    return path ? path->mode : 0;
}

/// @brief Set traversal speed in fixed-point world-units/second (×1000). Non-positive ignored.
/// Per-frame movement is computed as `(speed * dt_ms) / 1000`.
/// @param path Follower to modify.
/// @param speed Positive fixed-point units per second; nonpositive input is
///        ignored.
/// @details One world unit per second is represented by 1000. A non-null
///          wrong-class handle raises a runtime trap.
void rt_pathfollow_set_speed(rt_pathfollow path, int64_t speed) {
    path = checked_pathfollow(path, "PathFollower.SetSpeed: expected Zanna.Game.PathFollower");
    if (path && speed > 0)
        path->speed = speed;
}

/// @brief Read the configured traversal speed.
/// @param path Follower to query.
/// @return Fixed-point units per second, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_speed(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Speed: expected Zanna.Game.PathFollower");
    return path ? path->speed : 0;
}

/// @brief Begin (or resume) following the path. Requires at least 2 waypoints; otherwise no-op.
/// Clears the `finished` flag so a previously completed ONCE path can be replayed.
/// @param path Follower to start.
/// @details A finished follower rewinds before restarting; a paused follower
///          resumes at its current state. Null or fewer-than-two-point paths
///          are no-ops. A non-null wrong-class handle raises a runtime trap.
void rt_pathfollow_start(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Start: expected Zanna.Game.PathFollower");
    if (!path || path->point_count < 2)
        return;

    if (path->finished)
        rt_pathfollow_rewind(path);

    path->active = 1;
    path->finished = 0;
}

/// @brief Suspend traversal without changing position or progress.
/// @param path Follower to pause; `NULL` is a no-op.
/// @details Resume with rt_pathfollow_start(). A non-null wrong-class handle
///          raises a runtime trap.
void rt_pathfollow_pause(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Pause: expected Zanna.Game.PathFollower");
    if (path)
        path->active = 0;
}

/// @brief Stop and rewind to the first waypoint (segment=0, progress=0). Waypoints are preserved.
/// Differs from `pause()` (which keeps current position) and `clear()` (which discards waypoints).
/// @param path Follower to stop; `NULL` is a no-op.
/// @details Finished/reverse state and fractional remainders are cleared. A
///          non-null wrong-class handle raises a runtime trap.
void rt_pathfollow_stop(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Stop: expected Zanna.Game.PathFollower");
    if (!path)
        return;

    path->active = 0;
    path->finished = 0;
    rt_pathfollow_rewind(path);
}

/// @brief Test whether traversal updates are currently enabled.
/// @param path Follower to query.
/// @return `1` when active, or `0` when paused, stopped, finished, or null.
/// @details A non-null wrong-class handle raises a runtime trap.
int8_t rt_pathfollow_is_active(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.IsActive: expected Zanna.Game.PathFollower");
    return path ? path->active : 0;
}

/// @brief Returns 1 once a ONCE-mode path has reached its final waypoint. Always 0 for
/// LOOP/PINGPONG.
/// @param path Follower to query.
/// @return Stored finished flag, or zero for `NULL`.
/// @details Stop, clear, and restart clear the flag. A non-null wrong-class
///          handle raises a runtime trap.
int8_t rt_pathfollow_is_finished(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.IsFinished: expected Zanna.Game.PathFollower");
    return path ? path->finished : 0;
}

/// @brief Advance the follower by `dt` milliseconds. Spends `(speed * dt) / 1000` units of
/// distance, crossing segment boundaries as needed (a single update can traverse multiple short
/// segments). At end-of-path: ONCE → mark finished/inactive; LOOP → wrap to first segment; PINGPONG
/// → flip `reverse` flag and walk back. Recomputes `current_x/y` once at the end via linear interp
/// of
/// `(segment, segment_progress)`. Inactive/finished/empty paths early-out.
/// @param path Follower to advance.
/// @param dt Elapsed milliseconds; nonpositive values do nothing.
/// @details Fixed-point distance and per-segment per-mille fractions carry
///          remainders across calls. Movement may cross multiple segments,
///          skips zero-length segments with a cycle guard, and saturates an
///          overflowing distance numerator. A fully degenerate path becomes
///          inactive; ONCE also becomes finished. Length-cache allocation
///          failure leaves traversal active for a later retry. A non-null
///          wrong-class handle raises a runtime trap.
void rt_pathfollow_update(rt_pathfollow path, int64_t dt) {
    path = checked_pathfollow(path, "PathFollower.Update: expected Zanna.Game.PathFollower");
    if (!path || !path->active || path->finished || path->point_count < 2)
        return;

    ensure_lengths(path);
    if (path->lengths_dirty) {
        // The length cache could not be allocated. Stay active (not finished) so a
        // later update retries once memory frees — an allocation failure must not
        // masquerade as a completed path (VDOC-281).
        return;
    }
    if (!path->segment_lengths || path->total_length == 0) {
        // Degenerate path: every waypoint coincides, so there is no distance to
        // cover. Complete immediately instead of staying active forever — a
        // once-mode loop waiting on IsFinished would otherwise hang (VDOC-281).
        path->active = 0;
        if (path->mode == RT_PATHFOLLOW_ONCE)
            path->finished = 1;
        return;
    }
    if (dt <= 0 || path->speed <= 0)
        return;

    // Distance to move this frame = speed(units/sec, fixed-point) * dt(ms) / 1000.
    // Carry the sub-unit remainder (in milliunits) across calls so slow speeds no
    // longer truncate every frame's fraction to zero and stall forever (VDOC-280).
    int64_t numerator;
    if (dt > (INT64_MAX - path->move_remainder) / path->speed)
        numerator = INT64_MAX; // saturate on overflow; the leftover fraction is negligible here
    else
        numerator = path->speed * dt + path->move_remainder;
    int64_t move_dist = numerator / 1000;
    path->move_remainder = numerator % 1000;
    int64_t zero_hops = 0;

    while (move_dist > 0 && !path->finished) {
        int64_t seg = path->segment;
        if (seg < 0 || seg >= path->point_count - 1) {
            path->active = 0;
            path->finished = 1;
            break;
        }
        int64_t seg_len = path->segment_lengths[seg];

        if (seg_len <= 0) {
            if (++zero_hops > path->point_count * 2) {
                path->active = 0;
                break;
            }
            rt_pathfollow_advance_segment_boundary(path);
            continue;
        }
        zero_hops = 0;

        // Calculate remaining distance in current segment
        int64_t seg_traveled = pathfollow_scaled(seg_len, path->segment_progress, 1000);
        int64_t seg_remaining = seg_len - seg_traveled;

        if (path->reverse) {
            // Moving backwards
            seg_remaining = seg_traveled;
        }

        if (move_dist >= seg_remaining) {
            // Move to next segment
            move_dist -= seg_remaining;
            rt_pathfollow_advance_segment_boundary(path);
        } else {
            // Partial movement within segment. Convert distance to per-mille
            // progress while carrying the sub-per-mille remainder across calls, so
            // slow movement over a long segment accumulates instead of truncating
            // to zero progress every frame (VDOC-280).
            long double pnum =
                (long double)move_dist * 1000.0L + (long double)path->progress_remainder;
            int64_t progress_delta;
            if (pnum >= (long double)INT64_MAX) {
                progress_delta = 1000; // saturate (never exceeds a full segment here)
                path->progress_remainder = 0;
            } else {
                int64_t pn = (int64_t)pnum;
                progress_delta = pn / seg_len;
                path->progress_remainder = pn % seg_len;
            }
            if (path->reverse)
                path->segment_progress -= progress_delta;
            else
                path->segment_progress += progress_delta;

            // Clamp
            if (path->segment_progress < 0)
                path->segment_progress = 0;
            if (path->segment_progress > 1000)
                path->segment_progress = 1000;
            move_dist = 0;
        }
    }

    // Update current position based on segment and progress
    if (path->segment < 0)
        path->segment = 0;
    if (path->segment > path->point_count - 2)
        path->segment = path->point_count - 2;
    int64_t seg = path->segment;
    int64_t p = path->segment_progress;
    int64_t x1 = path->points[seg].x;
    int64_t y1 = path->points[seg].y;
    int64_t x2 = path->points[seg + 1].x;
    int64_t y2 = path->points[seg + 1].y;

    path->current_x = pathfollow_lerp(x1, x2, p);
    path->current_y = pathfollow_lerp(y1, y2, p);
}

/// @brief Read the follower's current world X position (fixed-point ×1000). Caller divides by
/// 1000 to recover pixel coordinates.
/// @param path Follower to query.
/// @return Current interpolated X, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_x(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.X: expected Zanna.Game.PathFollower");
    return path ? path->current_x : 0;
}

/// @brief Read the follower's current world Y position (fixed-point ×1000).
/// @param path Follower to query.
/// @return Current interpolated Y, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_y(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Y: expected Zanna.Game.PathFollower");
    return path ? path->current_y : 0;
}

/// @brief Compute total path progress as a 0–1000 fraction (per mille). Sums fully-traversed
/// segment lengths plus the partial distance into the current segment, divides by `total_length`.
/// Returns 0 for empty/unbuilt paths.
/// @param path Follower to query.
/// @return Positional distance from the first waypoint divided by total path
///         length, in `[0,1000]`, or zero when unavailable.
/// @details For ping-pong motion this value decreases while traveling in
///          reverse; for loops it resets after wrapping. The cache is rebuilt
///          lazily. A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_progress(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Progress: expected Zanna.Game.PathFollower");
    if (!path || path->point_count < 2)
        return 0;
    ensure_lengths(path);
    if (path->total_length == 0)
        return 0;

    // Calculate total distance traveled
    int64_t traveled = 0;
    for (int64_t i = 0; i < path->segment; i++) {
        traveled = pathfollow_saturating_add(traveled, path->segment_lengths[i]);
    }
    traveled = pathfollow_saturating_add(
        traveled,
        pathfollow_scaled(path->segment_lengths[path->segment], path->segment_progress, 1000));

    return pathfollow_scaled(traveled, 1000, path->total_length);
}

/// @brief Teleport the follower to a fractional path position (0–1000 per mille). Walks the
/// segment cache until the accumulated distance covers `target_dist`, then sets segment +
/// progress and updates `current_x/y`. Useful for cinematic seeks or save-state restoration.
/// @param path Follower to reposition.
/// @param progress Requested path-distance fraction, clamped to `[0,1000]`.
/// @details Movement/progress remainders are cleared. Active, finished, mode,
///          and reverse state are preserved. Null, fewer-than-two-point, and
///          zero-total-length paths are no-ops. A non-null wrong-class handle
///          raises a runtime trap.
void rt_pathfollow_set_progress(rt_pathfollow path, int64_t progress) {
    path = checked_pathfollow(path, "PathFollower.SetProgress: expected Zanna.Game.PathFollower");
    if (!path || path->point_count < 2)
        return;
    ensure_lengths(path);
    if (path->total_length == 0)
        return;

    if (progress < 0)
        progress = 0;
    if (progress > 1000)
        progress = 1000;

    // A teleport starts fresh: discard carried movement/progress fractions.
    path->move_remainder = 0;
    path->progress_remainder = 0;

    // Find the segment for this progress
    int64_t target_dist = pathfollow_scaled(path->total_length, progress, 1000);
    int64_t accumulated = 0;

    for (int64_t i = 0; i < path->point_count - 1; i++) {
        if (pathfollow_saturating_add(accumulated, path->segment_lengths[i]) >= target_dist) {
            path->segment = i;
            int64_t seg_dist = target_dist - accumulated;
            if (path->segment_lengths[i] == 0)
                path->segment_progress = 0;
            else
                path->segment_progress =
                    pathfollow_scaled(seg_dist, 1000, path->segment_lengths[i]);
            break;
        }
        accumulated = pathfollow_saturating_add(accumulated, path->segment_lengths[i]);
    }

    // Update position
    int64_t seg = path->segment;
    int64_t p = path->segment_progress;
    path->current_x = pathfollow_lerp(path->points[seg].x, path->points[seg + 1].x, p);
    path->current_y = pathfollow_lerp(path->points[seg].y, path->points[seg + 1].y, p);
}

/// @brief Read the current segment index.
/// @param path Follower to query.
/// @return Index connecting `point[i]` to `point[i+1]`, or zero for `NULL`.
/// @details A non-null wrong-class handle raises a runtime trap.
int64_t rt_pathfollow_get_segment(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Segment: expected Zanna.Game.PathFollower");
    return path ? path->segment : 0;
}

/// @brief Get the approximate direction angle of the current path segment.
/// @param path Follower to query.
/// @return One of `0, 45000, ..., 315000` fixed-point degrees, or zero when
///         unavailable or the segment has coincident endpoints.
/// @note Returns one of 8 cardinal/ordinal directions (0, 45, 90, ..., 315 degrees).
///       For smoother rotation, use atan2 on consecutive positions instead.
/// @details Screen-space positive Y points down, so 90000 means down. Reverse
///          traversal flips the direction. A non-null wrong-class handle
///          raises a runtime trap.
int64_t rt_pathfollow_get_angle(rt_pathfollow path) {
    path = checked_pathfollow(path, "PathFollower.Angle: expected Zanna.Game.PathFollower");
    if (!path || path->point_count < 2)
        return 0;

    int64_t seg = path->segment;
    int64_t dx = (path->points[seg + 1].x > path->points[seg].x)
                     ? 1
                     : (path->points[seg + 1].x < path->points[seg].x ? -1 : 0);
    int64_t dy = (path->points[seg + 1].y > path->points[seg].y)
                     ? 1
                     : (path->points[seg + 1].y < path->points[seg].y ? -1 : 0);

    if (path->reverse) {
        dx = -dx;
        dy = -dy;
    }

    // Approximate atan2 using lookup or simple quadrant detection
    // For simplicity, return angle in 8 cardinal/ordinal directions
    // More precise: use a lookup table or CORDIC

    if (dx == 0 && dy == 0)
        return 0;

    // Simple 8-direction approximation
    // Right=0, Down=90, Left=180, Up=270
    if (dx > 0 && dy == 0)
        return 0;
    if (dx > 0 && dy > 0)
        return 45000; // 45 degrees
    if (dx == 0 && dy > 0)
        return 90000; // 90 degrees
    if (dx < 0 && dy > 0)
        return 135000; // 135 degrees
    if (dx < 0 && dy == 0)
        return 180000; // 180 degrees
    if (dx < 0 && dy < 0)
        return 225000; // 225 degrees
    if (dx == 0 && dy < 0)
        return 270000; // 270 degrees
    if (dx > 0 && dy < 0)
        return 315000; // 315 degrees

    return 0;
}
