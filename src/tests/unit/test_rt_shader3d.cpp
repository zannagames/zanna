//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_shader3d.cpp
// Purpose: Verify ADR 0370 phase 1: `.zshader` header parsing, parameter
//          packing, per-backend sections, the exact error messages, and the
//          Material3D shader binding (defaults, named setters, textures).
// Key invariants:
//   - Params pack in declaration order with 16-byte block rules on every
//     backend; the generated declaration names them identically.
//   - A parse failure is recorded in Status/Error, never trapped.
//   - Binding a shader resets the material block to the declared defaults.
// Ownership/Lifetime:
//   - Every runtime object created here is released before the case ends.
// Links: docs/adr/0370-custom-shaders.md,
//        src/runtime/graphics/3d/render/rt_shader3d.c,
//        src/runtime/graphics/3d/render/rt_material3d.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_object.h"
#include "rt_string.h"

#include "../TestHarness.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
std::jmp_buf g_trap_jmp;
const char *g_last_trap = nullptr;
bool g_expect_trap = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::abort();
}

namespace {

template <typename Fn> bool traps_with(Fn &&fn, const char *expected) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && std::strcmp(g_last_trap, expected) == 0;
}

#define EXPECT_TRAPS(call, message) EXPECT_TRUE(traps_with([&]() { call; }, message))

rt_string str(const char *text) {
    return rt_string_from_bytes(text, strlen(text));
}

std::string take(rt_string s) {
    std::string out = s ? rt_string_cstr(s) : "";
    if (s)
        rt_str_release_maybe(s);
    return out;
}

const char *kHologram = "zshader 1\n"
                        "name Hologram\n"
                        "mode surface\n"
                        "blend alpha\n"
                        "cull none\n"
                        "# a comment line\n"
                        "param float scanSpeed 2.0\n"
                        "param float3 tint 0.2 0.8 1.0\n"
                        "param int bands 4\n"
                        "param float4 rect 1 2 3 4\n"
                        "texture noiseTex\n"
                        "[metal]\n"
                        "void zs_surface(...) { /* msl */ }\n"
                        "[hlsl]\n"
                        "void zs_surface(...) { /* hlsl */ }\n";

} // namespace

TEST(Shader3D, parses_header_sections_and_packs_params) {
    void *shader = rt_shader3d_from_source(str(kHologram));
    ASSERT_TRUE(shader != nullptr);
    EXPECT_EQ(take(rt_shader3d_get_name(shader)), std::string("Hologram"));
    EXPECT_EQ(rt_shader3d_get_mode(shader), (int64_t)RT_SHADER3D_MODE_SURFACE);
    /* Phase 1 has no backend compiler: a clean parse reports unsupported. */
    EXPECT_EQ(rt_shader3d_get_status(shader), (int64_t)RT_SHADER3D_STATUS_UNSUPPORTED);
    EXPECT_EQ((int)rt_shader3d_get_is_ready(shader), 0);
    EXPECT_EQ(take(rt_shader3d_get_error(shader)), std::string(""));
    EXPECT_EQ(rt_shader3d_get_param_count(shader), (int64_t)4);
    EXPECT_EQ(take(rt_shader3d_param_name(shader, 0)), std::string("scanSpeed"));
    EXPECT_EQ(take(rt_shader3d_param_type(shader, 0)), std::string("float"));
    EXPECT_EQ(take(rt_shader3d_param_name(shader, 1)), std::string("tint"));
    EXPECT_EQ(take(rt_shader3d_param_type(shader, 1)), std::string("float3"));
    EXPECT_EQ(take(rt_shader3d_param_type(shader, 2)), std::string("int"));
    EXPECT_EQ(take(rt_shader3d_param_type(shader, 3)), std::string("float4"));
    EXPECT_EQ(take(rt_shader3d_param_name(shader, 9)), std::string(""));
    EXPECT_EQ(rt_shader3d_get_texture_count(shader), (int64_t)1);
    EXPECT_EQ(take(rt_shader3d_texture_name(shader, 0)), std::string("noiseTex"));
    EXPECT_EQ((int)rt_shader3d_has_backend(shader, str("metal")), 1);
    EXPECT_EQ((int)rt_shader3d_has_backend(shader, str("hlsl")), 1);
    EXPECT_EQ((int)rt_shader3d_has_backend(shader, str("glsl")), 0);
    EXPECT_EQ((int)rt_shader3d_has_backend(shader, str("software")), 0);

    const rt_shader3d *s = (const rt_shader3d *)shader;
    EXPECT_EQ(s->blend, (int32_t)1);
    EXPECT_EQ(s->cull, (int32_t)2);
    /* float at lane 0; float3 needs a fresh 4-lane block -> 4; int fits at 8;
       float4 needs a fresh block -> 12; total 16 lanes. */
    EXPECT_EQ(s->params[0].offset, (int32_t)0);
    EXPECT_EQ(s->params[1].offset, (int32_t)4);
    EXPECT_EQ(s->params[2].offset, (int32_t)8);
    EXPECT_EQ(s->params[3].offset, (int32_t)12);
    EXPECT_EQ(s->param_floats, (int32_t)16);
    EXPECT_NEAR(s->params[1].defaults[2], 1.0, 1e-9);
    EXPECT_NEAR(s->params[3].defaults[3], 4.0, 1e-9);
    EXPECT_EQ(rt_shader3d_find_param(s, "bands"), (int32_t)2);
    EXPECT_EQ(rt_shader3d_find_param(s, "nope"), (int32_t)-1);
    EXPECT_EQ(rt_shader3d_find_texture(s, "noiseTex"), (int32_t)0);
    const char *msl = rt_shader3d_backend_source(s, RT_SHADER3D_BACKEND_METAL);
    ASSERT_TRUE(msl != nullptr);
    std::string msl_text = msl;
    EXPECT_CONTAINS(msl_text, "/* msl */");
    EXPECT_TRUE(rt_shader3d_backend_source(s, RT_SHADER3D_BACKEND_GLSL) == nullptr);

    char *decl = rt_shader3d_params_declaration(s, RT_SHADER3D_BACKEND_METAL);
    ASSERT_TRUE(decl != nullptr);
    std::string decl_text = decl;
    EXPECT_CONTAINS(decl_text, "struct ZsParams");
    EXPECT_CONTAINS(decl_text, "scanSpeed");
    EXPECT_CONTAINS(decl_text, "tint");
    free(decl);
    char *hlsl = rt_shader3d_params_declaration(s, RT_SHADER3D_BACKEND_HLSL);
    ASSERT_TRUE(hlsl != nullptr);
    std::string hlsl_text = hlsl;
    EXPECT_CONTAINS(hlsl_text, "rect");
    free(hlsl);
    rt_obj_release_check0(shader);
}

