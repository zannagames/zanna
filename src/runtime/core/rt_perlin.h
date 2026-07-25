//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_perlin.h
// Purpose: Declares the runtime-managed seeded PerlinNoise object and its
// deterministic 2D, 3D, and normalized multi-octave sampling operations.
//
// Key invariants:
//   - The same seed and coordinates reproduce the same field.
//   - Base noise is periodic every 256 integer cells on each axis.
//   - Sampling validates receiver kind, class ID, and payload size.
//   - Octave requests are clamped to at most 16 layers and normalized by their
//     amplitude sum when the accumulation remains finite.
//   - The permutation table is built once at creation time from the seed.
//
// Ownership/Lifetime:
//   - PerlinNoise handles are allocated through rt_obj_new_i64 and participate
//     in the runtime object/GC lifetime system.
//   - Callers borrow opaque handles and must not free or reinterpret them.
//
// Links: src/runtime/core/rt_perlin.c (implementation),
//        src/runtime/oop/rt_object.h (runtime object allocation)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Seeded improved-Perlin-noise runtime API.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable heap class id for PerlinNoise instances.
/// @details Used by rt_obj_new_i64 at construction and by the noise/octave
///          readers to validate the receiver's kind/class/size before casting
///          to the private permutation-table payload.
#define RT_PERLIN_CLASS_ID INT64_C(-0x430701)

/// @brief Create a new Perlin noise generator with the given seed.
/// @details Builds a deterministic permutation with a fixed 64-bit generator.
///          Equal seeds therefore create equivalent noise fields.
/// @param seed Seed reinterpreted modulo 2^64 for permutation generation.
/// @return New runtime-managed PerlinNoise handle, or `NULL` on allocation
///         failure.
void *rt_perlin_new(int64_t seed);

/// @brief Generate 2D Perlin noise.
/// @details Evaluates the raw, unclamped gradient field. Invalid receivers trap;
///          non-finite coordinates or lattice cells outside the C `int` range
///          return zero.
/// @param obj Runtime-managed PerlinNoise handle.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @return Deterministic raw noise sample, or `0.0` for an unusable coordinate
///         or after a returning invalid-receiver trap.
double rt_perlin_noise2d(void *obj, double x, double y);

/// @brief Generate 3D Perlin noise.
/// @details Evaluates the raw, unclamped 3D gradient field. Invalid receivers
///          trap; non-finite coordinates or lattice cells outside the C `int`
///          range return zero.
/// @param obj Runtime-managed PerlinNoise handle.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param z Z coordinate.
/// @return Deterministic raw noise sample, or `0.0` for an unusable coordinate
///         or after a returning invalid-receiver trap.
double rt_perlin_noise3d(void *obj, double x, double y, double z);

/// @brief Generate normalized multi-octave 2D noise.
/// @details Doubles frequency and multiplies amplitude by @p persistence for
///          each layer, then divides by the amplitude sum. Invalid receivers
///          trap. Non-finite persistence, unusable accumulation, or a zero
///          amplitude sum returns zero.
/// @param obj Runtime-managed PerlinNoise handle.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param octaves Layer count; non-positive values return zero and values above
///                 16 are clamped.
/// @param persistence Finite amplitude multiplier per octave.
/// @return Normalized weighted sample, or `0.0` for a documented failure case.
double rt_perlin_octave2d(void *obj, double x, double y, int64_t octaves, double persistence);

/// @brief Generate normalized multi-octave 3D noise.
/// @details Uses the same frequency, amplitude, clamping, validation, and
///          normalization rules as @ref rt_perlin_octave2d.
/// @param obj Runtime-managed PerlinNoise handle.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param z Z coordinate.
/// @param octaves Layer count; non-positive values return zero and values above
///                 16 are clamped.
/// @param persistence Finite amplitude multiplier per octave.
/// @return Normalized weighted sample, or `0.0` for a documented failure case.
double rt_perlin_octave3d(
    void *obj, double x, double y, double z, int64_t octaves, double persistence);

#ifdef __cplusplus
}
#endif
