//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_physics2d_collision.c
/// @brief Implements discrete and swept shape tests plus impulse-based
///        collision response for Physics2D.
///
/// @details Candidate body indices from the world's broad phase enter through
/// `maybe_resolve_pair`. This module applies bidirectional layer filtering,
/// dispatches AABB/circle overlap or continuous sweeps, records retained
/// contacts, rolls dynamic bodies back to swept impact positions, and resolves
/// velocity and penetration using restitution, Coulomb friction, and
/// Baumgarte-style positional correction.
///
// File: src/runtime/graphics/2d/rt_physics2d_collision.c
// Purpose: Broad- and narrow-phase collision detection plus impulse resolution
//          for the 2D physics world. Split out of rt_physics2d.c; operates on
//          the rt_world_impl/rt_body_impl types shared via
//          rt_physics2d_internal.h.
//
// Key invariants:
//   - maybe_resolve_pair is the single entry point the world step calls per
//     candidate body pair; all narrow-phase helpers are file-local.
//   - Collision math rejects non-finite manifolds (see world_record_contact)
//     so the growable contact list stays clean under degenerate input.
//   - Swept tests use previous-substep bounds (body_prev_*) to catch tunneling.
//
// Ownership/Lifetime:
//   - Collision helpers borrow world/body pointers. Successful contact records
//     retain both bodies until the world clears its per-step contact array.
//
// Links: src/runtime/graphics/2d/rt_physics2d.c (world/body/contact lifecycle + API),
//        src/runtime/graphics/2d/rt_physics2d_joint.c (joint solver),
//        src/runtime/graphics/2d/rt_physics2d_internal.h (shared types)
//
//===----------------------------------------------------------------------===//

#include "rt_physics2d.h"
#include "rt_physics2d_internal.h"
#include "rt_physics2d_joint.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @name Contact stabilization constants
/// @{
#define PHYSICS2D_CONTACT_SLOP 0.01
#define PHYSICS2D_CONTACT_CORRECTION_PERCENT 0.4

/* Approach speed (world units/sec) below which restitution is suppressed. Without
 * this, a body resting on the floor keeps re-bouncing the tiny velocity gravity
 * adds each frame (v = g*dt ≈ 0.16 at 60Hz) forever, since there is no sleeping
 * system to hide the jitter. Matches Box2D's b2_velocityThreshold default. */
#define PHYSICS2D_RESTITUTION_THRESHOLD 1.0
/// @}

//=============================================================================
// Narrow-phase forward declarations (all file-local)
//=============================================================================

/// @name File-local narrow-phase operations
/// @{
static int8_t aabb_overlap(rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *pen);
static int8_t shape_overlap(rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *pen);
static int8_t swept_bounds_pair(
    rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *entry);
static void resolve_collision(rt_body_impl *a, rt_body_impl *b, double nx, double ny, double pen);
/// @}

/// @brief Filter, detect, record, and resolve one broad-phase candidate pair.
/// @details Discrete overlap takes precedence over swept collision. Swept hits
///          record zero penetration and leave dynamic bodies at their
///          time-of-impact positions so untested remainder motion cannot tunnel
///          through a second obstacle. Bodies reaching either resolution path
///          are sanitized afterward. @p dt is retained for solver API
///          compatibility but is currently unused.
/// @param w Mutable world owning the body array and contact list.
/// @param ii First candidate body-array index.
/// @param jj Second candidate body-array index.
/// @param dt Current substep duration, currently unused.
void maybe_resolve_pair(rt_world_impl *w, int ii, int jj, double dt) {
    (void)dt; /* time-of-impact rollback no longer advances by the remainder */
    if (!w || ii < 0 || jj < 0 || ii >= w->body_count || jj >= w->body_count)
        return;

    rt_body_impl *bi = w->bodies[ii];
    rt_body_impl *bj = w->bodies[jj];
    if (!bi || !bj)
        return;

    if (!((bi->collision_layer & bj->collision_mask) && (bj->collision_layer & bi->collision_mask)))
        return;

    double nx, ny, pen, entry;
    if (shape_overlap(bi, bj, &nx, &ny, &pen)) {
        world_record_contact(w, bi, bj, nx, ny, pen);
        resolve_collision(bi, bj, nx, ny, pen);
    } else if (swept_bounds_pair(bi, bj, &nx, &ny, &entry)) {
        (void)entry;
        world_record_contact(w, bi, bj, nx, ny, 0.0);
        resolve_collision(bi, bj, nx, ny, 0.0);
        /* Both bodies are left at their time-of-impact positions. We intentionally
         * do NOT advance them by the post-impulse velocity for the remaining
         * (1-entry) fraction: that advance was never collision-tested, and because
         * the per-step pair-dedup skips any pair already resolved this step, a
         * reflected body could slide straight through a wall it was heading toward
         * (secondary tunnelling). Dropping the sub-step remainder is the safe
         * trade-off; the next step continues cleanly from the contact position. */
    }
    sanitize_body_state(bi);
    sanitize_body_state(bj);
}