TEST(Shader3D, reports_parse_errors_with_exact_messages) {
    struct Case {
        const char *text;
        const char *message;
    } cases[] = {
        {"name X\n", "Shader3D: missing 'zshader 1' header"},
        {"zshader 1\nfrobnicate 3\n", "Shader3D: line 2: unknown directive 'frobnicate'"},
        {"zshader 1\nparam float a\nparam float a\n", "Shader3D: param 'a' redeclared"},
        {"zshader 1\ntexture a\ntexture b\ntexture c\ntexture d\ntexture e\n",
         "Shader3D: more than 4 textures"},
    };

    for (const Case &c : cases) {
        void *shader = rt_shader3d_from_source(str(c.text));
        ASSERT_TRUE(shader != nullptr);
        EXPECT_EQ(rt_shader3d_get_status(shader), (int64_t)RT_SHADER3D_STATUS_FAILED);
        EXPECT_EQ(take(rt_shader3d_get_error(shader)), std::string(c.message));
        rt_obj_release_check0(shader);
    }

    std::string overflow = "zshader 1\n";
    for (int i = 0; i < 17; i++)
        overflow += "param float4 v" + std::to_string(i) + "\n";
    void *shader = rt_shader3d_from_source(str(overflow.c_str()));
    ASSERT_TRUE(shader != nullptr);
    EXPECT_EQ(take(rt_shader3d_get_error(shader)),
              std::string("Shader3D: params exceed 64 floats"));
    rt_obj_release_check0(shader);

    void *missing = rt_shader3d_load(str("/nonexistent/dir/none.zshader"));
    ASSERT_TRUE(missing != nullptr);
    EXPECT_EQ(rt_shader3d_get_status(missing), (int64_t)RT_SHADER3D_STATUS_FAILED);
    EXPECT_EQ(take(rt_shader3d_get_error(missing)),
              std::string("Shader3D.Load: cannot read '/nonexistent/dir/none.zshader'"));
    rt_obj_release_check0(missing);
}

TEST(Shader3D, load_and_reload_from_disk) {
    const char *path = "test_rt_shader3d_scratch.zshader";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fputs("zshader 1\nname First\nparam float a 1\n[glsl]\nvoid main(){}\n", f);
    fclose(f);
    void *shader = rt_shader3d_load(str(path));
    ASSERT_TRUE(shader != nullptr);
    EXPECT_EQ(take(rt_shader3d_get_name(shader)), std::string("First"));
    EXPECT_EQ((int)rt_shader3d_has_backend(shader, str("glsl")), 1);
    f = fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    fputs("zshader 1\nname Second\nparam float2 uv 0 1\n", f);
    fclose(f);
    rt_shader3d_reload(shader);
    EXPECT_EQ(take(rt_shader3d_get_name(shader)), std::string("Second"));
    EXPECT_EQ(take(rt_shader3d_param_type(shader, 0)), std::string("float2"));
    EXPECT_EQ((int)rt_shader3d_has_backend(shader, str("glsl")), 0);
    remove(path);
    rt_obj_release_check0(shader);
}

