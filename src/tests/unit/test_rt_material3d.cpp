//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_material3d.cpp
// Purpose: Unit tests for Material3D scalar state, texture references, and
//   cloning/instance behavior.
//
// Key invariants:
//   - Material scalar setters clamp to their documented runtime ranges.
//   - Clones and instances retain resource handles but keep scalar overrides independent.
//   - Invalid private texture refs are repaired without releasing unowned objects.
//
// Ownership/Lifetime:
//   - Tests allocate GC-managed runtime objects directly and rely on process teardown.
//   - Trap assertions use a local setjmp guard and abort on unexpected traps.
//
// Links: src/runtime/graphics/3d/render/rt_material3d.c,
//        src/runtime/graphics/3d/render/rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_heap.h"
#include "rt_pixels.h"

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::abort();
}

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (std::fabs((a) - (b)) > (eps))                                                          \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

template <typename Fn> static bool expect_trap_contains(Fn &&fn, const char *needle) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && (!needle || std::strstr(g_last_trap, needle) != nullptr);
}

static void test_new_pbr_defaults() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new_pbr(0.8, 0.6, 0.4);
    EXPECT_TRUE(mat != nullptr, "Material3D.NewPBR creates a material");
    if (!mat)
        return;

    EXPECT_TRUE(mat->workflow == RT_MATERIAL3D_WORKFLOW_PBR,
                "Material3D.NewPBR selects the PBR workflow");
    EXPECT_NEAR(mat->diffuse[0], 0.8, 0.001, "Material3D.NewPBR stores albedo color");
    EXPECT_NEAR(mat->metallic, 0.0, 0.001, "Material3D.NewPBR defaults metallic to dielectric");
    EXPECT_NEAR(mat->roughness, 0.5, 0.001, "Material3D.NewPBR defaults roughness to mid value");
    EXPECT_NEAR(mat->ao, 1.0, 0.001, "Material3D.NewPBR defaults AO to full");
    EXPECT_NEAR(
        mat->emissive_intensity, 1.0, 0.001, "Material3D.NewPBR defaults emissive intensity to 1");
    EXPECT_NEAR(mat->normal_scale, 1.0, 0.001, "Material3D.NewPBR defaults normal scale to 1");
    EXPECT_TRUE(mat->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE,
                "Material3D.NewPBR defaults to opaque alpha mode");
    EXPECT_TRUE(mat->double_sided == 0, "Material3D.NewPBR defaults to backface culling");
}

static void test_pbr_setters_promote_legacy_material() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new_color(0.2, 0.4, 0.6);
    EXPECT_TRUE(mat != nullptr, "Material3D.NewColor creates a legacy material");
    if (!mat)
        return;

    EXPECT_TRUE(mat->workflow == RT_MATERIAL3D_WORKFLOW_LEGACY,
                "Material3D.NewColor stays on the legacy workflow");
    rt_material3d_set_metallic(mat, 0.85);
    rt_material3d_set_roughness(mat, 0.25);
    rt_material3d_set_ao(mat, 0.9);

    EXPECT_TRUE(mat->workflow == RT_MATERIAL3D_WORKFLOW_PBR,
                "PBR setters promote legacy materials into the PBR workflow");
    EXPECT_NEAR(rt_material3d_get_metallic(mat), 0.85, 0.001, "Metallic getter round-trips");
    EXPECT_NEAR(rt_material3d_get_roughness(mat), 0.25, 0.001, "Roughness getter round-trips");
    EXPECT_NEAR(rt_material3d_get_ao(mat), 0.9, 0.001, "AO getter round-trips");
}

static void test_anisotropy_clamps_and_round_trips() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    EXPECT_TRUE(mat != nullptr, "Material3D.New creates an anisotropy fixture");
    if (!mat)
        return;

    EXPECT_TRUE(rt_material3d_get_anisotropy(mat) == 1, "Material3D.Anisotropy defaults to off");
    rt_material3d_set_anisotropy(mat, 8);
    EXPECT_TRUE(rt_material3d_get_anisotropy(mat) == 8,
                "Material3D.Anisotropy round-trips valid values");
    EXPECT_TRUE(mat->anisotropy == 8,
                "Material3D.Anisotropy stores the material-wide sampler state");
    for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++)
        EXPECT_TRUE(mat->texture_slot_anisotropy[slot] == 8,
                    "Material3D.Anisotropy updates every texture slot");
    rt_material3d_set_anisotropy(mat, 0);
    EXPECT_TRUE(rt_material3d_get_anisotropy(mat) == 1, "Material3D.Anisotropy clamps zero to one");
    rt_material3d_set_anisotropy(mat, 64);
    EXPECT_TRUE(rt_material3d_get_anisotropy(mat) == 16,
                "Material3D.Anisotropy clamps high values to sixteen");
}

