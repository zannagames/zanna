//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/math/rt_spline.c
// Purpose: Spline curve interpolation for the Zanna.Spline class. Supports three
//   curve types over Vec2 control points: linear (piecewise straight segments),
//   Catmull-Rom (smooth curve through all control points), and cubic Bezier
//   (curve guided by explicit tangent handles). All splines are parameterized on
//   t in [0.0, 1.0] and return an interpolated Vec2 position or tangent.
//
// Key invariants:
//   - Control point coordinates (x, y) are stored as separate double arrays xs
//     and ys, extracted from the Vec2 sequence at construction time.
//   - Catmull-Rom uses the uniform 0.5-tangent basis and duplicates the first or
//     last control point as the out-of-range neighbor at each endpoint.
//   - Bezier evaluation uses the cubic Bernstein basis over exactly four points.
//   - Spline objects are immutable after construction; the control point arrays
//     are allocated with calloc and freed via the GC finalizer.
//   - Every spline kind clamps the parameter to [0,1] via spline_sanitize_param
//     (-Inf/below-0 -> 0, +Inf/above-1 -> 1, NaN -> 0), so evaluation is
//     well-defined for all kinds and all inputs (VDOC-201).
//
// Ownership/Lifetime:
//   - ZannaSpline structs are allocated via rt_obj_new_i64 (GC heap); the xs
//     and ys double arrays are malloc'd separately and freed in spline_finalizer,
//     registered as the GC finalizer at construction.
//
// Links: src/runtime/graphics/math/rt_spline.h (public API),
//        src/runtime/graphics/math/rt_vec2.h (Vec2 control point and return type),
//        src/runtime/collections/rt_seq.h (input sequence of Vec2 control points)
//
//===----------------------------------------------------------------------===//

#include "rt_spline.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_vec2.h"

#include <math.h>
#include <stdlib.h>

typedef enum { SPLINE_LINEAR = 0, SPLINE_CATMULL_ROM = 1, SPLINE_BEZIER = 2 } SplineKind;

typedef struct {
    SplineKind kind;
    int64_t count;
    double *xs;
    double *ys;
} ZannaSpline;

/// @brief GC finalizer for a ZannaSpline — frees the coordinate arrays.
/// @details `xs` and `ys` are heap-allocated by `spline_alloc` and must be freed
///   independently because the spline object itself is managed by the GC pool.
///   NULLing the pointers after free is defensive but harmless since the finalizer
///   is called exactly once before the object memory is reclaimed.
/// @param payload Spline payload being finalized; NULL is accepted as a no-op.
static void spline_finalizer(void *payload) {
    ZannaSpline *s = (ZannaSpline *)payload;
    if (s) {
        free(s->xs);
        free(s->ys);
        s->xs = NULL;
        s->ys = NULL;
    }
}

/// @brief Return whether @p spline is a Spline heap payload.
/// @details Requires the explicit Spline class id: unlike Vec3/Quat there is no legacy
///          classless layout to accept, and the struct holds raw pointers, so any
///          unrecognized payload must be rejected before its fields are dereferenced.
/// @param spline Candidate runtime object payload.
/// @return 1 for a compatible Spline payload, otherwise 0.
static int spline_is_compatible_object(void *spline) {
    if (!spline)
        return 0;
    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(spline, &heap_info))
        return 0;
    if (heap_info.kind != RT_HEAP_OBJECT || heap_info.elem_kind != RT_ELEM_NONE)
        return 0;
    return heap_info.class_id == RT_SPLINE_CLASS_ID &&
           heap_info.cap >= (uint64_t)sizeof(ZannaSpline);
}

/// @brief Validate and cast an opaque handle to a spline payload.
/// @details Rejects NULL, non-object heap payloads, and wrong-class handles before the
///          spline's kind/count/coordinate-array fields are read.
/// @param spline Candidate Spline runtime handle.
/// @param op Diagnostic prefix used if validation fails.
/// @return Typed spline payload, or NULL after trapping.
static ZannaSpline *spline_checked(void *spline, const char *op) {
    if (!spline_is_compatible_object(spline)) {
        rt_trap(op ? op : "Spline: invalid spline");
        return NULL;
    }
    return (ZannaSpline *)spline;
}