//=============================================================================
// Collision detection and resolution
//=============================================================================

/// @brief Tests whether two AABB bodies overlap and computes the contact
///   manifold (normal direction and penetration depth).
///
/// Uses the Separating Axis Theorem (SAT) for AABBs. Computes the overlap on
/// each axis and selects the axis with the smallest overlap as the contact
/// normal. The normal always points from body A toward body B.
///
/// @param a       First body.
/// @param b       Second body.
/// @param nx      Output: contact normal X component (±1 or 0).
/// @param ny      Output: contact normal Y component (±1 or 0).
/// @param pen     Output: penetration depth along the contact normal.
/// @return 1 if the bodies overlap, 0 if they are separated.
static int8_t aabb_overlap(rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *pen) {
    double ax1 = a->x, ay1 = a->y;
    double ax2 = a->x + a->w, ay2 = a->y + a->h;
    double bx1 = b->x, by1 = b->y;
    double bx2 = b->x + b->w, by2 = b->y + b->h;
    double ox, oy;

    if (ax2 <= bx1 || bx2 <= ax1 || ay2 <= by1 || by2 <= ay1)
        return 0;

    /* Minimum translation distance on each axis, paired with the sign that ejects
     * B (A->B normal convention). Each axis has two candidate exits: push B toward
     * +axis (ax2 - b_min) or toward -axis (b_max - ax1); the smaller is the true
     * MTV. For a partial overlap this equals the intersection width and agrees with
     * the old center-comparison sign; for containment (one box's extent inside the
     * other's) the naive intersection-width formula returns the contained box's
     * full width instead of the nearest exit distance, mispairing depth and normal
     * and causing violent Baumgarte pops — this form selects the correct exit. */
    double ox_pos = ax2 - bx1; /* eject B toward +x */
    double ox_neg = bx2 - ax1; /* eject B toward -x */
    double ox_sign;
    if (ox_pos < ox_neg) {
        ox = ox_pos;
        ox_sign = 1.0;
    } else {
        ox = ox_neg;
        ox_sign = -1.0;
    }
    double oy_pos = ay2 - by1; /* eject B toward +y */
    double oy_neg = by2 - ay1; /* eject B toward -y */
    double oy_sign;
    if (oy_pos < oy_neg) {
        oy = oy_pos;
        oy_sign = 1.0;
    } else {
        oy = oy_neg;
        oy_sign = -1.0;
    }

    /* Use minimum overlap axis as contact normal (minimum translation vector) */
    if (ox < oy) {
        *pen = ox;
        *ny = 0.0;
        *nx = ox_sign;
    } else {
        *pen = oy;
        *nx = 0.0;
        *ny = oy_sign;
    }
    return 1;
}