static void test_clone_and_instance_share_resources_but_copy_scalars() {
    rt_material3d *base = (rt_material3d *)rt_material3d_new_pbr(0.7, 0.5, 0.3);
    void *px = rt_pixels_new(1, 1);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    EXPECT_TRUE(base != nullptr, "Base PBR material exists");
    EXPECT_TRUE(px != nullptr && cubemap != nullptr, "Pixels and CubeMap fixtures exist");
    if (!base || !px || !cubemap)
        return;

    rt_material3d_set_albedo_map(base, px);
    rt_material3d_set_normal_map(base, px);
    rt_material3d_set_metallic_roughness_map(base, px);
    rt_material3d_set_ao_map(base, px);
    rt_material3d_set_emissive_map(base, px);
    rt_material3d_set_env_map(base, cubemap);
    rt_material3d_set_metallic(base, 0.9);
    rt_material3d_set_roughness(base, 0.15);
    rt_material3d_set_ao(base, 0.8);
    rt_material3d_set_emissive_intensity(base, 2.0);
    rt_material3d_set_normal_scale(base, 0.6);
    rt_material3d_set_anisotropy(base, 12);
    rt_material3d_set_alpha_mode(base, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    rt_material3d_set_double_sided(base, 1);

    rt_material3d *clone = (rt_material3d *)rt_material3d_clone(base);
    rt_material3d *inst = (rt_material3d *)rt_material3d_make_instance(base);
    EXPECT_TRUE(clone != nullptr, "Material3D.Clone duplicates materials");
    EXPECT_TRUE(inst != nullptr, "Material3D.MakeInstance duplicates materials");
    if (!clone || !inst)
        return;

    EXPECT_TRUE(clone->texture == base->texture && clone->normal_map == base->normal_map &&
                    clone->metallic_roughness_map == base->metallic_roughness_map &&
                    clone->ao_map == base->ao_map && clone->emissive_map == base->emissive_map,
                "Material3D.Clone shares immutable texture resources by reference");
    EXPECT_TRUE(clone->env_map == base->env_map,
                "Material3D.Clone shares environment resources by reference");
    EXPECT_TRUE(inst->texture == base->texture && inst->env_map == base->env_map,
                "Material3D.MakeInstance shares immutable resources by reference");

    rt_material3d_set_roughness(inst, 0.65);
    rt_material3d_set_metallic(inst, 0.1);
    rt_material3d_set_double_sided(inst, 0);

    EXPECT_NEAR(
        base->roughness, 0.15, 0.001, "Material3D.MakeInstance keeps scalar overrides independent");
    EXPECT_NEAR(
        base->metallic, 0.9, 0.001, "Instance scalar writes do not mutate the source material");
    EXPECT_TRUE(base->double_sided == 1 && inst->double_sided == 0,
                "Instance boolean overrides do not mutate the source material");
    EXPECT_NEAR(
        clone->emissive_intensity, 2.0, 0.001, "Material3D.Clone preserves PBR scalar state");
    EXPECT_TRUE(rt_material3d_get_anisotropy(clone) == 12 &&
                    rt_material3d_get_anisotropy(inst) == 12,
                "Material3D.Clone and MakeInstance preserve sampler anisotropy");
    EXPECT_TRUE(clone->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND,
                "Material3D.Clone preserves alpha-mode state");
}

static void test_setters_replace_wrong_class_private_refs_without_release() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    void *wrong = rt_material3d_new_color(0.4, 0.5, 0.6);
    void *px = rt_pixels_new(1, 1);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    EXPECT_TRUE(mat != nullptr && wrong != nullptr && px != nullptr && cubemap != nullptr,
                "Material private-slot corruption fixture exists");
    if (!mat || !wrong || !px || !cubemap)
        return;

    size_t wrong_refcnt = rt_heap_hdr(wrong)->refcnt;
    mat->texture = wrong;
    rt_material3d_set_texture(mat, px);
    EXPECT_TRUE(mat->texture == px, "Material3D.SetTexture replaces wrong-class private refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D.SetTexture does not release unowned wrong-class refs");

    mat->normal_map = wrong;
    rt_material3d_set_normal_map(mat, px);
    EXPECT_TRUE(mat->normal_map == px, "Material3D.SetNormalMap replaces wrong-class private refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D.SetNormalMap does not release unowned wrong-class refs");

    mat->metallic_roughness_map = wrong;
    rt_material3d_set_metallic_roughness_map(mat, px);
    EXPECT_TRUE(mat->metallic_roughness_map == px,
                "Material3D.SetMetallicRoughnessMap replaces wrong-class private refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D.SetMetallicRoughnessMap does not release unowned wrong-class refs");

    mat->ao_map = wrong;
    rt_material3d_set_ao_map(mat, px);
    EXPECT_TRUE(mat->ao_map == px, "Material3D.SetAOMap replaces wrong-class private refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D.SetAOMap does not release unowned wrong-class refs");

    mat->specular_map = wrong;
    rt_material3d_set_specular_map(mat, px);
    EXPECT_TRUE(mat->specular_map == px,
                "Material3D.SetSpecularMap replaces wrong-class private refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D.SetSpecularMap does not release unowned wrong-class refs");

    mat->emissive_map = wrong;
    rt_material3d_set_emissive_map(mat, px);
    EXPECT_TRUE(mat->emissive_map == px,
                "Material3D.SetEmissiveMap replaces wrong-class private refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D.SetEmissiveMap does not release unowned wrong-class refs");

    mat->env_map = wrong;
    rt_material3d_set_env_map(mat, cubemap);
    EXPECT_TRUE(mat->env_map == cubemap,
                "Material3D.SetEnvMap replaces wrong-class private env-map refs");
    EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                "Material3D.SetEnvMap does not release unowned wrong-class env-map refs");
}