/// @brief Allocate a GC-managed spline object with `count` control point slots.
/// @details Allocates xs/ys double arrays via calloc and sets the spline kind.
///          On allocation failure, releases the partially-constructed object and traps.
/// @param kind Evaluation algorithm assigned to the spline.
/// @param count Number of x/y coordinate slots to allocate.
/// @return New ZannaSpline or NULL on OOM.
static ZannaSpline *spline_alloc(SplineKind kind, int64_t count) {
    ZannaSpline *s =
        (ZannaSpline *)rt_obj_new_i64(RT_SPLINE_CLASS_ID, (int64_t)sizeof(ZannaSpline));
    if (!s) {
        rt_trap("Spline: memory allocation failed");
        return NULL;
    }
    s->kind = kind;
    s->count = count;
    s->xs = (double *)calloc((size_t)count, sizeof(double));
    s->ys = (double *)calloc((size_t)count, sizeof(double));
    if (!s->xs || !s->ys) {
        free(s->xs);
        free(s->ys);
        rt_trap("Spline: memory allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(s, spline_finalizer);
    return s;
}

/// @brief Copy Vec2 coordinates from a Seq of points into the spline's xs/ys arrays.
/// @details Iterates up to `min(rt_seq_len(points), s->count)` entries, reading each
///   element as a Vec2. NULL elements (e.g., from a sparse Seq) leave the corresponding
///   xs/ys entries at their calloc-initialized zero, which is a safe but possibly
///   incorrect default — callers should ensure the Seq contains only valid Vec2 values.
/// @param points Runtime Seq object containing Vec2 elements.
/// @param s      Pre-allocated spline with xs/ys buffers of capacity s->count.
static void extract_points(void *points, ZannaSpline *s) {
    int64_t n = rt_seq_len(points);
    int64_t count = n < s->count ? n : s->count;
    for (int64_t i = 0; i < count; ++i) {
        void *pt = rt_seq_get(points, i);
        if (pt) {
            s->xs[i] = rt_vec2_x(pt);
            s->ys[i] = rt_vec2_y(pt);
        }
    }
}

//=============================================================================
// Constructors
//=============================================================================

/// @brief Create a Catmull-Rom spline through a sequence of Vec2 control points.
/// @details Uses uniform 0.5 tangents and interpolates every stored point with C1 continuity.
///          NULL input or fewer than two points raises a runtime trap.
/// @param points Runtime Seq containing at least two Vec2 handles.
/// @return New GC-managed Spline, or NULL after trapping for invalid input or allocation failure.
void *rt_spline_catmull_rom(void *points) {
    if (!points) {
        rt_trap("Spline.CatmullRom: null points");
        return NULL;
    }
    int64_t n = rt_seq_len(points);
    if (n < 2) {
        rt_trap("Spline.CatmullRom: need at least 2 points");
        return NULL;
    }
    ZannaSpline *s = spline_alloc(SPLINE_CATMULL_ROM, n);
    if (!s) // spline_alloc traps on OOM, but rt_trap is not _Noreturn under test hooks
        return NULL;
    extract_points(points, s);
    return s;
}

/// @brief Create a cubic Bezier spline from four Vec2 control points.
/// @details The curve interpolates @p p0 and @p p3 and is shaped by the interior handles @p p1 and
///          @p p2. Any NULL control point raises a runtime trap.
/// @param p0 Starting anchor point.
/// @param p1 First interior control handle.
/// @param p2 Second interior control handle.
/// @param p3 Ending anchor point.
/// @return New GC-managed Spline, or NULL after trapping for invalid input or allocation failure.
void *rt_spline_bezier(void *p0, void *p1, void *p2, void *p3) {
    if (!p0 || !p1 || !p2 || !p3) {
        rt_trap("Spline.Bezier: null control point");
        return NULL;
    }
    ZannaSpline *s = spline_alloc(SPLINE_BEZIER, 4);
    if (!s) // spline_alloc traps on OOM, but rt_trap is not _Noreturn under test hooks
        return NULL;
    s->xs[0] = rt_vec2_x(p0);
    s->ys[0] = rt_vec2_y(p0);
    s->xs[1] = rt_vec2_x(p1);
    s->ys[1] = rt_vec2_y(p1);
    s->xs[2] = rt_vec2_x(p2);
    s->ys[2] = rt_vec2_y(p2);
    s->xs[3] = rt_vec2_x(p3);
    s->ys[3] = rt_vec2_y(p3);
    return s;
}

/// @brief Create a piecewise-linear spline through a sequence of Vec2 points.
/// @details Joins adjacent control points with straight segments. NULL input or fewer than two
///          points raises a runtime trap.
/// @param points Runtime Seq containing at least two Vec2 handles.
/// @return New GC-managed Spline, or NULL after trapping for invalid input or allocation failure.
void *rt_spline_linear(void *points) {
    if (!points) {
        rt_trap("Spline.Linear: null points");
        return NULL;
    }
    int64_t n = rt_seq_len(points);
    if (n < 2) {
        rt_trap("Spline.Linear: need at least 2 points");
        return NULL;
    }
    ZannaSpline *s = spline_alloc(SPLINE_LINEAR, n);
    if (!s) // spline_alloc traps on OOM, but rt_trap is not _Noreturn under test hooks
        return NULL;
    extract_points(points, s);
    return s;
}

//=============================================================================
// Evaluation Helpers
//=============================================================================

/// @brief Evaluate a polyline spline at normalized parameter @p t in [0, 1].
/// @details Maps @p t to a continuous segment index via `t * (count - 1)`, then
///   linearly interpolates within the selected segment. Values outside [0, 1] are
///   clamped to the first or last point so evaluation beyond the ends is well-defined.
/// @param s  Linear spline with at least 2 control points.
/// @param t  Normalized curve parameter; 0 = first point, 1 = last point.
/// @param ox Output X coordinate.
/// @param oy Output Y coordinate.
static void eval_linear(ZannaSpline *s, double t, double *ox, double *oy) {
    if (t <= 0.0) {
        *ox = s->xs[0];
        *oy = s->ys[0];
        return;
    }
    if (t >= 1.0) {
        *ox = s->xs[s->count - 1];
        *oy = s->ys[s->count - 1];
        return;
    }
    double seg = t * (double)(s->count - 1);
    int64_t i = (int64_t)seg;
    if (i >= s->count - 1)
        i = s->count - 2;
    double f = seg - (double)i;
    *ox = s->xs[i] + (s->xs[i + 1] - s->xs[i]) * f;
    *oy = s->ys[i] + (s->ys[i + 1] - s->ys[i]) * f;
}

/// @brief Evaluate a cubic Bézier spline at normalized parameter @p t in [0, 1].
/// @details Uses the standard Bernstein basis: B(t) = (1-t)³P0 + 3(1-t)²tP1 +
///   3(1-t)t²P2 + t³P3. The curve interpolates P0 and P3 (endpoints) and is pulled
///   toward P1 and P2 (interior control handles) without passing through them.
/// @param s  Bezier spline with exactly 4 control points.
/// @param t  Normalized parameter in [0, 1].
/// @param ox Output X coordinate.
/// @param oy Output Y coordinate.
static void eval_bezier(ZannaSpline *s, double t, double *ox, double *oy) {
    double u = 1.0 - t;
    double u2 = u * u;
    double t2 = t * t;
    double a = u2 * u;
    double b = 3.0 * u2 * t;
    double c = 3.0 * u * t2;
    double d = t2 * t;
    *ox = a * s->xs[0] + b * s->xs[1] + c * s->xs[2] + d * s->xs[3];
    *oy = a * s->ys[0] + b * s->ys[1] + c * s->ys[2] + d * s->ys[3];
}

/// @brief Evaluate one segment of a Catmull-Rom spline at local parameter @p t in [0, 1].
/// @details The standard Catmull-Rom formula derived from the cubic Hermite basis:
///   the tangent at each interpolated point is `0.5 * (next - prev)`. Produces C¹
///   continuity across segment boundaries when adjacent segments share the same p1/p2
///   as neighbors' p2/p1. P0 and P3 are the ghost points (neighbors outside the
///   segment); p1 and p2 are the actual interpolated endpoints for this segment.
/// @param p0x Previous control point x coordinate.
/// @param p0y Previous control point y coordinate.
/// @param p1x Segment-start x coordinate.
/// @param p1y Segment-start y coordinate.
/// @param p2x Segment-end x coordinate.
/// @param p2y Segment-end y coordinate.
/// @param p3x Following control point x coordinate.
/// @param p3y Following control point y coordinate.
/// @param t Local segment parameter in [0, 1].
/// @param ox Output position x coordinate.
/// @param oy Output position y coordinate.
static void catmull_rom_segment(double p0x,
                                double p0y,
                                double p1x,
                                double p1y,
                                double p2x,
                                double p2y,
                                double p3x,
                                double p3y,
                                double t,
                                double *ox,
                                double *oy) {
    double t2 = t * t;
    double t3 = t2 * t;
    *ox = 0.5 * ((2.0 * p1x) + (-p0x + p2x) * t + (2.0 * p0x - 5.0 * p1x + 4.0 * p2x - p3x) * t2 +
                 (-p0x + 3.0 * p1x - 3.0 * p2x + p3x) * t3);
    *oy = 0.5 * ((2.0 * p1y) + (-p0y + p2y) * t + (2.0 * p0y - 5.0 * p1y + 4.0 * p2y - p3y) * t2 +
                 (-p0y + 3.0 * p1y - 3.0 * p2y + p3y) * t3);
}

/// @brief Evaluate a Catmull-Rom spline through all control points at parameter @p t.
/// @details Maps @p t to a segment index in the same way as `eval_linear`, then
///   constructs the four-point neighborhood for `catmull_rom_segment`. The endpoint
///   ghost points clamp to the first/last stored point rather than reflecting, which
///   produces a slightly tighter curve at the ends compared to reflective ghosts.
/// @param s  Catmull-Rom spline with at least 2 control points.
/// @param t  Normalized parameter in [0, 1].
/// @param ox Output X coordinate.
/// @param oy Output Y coordinate.
static void eval_catmull_rom(ZannaSpline *s, double t, double *ox, double *oy) {
    int64_t n = s->count;
    if (n < 2) {
        *ox = s->xs[0];
        *oy = s->ys[0];
        return;
    }
    if (t <= 0.0) {
        *ox = s->xs[0];
        *oy = s->ys[0];
        return;
    }
    if (t >= 1.0) {
        *ox = s->xs[n - 1];
        *oy = s->ys[n - 1];
        return;
    }
    double seg = t * (double)(n - 1);
    int64_t i = (int64_t)seg;
    if (i >= n - 1)
        i = n - 2;
    double f = seg - (double)i;

    int64_t i0 = i > 0 ? i - 1 : 0;
    int64_t i1 = i;
    int64_t i2 = i + 1;
    int64_t i3 = i + 2 < n ? i + 2 : n - 1;

    catmull_rom_segment(s->xs[i0],
                        s->ys[i0],
                        s->xs[i1],
                        s->ys[i1],
                        s->xs[i2],
                        s->ys[i2],
                        s->xs[i3],
                        s->ys[i3],
                        f,
                        ox,
                        oy);
}

//=============================================================================
// Tangent Helpers
//=============================================================================

/// @brief Compute the unnormalized tangent vector of a polyline at parameter @p t.
/// @details Returns the difference vector of the active segment (P[i+1] - P[i]), which
///   is constant within each segment. The magnitude equals the segment length so the
///   caller must normalize if a unit tangent is needed. There is no averaging at
///   segment joints — the tangent is discontinuous at those points.
/// @param s  Linear spline with at least 2 control points.
/// @param t  Normalized parameter in [0, 1].
/// @param ox Output tangent X component.
/// @param oy Output tangent Y component.
static void tangent_linear(ZannaSpline *s, double t, double *ox, double *oy) {
    int64_t n = s->count;
    double seg = t * (double)(n - 1);
    int64_t i = (int64_t)seg;
    if (i >= n - 1)
        i = n - 2;
    if (i < 0)
        i = 0;
    *ox = s->xs[i + 1] - s->xs[i];
    *oy = s->ys[i + 1] - s->ys[i];
}

/// @brief Compute the analytical tangent (derivative) of the cubic Bézier at @p t.
/// @details The derivative of B(t) is B'(t) = 3[(1-t)²(P1-P0) + 2t(1-t)(P2-P1) + t²(P3-P2)],
///   expanded here using the four Bernstein basis derivatives. The result is
///   unnormalized (magnitude varies with t); callers that need a unit tangent must
///   normalize the output.
/// @param s  Bezier spline with exactly 4 control points.
/// @param t  Normalized parameter in [0, 1].
/// @param ox Output tangent X component.
/// @param oy Output tangent Y component.
static void tangent_bezier(ZannaSpline *s, double t, double *ox, double *oy) {
    double u = 1.0 - t;
    double a = -3.0 * u * u;
    double b = 3.0 * u * u - 6.0 * u * t;
    double c = 6.0 * u * t - 3.0 * t * t;
    double d = 3.0 * t * t;
    *ox = a * s->xs[0] + b * s->xs[1] + c * s->xs[2] + d * s->xs[3];
    *oy = a * s->ys[0] + b * s->ys[1] + c * s->ys[2] + d * s->ys[3];
}

/// @brief Approximate the tangent of a Catmull-Rom spline via central-difference.
/// @details The analytical derivative of the Catmull-Rom basis exists but is complex
///   to implement correctly across ghost-point boundaries. A symmetric finite
///   difference with h=0.0001 is accurate enough for all current uses (orientation
///   of objects along a path) and avoids the branch-heavy boundary logic. The step
///   is halved at the endpoints (t=0 and t=1) where the symmetric window would
///   overshoot the range; the smaller asymmetric step introduces negligible error.
/// @param s  Catmull-Rom spline with at least 2 control points.
/// @param t  Normalized parameter in [0, 1].
/// @param ox Output tangent X component (not normalized).
/// @param oy Output tangent Y component (not normalized).
static void tangent_catmull_rom(ZannaSpline *s, double t, double *ox, double *oy) {
    /* Numerical derivative via central difference. */
    double h = 0.0001;
    double t0 = t - h;
    double t1 = t + h;
    if (t0 < 0.0)
        t0 = 0.0;
    if (t1 > 1.0)
        t1 = 1.0;
    double x0, y0, x1, y1;
    eval_catmull_rom(s, t0, &x0, &y0);
    eval_catmull_rom(s, t1, &x1, &y1);
    double dt = t1 - t0;
    if (dt == 0.0) {
        *ox = 0.0;
        *oy = 0.0;
        return;
    }
    *ox = (x1 - x0) / dt;
    *oy = (y1 - y0) / dt;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Normalize a spline parameter to one rule for every spline kind.
/// @details The parameter is clamped to [0,1]: `-Inf` and any value below 0 map
///          to the curve start (0), `+Inf` and any value above 1 map to the
///          curve end (1). NaN has no meaningful clamp target, so it maps to the
///          curve start (0). This gives all spline kinds — including Bezier,
///          which previously extrapolated — the same well-defined domain and
///          ensures no evaluator ever converts a NaN/Inf to a segment index,
///          which is undefined in C (VDOC-201).
/// @param t Raw curve parameter.
/// @return Finite parameter clamped to [0, 1], with NaN mapped to zero.
static double spline_sanitize_param(double t) {
    if (isnan(t))
        return 0.0;
    if (t < 0.0) // also catches -Inf
        return 0.0;
    if (t > 1.0) // also catches +Inf
        return 1.0;
    return t;
}

/// @brief Evaluate the spline at parameter `t`, normally in [0, 1], and return a fresh Vec2.
/// @details Every spline kind clamps `t` to [0,1]; a non-finite `t` maps to the
///          curve start (0). See `spline_sanitize_param` (VDOC-201).
/// @param spline Spline handle to evaluate.
/// @param t Curve parameter; finite values are clamped to [0, 1] and NaN maps to zero.
/// @return New Vec2 position, or NULL after trapping for an invalid spline or allocation failure.
void *rt_spline_eval(void *spline, double t) {
    ZannaSpline *s = spline_checked(spline, "Spline.Eval: invalid spline");
    if (!s)
        return NULL;
    t = spline_sanitize_param(t);
    double ox = 0.0, oy = 0.0;
    switch (s->kind) {
        case SPLINE_LINEAR:
            eval_linear(s, t, &ox, &oy);
            break;
        case SPLINE_BEZIER:
            eval_bezier(s, t, &ox, &oy);
            break;
        case SPLINE_CATMULL_ROM:
            eval_catmull_rom(s, t, &ox, &oy);
            break;
    }
    return rt_vec2_new(ox, oy);
}

/// @brief Return the spline's unnormalized tangent vector at `t` as a fresh Vec2.
/// @details Linear uses the active segment delta, Bezier uses its analytic derivative,
///          and Catmull-Rom uses a finite-difference derivative.
/// @param spline Spline handle to differentiate.
/// @param t Curve parameter sanitized to the inclusive range [0, 1].
/// @return New unnormalized tangent Vec2, or NULL after trapping for an invalid spline or
///         allocation failure.
void *rt_spline_tangent(void *spline, double t) {
    ZannaSpline *s = spline_checked(spline, "Spline.Tangent: invalid spline");
    if (!s)
        return NULL;
    t = spline_sanitize_param(t);
    double ox = 0.0, oy = 0.0;
    switch (s->kind) {
        case SPLINE_LINEAR:
            tangent_linear(s, t, &ox, &oy);
            break;
        case SPLINE_BEZIER:
            tangent_bezier(s, t, &ox, &oy);
            break;
        case SPLINE_CATMULL_ROM:
            tangent_catmull_rom(s, t, &ox, &oy);
            break;
    }
    return rt_vec2_new(ox, oy);
}

/// @brief Return the number of stored spline control points.
/// @param spline Spline handle to inspect.
/// @return Control-point count, or 0 after trapping for an invalid spline.
int64_t rt_spline_point_count(void *spline) {
    ZannaSpline *s = spline_checked(spline, "Spline.PointCount: invalid spline");
    return s ? s->count : 0;
}

/// @brief Copy one stored spline control point into a new Vec2.
/// @details Invalid spline handles and indices outside `[0, point_count)` raise a runtime trap.
/// @param spline Spline handle to inspect.
/// @param index Zero-based control-point index.
/// @return New Vec2 containing the point, or NULL after trapping for invalid input or allocation
///         failure.
void *rt_spline_point_at(void *spline, int64_t index) {
    ZannaSpline *s = spline_checked(spline, "Spline.PointAt: invalid spline");
    if (!s)
        return NULL;
    if (index < 0 || index >= s->count) {
        rt_trap("Spline.PointAt: index out of range");
        return NULL;
    }
    return rt_vec2_new(s->xs[index], s->ys[index]);
}

/// @brief Approximate spline arc length over a parameter interval.
/// @details Sanitizes both endpoints to [0, 1], samples @p steps equal parameter intervals, and
///          sums the resulting chord lengths. Values of @p steps below one are promoted to one.
///          Reversed endpoints traverse the same sampled path in reverse and still yield a
///          non-negative length.
/// @param spline Spline handle to measure.
/// @param t0 Starting curve parameter.
/// @param t1 Ending curve parameter.
/// @param steps Number of line segments in the approximation.
/// @return Approximated arc length, or 0.0 after trapping for an invalid spline.
double rt_spline_arc_length(void *spline, double t0, double t1, int64_t steps) {
    ZannaSpline *s = spline_checked(spline, "Spline.ArcLength: invalid spline");
    if (!s)
        return 0.0;
    if (steps < 1)
        steps = 1;
    // Sanitize both endpoints so every sampled t stays finite and in [0,1] — an
    // unsanitized non-finite t0/t1 would feed NaN/Inf to the evaluators and the
    // NaN→segment-index conversion is undefined (VDOC-201).
    t0 = spline_sanitize_param(t0);
    t1 = spline_sanitize_param(t1);
    double length = 0.0;
    double dt = (t1 - t0) / (double)steps;
    double px = 0.0, py = 0.0;

    switch (s->kind) {
        case SPLINE_LINEAR:
            eval_linear(s, t0, &px, &py);
            break;
        case SPLINE_BEZIER:
            eval_bezier(s, t0, &px, &py);
            break;
        case SPLINE_CATMULL_ROM:
            eval_catmull_rom(s, t0, &px, &py);
            break;
    }

    for (int64_t i = 1; i <= steps; ++i) {
        double t = t0 + dt * (double)i;
        double cx = 0.0, cy = 0.0;
        switch (s->kind) {
            case SPLINE_LINEAR:
                eval_linear(s, t, &cx, &cy);
                break;
            case SPLINE_BEZIER:
                eval_bezier(s, t, &cx, &cy);
                break;
            case SPLINE_CATMULL_ROM:
                eval_catmull_rom(s, t, &cx, &cy);
                break;
        }
        double dx = cx - px;
        double dy = cy - py;
        length += sqrt(dx * dx + dy * dy);
        px = cx;
        py = cy;
    }
    return length;
}

/// @brief Sample a spline at evenly spaced parameter values.
/// @details Promotes counts below two to two and includes both parameter endpoints.
/// @param spline Spline handle to sample.
/// @param count Requested number of samples.
/// @return New Seq containing at least two Vec2 samples, or NULL after an invalid-spline trap.
void *rt_spline_sample(void *spline, int64_t count) {
    if (!spline_checked(spline, "Spline.Sample: invalid spline"))
        return NULL;
    if (count < 2)
        count = 2;

    void *seq = rt_seq_new();
    for (int64_t i = 0; i < count; ++i) {
        double t = (double)i / (double)(count - 1);
        void *pt = rt_spline_eval(spline, t);
        rt_seq_push(seq, pt);
    }
    return seq;
}