/// @brief Narrow-phase overlap test for two circle bodies.
/// @details Computes the distance between centres; if less than the sum of radii the
///   circles overlap.  Contact normal is the centre-to-centre direction; a degenerate
///   case (centres coincident, dist < 1e-12) emits a default +X normal to avoid a
///   division-by-zero.
/// @param a First circle body.
/// @param b Second circle body.
/// @param nx Receives the unit normal X component from @p a toward @p b.
/// @param ny Receives the unit normal Y component from @p a toward @p b.
/// @param pen Receives the positive radial penetration depth.
/// @return `1` for strict overlap, otherwise `0`; tangency is not overlap.
static int8_t circle_circle_overlap(
    rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *pen) {
    double dx = b->x - a->x;
    double dy = b->y - a->y;
    double radii = a->radius + b->radius;
    double dist_sq = dx * dx + dy * dy;

    if (dist_sq >= radii * radii)
        return 0;

    if (dist_sq < 1e-12) {
        *nx = 1.0;
        *ny = 0.0;
        *pen = radii;
        return 1;
    }

    double dist = sqrt(dist_sq);
    *nx = dx / dist;
    *ny = dy / dist;
    *pen = radii - dist;
    return 1;
}

/// @brief Narrow-phase overlap test for a circle body vs. an AABB body.
/// @details Finds the closest point on the rectangle to the circle's centre, then
///   checks whether the distance to that point is less than the circle's radius.
///   When the circle centre is inside the rectangle (dist_sq ≈ 0), selects the
///   nearest exit face using four edge distances and produces an inside-out normal
///   with a penetration depth that moves the circle fully outside.
/// @param circle First body using circle geometry.
/// @param rect Second body using AABB geometry.
/// @param nx Receives the unit normal X component from circle toward rectangle.
/// @param ny Receives the unit normal Y component from circle toward rectangle.
/// @param pen Receives the positive minimum-translation penetration depth.
/// @return `1` for strict overlap, otherwise `0`; tangency is not overlap.
static int8_t circle_aabb_overlap(
    rt_body_impl *circle, rt_body_impl *rect, double *nx, double *ny, double *pen) {
    double rx1 = rect->x;
    double ry1 = rect->y;
    double rx2 = rect->x + rect->w;
    double ry2 = rect->y + rect->h;
    double cx = circle->x;
    double cy = circle->y;
    double closest_x = cx < rx1 ? rx1 : (cx > rx2 ? rx2 : cx);
    double closest_y = cy < ry1 ? ry1 : (cy > ry2 ? ry2 : cy);
    double dx = closest_x - cx;
    double dy = closest_y - cy;
    double dist_sq = dx * dx + dy * dy;
    double radius = circle->radius;

    if (dist_sq >= radius * radius)
        return 0;

    if (dist_sq > 1e-12) {
        double dist = sqrt(dist_sq);
        *nx = dx / dist;
        *ny = dy / dist;
        *pen = radius - dist;
        return 1;
    }

    /* Circle center is inside the rectangle. Choose the nearest exit side and
     * use the minimum translation vector that moves the circle fully outside. */
    double left = cx - rx1;
    double right = rx2 - cx;
    double top = cy - ry1;
    double bottom = ry2 - cy;
    double best = left;
    *nx = 1.0;
    *ny = 0.0;

    if (right < best) {
        best = right;
        *nx = -1.0;
        *ny = 0.0;
    }
    if (top < best) {
        best = top;
        *nx = 0.0;
        *ny = 1.0;
    }
    if (bottom < best) {
        best = bottom;
        *nx = 0.0;
        *ny = -1.0;
    }
    *pen = radius + best;
    return 1;
}

/// @brief Dispatch the correct narrow-phase overlap test based on each body's shape type.
/// @details Handles the three possible pairings: circle/circle, circle/AABB, and AABB/AABB.
///   For circle-vs-AABB where the circle is body B, the normal is negated after the call
///   to restore the A-to-B direction convention.
/// @param a First sanitized body.
/// @param b Second sanitized body.
/// @param nx Receives the contact-normal X component from @p a toward @p b.
/// @param ny Receives the contact-normal Y component from @p a toward @p b.
/// @param pen Receives the penetration depth.
/// @return `1` when the current shapes strictly overlap, otherwise `0`.
static int8_t shape_overlap(rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *pen) {
    if (!a || !b || !nx || !ny || !pen)
        return 0;

    if (a->is_circle && b->is_circle)
        return circle_circle_overlap(a, b, nx, ny, pen);

    if (a->is_circle && !b->is_circle)
        return circle_aabb_overlap(a, b, nx, ny, pen);

    if (!a->is_circle && b->is_circle) {
        int8_t hit = circle_aabb_overlap(b, a, nx, ny, pen);
        if (hit) {
            *nx = -*nx;
            *ny = -*ny;
        }
        return hit;
    }

    return aabb_overlap(a, b, nx, ny, pen);
}

