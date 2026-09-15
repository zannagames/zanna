//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_vgfx3d_backend_d3d11_shader_bake.c
// Purpose: Windows tests for D3D11 shader compilation: the manifest flags keep HLSL NaN
//          guards, and the build-time DXBC is complete, correctly staged, built from this
//          tree's HLSL, and byte-identical to what the runtime compile path produces.
// Key invariants:
//   - Under VGFX3D_D3D11_SHADER_COMPILE_FLAGS `isnan()` survives as a NaN self-comparison.
//   - Every manifest entry has embedded DXBC whose reflected stage matches its target.
// Ownership/Lifetime:
//   - Every compiler blob and reflection interface is released by the test that made it.
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_shader_manifest.inc,
//        src/tools/d3d11_shader_bake/d3d11_shader_bake.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <windows.h>

#include "rt_textureasset3d.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_d3d11_shared.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VGFX3D_STR_IMPL(x) #x
#define VGFX3D_STR(x) VGFX3D_STR_IMPL(x)
#include "vgfx3d_backend_d3d11_shaders.inc"
/* The manifest expands the shader-source accessors defined above. */
#include "vgfx3d_backend_d3d11_shader_manifest.inc"
/* Build-time DXBC generated into the build tree by zanna_d3d11_shader_bake. */
#include "vgfx3d_backend_d3d11_bytecode.inc"

static int tests_run = 0;
static int tests_passed = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

/// The backend's finite-vector guard, reading a constant so FXC cannot fold the input.
static const char k_nan_guard_hlsl[] =
    "cbuffer Probe : register(b0) { float4 value; };\n"
    "bool finite3(float3 v) { return !any(isnan(v)) && !any(isinf(v)); }\n"
    "float4 main() : SV_Target {\n"
    "    return finite3(value.xyz) ? float4(value.xyz, 1.0) : float4(1.0, 0.0, 1.0, 1.0);\n"
    "}\n";

