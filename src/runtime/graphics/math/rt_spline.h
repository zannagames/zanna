//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/math/rt_spline.h
// Purpose: Parameterized 2D paths using linear, uniform Catmull-Rom, or cubic Bezier interpolation
// over copied Vec2 control-point coordinates.
//
// Key invariants:
//   - At least 2 control points are required for a valid spline.
//   - Catmull-Rom splines pass through all control points.
//   - Public evaluation parameters are sanitized to [0, 1]; NaN maps to zero.
//   - Parameters are distributed by control-point segment, not by arc length.
//   - Point and tangent evaluation are O(1) after construction.
//
// Ownership/Lifetime:
//   - Spline objects are runtime-managed opaque handles with finalized coordinate arrays.
//   - Constructors copy coordinates and neither retain nor consume the input Seq or Vec2 handles.
//
// Links: src/runtime/graphics/math/rt_spline.c (implementation),
//        src/runtime/graphics/math/rt_vec2.h (point type),
//        src/runtime/collections/rt_seq.h (sequence inputs and sampling result)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

/// @brief Runtime class id tagging Spline heap payloads.
/// @details Lets public entry points validate script-supplied handles before
///          dereferencing the spline's internal coordinate-array pointers.
#define RT_SPLINE_CLASS_ID INT64_C(-0x600209)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a Catmull-Rom spline from a Seq of Vec2 control points.
/// @details Uses uniform 0.5 tangents and duplicates endpoint neighbors. NULL input or fewer than
///          two elements raises a runtime trap.
/// @param points Runtime Seq containing at least two Vec2 handles.
/// @return New runtime-managed Spline, or NULL after trapping for invalid input or allocation
///         failure.
void *rt_spline_catmull_rom(void *points);

/// @brief Create a cubic Bezier spline from 4 Vec2 control points.
/// @details The curve interpolates @p p0 and @p p3 and is shaped by the two interior handles.
///          Any NULL point raises a runtime trap.
/// @param p0 Starting anchor point.
/// @param p1 First interior control handle.
/// @param p2 Second interior control handle.
/// @param p3 Ending anchor point.
/// @return New runtime-managed Spline, or NULL after trapping for invalid input or allocation
///         failure.
void *rt_spline_bezier(void *p0, void *p1, void *p2, void *p3);

/// @brief Create a linear spline (polyline) from a Seq of Vec2 control points.
/// @details Joins adjacent copied points with straight segments. NULL input or fewer than two
///          elements raises a runtime trap.
/// @param points Runtime Seq containing at least two Vec2 handles.
/// @return New runtime-managed Spline, or NULL after trapping for invalid input or allocation
///         failure.
void *rt_spline_linear(void *points);

/// @brief Evaluate the spline at parameter t (0.0 to 1.0).
/// @details Clamps finite values to [0, 1], maps NaN to zero, and dispatches to the spline's
///          interpolation algorithm.
/// @param spline Spline handle to evaluate.
/// @param t Curve parameter.
/// @return New Vec2 position, or NULL after trapping for an invalid spline or allocation failure.
void *rt_spline_eval(void *spline, double t);

/// @brief Evaluate the tangent (derivative) at parameter t.
/// @details Returns a segment delta for linear splines, an analytic derivative for Bezier, and a
///          finite-difference derivative for Catmull-Rom. The result is not normalized.
/// @param spline Spline handle to differentiate.
/// @param t Curve parameter sanitized to [0, 1].
/// @return New tangent Vec2, or NULL after trapping for an invalid spline or allocation failure.
void *rt_spline_tangent(void *spline, double t);

/// @brief Get the number of control points in the spline.
/// @param spline Spline handle to inspect.
/// @return Stored point count, or 0 after trapping for an invalid spline.
int64_t rt_spline_point_count(void *spline);

/// @brief Get a control point by index.
/// @details An invalid spline or index outside `[0, point_count)` raises a runtime trap.
/// @param spline Spline handle to inspect.
/// @param index Zero-based control-point index.
/// @return New Vec2 containing the copied control point, or NULL after trapping for invalid input
///         or allocation failure.
void *rt_spline_point_at(void *spline, int64_t index);

/// @brief Approximate the arc length of the spline between t0 and t1.
/// @details Sanitizes both endpoints and sums chord lengths over equal parameter intervals. Values
///          of @p steps below one are promoted to one.
/// @param spline Spline handle to measure.
/// @param t0 Starting curve parameter.
/// @param t1 Ending curve parameter.
/// @param steps Number of segments in the polyline approximation.
/// @return Approximate non-negative arc length, or 0.0 after trapping for an invalid spline.
double rt_spline_arc_length(void *spline, double t0, double t1, int64_t steps);

/// @brief Sample the spline into a Seq of Vec2 points.
/// @details Samples equal parameter intervals including both endpoints; requested counts below two
///          are promoted to two.
/// @param spline Spline handle to sample.
/// @param count Requested number of samples.
/// @return New Seq containing at least two Vec2 samples, or NULL after an invalid-spline trap.
void *rt_spline_sample(void *spline, int64_t count);

#ifdef __cplusplus
}
#endif
