//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_perlin.c
// Purpose: Implements Ken Perlin's improved noise algorithm (2002) for the
//          Zanna runtime. Supports 2D and 3D noise evaluation and octave/fractal
//          layering for procedural terrain, textures, and animations.
//
// Key invariants:
//   - The permutation table is seeded from the caller-supplied integer seed
//     using a Fisher-Yates shuffle; the same seed reproduces the same field.
//   - The doubled permutation table (perm[512]) avoids modular arithmetic in
//     the inner loop; indices are masked with 0xFF before lookup.
//   - The base noise field repeats every 256 integer cells on each axis.
//   - Receivers are validated by runtime kind, class ID, and payload size before
//     the immutable permutation table is read; invalid receivers trap.
//   - Valid instances are immutable after construction, so concurrent reads of
//     the same or different instances require no module synchronization.
//   - Octave counts are clamped to 16 and results are normalized by the sum of
//     layer amplitudes when that divisor and accumulated total remain finite.
//
// Ownership/Lifetime:
//   - Perlin instances are allocated through rt_obj_new_i64 and managed by the
//     runtime object/GC lifetime system.
//   - The permutation table is inline, so the finalizer owns no subordinate
//     allocation and callers must not free the returned pointer directly.
//
// Links: src/runtime/core/rt_perlin.h (public API),
//        src/runtime/core/rt_easing.c (complementary interpolation utilities)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Seeded 2D/3D improved Perlin noise and normalized octave layering.

#include "rt_perlin.h"

#include "rt_internal.h"
#include "rt_object.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @brief Maximum number of layers evaluated by an octave-noise call.
#define RT_PERLIN_MAX_OCTAVES 16

/// @brief Runtime payload containing one immutable doubled permutation table.
typedef struct rt_perlin_impl {
    /// Reserved object dispatch-table slot; initialized to `NULL`.
    void **vptr;
    /// Seeded 256-entry permutation repeated twice.
    uint8_t perm[512]; // Doubled permutation table
} rt_perlin_impl;

/// @brief Validate a receiver as a PerlinNoise instance before reading its
///        permutation table.
/// @details Checks heap kind/class/size via rt_obj_is_instance and traps on a
///          mismatch, returning NULL so callers stop rather than reading an
///          unrelated object's memory as a 512-byte table (VDOC-202). Returns
///          NULL (not the pointer) so a returning trap hook cannot fall through.
/// @param obj Candidate runtime object.
/// @return The validated payload pointer, or `NULL` after trapping for an
///         invalid receiver.
static rt_perlin_impl *as_perlin(void *obj) {
    if (!rt_obj_is_instance(obj, RT_PERLIN_CLASS_ID, sizeof(rt_perlin_impl))) {
        rt_trap("PerlinNoise: invalid PerlinNoise object");
        return NULL;
    }
    return (rt_perlin_impl *)obj;
}