/// @brief Compute the entry and exit times for two 1D intervals moving with relative velocity
/// `rel`.
/// @details Part of the continuous collision detection (CCD) swept AABB test.  When `rel`
///   is zero, the intervals are either always overlapping (returns 1, entry=-inf, exit=+inf)
///   or always separated (returns 0).  Used by swept_bounds_pair to check each axis
///   independently before combining via the max(entry)/min(exit) convention.
/// @param a_min Previous minimum coordinate of interval A.
/// @param a_max Previous maximum coordinate of interval A.
/// @param b_min Previous minimum coordinate of interval B.
/// @param b_max Previous maximum coordinate of interval B.
/// @param rel Relative displacement of A with respect to B over the substep.
/// @param entry Receives the normalized axis entry time.
/// @param exit Receives the normalized axis exit time.
/// @return `1` when the axis permits an overlap interval, otherwise `0`.
static int8_t swept_axis(double a_min,
                         double a_max,
                         double b_min,
                         double b_max,
                         double rel,
                         double *entry,
                         double *exit) {
    if (rel > 0.0) {
        *entry = (b_min - a_max) / rel;
        *exit = (b_max - a_min) / rel;
        return 1;
    }
    if (rel < 0.0) {
        *entry = (b_max - a_min) / rel;
        *exit = (b_min - a_max) / rel;
        return 1;
    }

    if (a_max <= b_min || b_max <= a_min)
        return 0;
    *entry = -INFINITY;
    *exit = INFINITY;
    return 1;
}

/// @brief Perform continuous collision detection for two moving circles.
/// @details Solves the relative-motion quadratic for the earliest surface
///          intersection in normalized substep time `[0, 1]`. Initially
///          overlapping circles and negligible/non-finite relative motion are
///          rejected because the discrete path handles overlap. On success,
///          dynamic bodies are rolled back to their impact positions.
/// @param a First circle body.
/// @param b Second circle body.
/// @param nx Receives the unit impact-normal X component from @p a toward @p b.
/// @param ny Receives the unit impact-normal Y component from @p a toward @p b.
/// @param entry_out Receives normalized time of impact in `[0, 1]`.
/// @return `1` for a swept impact, otherwise `0`.
static int8_t swept_circle_circle_pair(
    rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *entry_out) {
    double adx = a->x - a->prev_x;
    double ady = a->y - a->prev_y;
    double bdx = b->x - b->prev_x;
    double bdy = b->y - b->prev_y;
    double px = a->prev_x - b->prev_x;
    double py = a->prev_y - b->prev_y;
    double vx = adx - bdx;
    double vy = ady - bdy;
    double radii = a->radius + b->radius;
    double aa = vx * vx + vy * vy;
    double bb = 2.0 * (px * vx + py * vy);
    double cc = px * px + py * py - radii * radii;
    if (aa < 1e-18 || !isfinite(aa) || !isfinite(bb) || !isfinite(cc) || cc < 0.0)
        return 0;
    double disc = bb * bb - 4.0 * aa * cc;
    if (disc < 0.0 || !isfinite(disc))
        return 0;
    double t = (-bb - sqrt(disc)) / (2.0 * aa);
    if (t < 0.0 || t > 1.0 || !isfinite(t))
        return 0;

    double ahx = a->prev_x + adx * t;
    double ahy = a->prev_y + ady * t;
    double bhx = b->prev_x + bdx * t;
    double bhy = b->prev_y + bdy * t;
    double hx = bhx - ahx;
    double hy = bhy - ahy;
    double len = sqrt(hx * hx + hy * hy);
    if (len < 1e-12) {
        *nx = 1.0;
        *ny = 0.0;
    } else {
        *nx = hx / len;
        *ny = hy / len;
    }

    if (a->inv_mass > 0.0) {
        a->x = ahx;
        a->y = ahy;
    }
    if (b->inv_mass > 0.0) {
        b->x = bhx;
        b->y = bhy;
    }
    *entry_out = t;
    return 1;
}