TEST(Shader3D, material_binding_defaults_setters_and_textures) {
    void *shader = rt_shader3d_from_source(str(kHologram));
    void *mat = rt_material3d_new();
    ASSERT_TRUE(shader != nullptr);
    ASSERT_TRUE(mat != nullptr);
    EXPECT_TRUE(rt_material3d_get_shader(mat) == nullptr);
    rt_material3d_set_shader(mat, shader);
    EXPECT_TRUE(rt_material3d_get_shader(mat) == shader);
    rt_material3d *m = (rt_material3d *)mat;
    EXPECT_NEAR(m->shader_params[0], 2.0, 1e-9);
    EXPECT_NEAR(m->shader_params[4], 0.2, 1e-9);
    EXPECT_NEAR(m->shader_params[6], 1.0, 1e-9);
    EXPECT_NEAR(m->shader_params[8], 4.0, 1e-9);
    EXPECT_NEAR(m->shader_params[15], 4.0, 1e-9);

    rt_material3d_set_shader_param(mat, str("scanSpeed"), 7.5);
    EXPECT_NEAR(rt_material3d_get_shader_param(mat, str("scanSpeed")), 7.5, 1e-9);
    rt_material3d_set_shader_param3(mat, str("tint"), 0.1, 0.2, 0.3);
    EXPECT_NEAR(m->shader_params[4], 0.1, 1e-9);
    EXPECT_NEAR(m->shader_params[6], 0.3, 1e-9);
    /* Fewer components than declared is allowed (first lanes only). */
    rt_material3d_set_shader_param2(mat, str("rect"), 9, 8);
    EXPECT_NEAR(m->shader_params[12], 9.0, 1e-9);
    EXPECT_NEAR(m->shader_params[13], 8.0, 1e-9);
    EXPECT_NEAR(m->shader_params[15], 4.0, 1e-9);
    rt_material3d_set_shader_int(mat, str("bands"), 11);
    EXPECT_NEAR(rt_material3d_get_shader_param(mat, str("bands")), 11.0, 1e-9);
    EXPECT_NEAR(rt_material3d_get_shader_param(mat, str("unknown")), 0.0, 1e-9);

    /* Clone carries the shader and the block. */
    void *copy = rt_material3d_clone(mat);
    ASSERT_TRUE(copy != nullptr);
    EXPECT_TRUE(rt_material3d_get_shader(copy) == shader);
    EXPECT_NEAR(rt_material3d_get_shader_param(copy, str("scanSpeed")), 7.5, 1e-9);
    rt_obj_release_check0(copy);

    /* Re-binding the same shader resets to defaults. */
    rt_material3d_set_shader(mat, shader);
    EXPECT_NEAR(rt_material3d_get_shader_param(mat, str("scanSpeed")), 2.0, 1e-9);

    rt_material3d_clear_shader(mat);
    EXPECT_TRUE(rt_material3d_get_shader(mat) == nullptr);
    EXPECT_NEAR(rt_material3d_get_shader_param(mat, str("scanSpeed")), 0.0, 1e-9);

    rt_obj_release_check0(mat);
    rt_obj_release_check0(shader);
}

TEST(Shader3D, material_setters_trap_on_unknown_or_mismatched_params) {
    void *shader = rt_shader3d_from_source(str(kHologram));
    void *mat = rt_material3d_new();
    ASSERT_TRUE(shader != nullptr && mat != nullptr);
    EXPECT_TRAPS(rt_material3d_set_shader_param(mat, str("scanSpeed"), 1.0),
                 "Material3D.SetShaderParam: material has no shader");
    rt_material3d_set_shader(mat, shader);
    EXPECT_TRAPS(rt_material3d_set_shader_param(mat, str("missing"), 1.0),
                 "Material3D.SetShaderParam: unknown param 'missing'");
    EXPECT_TRAPS(rt_material3d_set_shader_param4(mat, str("scanSpeed"), 1, 2, 3, 4),
                 "Material3D.SetShaderParam4: param 'scanSpeed' is float");
    EXPECT_TRAPS(rt_material3d_set_shader_int(mat, str("tint"), 3),
                 "Material3D.SetShaderInt: param 'tint' is float3");
    EXPECT_TRAPS(rt_material3d_set_shader_texture(mat, str("albedoTex"), nullptr),
                 "Material3D.SetShaderTexture: unknown texture 'albedoTex'");
    EXPECT_TRAPS(rt_material3d_set_shader(mat, mat),
                 "Material3D.SetShader: shader must be a Shader3D");
    rt_obj_release_check0(mat);
    rt_obj_release_check0(shader);
}

int main() {
    return zanna_test::run_all_tests();
}