static void test_env_map_setter_repairs_stale_slot_before_rejecting_new_value() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    void *wrong = rt_material3d_new_color(0.1, 0.2, 0.3);
    void *px = rt_pixels_new(1, 1);
    void *cubemap = rt_cubemap3d_new(px, px, px, px, px, px);
    EXPECT_TRUE(mat != nullptr && wrong != nullptr && px != nullptr && cubemap != nullptr,
                "Material stale env-map fixture exists");
    if (!mat || !wrong || !px || !cubemap)
        return;

    rt_material3d_set_env_map(mat, cubemap);
    EXPECT_TRUE(mat->env_map == cubemap, "Material3D.SetEnvMap binds a valid cubemap");
    ((rt_cubemap3d *)cubemap)->face_size = 2;
    EXPECT_TRUE(expect_trap_contains([&] { rt_material3d_set_env_map(mat, wrong); }, "CubeMap3D"),
                "Material3D.SetEnvMap traps on invalid replacements");
    EXPECT_TRUE(
        mat->env_map == cubemap && ((rt_cubemap3d *)cubemap)->face_size == 1,
        "Material3D.SetEnvMap repairs its current env-map before rejecting a bad replacement");
    EXPECT_TRUE(rt_material3d_get_has_env_map(mat) == 1,
                "Material3D env-map presence stays true after recoverable-state repair");
}

static void test_raw_reference_getters_repair_wrong_class_state() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    void *wrong = rt_material3d_new_color(0.3, 0.4, 0.5);
    EXPECT_TRUE(mat != nullptr && wrong != nullptr,
                "Material raw-reference corruption fixture exists");
    if (!mat || !wrong)
        return;

    size_t wrong_refcnt = rt_heap_hdr(wrong)->refcnt;
    void **slots[] = {&mat->texture,
                      &mat->normal_map,
                      &mat->specular_map,
                      &mat->emissive_map,
                      &mat->metallic_roughness_map,
                      &mat->ao_map,
                      &mat->lightmap,
                      &mat->env_map};
    void *(*getters[])(void *) = {rt_material3d_get_texture,
                                  rt_material3d_get_normal_map,
                                  rt_material3d_get_specular_map,
                                  rt_material3d_get_emissive_map,
                                  rt_material3d_get_metallic_roughness_map,
                                  rt_material3d_get_ao_map,
                                  rt_material3d_get_lightmap,
                                  rt_material3d_get_env_map};
    const char *messages[] = {"Texture getter clears wrong-class private state",
                              "NormalMap getter clears wrong-class private state",
                              "SpecularMap getter clears wrong-class private state",
                              "EmissiveMap getter clears wrong-class private state",
                              "MetallicRoughnessMap getter clears wrong-class private state",
                              "AOMap getter clears wrong-class private state",
                              "Lightmap getter clears wrong-class private state",
                              "EnvMap getter clears wrong-class private state"};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        *slots[i] = wrong;
        EXPECT_TRUE(getters[i](mat) == nullptr && *slots[i] == nullptr, messages[i]);
        EXPECT_TRUE(rt_heap_hdr(wrong)->refcnt == wrong_refcnt,
                    "Raw-reference repair does not release borrowed corrupt state");
    }
}