/// @brief Swept (continuous) collision of a moving point against a static AABB.
/// @details Returns the entry time t in [0,1] and contact normal if the point's
///          motion this step crosses the box; used so fast bodies don't tunnel.
/// @param px Point X coordinate at the start of the substep.
/// @param py Point Y coordinate at the start of the substep.
/// @param vx Point displacement along X over the substep.
/// @param vy Point displacement along Y over the substep.
/// @param rx1 Rectangle minimum X.
/// @param ry1 Rectangle minimum Y.
/// @param rx2 Rectangle maximum X.
/// @param ry2 Rectangle maximum Y.
/// @param nx Receives the axis-aligned impact-normal X component.
/// @param ny Receives the axis-aligned impact-normal Y component.
/// @param entry_out Receives normalized entry time in `[0, 1]`.
/// @return Non-zero on a hit (out params written), 0 otherwise.
static int8_t swept_point_aabb(double px,
                               double py,
                               double vx,
                               double vy,
                               double rx1,
                               double ry1,
                               double rx2,
                               double ry2,
                               double *nx,
                               double *ny,
                               double *entry_out) {
    double x_entry, x_exit, y_entry, y_exit;
    if (!swept_axis(px, px, rx1, rx2, vx, &x_entry, &x_exit) ||
        !swept_axis(py, py, ry1, ry2, vy, &y_entry, &y_exit))
        return 0;
    double entry = x_entry > y_entry ? x_entry : y_entry;
    double exit = x_exit < y_exit ? x_exit : y_exit;
    if (entry > exit || entry < 0.0 || entry > 1.0 || !isfinite(entry))
        return 0;
    if (x_entry > y_entry) {
        *nx = vx > 0.0 ? 1.0 : -1.0;
        *ny = 0.0;
    } else {
        *nx = 0.0;
        *ny = vy > 0.0 ? 1.0 : -1.0;
    }
    *entry_out = entry;
    return 1;
}

