//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_brdf_lut.c
// Purpose: Deterministic CPU precomputation of the split-sum environment BRDF
//   table (Karis, "Real Shading in Unreal Engine 4"): for each (NdotV,
//   roughness) texel, integrate the GGX specular BRDF over importance-sampled
//   half vectors and store the Fresnel scale/bias pair (A, B) so shaders can
//   evaluate specular IBL as prefiltered * (F0 * A + B).
// Key invariants:
//   - Fixed 1024-sample Hammersley sequence: bit-identical output on every
//     platform, satisfying the VM/native determinism requirement.
//   - Texel centers sample NdotV, roughness in (0, 1]; row-major, NdotV on X.
// Ownership/Lifetime:
//   - Static process-lifetime storage, built once on first use.
// Links: vgfx3d_brdf_lut.h
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_brdf_lut.c
 * @brief Builds and samples the deterministic split-sum environment BRDF lookup table.
 *
 * The table stores the Fresnel scale and bias pair used to reconstruct specular image-based
 * lighting from a prefiltered environment map. A single thread computes the process-lifetime
 * table from a fixed Hammersley sequence; concurrent callers wait for the immutable result and
 * subsequently read it without locking.
 */
#include "vgfx3d_brdf_lut.h"

#include "rt_platform.h"

#include <math.h>
#include <stddef.h>

/// Number of deterministic GGX importance samples accumulated per lookup-table texel.
#define BRDF_LUT_SAMPLES 1024u

/// Process-lifetime table of interleaved Fresnel scale and bias pairs.
static float g_brdf_lut[VGFX3D_BRDF_LUT_SIZE * VGFX3D_BRDF_LUT_SIZE * 2];
/* 0 = uninitialized, 1 = one builder active, 2 = immutable/ready. */
static int g_brdf_lut_state = 0;

/// @brief Van der Corput radical inverse in base 2 (bit reversal).
/// @param bits Hammersley sample index whose bits are reflected across the radix point.
/// @return Deterministic value in `[0, 1)` obtained by scaling the reversed bit pattern by
///         `1 / 2^32`.
static float brdf_radical_inverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f; /* 1 / 2^32 */
}

/// @brief Smith height-correlated visibility for the split-sum integration
///   (the k = a^2 / 2 form used by the reference implementation).
/// @param ndotv Clamped cosine between the surface normal and view direction.
/// @param ndotl Positive cosine between the surface normal and sampled light direction.
/// @param roughness Perceptual surface roughness.
/// @return Product of the view- and light-direction masking terms.
static float brdf_g_smith_ibl(float ndotv, float ndotl, float roughness) {
    float a = roughness * roughness;
    float k = a / 2.0f;
    float gv = ndotv / (ndotv * (1.0f - k) + k);
    float gl = ndotl / (ndotl * (1.0f - k) + k);
    return gv * gl;
}

/// @brief Integrate the (A, B) split-sum pair for one (NdotV, roughness) pair.
/// @details Uses @c BRDF_LUT_SAMPLES GGX importance samples and double-precision accumulators to
///          reduce platform-dependent summation error. The local surface normal is positive Z.
/// @param ndotv Positive cosine between the surface normal and view direction.
/// @param roughness Positive perceptual roughness used to construct the GGX distribution.
/// @param out_a Receives the Fresnel-independent scale term.
/// @param out_b Receives the grazing-angle Fresnel bias term.
static void brdf_integrate(float ndotv, float roughness, float *out_a, float *out_b) {
    /* View vector in the local frame (N = +Z). */
    float vx = sqrtf(1.0f - ndotv * ndotv);
    float vz = ndotv;
    float a = roughness * roughness;
    double sum_a = 0.0;
    double sum_b = 0.0;
    for (uint32_t i = 0; i < BRDF_LUT_SAMPLES; i++) {
        /* GGX importance-sampled half vector from the Hammersley point. */
        float e1 = ((float)i + 0.5f) / (float)BRDF_LUT_SAMPLES;
        float e2 = brdf_radical_inverse(i);
        float phi = 2.0f * 3.14159265358979323846f * e1;
        float cos_theta = sqrtf((1.0f - e2) / (1.0f + (a * a - 1.0f) * e2));
        float sin_theta = sqrtf(1.0f - cos_theta * cos_theta);
        float hx = sin_theta * cosf(phi);
        float hy = sin_theta * sinf(phi);
        float hz = cos_theta;
        float vdoth = vx * hx + vz * hz;
        /* Only L.z is needed: N = +Z, so NdotL falls out of the reflection. */
        float ndotl = 2.0f * vdoth * hz - vz;
        float ndoth = hz;
        (void)hy;
        if (ndotl > 0.0f && vdoth > 0.0f && ndoth > 0.0f) {
            float g = brdf_g_smith_ibl(vz, ndotl, roughness);
            float g_vis = g * vdoth / (ndoth * vz);
            float fc = 1.0f - vdoth;
            float fc2 = fc * fc;
            float fc5 = fc2 * fc2 * fc;
            sum_a += (double)((1.0f - fc5) * g_vis);
            sum_b += (double)(fc5 * g_vis);
        }
    }
    *out_a = (float)(sum_a / (double)BRDF_LUT_SAMPLES);
    *out_b = (float)(sum_b / (double)BRDF_LUT_SAMPLES);
}

