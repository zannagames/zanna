//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_world3d_common.h
// Purpose: Shared numeric-sanitization inline helpers for the Zanna.Graphics3D world
//   subsystem (terrain/water/vegetation/particles) — finite/range clamping and
//   overflow-checked size multiplication used to harden untrusted scalar inputs from
//   scripts and assets before they reach allocation sizing or GPU state.
// Key invariants:
//   - All helpers are pure, branch-only, and allocation-free.
//   - Non-finite (NaN/Inf) inputs never propagate: they map to a caller-supplied fallback
//     or a safe default instead.
// Ownership/Lifetime:
//   - None — stateless value transforms over scalars and caller-owned out-parameters.
// Links: rt_terrain3d.c, rt_water3d.c, rt_vegetation3d.c, rt_particles3d.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_world3d_common.h
 * @brief Provides allocation-free numeric sanitizers shared by Graphics3D world systems.
 *
 * These internal inline helpers stop non-finite script or asset inputs from reaching geometry,
 * allocation, and rendering state, and centralize overflow-safe size calculations.
 */

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/// @brief Return @p value if it is finite, otherwise @p fallback.
/// @param value Candidate floating-point value.
/// @param fallback Replacement for NaN or infinity.
/// @return @p value when finite, otherwise @p fallback.
static inline double rt_world3d_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp @p value to [-@p max_abs, @p max_abs]; non-finite input returns @p fallback.
/// @param value Candidate signed value.
/// @param fallback Replacement for non-finite input.
/// @param max_abs Inclusive absolute-value limit.
/// @return Finite value constrained to the symmetric interval.
static inline double rt_world3d_clamp_abs_or(double value, double fallback, double max_abs) {
    if (!isfinite(value))
        return fallback;
    if (value > max_abs)
        return max_abs;
    if (value < -max_abs)
        return -max_abs;
    return value;
}

/// @brief Clamp @p value to [0, 1]; non-finite input returns 0.
/// @param value Candidate normalized scalar.
/// @return Finite value in `[0, 1]`.
static inline double rt_world3d_clamp01(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Clamp @p value to (0, @p max_value]; non-finite or non-positive input returns
///   @p fallback.
/// @param value Candidate positive scalar.
/// @param fallback Replacement for non-finite or nonpositive input.
/// @param max_value Inclusive upper limit.
/// @return Positive sanitized scalar.
static inline double rt_world3d_clamp_positive_or(double value,
                                                  double fallback,
                                                  double max_value) {
    if (!isfinite(value) || value <= 0.0)
        return fallback;
    return value > max_value ? max_value : value;
}

/// @brief Clamp @p value to [0, @p max_value]; non-finite or negative input returns 0.
/// @param value Candidate nonnegative scalar.
/// @param max_value Inclusive upper limit.
/// @return Finite scalar in `[0, max_value]`.
static inline double rt_world3d_clamp_nonnegative(double value, double max_value) {
    if (!isfinite(value) || value < 0.0)
        return 0.0;
    return value > max_value ? max_value : value;
}

/// @brief Overflow-checked size_t multiply: writes @p a * @p b to @p out.
/// @param a First nonnegative factor.
/// @param b Second nonnegative factor.
/// @param out Destination receiving the product on success.
/// @return 1 on success, 0 on overflow or when @p out is NULL.
static inline int rt_world3d_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}