/// @brief Swept collision of a moving circle against a (possibly moving) AABB,
///        reduced to a point-vs-expanded-AABB test on the relative motion.
/// @details Face candidates from the expanded box are geometrically checked
///          against the original rectangle, and corner-circle quadratics are
///          tested separately to reject rounded-corner false positives. On
///          success dynamic bodies are rolled back to the earliest impact.
/// @param circle First body using circle geometry.
/// @param rect Second body using AABB geometry.
/// @param nx Receives the impact-normal X component from circle toward box.
/// @param ny Receives the impact-normal Y component from circle toward box.
/// @param entry_out Receives normalized time of impact in `[0, 1]`.
/// @return Non-zero on a hit (entry time/normal written), 0 otherwise.
static int8_t swept_circle_aabb_pair(
    rt_body_impl *circle, rt_body_impl *rect, double *nx, double *ny, double *entry_out) {
    double cdx = circle->x - circle->prev_x;
    double cdy = circle->y - circle->prev_y;
    double rdx = rect->x - rect->prev_x;
    double rdy = rect->y - rect->prev_y;
    double rvx = cdx - rdx;
    double rvy = cdy - rdy;
    if (fabs(rvx) < 1e-12 && fabs(rvy) < 1e-12)
        return 0;

    double best_entry = INFINITY;
    double best_nx = 0.0;
    double best_ny = 0.0;
    double entry = 0.0;
    double cand_nx = 0.0;
    double cand_ny = 0.0;
    double rx1 = rect->prev_x;
    double ry1 = rect->prev_y;
    double rx2 = rect->prev_x + rect->w;
    double ry2 = rect->prev_y + rect->h;

    if (swept_point_aabb(circle->prev_x,
                         circle->prev_y,
                         rvx,
                         rvy,
                         rx1 - circle->radius,
                         ry1 - circle->radius,
                         rx2 + circle->radius,
                         ry2 + circle->radius,
                         &cand_nx,
                         &cand_ny,
                         &entry)) {
        double hx = circle->prev_x + rvx * entry;
        double hy = circle->prev_y + rvy * entry;
        double closest_x = hx < rx1 ? rx1 : (hx > rx2 ? rx2 : hx);
        double closest_y = hy < ry1 ? ry1 : (hy > ry2 ? ry2 : hy);
        double ddx = closest_x - hx;
        double ddy = closest_y - hy;
        if (ddx * ddx + ddy * ddy <= circle->radius * circle->radius + 1e-9) {
            best_entry = entry;
            best_nx = cand_nx;
            best_ny = cand_ny;
        }
    }

    double corners[4][2] = {{rx1, ry1}, {rx2, ry1}, {rx1, ry2}, {rx2, ry2}};
    double aa = rvx * rvx + rvy * rvy;
    if (aa > 1e-18 && isfinite(aa)) {
        for (int i = 0; i < 4; i++) {
            double px = circle->prev_x - corners[i][0];
            double py = circle->prev_y - corners[i][1];
            double bb = 2.0 * (px * rvx + py * rvy);
            double cc = px * px + py * py - circle->radius * circle->radius;
            double disc = bb * bb - 4.0 * aa * cc;
            if (disc < 0.0 || !isfinite(disc))
                continue;
            double t = (-bb - sqrt(disc)) / (2.0 * aa);
            if (t < 0.0 || t > 1.0 || t >= best_entry || !isfinite(t))
                continue;
            double hx = circle->prev_x + rvx * t;
            double hy = circle->prev_y + rvy * t;
            double nnx = corners[i][0] - hx;
            double nny = corners[i][1] - hy;
            double len = sqrt(nnx * nnx + nny * nny);
            if (len < 1e-12)
                continue;
            best_entry = t;
            best_nx = nnx / len;
            best_ny = nny / len;
        }
    }

    if (!isfinite(best_entry))
        return 0;
    entry = best_entry;
    *nx = best_nx;
    *ny = best_ny;

    if (circle->inv_mass > 0.0) {
        circle->x = circle->prev_x + cdx * entry;
        circle->y = circle->prev_y + cdy * entry;
    }
    if (rect->inv_mass > 0.0) {
        rect->x = rect->prev_x + rdx * entry;
        rect->y = rect->prev_y + rdy * entry;
    }
    *entry_out = entry;
    return 1;
}

/// @brief Swept collision between two AABBs using their relative velocity
///        and previous-substep interval bounds.
/// @details Combines per-axis entry/exit intervals and rolls dynamic bodies
///          back to the normalized impact time. Static bodies remain fixed.
/// @param a First AABB body.
/// @param b Second AABB body.
/// @param nx Receives the axis-aligned impact-normal X component.
/// @param ny Receives the axis-aligned impact-normal Y component.
/// @param entry_out Receives normalized time of impact in `[0, 1]`.
/// @return Nonzero on a swept hit, otherwise zero.
static int8_t swept_aabb_pair(
    rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *entry_out) {
    if (!a || !b || !nx || !ny || !entry_out)
        return 0;

    double adx = a->x - a->prev_x;
    double ady = a->y - a->prev_y;
    double bdx = b->x - b->prev_x;
    double bdy = b->y - b->prev_y;
    if (!isfinite(adx) || !isfinite(ady) || !isfinite(bdx) || !isfinite(bdy))
        return 0;

    double rvx = adx - bdx;
    double rvy = ady - bdy;
    if (fabs(rvx) < 1e-12 && fabs(rvy) < 1e-12)
        return 0;

    double x_entry, x_exit, y_entry, y_exit;
    if (!swept_axis(body_prev_min_x(a),
                    body_prev_max_x(a),
                    body_prev_min_x(b),
                    body_prev_max_x(b),
                    rvx,
                    &x_entry,
                    &x_exit) ||
        !swept_axis(body_prev_min_y(a),
                    body_prev_max_y(a),
                    body_prev_min_y(b),
                    body_prev_max_y(b),
                    rvy,
                    &y_entry,
                    &y_exit))
        return 0;

    double entry = x_entry > y_entry ? x_entry : y_entry;
    double exit = x_exit < y_exit ? x_exit : y_exit;
    if (entry > exit || entry < 0.0 || entry > 1.0 || !isfinite(entry))
        return 0;

    if (x_entry > y_entry) {
        *nx = rvx > 0.0 ? 1.0 : -1.0;
        *ny = 0.0;
    } else {
        *nx = 0.0;
        *ny = rvy > 0.0 ? 1.0 : -1.0;
    }

    if (a->inv_mass > 0.0) {
        a->x = a->prev_x + adx * entry;
        a->y = a->prev_y + ady * entry;
    }
    if (b->inv_mass > 0.0) {
        b->x = b->prev_x + bdx * entry;
        b->y = b->prev_y + bdy * entry;
    }
    *entry_out = entry;
    return 1;
}