/// @brief Build the process-wide BRDF lookup table exactly once.
/// @details The winning caller performs the deterministic integration and publishes the immutable
///          table with release ordering. Concurrent callers spin on an acquire load until
///          publication completes; calls after initialization return immediately.
void vgfx3d_brdf_lut_ensure(void) {
    if (rt_atomic_load_i32(&g_brdf_lut_state, __ATOMIC_ACQUIRE) == 2)
        return;
    int expected = 0;
    if (rt_atomic_compare_exchange_i32(
            &g_brdf_lut_state, &expected, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        for (int y = 0; y < VGFX3D_BRDF_LUT_SIZE; y++) {
            float roughness = ((float)y + 0.5f) / (float)VGFX3D_BRDF_LUT_SIZE;
            for (int x = 0; x < VGFX3D_BRDF_LUT_SIZE; x++) {
                float ndotv = ((float)x + 0.5f) / (float)VGFX3D_BRDF_LUT_SIZE;
                float *texel = &g_brdf_lut[(size_t)(y * VGFX3D_BRDF_LUT_SIZE + x) * 2u];
                brdf_integrate(ndotv, roughness, &texel[0], &texel[1]);
            }
        }
        rt_atomic_store_i32(&g_brdf_lut_state, 2, __ATOMIC_RELEASE);
        return;
    }
    while (rt_atomic_load_i32(&g_brdf_lut_state, __ATOMIC_ACQUIRE) != 2) {
        /* One backend is building the process-lifetime table. */
    }
}

/// @brief Return the initialized interleaved BRDF lookup-table storage.
/// @return Borrowed process-lifetime pointer to
///         `VGFX3D_BRDF_LUT_SIZE * VGFX3D_BRDF_LUT_SIZE` `(A, B)` pairs.
const float *vgfx3d_brdf_lut_data(void) {
    vgfx3d_brdf_lut_ensure();
    return g_brdf_lut;
}

/// @brief Bilinearly sample the split-sum BRDF table at a view cosine and roughness.
/// @details Both coordinates are clamped to `[0, 1]`, with NaN and non-positive values mapping to
///          the lower edge. Sampling is aligned to texel centers and clamps neighbor selection at
///          the table boundary.
/// @param ndotv Cosine between the surface normal and view direction.
/// @param roughness Perceptual surface roughness.
/// @param out_ab Caller-owned two-float output receiving the interpolated Fresnel scale and bias;
///               must be non-null.
void vgfx3d_brdf_lut_sample(float ndotv, float roughness, float *out_ab) {
    const float *lut;
    float fx, fy;
    int x0, y0, x1, y1;
    float tx, ty;
    const float *t00;
    const float *t10;
    const float *t01;
    const float *t11;
    vgfx3d_brdf_lut_ensure();
    lut = g_brdf_lut;
    if (!(ndotv > 0.0f))
        ndotv = 0.0f;
    if (ndotv > 1.0f)
        ndotv = 1.0f;
    if (!(roughness > 0.0f))
        roughness = 0.0f;
    if (roughness > 1.0f)
        roughness = 1.0f;
    /* Texel-center bilinear with edge clamping. */
    fx = ndotv * (float)VGFX3D_BRDF_LUT_SIZE - 0.5f;
    fy = roughness * (float)VGFX3D_BRDF_LUT_SIZE - 0.5f;
    if (fx < 0.0f)
        fx = 0.0f;
    if (fy < 0.0f)
        fy = 0.0f;
    x0 = (int)fx;
    y0 = (int)fy;
    if (x0 > VGFX3D_BRDF_LUT_SIZE - 1)
        x0 = VGFX3D_BRDF_LUT_SIZE - 1;
    if (y0 > VGFX3D_BRDF_LUT_SIZE - 1)
        y0 = VGFX3D_BRDF_LUT_SIZE - 1;
    x1 = x0 + 1 < VGFX3D_BRDF_LUT_SIZE ? x0 + 1 : x0;
    y1 = y0 + 1 < VGFX3D_BRDF_LUT_SIZE ? y0 + 1 : y0;
    tx = fx - (float)x0;
    ty = fy - (float)y0;
    t00 = &lut[(size_t)(y0 * VGFX3D_BRDF_LUT_SIZE + x0) * 2u];
    t10 = &lut[(size_t)(y0 * VGFX3D_BRDF_LUT_SIZE + x1) * 2u];
    t01 = &lut[(size_t)(y1 * VGFX3D_BRDF_LUT_SIZE + x0) * 2u];
    t11 = &lut[(size_t)(y1 * VGFX3D_BRDF_LUT_SIZE + x1) * 2u];
    for (int c = 0; c < 2; c++) {
        float top = t00[c] + (t10[c] - t00[c]) * tx;
        float bottom = t01[c] + (t11[c] - t01[c]) * tx;
        out_ab[c] = top + (bottom - top) * ty;
    }
}