/// @brief Ken Perlin's 6t⁵−15t⁴+10t³ fade curve for smooth interpolation.
/// @details Smoothstep replacement chosen so the first AND second derivatives are zero
///          at t=0 and t=1 — produces visually continuous gradients across cell
///          boundaries in the noise field.
/// @param t Fractional cell coordinate, normally in `[0, 1)`.
/// @return `6t^5 - 15t^4 + 10t^3`.
static double fade(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

/// @brief Linear interpolation between @p a and @p b parameterised by @p t.
/// @param t Interpolation factor.
/// @param a Value at factor zero.
/// @param b Value at factor one.
/// @return `a + t * (b - a)` without clamping @p t.
static double lerp(double t, double a, double b) {
    return a + t * (b - a);
}

/// @brief 3D gradient hash: dot product of (x, y, z) with one of 12 fixed gradient vectors.
/// @details Selects from the 12 edge-midpoint vectors of a cube (the original Perlin '02
///          gradient set) by indexing into the low 4 bits of @p hash. The bit-twiddled
///          form avoids a lookup table — `(h & 1) ? -u : u` flips signs based on the
///          hash bits, and the `u`/`v` selection picks two of the three input
///          coordinates per gradient.
/// @param hash Permutation-derived hash; only its low four bits are inspected.
/// @param x X displacement from the hashed lattice corner.
/// @param y Y displacement from the hashed lattice corner.
/// @param z Z displacement from the hashed lattice corner.
/// @return The selected gradient's dot product with `(x, y, z)`.
static double grad3(int hash, double x, double y, double z) {
    int h = hash & 15;
    double u = h < 8 ? x : y;
    double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

/// @brief 2D gradient hash: dot product of (x, y) with one of four diagonal gradients.
/// @details The four cases cover the diagonals (+x+y, -x+y, +x-y, -x-y), which is the
///          standard 2D Perlin gradient set.
/// @param hash Permutation-derived hash; only its low two bits are inspected.
/// @param x X displacement from the hashed lattice corner.
/// @param y Y displacement from the hashed lattice corner.
/// @return The selected diagonal gradient's dot product with `(x, y)`.
static double grad2(int hash, double x, double y) {
    int h = hash & 3;
    switch (h) {
        case 0:
            return x + y;
        case 1:
            return -x + y;
        case 2:
            return x - y;
        case 3:
            return -x - y;
    }
    return 0;
}

/// @brief Convert a coordinate to a safe Perlin cell index and fractional offset.
/// @details Rejects non-finite values and floor results outside the C `int`
///          range. Valid cell indices are wrapped modulo 256 for permutation
///          lookup, while the fraction remains in `[0, 1)`.
/// @param value Coordinate to decompose.
/// @param index_out Required destination for the wrapped lattice-cell index.
/// @param fraction_out Required destination for the fractional displacement.
/// @return One on success, or zero without writing either output when the
///         coordinate cannot be represented safely.
static int perlin_cell(double value, int *index_out, double *fraction_out) {
    if (!isfinite(value))
        return 0;
    double cell = floor(value);
    if (cell < (double)INT_MIN || cell > (double)INT_MAX)
        return 0;
    *index_out = ((int)cell) & 255;
    *fraction_out = value - cell;
    return 1;
}

/// @brief Normalize a requested octave count to the supported range.
/// @param octaves Requested number of layers.
/// @return Zero for a non-positive request, 16 for a request above the limit,
///         or @p octaves unchanged otherwise.
static int64_t perlin_clamp_octaves(int64_t octaves) {
    if (octaves <= 0)
        return 0;
    return octaves > RT_PERLIN_MAX_OCTAVES ? RT_PERLIN_MAX_OCTAVES : octaves;
}

/// @brief Normalize an accumulated octave sum by its amplitude sum.
/// @param total Weighted sum of layer samples.
/// @param max_value Sum of layer amplitudes.
/// @return `total / max_value` when both values are finite and the divisor is
///         non-zero; otherwise `0.0`.
static double perlin_finish_octaves(double total, double max_value) {
    if (!isfinite(total) || !isfinite(max_value) || max_value == 0.0)
        return 0.0;
    return total / max_value;
}

/// @brief GC finalizer for Perlin generators; no dynamic allocations to release.
/// @param obj Perlin payload being finalized; intentionally unused because all
///            state is stored inline in the runtime allocation.
static void rt_perlin_finalize(void *obj) {
    // No dynamic allocations beyond the object itself
    (void)obj;
}

/// @brief Create a deterministic Perlin generator for @p seed.
/// @details Allocates a runtime object stamped with @ref RT_PERLIN_CLASS_ID,
///          initializes the base permutation with `0..255`, and applies a
///          Fisher-Yates shuffle driven by a fixed 64-bit LCG. The shuffled
///          table is copied into both halves of the 512-byte lookup array and
///          the no-op finalizer is registered.
/// @param seed Signed seed reinterpreted modulo 2^64 for the LCG state.
/// @return A new runtime-managed generator, or `NULL` if object allocation
///         fails.
void *rt_perlin_new(int64_t seed) {
    rt_perlin_impl *p =
        (rt_perlin_impl *)rt_obj_new_i64(RT_PERLIN_CLASS_ID, (int64_t)sizeof(rt_perlin_impl));
    if (!p)
        return NULL;
    p->vptr = NULL;

    // Initialize permutation table with 0..255
    uint8_t base[256];
    for (int i = 0; i < 256; ++i)
        base[i] = (uint8_t)i;

    // Fisher-Yates shuffle with seed
    uint64_t s = (uint64_t)seed;
    for (int i = 255; i > 0; --i) {
        // Simple LCG for deterministic shuffle
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        int j = (int)((s >> 16) % (uint64_t)(i + 1));
        uint8_t tmp = base[i];
        base[i] = base[j];
        base[j] = tmp;
    }

    // Double the table for wrapping
    for (int i = 0; i < 256; ++i) {
        p->perm[i] = base[i];
        p->perm[i + 256] = base[i];
    }

    rt_obj_set_finalizer(p, rt_perlin_finalize);
    return p;
}

/// @brief Compute 2D Perlin noise at the given coordinates.
/// @details Wraps lattice indices modulo 256, evaluates four diagonal
///          gradients, and blends them with the quintic fade curve. The result
///          is deterministic and the field is periodic every 256 integer units
///          on each axis. This API does not clamp or remap the raw gradient
///          result. An invalid receiver traps; an invalid or out-of-range
///          coordinate returns zero without trapping.
/// @param obj Runtime-managed PerlinNoise object containing the permutation table.
/// @param x X coordinate in noise space.
/// @param y Y coordinate in noise space.
/// @return Raw interpolated noise, or `0.0` for an unusable coordinate or after
///         a returning invalid-receiver trap.
double rt_perlin_noise2d(void *obj, double x, double y) {
    rt_perlin_impl *p = as_perlin(obj);
    if (!p)
        return 0.0;

    int xi = 0;
    int yi = 0;
    double xf = 0.0;
    double yf = 0.0;
    if (!perlin_cell(x, &xi, &xf) || !perlin_cell(y, &yi, &yf))
        return 0.0;

    double u = fade(xf);
    double v = fade(yf);

    int aa = p->perm[p->perm[xi] + yi];
    int ab = p->perm[p->perm[xi] + yi + 1];
    int ba = p->perm[p->perm[xi + 1] + yi];
    int bb = p->perm[p->perm[xi + 1] + yi + 1];

    double x1 = lerp(u, grad2(aa, xf, yf), grad2(ba, xf - 1, yf));
    double x2 = lerp(u, grad2(ab, xf, yf - 1), grad2(bb, xf - 1, yf - 1));
    return lerp(v, x1, x2);
}

/// @brief Compute 3D Perlin noise at the given coordinates.
/// @details Evaluates eight hashed gradients and blends them along each axis
///          with the quintic fade curve. The deterministic field repeats every
///          256 integer units per axis and is returned without clamping or
///          remapping. An invalid receiver traps; an invalid or out-of-range
///          coordinate returns zero without trapping.
/// @param obj Runtime-managed PerlinNoise object containing the permutation table.
/// @param x X coordinate in noise space.
/// @param y Y coordinate in noise space.
/// @param z Z coordinate in noise space.
/// @return Raw interpolated noise, or `0.0` for an unusable coordinate or after
///         a returning invalid-receiver trap.
double rt_perlin_noise3d(void *obj, double x, double y, double z) {
    rt_perlin_impl *p = as_perlin(obj);
    if (!p)
        return 0.0;

    int xi = 0;
    int yi = 0;
    int zi = 0;
    double xf = 0.0;
    double yf = 0.0;
    double zf = 0.0;
    if (!perlin_cell(x, &xi, &xf) || !perlin_cell(y, &yi, &yf) || !perlin_cell(z, &zi, &zf))
        return 0.0;

    double u = fade(xf);
    double v = fade(yf);
    double w = fade(zf);

    int a = p->perm[xi] + yi;
    int aa = p->perm[a] + zi;
    int ab = p->perm[a + 1] + zi;
    int b = p->perm[xi + 1] + yi;
    int ba = p->perm[b] + zi;
    int bb = p->perm[b + 1] + zi;

    return lerp(
        w,
        lerp(v,
             lerp(u, grad3(p->perm[aa], xf, yf, zf), grad3(p->perm[ba], xf - 1, yf, zf)),
             lerp(u, grad3(p->perm[ab], xf, yf - 1, zf), grad3(p->perm[bb], xf - 1, yf - 1, zf))),
        lerp(v,
             lerp(u,
                  grad3(p->perm[aa + 1], xf, yf, zf - 1),
                  grad3(p->perm[ba + 1], xf - 1, yf, zf - 1)),
             lerp(u,
                  grad3(p->perm[ab + 1], xf, yf - 1, zf - 1),
                  grad3(p->perm[bb + 1], xf - 1, yf - 1, zf - 1))));
}

/// @brief Compute fractal Brownian motion (fBm) using multiple Perlin octaves.
/// @details Sums noise layers at exponentially increasing frequency (lacunarity=2)
///          and amplitude multiplied by @p persistence, then divides by the sum
///          of amplitudes. More octaves add fine detail but cost proportionally
///          more computation. An invalid receiver traps. Non-positive octave
///          counts and non-finite persistence return zero after receiver
///          validation. Non-finite accumulation, amplitude cancellation to
///          zero, and coordinate overflow also collapse to zero.
/// @param obj Runtime-managed PerlinNoise object.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param octaves Number of noise layers to sum (non-positive returns 0; positive values clamp to 16).
/// @param persistence Amplitude multiplier per octave; arbitrary finite values
///                    are accepted, including negative values.
/// @return The normalized weighted layer sum, or `0.0` for a documented
///         validation or normalization failure.
double rt_perlin_octave2d(void *obj, double x, double y, int64_t octaves, double persistence) {
    octaves = perlin_clamp_octaves(octaves);
    if (!as_perlin(obj) || octaves <= 0 || !isfinite(persistence))
        return 0.0;

    double total = 0.0;
    double frequency = 1.0;
    double amplitude = 1.0;
    double max_value = 0.0;

    for (int64_t i = 0; i < octaves; ++i) {
        total += rt_perlin_noise2d(obj, x * frequency, y * frequency) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2.0;
    }

    return perlin_finish_octaves(total, max_value);
}

/// @brief Compute normalized multi-octave 3D Perlin noise.
/// @details Evaluates up to 16 layers with frequency doubled and amplitude
///          multiplied by @p persistence after each layer, then divides the
///          weighted total by the amplitude sum. Receiver validation occurs
///          even for a non-positive octave request. Non-finite persistence,
///          accumulation, or normalization and a zero amplitude sum produce
///          zero.
/// @param obj Runtime-managed PerlinNoise object.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param z Z coordinate.
/// @param octaves Requested layer count; non-positive becomes zero and values
///                 above 16 are clamped.
/// @param persistence Finite amplitude multiplier per layer.
/// @return The normalized weighted layer sum, or `0.0` for a documented
///         validation or normalization failure.
double rt_perlin_octave3d(
    void *obj, double x, double y, double z, int64_t octaves, double persistence) {
    octaves = perlin_clamp_octaves(octaves);
    if (!as_perlin(obj) || octaves <= 0 || !isfinite(persistence))
        return 0.0;

    double total = 0.0;
    double frequency = 1.0;
    double amplitude = 1.0;
    double max_value = 0.0;

    for (int64_t i = 0; i < octaves; ++i) {
        total += rt_perlin_noise3d(obj, x * frequency, y * frequency, z * frequency) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2.0;
    }

    return perlin_finish_octaves(total, max_value);
}