/// @brief Swept test between two bodies, dispatching to the circle/AABB sweep
///        appropriate to their shapes; writes contact normal and entry time.
/// @param a First sanitized body.
/// @param b Second sanitized body.
/// @param nx Receives the impact-normal X component from @p a toward @p b.
/// @param ny Receives the impact-normal Y component from @p a toward @p b.
/// @param entry_out Receives normalized time of impact in `[0, 1]`.
/// @return Non-zero if the bodies collide during this step, 0 otherwise.
static int8_t swept_bounds_pair(
    rt_body_impl *a, rt_body_impl *b, double *nx, double *ny, double *entry_out) {
    if (!a || !b || !nx || !ny || !entry_out)
        return 0;

    if (a->is_circle && b->is_circle)
        return swept_circle_circle_pair(a, b, nx, ny, entry_out);

    if (a->is_circle && !b->is_circle)
        return swept_circle_aabb_pair(a, b, nx, ny, entry_out);

    if (!a->is_circle && b->is_circle) {
        int8_t hit = swept_circle_aabb_pair(b, a, nx, ny, entry_out);
        if (hit) {
            *nx = -*nx;
            *ny = -*ny;
        }
        return hit;
    }

    return swept_aabb_pair(a, b, nx, ny, entry_out);
}

/// @brief Resolves a collision between two bodies using impulse-based dynamics.
///
/// Implements the standard game-physics collision response algorithm:
///
///   1. **Early-out**: If both bodies are static (inv_mass == 0), nothing moves.
///   2. **Relative velocity check**: Compute the relative velocity along the
///      contact normal. If it is positive (separating), skip resolution — the
///      bodies are already moving apart.
///   3. **Restitution (bounce) impulse**: Apply an impulse J along the contact
///      normal using the formula: J = -(1 + e) * vel_along_n / (1/mA + 1/mB),
///      where e = min(restitution_A, restitution_B). This is the standard
///      coefficient-of-restitution formula for instantaneous collision response.
///   4. **Friction impulse (Coulomb model)**: Compute the tangential relative
///      velocity and apply a friction impulse clamped to J * mu, where
///      mu = (friction_A + friction_B) / 2 (averaged coefficient).
///   5. **Positional correction (Baumgarte)**: Gently push overlapping bodies
///      apart by 40% of the excess penetration (with a 0.01-world-unit slop
///      threshold) to prevent slow sinking without causing jitter.
///
/// @param a   First body. Modified in-place (velocity and position).
/// @param b   Second body. Modified in-place (velocity and position).
/// @param nx  Contact normal X (from A toward B, magnitude 1).
/// @param ny  Contact normal Y (from A toward B, magnitude 1).
/// @param pen Penetration depth along the contact normal.
static void resolve_collision(rt_body_impl *a, rt_body_impl *b, double nx, double ny, double pen) {
    double rvx, rvy, vel_along_n, total_inv, correction;

    /* Both static — neither body can move, skip entirely */
    if (a->inv_mass == 0.0 && b->inv_mass == 0.0)
        return;
    total_inv = a->inv_mass + b->inv_mass;
    if (total_inv <= 0.0 || !isfinite(total_inv))
        return;

    /* Relative velocity of B w.r.t. A along all axes */
    rvx = b->vx - a->vx;
    rvy = b->vy - a->vy;

    /* Project relative velocity onto the contact normal */
    vel_along_n = rvx * nx + rvy * ny;

    if (vel_along_n <= 0.0 && isfinite(vel_along_n)) {
        double e, j;

        /* Use the less elastic material's coefficient so a rubber ball bouncing on
         * concrete uses the concrete's zero restitution, not the ball's high one */
        e = a->restitution < b->restitution ? a->restitution : b->restitution;

        /* Suppress restitution for low-speed contacts so resting bodies settle
         * instead of jittering on the per-frame velocity gravity injects. */
        if (-vel_along_n < PHYSICS2D_RESTITUTION_THRESHOLD)
            e = 0.0;

        /* Scalar impulse magnitude. Derivation: we want the post-collision relative
         * velocity along n to equal -e * vel_along_n (restitution). Solving for j
         * gives: j = -(1+e)*vel_along_n / (1/mA + 1/mB) */
        j = -(1.0 + e) * vel_along_n / total_inv;

        /* Apply the normal impulse to each body proportional to its inverse mass */
        a->vx -= j * a->inv_mass * nx;
        a->vy -= j * a->inv_mass * ny;
        b->vx += j * b->inv_mass * nx;
        b->vy += j * b->inv_mass * ny;

        /* Friction impulse: computed from the post-normal-impulse relative
         * velocity in the tangent direction. */
        rvx = b->vx - a->vx;
        rvy = b->vy - a->vy;
        {
            double vel_n_after = rvx * nx + rvy * ny;
            double tx = rvx - vel_n_after * nx;
            double ty = rvy - vel_n_after * ny;
            double t_len = sqrt(tx * tx + ty * ty);
            if (t_len > 1e-9) {
                double mu, jt, vel_along_t;
                tx /= t_len; /* Normalise tangent */
                ty /= t_len;
                vel_along_t = rvx * tx + rvy * ty;
                mu = (a->friction + b->friction) * 0.5; /* Average both surfaces */
                jt = -vel_along_t / total_inv;
                /* Clamp to the Coulomb friction cone. j >= 0 here (vel_along_n <= 0
                 * above), so fabs(j) == j; using fabs keeps the cone well-formed even
                 * if a future caller reaches this path with a negative normal impulse
                 * (e.g. the swept-CCD entry where pen==0 but velocities are full). */
                double jmax = fabs(j) * mu;
                if (jt > jmax)
                    jt = jmax;
                else if (jt < -jmax)
                    jt = -jmax;
                a->vx -= jt * a->inv_mass * tx;
                a->vy -= jt * a->inv_mass * ty;
                b->vx += jt * b->inv_mass * tx;
                b->vy += jt * b->inv_mass * ty;
            }
        }
    }

    /* Positional correction (Baumgarte stabilisation): directly move bodies
     * apart to counter numerical drift that causes objects to slowly sink into
     * each other. A small slop is tolerated before correcting to avoid
     * jittering on resting contacts. The correction factor spreads the correction
     * over several frames rather than snapping immediately (prevents bouncing). */
    {
        correction = (pen - PHYSICS2D_CONTACT_SLOP > 0.0 ? pen - PHYSICS2D_CONTACT_SLOP : 0.0) *
                     PHYSICS2D_CONTACT_CORRECTION_PERCENT / total_inv;
        a->x -= correction * a->inv_mass * nx;
        a->y -= correction * a->inv_mass * ny;
        b->x += correction * b->inv_mass * nx;
        b->y += correction * b->inv_mass * ny;
    }
}