static void test_scalar_readback_repairs_corrupt_state() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    EXPECT_TRUE(mat != nullptr, "Material scalar-corruption fixture exists");
    if (!mat)
        return;

    mat->emissive[0] = NAN;
    mat->emissive[1] = INFINITY;
    mat->emissive[2] = -4.0;
    void *emissive = rt_material3d_get_emissive_color(mat);
    EXPECT_TRUE(emissive != nullptr, "EmissiveColor getter returns a repaired snapshot");
    EXPECT_TRUE(emissive && rt_vec3_x(emissive) == 0.0 && rt_vec3_y(emissive) == 0.0 &&
                    rt_vec3_z(emissive) == 0.0,
                "EmissiveColor getter bounds every corrupt channel");

    mat->shininess = INFINITY;
    mat->depth_bias = NAN;
    mat->slope_scaled_depth_bias = -INFINITY;
    mat->custom_params[3] = NAN;
    EXPECT_TRUE(rt_material3d_get_shininess(mat) == 0.0 && mat->shininess == 0.0,
                "Shininess getter persists a finite bounded fallback");
    EXPECT_TRUE(rt_material3d_get_depth_bias(mat) == 0.0 && mat->depth_bias == 0.0,
                "DepthBias getter persists a neutral non-finite fallback");
    EXPECT_TRUE(rt_material3d_get_depth_slope_bias(mat) == 0.0 &&
                    mat->slope_scaled_depth_bias == 0.0,
                "DepthSlopeBias getter persists a neutral non-finite fallback");
    EXPECT_TRUE(rt_material3d_get_custom_param(mat, 3) == 0.0 && mat->custom_params[3] == 0.0,
                "Custom parameter getter persists a neutral non-finite fallback");
}

static void test_symmetric_setters_use_neutral_nonfinite_fallbacks() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    EXPECT_TRUE(mat != nullptr, "Material symmetric-scalar fixture exists");
    if (!mat)
        return;

    rt_material3d_set_custom_param(mat, 2, NAN);
    rt_material3d_set_depth_bias(mat, INFINITY, -INFINITY);
    EXPECT_TRUE(mat->custom_params[2] == 0.0,
                "Custom parameter NaN does not become the negative magnitude limit");
    EXPECT_TRUE(mat->depth_bias == 0.0, "Constant depth-bias infinity uses the neutral fallback");
    EXPECT_TRUE(mat->slope_scaled_depth_bias == 0.0,
                "Slope depth-bias infinity uses the neutral fallback");
}

static void test_clone_preserves_and_repairs_extended_state() {
    rt_material3d *source = (rt_material3d *)rt_material3d_new();
    EXPECT_TRUE(source != nullptr, "Material clone extended-state fixture exists");
    if (!source)
        return;

    source->soft_fade = 12.5;
    rt_material3d_set_ssr_enabled(source, 1);
    rt_material3d *clone = (rt_material3d *)rt_material3d_clone(source);
    EXPECT_TRUE(clone != nullptr, "Material clone succeeds for extended state");
    EXPECT_TRUE(clone && clone->soft_fade == 12.5,
                "Material clone preserves soft-particle fade distance");
    EXPECT_TRUE(clone && clone->ssr_enabled == 1,
                "Material clone preserves screen-space-reflection opt-in");

    source->soft_fade = NAN;
    source->ssr_enabled = -7;
    source->alpha_mode_explicit = -9;
    rt_material3d *repaired = (rt_material3d *)rt_material3d_clone(source);
    EXPECT_TRUE(repaired != nullptr, "Material clone succeeds for corrupt extended state");
    EXPECT_TRUE(repaired && repaired->soft_fade == 0.0,
                "Material clone repairs a non-finite soft-particle fade distance");
    EXPECT_TRUE(repaired && repaired->ssr_enabled == 1,
                "Material clone canonicalizes retained SSR state");
    EXPECT_TRUE(repaired && repaired->alpha_mode_explicit == 1,
                "Material clone canonicalizes explicit alpha-mode state");
}

/* ADR 0312: the projected decal layer round-trips its source, projector and opacity,
 * survives Clone/MakeInstance, and refuses degenerate projectors. */