/// @brief Whether a disassembly listing holds an `ne` whose two compared operands are identical.
/// @details That is how FXC encodes `isnan(x)` (`x != x`), e.g.
///   `ne [precise(yzw)] r0.yzw, cb0[0].xxyz, cb0[0].xxyz`.
static int listing_has_nan_self_compare(const char *listing) {
    const char *line = listing;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length > 3u && strncmp(line, "ne ", 3u) == 0 && length < 256u) {
            char text[256];
            const char *lhs;
            const char *rhs;
            size_t rhs_length;
            memcpy(text, line, length);
            text[length] = '\0';
            lhs = strstr(text, ", ");
            rhs = lhs ? strstr(lhs + 2, ", ") : NULL;
            if (rhs) {
                size_t lhs_length = (size_t)(rhs - (lhs + 2));
                rhs += 2;
                rhs_length = strlen(rhs);
                while (rhs_length > 0 &&
                       (rhs[rhs_length - 1] == ' ' || rhs[rhs_length - 1] == '\r'))
                    rhs_length--;
                if (rhs_length == lhs_length && strncmp(lhs + 2, rhs, lhs_length) == 0)
                    return 1;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return 0;
}

/// @brief Compile the NaN-guard probe and report whether the NaN self-comparison survived.
/// @param[in] flags D3DCompile flags under test.
/// @param[out] compiled Set nonzero when compilation and disassembly both succeeded.
/// @return Nonzero when the disassembly compares the constant with itself (`x != x`).
static int nan_self_compare_survives(UINT flags, int *compiled) {
    ID3DBlob *bytecode = NULL;
    ID3DBlob *errors = NULL;
    ID3DBlob *listing = NULL;
    int survives = 0;
    HRESULT hr = D3DCompile(k_nan_guard_hlsl,
                            sizeof(k_nan_guard_hlsl) - 1u,
                            "nan_guard_probe",
                            NULL,
                            NULL,
                            "main",
                            "ps_5_0",
                            flags,
                            0,
                            &bytecode,
                            &errors);
    *compiled = 0;
    if (SUCCEEDED(hr) && bytecode &&
        SUCCEEDED(D3DDisassemble(ID3D10Blob_GetBufferPointer(bytecode),
                                 ID3D10Blob_GetBufferSize(bytecode),
                                 0,
                                 NULL,
                                 &listing)) &&
        listing) {
        *compiled = 1;
        survives = listing_has_nan_self_compare((const char *)ID3D10Blob_GetBufferPointer(listing));
    }
    if (listing)
        ID3D10Blob_Release(listing);
    if (errors)
        ID3D10Blob_Release(errors);
    if (bytecode)
        ID3D10Blob_Release(bytecode);
    return survives;
}

static void test_manifest_flags_keep_nan_guards(void) {
    int compiled = 0;
    int kept = nan_self_compare_survives(VGFX3D_D3D11_SHADER_COMPILE_FLAGS, &compiled);
    EXPECT_TRUE(compiled && kept,
                "D3D11 manifest flags keep the HLSL isnan() self-comparison (ZB-38)");
    kept = nan_self_compare_survives(D3DCOMPILE_ENABLE_STRICTNESS, &compiled);
    EXPECT_TRUE(compiled && !kept,
                "Without IEEE strictness FXC deletes isnan(), which is why the flag is required");
}

/// @brief Validate one embedded DXBC array: container magic and reflected shader stage.
static void check_embedded_bytecode(const char *entry,
                                    const char *target,
                                    const unsigned char *bytes,
                                    size_t size) {
    ID3D11ShaderReflection *reflection = NULL;
    D3D11_SHADER_DESC desc;
    UINT expected_stage = target[0] == 'v' ? D3D11_SHVER_VERTEX_SHADER : D3D11_SHVER_PIXEL_SHADER;
    char message[192];
    int staged = 0;

    snprintf(message, sizeof(message), "%s embedded bytecode is a DXBC container", entry);
    EXPECT_TRUE(size > 4u && memcmp(bytes, "DXBC", 4u) == 0, message);
    if (SUCCEEDED(D3DReflect(bytes, size, &IID_ID3D11ShaderReflection, (void **)&reflection)) &&
        reflection && SUCCEEDED(reflection->lpVtbl->GetDesc(reflection, &desc)))
        staged = D3D11_SHVER_GET_TYPE(desc.Version) == expected_stage;
    snprintf(
        message, sizeof(message), "%s embedded bytecode reflects as a %s shader", entry, target);
    EXPECT_TRUE(staged, message);
    if (reflection)
        reflection->lpVtbl->Release(reflection);
}

static void test_embedded_bytecode_covers_manifest(void) {
    EXPECT_TRUE(VGFX3D_D3D11_EMBEDDED_BYTECODE_DIGEST == vgfx3d_d3d11_shader_manifest_digest(),
                "Embedded DXBC was baked from this tree's HLSL and compile flags (ZB-39)");
#define CHECK_EMBEDDED_BYTECODE(member, source, entry, target)                                     \
    check_embedded_bytecode(                                                                       \
        entry, target, vgfx3d_d3d11_dxbc_##member, sizeof(vgfx3d_d3d11_dxbc_##member));
    VGFX3D_D3D11_SHADER_LIST(CHECK_EMBEDDED_BYTECODE)
#undef CHECK_EMBEDDED_BYTECODE
}

static void test_embedded_bytecode_is_runtime_compile_output(void) {
    ID3DBlob *bytecode = NULL;
    ID3DBlob *errors = NULL;
    const char *source = d3d11_skybox_shader_source;
    HRESULT hr = D3DCompile(source,
                            source ? strlen(source) : 0u,
                            "vgfx3d_d3d11",
                            NULL,
                            NULL,
                            "VSSkybox",
                            "vs_5_0",
                            VGFX3D_D3D11_SHADER_COMPILE_FLAGS,
                            0,
                            &bytecode,
                            &errors);
    EXPECT_TRUE(SUCCEEDED(hr) && bytecode &&
                    ID3D10Blob_GetBufferSize(bytecode) == sizeof(vgfx3d_d3d11_dxbc_vs_skybox) &&
                    memcmp(ID3D10Blob_GetBufferPointer(bytecode),
                           vgfx3d_d3d11_dxbc_vs_skybox,
                           sizeof(vgfx3d_d3d11_dxbc_vs_skybox)) == 0,
                "Baked DXBC is byte-identical to the runtime compile of the same entry");
    if (errors)
        ID3D10Blob_Release(errors);
    if (bytecode)
        ID3D10Blob_Release(bytecode);
}

int main(void) {
    test_manifest_flags_keep_nan_guards();
    test_embedded_bytecode_covers_manifest();
    test_embedded_bytecode_is_runtime_compile_output();
    printf("%d/%d D3D11 shader bake checks passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
