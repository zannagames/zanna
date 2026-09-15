//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_shader_manifest.inc
// Purpose: The one inventory of D3D11 backend shader entry points, their HLSL sources and the
//          D3DCompile flags, shared by the runtime compile path and the build-time DXBC bake.
// Key invariants:
//   - Runtime compilation and zanna_d3d11_shader_bake compile identical (source, entry, target,
//     flags) tuples, so embedded bytecode is the bytecode the runtime would have produced.
//   - The manifest digest covers every compiled byte and the flags; embedded bytecode is used
//     only when its recorded digest equals the digest of the HLSL linked into this binary.
// Ownership/Lifetime:
//   - Macros and a pure hashing helper only; the source accessors own their joined strings.
// Links: vgfx3d_backend_d3d11_shaders.inc, vgfx3d_backend_d3d11_context.inc,
//        src/tools/d3d11_shader_bake/d3d11_shader_bake.c
//
//===----------------------------------------------------------------------===//

/// @file vgfx3d_backend_d3d11_shader_manifest.inc
/// @brief D3D11 shader entry-point inventory, compile flags, and source digest.
/// @details Include after `d3dcompiler.h` and `vgfx3d_backend_d3d11_shaders.inc`.

#include <stdint.h>
#include <stdio.h>

/// @brief D3DCompile flags for every backend shader.
/// @details IEEE strictness keeps the HLSL `isnan()` guards. Without it FXC folds the NaN
///   self-comparison to false (warning X3577), so every "finite" check passes NaN through.
#define VGFX3D_D3D11_SHADER_COMPILE_FLAGS                                                          \
    ((UINT)(D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS))

/// @brief Expand `X(member, source, entry, target)` once per backend shader, in blob order.
/// @details `member` names the `d3d11_shader_blobs_t` field and the embedded DXBC array suffix.
#define VGFX3D_D3D11_SHADER_LIST(X)                                                                \
    X(vs, d3d11_shader_source, "VSMain", "vs_5_0")                                                 \
    X(vs_instanced, d3d11_shader_source, "VSMainInstanced", "vs_5_0")                              \
    X(vs_particles, d3d11_shader_source, "VSMainParticles", "vs_5_0")                              \
    X(ps, d3d11_shader_source, "PSMain", "ps_5_0")                                                 \
    X(vs_shadow, d3d11_shader_source, "VSShadow", "vs_5_0")                                        \
    X(vs_shadow_instanced, d3d11_shader_source, "VSShadowInstanced", "vs_5_0")                     \
    X(ps_shadow, d3d11_shader_source, "PSShadow", "ps_5_0")                                        \
    X(vs_skybox, d3d11_skybox_shader_source, "VSSkybox", "vs_5_0")                                 \
    X(ps_skybox, d3d11_skybox_shader_source, "PSSkybox", "ps_5_0")                                 \
    X(vs_postfx, d3d11_postfx_shader_source, "VSPostFX", "vs_5_0")                                 \
    X(ps_postfx, d3d11_postfx_shader_source, "PSPostFX", "ps_5_0")                                 \
    X(ps_overlay, d3d11_postfx_shader_source, "PSOverlayComposite", "ps_5_0")                      \
    X(ps_bloom_down, d3d11_bloom_shader_source, "PSBloomDown", "ps_5_0")                           \
    X(ps_bloom_up, d3d11_bloom_shader_source, "PSBloomUp", "ps_5_0")                               \
    X(ps_taa, d3d11_taa_shader_source, "PSTAAResolve", "ps_5_0")                                   \
    X(ps_ssr, d3d11_ssr_shader_source, "PSSSR", "ps_5_0")

/// @brief Fold one NUL-terminated string (and a terminator byte) into an FNV-1a 64 hash.
/// @param[in] hash Running hash value.
/// @param[in] text String to fold; NULL folds only the terminator so it cannot match real text.
/// @return Updated hash value.
static inline uint64_t vgfx3d_d3d11_manifest_fold(uint64_t hash, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    if (cursor) {
        while (*cursor) {
            hash ^= (uint64_t)*cursor++;
            hash *= UINT64_C(0x100000001b3);
        }
    }
    /* A byte no UTF-8 string contains keeps ("ab","c") distinct from ("a","bc"). */
    hash ^= UINT64_C(0xff);
    hash *= UINT64_C(0x100000001b3);
    return hash;
}

/// @brief Digest of the compile flags and every (entry, target, HLSL source) in the manifest.
/// @return FNV-1a 64 digest; an unavailable source (allocation failure) yields a digest that
///   cannot equal a baked one, which forces the runtime compile path.
static inline uint64_t vgfx3d_d3d11_shader_manifest_digest(void) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    char flags[16];
    snprintf(flags, sizeof(flags), "%u", (unsigned)VGFX3D_D3D11_SHADER_COMPILE_FLAGS);
    hash = vgfx3d_d3d11_manifest_fold(hash, flags);
#define VGFX3D_D3D11_FOLD_SHADER(member, source, entry, target)                                    \
    hash = vgfx3d_d3d11_manifest_fold(hash, entry);                                                \
    hash = vgfx3d_d3d11_manifest_fold(hash, target);                                               \
    hash = vgfx3d_d3d11_manifest_fold(hash, source);
    VGFX3D_D3D11_SHADER_LIST(VGFX3D_D3D11_FOLD_SHADER)
#undef VGFX3D_D3D11_FOLD_SHADER
    return hash;
}