static void test_decal_layer_set_get_clone() {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new_pbr(1.0, 1.0, 1.0);
    void *px = rt_pixels_new(2, 2);
    EXPECT_TRUE(mat != nullptr && px != nullptr, "decal fixtures exist");
    if (!mat || !px)
        return;
    EXPECT_TRUE(rt_material3d_get_has_decal_map(mat) == 0, "decal starts unarmed");
    EXPECT_NEAR(rt_material3d_get_decal_opacity(mat), 1.0, 1e-9, "decal opacity defaults to 1");

    /* A source without a projector is not armed. */
    rt_material3d_set_decal_map(mat, px);
    EXPECT_TRUE(rt_material3d_get_has_decal_map(mat) == 0, "decal starts unarmed");

    /* Box centred at (0, 1, -0.1) looking along -Z: right = +X, up = +Y, 0.5 x 0.25
     * half extents, 0.2 deep. */
    rt_material3d_set_decal_projector(mat, 0.0, 1.0, -0.1, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.5, 0.25, 0.2);
    EXPECT_TRUE(rt_material3d_get_has_decal_map(mat) == 1, "decal armed once source and projector are set");
    /* Centre maps to (0.5, 0.5, 0); the +right/+up corner to (1, 0, 0); depth to 1. */
    const double *r = mat->decal_rows;
    auto s_at = [&](double x, double y, double z) { return r[0] * x + r[1] * y + r[2] * z + r[3]; };
    auto t_at = [&](double x, double y, double z) { return r[4] * x + r[5] * y + r[6] * z + r[7]; };
    auto d_at = [&](double x, double y, double z) { return r[8] * x + r[9] * y + r[10] * z + r[11]; };
    EXPECT_NEAR(s_at(0.0, 1.0, -0.1), 0.5, 1e-9, "centre maps to s=0.5");
    EXPECT_NEAR(t_at(0.0, 1.0, -0.1), 0.5, 1e-9, "centre maps to t=0.5");
    EXPECT_NEAR(d_at(0.0, 1.0, -0.1), 0.0, 1e-9, "centre maps to d=0");
    EXPECT_NEAR(s_at(0.5, 1.25, -0.1), 1.0, 1e-9, "+right corner maps to s=1");
    EXPECT_NEAR(t_at(0.5, 1.25, -0.1), 0.0, 1e-9, "+up corner maps to t=0 (image top)");
    EXPECT_NEAR(d_at(0.0, 1.0, -0.1 + 0.2), 1.0, 1e-9, "depth maps to d=1");
    /* forward = right x up = +Z. */
    EXPECT_NEAR(mat->decal_forward[2], 1.0, 1e-9, "forward = right x up");

    rt_material3d_set_decal_opacity(mat, 0.25);
    EXPECT_NEAR(rt_material3d_get_decal_opacity(mat), 0.25, 1e-9, "opacity round-trips");
    rt_material3d_set_decal_opacity(mat, 7.0);
    EXPECT_NEAR(rt_material3d_get_decal_opacity(mat), 1.0, 1e-9, "decal opacity defaults to 1");

    rt_material3d *inst = (rt_material3d *)rt_material3d_make_instance(mat);
    EXPECT_TRUE(inst != nullptr, "MakeInstance duplicates the decal layer");
    if (inst) {
        EXPECT_TRUE(rt_material3d_get_has_decal_map(inst) == 1, "instance keeps the decal");
        EXPECT_NEAR(inst->decal_rows[3], mat->decal_rows[3], 1e-12, "instance copies the projector");
        EXPECT_NEAR(rt_material3d_get_decal_opacity(inst), 1.0, 1e-9, "instance copies the opacity");
        rt_material3d_set_decal_map(inst, nullptr);
        EXPECT_TRUE(rt_material3d_get_has_decal_map(inst) == 0, "clearing the instance source disarms it");
        EXPECT_TRUE(rt_material3d_get_has_decal_map(mat) == 1, "decal armed once source and projector are set");
        if (rt_obj_release_check0(inst))
            rt_obj_free(inst);
    }

    if (rt_obj_release_check0(mat))
        rt_obj_free(mat);
    if (rt_obj_release_check0(px))
        rt_obj_free(px);
}

int main() {
    test_decal_layer_set_get_clone();
    test_new_pbr_defaults();
    test_pbr_setters_promote_legacy_material();
    test_anisotropy_clamps_and_round_trips();
    test_clone_and_instance_share_resources_but_copy_scalars();
    test_setters_replace_wrong_class_private_refs_without_release();
    test_env_map_setter_repairs_stale_slot_before_rejecting_new_value();
    test_raw_reference_getters_repair_wrong_class_state();
    test_scalar_readback_repairs_corrupt_state();
    test_symmetric_setters_use_neutral_nonfinite_fallbacks();
    test_clone_preserves_and_repairs_extended_state();

    if (tests_passed != tests_run) {
        std::fprintf(stderr, "test_rt_material3d: %d/%d checks passed\n", tests_passed, tests_run);
        return 1;
    }

    std::printf("test_rt_material3d: %d/%d checks passed\n", tests_passed, tests_run);
    return 0;
}
