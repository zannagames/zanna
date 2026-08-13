//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_vgfx3d_backend_d3d11_shared.c
// Purpose: Unit tests for D3D11 backend shared helper functions.
//
// Key invariants:
//   - Shared helper math and cache policies are deterministic without D3D11.
//   - Constant-buffer layouts remain compatible with backend shaders.
//   - Sampler-state cache indices stay clamped to their fixed array bounds.
//
// Ownership/Lifetime:
//   - Tests allocate only stack-local fixtures and process-local scratch buffers.
//
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_shared.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_textureasset3d.h"
#include "vgfx3d_backend_d3d11_shared.h"

#define VGFX3D_STR_IMPL(x) #x
#define VGFX3D_STR(x) VGFX3D_STR_IMPL(x)
// The embedded HLSL shader sources are single string literals well past ISO
// C99's 4095-char minimum, which Apple Clang flags under -Werror. The runtime
// target that also includes this file is not built warning-as-error, so this
// suppression only re-aligns the test's stricter flags with it. Guarded to
// GCC/Clang so MSVC (which uses a different long-string limit) is untouched.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "vgfx3d_backend_d3d11_shaders.inc"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef VGFX3D_STR
#undef VGFX3D_STR_IMPL

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZANNA_SOURCE_DIR
#define ZANNA_SOURCE_DIR "."
#endif

static int tests_run = 0;
static int tests_passed = 0;

static int contains_text(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static size_t count_text(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t needle_len;

    if (!haystack || !needle || !needle[0])
        return 0;
    needle_len = strlen(needle);
    while ((haystack = strstr(haystack, needle)) != NULL) {
        count++;
        haystack += needle_len;
    }
    return count;
}

static int text_appears_in_order_after(const char *source,
                                       const char *anchor,
                                       const char *first,
                                       const char *second) {
    const char *anchor_pos = source ? strstr(source, anchor) : NULL;
    const char *first_pos = anchor_pos ? strstr(anchor_pos, first) : NULL;
    const char *second_pos = first_pos ? strstr(first_pos, second) : NULL;
    return second_pos != NULL;
}

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (fabs((double)(a) - (double)(b)) > (eps))                                               \
            fprintf(stderr, "FAIL: %s (got %.6f expected %.6f)\n", msg, (double)(a), (double)(b)); \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

static void set_identity4x4(float *m) {
    memset(m, 0, sizeof(float) * 16u);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void test_pack_bone_palette_identity_pads_unused_bones(void) {
    float src[16];
    float dst[VGFX3D_D3D11_MAX_BONES * 16];

    for (int i = 0; i < 16; i++)
        src[i] = (float)(i + 1);
    memset(dst, 0xCD, sizeof(dst));
    vgfx3d_d3d11_pack_bone_palette(dst, src, 1);

    for (int i = 0; i < 16; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Bone palette preserves matrix element %d", i);
        EXPECT_NEAR(dst[i], src[i], 1e-6f, msg);
    }
    EXPECT_NEAR(dst[16], 1.0f, 1e-6f, "Bone palette identity-pads the second bone");
    EXPECT_NEAR(dst[21], 1.0f, 1e-6f, "Bone palette identity-pads the second bone diagonal");
    EXPECT_NEAR(dst[31], 1.0f, 1e-6f, "Bone palette identity-pads the second bone tail diagonal");
    EXPECT_NEAR(dst[17], 0.0f, 1e-6f, "Bone palette clears off-diagonal padding");
    EXPECT_NEAR(dst[sizeof(dst) / sizeof(dst[0]) - 1],
                1.0f,
                1e-6f,
                "Bone palette identity-pads the tail of the upload buffer");
}

static void test_pack_bone_palette_identity_pads_empty_source(void) {
    float dst[VGFX3D_D3D11_MAX_BONES * 16];

    memset(dst, 0xCD, sizeof(dst));
    vgfx3d_d3d11_pack_bone_palette(dst, NULL, 0);

    EXPECT_NEAR(dst[0], 1.0f, 1e-6f, "Empty bone palette starts with identity");
    EXPECT_NEAR(dst[5], 1.0f, 1e-6f, "Empty bone palette fills identity diagonal");
    EXPECT_NEAR(dst[10], 1.0f, 1e-6f, "Empty bone palette fills third diagonal");
    EXPECT_NEAR(dst[15], 1.0f, 1e-6f, "Empty bone palette fills fourth diagonal");
    EXPECT_NEAR(dst[1], 0.0f, 1e-6f, "Empty bone palette clears off-diagonal values");
}

static void test_pack_bone_palette_keeps_highest_supported_bone(void) {
    float src[VGFX3D_D3D11_BONE_PALETTE_FLOATS];
    float dst[VGFX3D_D3D11_BONE_PALETTE_FLOATS];
    size_t tail = (size_t)(VGFX3D_D3D11_MAX_BONES - 1) * 16u;

    for (size_t i = 0; i < sizeof(src) / sizeof(src[0]); i++)
        src[i] = (float)i;
    memset(dst, 0, sizeof(dst));
    vgfx3d_d3d11_pack_bone_palette(dst, src, VGFX3D_D3D11_MAX_BONES);

    EXPECT_NEAR(
        dst[tail + 0], src[tail + 0], 1e-6f, "Bone packing preserves the final supported bone");
    EXPECT_NEAR(dst[tail + 15],
                src[tail + 15],
                1e-6f,
                "Bone packing preserves the tail matrix element of the final supported bone");
}

static void test_pack_bone_palette_identity_pads_invalid_bones(void) {
    float src[16];
    float dst[VGFX3D_D3D11_BONE_PALETTE_FLOATS];

    set_identity4x4(src);
    src[3] = HUGE_VALF;
    memset(dst, 0xCD, sizeof(dst));
    vgfx3d_d3d11_pack_bone_palette(dst, src, 1);

    EXPECT_NEAR(dst[0], 1.0f, 1e-6f, "Invalid bone matrix falls back to identity");
    EXPECT_NEAR(dst[3], 0.0f, 1e-6f, "Invalid bone matrix clears non-finite translation");
    EXPECT_NEAR(dst[15], 1.0f, 1e-6f, "Invalid bone matrix preserves identity tail");
}

static void test_bone_palette_upload_size_covers_supported_bone_count(void) {
    EXPECT_TRUE(VGFX3D_D3D11_BONE_PALETTE_FLOATS == VGFX3D_D3D11_MAX_BONES * 16u,
                "Bone palette upload has one 4x4 matrix per supported bone");
    EXPECT_TRUE(VGFX3D_D3D11_BONE_PALETTE_BYTES == sizeof(float) * VGFX3D_D3D11_MAX_BONES * 16u,
                "Bone palette upload byte size matches the packed float payload");
    EXPECT_TRUE(VGFX3D_D3D11_BONE_PALETTE_BYTES == 16384u,
                "D3D11 bone cbuffer covers all 256 shader palette entries");
}

static void test_pack_scalar_array4_matches_hlsl_layout(void) {
    float packed[2][4];
    float src[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};

    vgfx3d_d3d11_pack_scalar_array4(packed, 2, src, 8);
    EXPECT_NEAR(packed[0][0], 1.0f, 1e-6f, "Packed scalar array stores lane 0");
    EXPECT_NEAR(packed[0][3], 4.0f, 1e-6f, "Packed scalar array stores lane 3");
    EXPECT_NEAR(packed[1][0], 5.0f, 1e-6f, "Packed scalar array advances to the second vector");
    EXPECT_NEAR(packed[1][3], 8.0f, 1e-6f, "Packed scalar array stores the final scalar");

    src[2] = HUGE_VALF;
    src[5] = -HUGE_VALF;
    vgfx3d_d3d11_pack_scalar_array4(packed, 2, src, 8);
    EXPECT_NEAR(packed[0][2], 0.0f, 1e-6f, "Packed scalar array sanitizes positive infinity");
    EXPECT_NEAR(packed[1][1], 0.0f, 1e-6f, "Packed scalar array sanitizes negative infinity");
}

static void test_finite_copy_helpers(void) {
    float good[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float bad[4] = {1.0f, HUGE_VALF, 3.0f, -HUGE_VALF};
    float direction[3] = {0.0f, 0.0f, -2.0f};
    float zero_direction[3] = {0.0f, 0.0f, 0.0f};
    float huge_direction[3] = {1.0e11f, 0.0f, 0.0f};
    float fallback_direction[3] = {1.0f, 0.0f, 0.0f};
    float copied_direction[3];
    float copied[4];
    float extreme[4] = {-5.0f, 2.0f, 8.0f, HUGE_VALF};
    float matrix[16];
    float fallback[16];
    float out[16];

    EXPECT_TRUE(vgfx3d_d3d11_float_array_is_finite(good, 4u) == 1,
                "Finite-array helper accepts finite values");
    EXPECT_TRUE(vgfx3d_d3d11_float_array_is_finite(bad, 4u) == 0,
                "Finite-array helper rejects non-finite values");
    vgfx3d_d3d11_copy_float_array_finite_or(copied, bad, 4u, 0.25f);
    EXPECT_NEAR(copied[0], 1.0f, 1e-6f, "Finite-array copy preserves finite lanes");
    EXPECT_NEAR(copied[1], 0.25f, 1e-6f, "Finite-array copy replaces positive infinity");
    EXPECT_NEAR(copied[3], 0.25f, 1e-6f, "Finite-array copy replaces negative infinity");
    EXPECT_TRUE(vgfx3d_d3d11_float_array_is_bounded(good, 4u, 4.0f) == 1,
                "Bounded-array helper accepts values within the requested limit");
    EXPECT_TRUE(vgfx3d_d3d11_float_array_is_bounded(good, 4u, 3.0f) == 0,
                "Bounded-array helper rejects finite extremes outside the limit");
    vgfx3d_d3d11_copy_float_array_clamped_finite_or(copied, extreme, 4u, -2.0f, 4.0f, 0.5f);
    EXPECT_NEAR(copied[0], -2.0f, 1e-6f, "Clamped copy applies its lower bound");
    EXPECT_NEAR(copied[1], 2.0f, 1e-6f, "Clamped copy preserves in-range values");
    EXPECT_NEAR(copied[2], 4.0f, 1e-6f, "Clamped copy applies its upper bound");
    EXPECT_NEAR(copied[3], 0.5f, 1e-6f, "Clamped copy replaces non-finite values");

    set_identity4x4(matrix);
    matrix[7] = HUGE_VALF;
    vgfx3d_d3d11_copy_mat4_finite_or_identity(out, matrix);
    EXPECT_NEAR(out[0], 1.0f, 1e-6f, "Invalid matrix identity fallback writes first diagonal");
    EXPECT_NEAR(out[7], 0.0f, 1e-6f, "Invalid matrix identity fallback clears bad lane");

    set_identity4x4(fallback);
    fallback[3] = 9.0f;
    vgfx3d_d3d11_copy_mat4_finite_or(out, matrix, fallback);
    EXPECT_NEAR(out[3], 9.0f, 1e-6f, "Invalid matrix copies finite fallback matrix");

    set_identity4x4(matrix);
    matrix[3] = VGFX3D_D3D11_MATRIX_COMPONENT_ABS_MAX * 2.0f;
    vgfx3d_d3d11_copy_mat4_finite_or_identity(out, matrix);
    EXPECT_NEAR(out[3], 0.0f, 1e-6f, "Matrix copy rejects finite overflow-prone components");
    memset(matrix, 0, sizeof(matrix));
    EXPECT_TRUE(vgfx3d_d3d11_shadow_matrix_is_usable(matrix) == 0,
                "Shadow matrix validation rejects a zero transform");
    set_identity4x4(matrix);
    EXPECT_TRUE(vgfx3d_d3d11_shadow_matrix_is_usable(matrix) == 1,
                "Shadow matrix validation accepts a bounded non-zero transform");

    EXPECT_TRUE(vgfx3d_d3d11_vec3_direction_is_usable(direction) == 1,
                "Direction helper accepts finite non-degenerate vectors");
    EXPECT_TRUE(vgfx3d_d3d11_vec3_direction_is_usable(zero_direction) == 0,
                "Direction helper rejects zero-length vectors before HLSL normalize");
    EXPECT_TRUE(vgfx3d_d3d11_vec3_direction_is_usable(huge_direction) == 0,
                "Direction helper rejects vectors whose squared length exceeds shader guards");
    vgfx3d_d3d11_copy_vec3_direction_or(copied_direction, direction, fallback_direction);
    EXPECT_NEAR(copied_direction[2], -1.0f, 1e-6f, "Direction copy normalizes valid vectors");
    vgfx3d_d3d11_copy_vec3_direction_or(copied_direction, zero_direction, fallback_direction);
    EXPECT_NEAR(copied_direction[0], 1.0f, 1e-6f, "Direction copy uses fallback X");
    EXPECT_NEAR(copied_direction[2], 0.0f, 1e-6f, "Direction copy uses fallback Z");
    vgfx3d_d3d11_copy_vec3_direction_or(copied_direction, bad, zero_direction);
    EXPECT_NEAR(copied_direction[0], 0.0f, 1e-6f, "Direction copy defaults invalid fallback X");
    EXPECT_NEAR(copied_direction[2], -1.0f, 1e-6f, "Direction copy defaults invalid fallback Z");
}

static void test_constant_buffer_struct_sizes_match_expected_layout(void) {
    EXPECT_TRUE(sizeof(vgfx3d_d3d11_per_object_t) == 752u,
                "PerObject C struct matches the packed HLSL cbuffer size");
    EXPECT_TRUE(sizeof(vgfx3d_d3d11_per_material_t) == 448u,
                "PerMaterial C struct matches the packed HLSL cbuffer size");
}

static void test_fill_instance_data_uses_previous_or_current_matrices(void) {
    vgfx3d_d3d11_instance_data_t instances[2];
    float models[32];
    float prev_models[32];

    set_identity4x4(&models[0]);
    set_identity4x4(&models[16]);
    models[3] = 2.0f;
    models[19] = -4.0f;
    set_identity4x4(&prev_models[0]);
    set_identity4x4(&prev_models[16]);
    prev_models[3] = 1.0f;
    prev_models[19] = -6.0f;

    memset(instances, 0, sizeof(instances));
    vgfx3d_d3d11_fill_instance_data(instances, 2, models, prev_models, 1);
    EXPECT_NEAR(instances[0].model[3], 2.0f, 1e-6f, "Instance data preserves the model matrix");
    EXPECT_NEAR(instances[0].prev_model[3],
                1.0f,
                1e-6f,
                "Instance data preserves the previous model matrix");
    EXPECT_NEAR(instances[1].prev_model[3],
                -6.0f,
                1e-6f,
                "Instance data preserves the second previous model matrix");

    memset(instances, 0, sizeof(instances));
    vgfx3d_d3d11_fill_instance_data(instances, 2, models, NULL, 0);
    EXPECT_NEAR(instances[1].prev_model[3],
                instances[1].model[3],
                1e-6f,
                "Instance data falls back to the current matrix when no history exists");
    models[16] = HUGE_VALF;
    memset(instances, 0, sizeof(instances));
    vgfx3d_d3d11_fill_instance_data(instances, 2, models, NULL, 0);
    EXPECT_NEAR(instances[1].model[0],
                1.0f,
                1e-6f,
                "Instance data replaces invalid model matrices with identity");
    EXPECT_NEAR(instances[1].model[3], 0.0f, 1e-6f, "Instance data clears non-finite matrix lanes");
    EXPECT_TRUE(vgfx3d_d3d11_should_use_previous_instance_matrices(prev_models, 1) == 1,
                "Previous-instance helper accepts a flagged non-null history payload");
    EXPECT_TRUE(vgfx3d_d3d11_should_use_previous_instance_matrices(NULL, 1) == 0,
                "Previous-instance helper rejects a missing history payload");
    EXPECT_TRUE(vgfx3d_d3d11_should_use_previous_instance_matrices(prev_models, 0) == 0,
                "Previous-instance helper rejects an unflagged history payload");
}

static void test_frame_history_preserves_scene_state_across_overlay_passes(void) {
    vgfx3d_d3d11_frame_history_t history;
    float scene_vp0[16];
    float scene_vp1[16];
    float overlay_vp[16];
    float inv0[16];
    float inv1[16];
    float overlay_inv[16];
    float cam0[3] = {1.0f, 2.0f, 3.0f};
    float cam1[3] = {4.0f, 5.0f, 6.0f};
    float overlay_cam[3] = {9.0f, 8.0f, 7.0f};

    memset(&history, 0, sizeof(history));
    for (int i = 0; i < 16; i++) {
        scene_vp0[i] = (float)(i + 1);
        scene_vp1[i] = (float)(i + 21);
        overlay_vp[i] = (float)(i + 41);
        inv0[i] = (float)(i + 61);
        inv1[i] = (float)(i + 81);
        overlay_inv[i] = (float)(i + 101);
    }

    vgfx3d_d3d11_update_frame_history(&history, scene_vp0, inv0, cam0, 0, 0);
    EXPECT_TRUE(history.scene_history_valid == 1, "Main-pass history becomes valid on first scene");
    EXPECT_NEAR(history.scene_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "First main pass seeds prevViewProjection from the current scene");
    EXPECT_NEAR(history.draw_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "First main pass uses the current VP as draw-time history");

    vgfx3d_d3d11_update_frame_history(&history, scene_vp1, inv1, cam1, 0, 0);
    EXPECT_NEAR(history.scene_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "Second main pass preserves the previous scene VP");
    EXPECT_NEAR(history.scene_vp[0], scene_vp1[0], 1e-6f, "Second main pass updates scene VP");
    EXPECT_NEAR(history.draw_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "Second main pass draws against the prior scene VP");
    EXPECT_NEAR(
        history.scene_inv_vp[0], inv1[0], 1e-6f, "Second main pass updates the scene inverse VP");
    EXPECT_NEAR(history.scene_cam_pos[0],
                cam1[0],
                1e-6f,
                "Second main pass updates the scene camera position");

    vgfx3d_d3d11_update_frame_history(&history, overlay_vp, overlay_inv, overlay_cam, 1, 1);
    EXPECT_NEAR(history.scene_vp[0],
                scene_vp1[0],
                1e-6f,
                "Overlay pass preserves the scene VP for later postfx");
    EXPECT_NEAR(history.scene_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "Overlay pass preserves the previous scene VP");
    EXPECT_NEAR(
        history.scene_inv_vp[0], inv1[0], 1e-6f, "Overlay pass preserves the scene inverse VP");
    EXPECT_NEAR(history.scene_cam_pos[0],
                cam1[0],
                1e-6f,
                "Overlay pass preserves the scene camera position");
    EXPECT_NEAR(history.draw_prev_vp[0],
                overlay_vp[0],
                1e-6f,
                "Overlay pass uses its own VP for draw-time history");
    EXPECT_TRUE(history.overlay_used_this_frame == 1,
                "Overlay pass marks the separate overlay target as used");

    memset(&history, 0, sizeof(history));
    scene_vp0[0] = HUGE_VALF;
    inv0[5] = -HUGE_VALF;
    cam0[1] = HUGE_VALF;
    vgfx3d_d3d11_update_frame_history(&history, scene_vp0, inv0, cam0, 0, 0);
    EXPECT_NEAR(history.scene_vp[0],
                1.0f,
                1e-6f,
                "Frame history replaces invalid scene VP matrices with identity");
    EXPECT_NEAR(history.scene_vp[1],
                0.0f,
                1e-6f,
                "Frame history clears invalid scene VP off-diagonal lanes");
    EXPECT_NEAR(history.scene_inv_vp[5],
                1.0f,
                1e-6f,
                "Frame history replaces invalid inverse VP matrices with identity");
    EXPECT_NEAR(history.scene_cam_pos[1],
                0.0f,
                1e-6f,
                "Frame history replaces invalid camera-position lanes with zero");
}

static void test_upload_status_helpers_drop_stale_state(void) {
    vgfx3d_d3d11_per_object_t object_data;

    memset(&object_data, 0, sizeof(object_data));
    object_data.has_skinning = 1;
    object_data.has_prev_skinning = 1;
    vgfx3d_d3d11_resolve_bone_upload_status(&object_data, 0, 1);
    EXPECT_TRUE(object_data.has_skinning == 0 && object_data.has_prev_skinning == 0,
                "Bone upload failure disables both current and previous skinning flags");
    EXPECT_TRUE(vgfx3d_d3d11_should_enable_skinning((const float *)&object_data,
                                                    VGFX3D_D3D11_MAX_BONES + 8) == 1,
                "Skinning enable helper keeps oversized palettes active for clamped uploads");
    EXPECT_TRUE(vgfx3d_d3d11_should_enable_skinning(NULL, VGFX3D_D3D11_MAX_BONES + 8) == 0,
                "Skinning enable helper still rejects missing palettes");

    memset(&object_data, 0, sizeof(object_data));
    object_data.has_skinning = 1;
    object_data.has_prev_skinning = 1;
    object_data.morph_shape_count = 4;
    object_data.vertex_count = 16;
    object_data.has_prev_morph_weights = 1;
    object_data.has_morph_normal_deltas = 1;
    vgfx3d_d3d11_resolve_morph_upload_status(&object_data, 1, 0);
    EXPECT_TRUE(object_data.morph_shape_count == 4 && object_data.vertex_count == 16,
                "Morph normal upload failure keeps the main morph payload active");
    EXPECT_TRUE(object_data.has_morph_normal_deltas == 0,
                "Morph normal upload failure disables only the normal-delta flag");

    vgfx3d_d3d11_resolve_morph_upload_status(&object_data, 0, 0);
    EXPECT_TRUE(object_data.morph_shape_count == 0 && object_data.vertex_count == 0 &&
                    object_data.has_prev_morph_weights == 0 &&
                    object_data.has_morph_normal_deltas == 0,
                "Morph upload failure clears all morph-related state");
}

static void test_target_kind_blend_and_color_format_helpers(void) {
    vgfx3d_draw_cmd_t cmd;

    EXPECT_TRUE(vgfx3d_d3d11_choose_target_kind(1, 1, 0) == VGFX3D_D3D11_TARGET_RTT,
                "RTT rendering always chooses the RTT target kind");
    EXPECT_TRUE(vgfx3d_d3d11_choose_target_kind(0, 0, 1) == VGFX3D_D3D11_TARGET_SWAPCHAIN,
                "Without GPU postfx the backend renders directly to the swapchain");
    EXPECT_TRUE(vgfx3d_d3d11_choose_target_kind(0, 1, 0) == VGFX3D_D3D11_TARGET_SCENE,
                "GPU postfx main passes render into the HDR scene target");
    EXPECT_TRUE(vgfx3d_d3d11_choose_target_kind(0, 1, 1) == VGFX3D_D3D11_TARGET_OVERLAY,
                "GPU postfx overlay passes render into the overlay target");
    EXPECT_TRUE(vgfx3d_d3d11_should_load_existing_color(VGFX3D_D3D11_TARGET_SCENE, 1, 0) == 1,
                "Scene passes honor requested color preservation");
    EXPECT_TRUE(vgfx3d_d3d11_should_load_existing_color(VGFX3D_D3D11_TARGET_OVERLAY, 1, 0) == 0,
                "First overlay pass clears the overlay target");
    EXPECT_TRUE(vgfx3d_d3d11_should_load_existing_color(VGFX3D_D3D11_TARGET_OVERLAY, 1, 1) == 1,
                "Later overlay passes preserve prior overlay contents");

    memset(&cmd, 0, sizeof(cmd));
    cmd.workflow = RT_MATERIAL3D_WORKFLOW_PBR;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    EXPECT_TRUE(vgfx3d_d3d11_choose_blend_mode(&cmd) == VGFX3D_D3D11_BLEND_ALPHA,
                "PBR blend materials use alpha blending");
    EXPECT_TRUE(vgfx3d_d3d11_choose_motion_attachment_mode(VGFX3D_D3D11_TARGET_SCENE, &cmd) ==
                    VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY,
                "Alpha-blended scene draws disable the motion attachment");
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_MASK;
    EXPECT_TRUE(vgfx3d_d3d11_choose_blend_mode(&cmd) == VGFX3D_D3D11_BLEND_OPAQUE,
                "PBR mask materials keep opaque render-target writes");
    EXPECT_TRUE(vgfx3d_d3d11_choose_motion_attachment_mode(VGFX3D_D3D11_TARGET_SCENE, &cmd) ==
                    VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_AND_MOTION,
                "Opaque scene draws keep the motion attachment enabled");
    EXPECT_TRUE(vgfx3d_d3d11_choose_motion_attachment_mode(VGFX3D_D3D11_TARGET_SWAPCHAIN, &cmd) ==
                    VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY,
                "Swapchain draws never target a scene-motion attachment");
    EXPECT_TRUE(vgfx3d_d3d11_choose_motion_attachment_mode(VGFX3D_D3D11_TARGET_SCENE, NULL) ==
                    VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY,
                "Missing draw commands never target the motion attachment");
    cmd.disable_depth_test = 1;
    EXPECT_TRUE(vgfx3d_d3d11_choose_motion_attachment_mode(VGFX3D_D3D11_TARGET_SCENE, &cmd) ==
                    VGFX3D_D3D11_MOTION_ATTACHMENTS_COLOR_ONLY,
                "Depth-disabled overlay-style draws do not corrupt scene motion vectors");
    cmd.disable_depth_test = 0;
    cmd.workflow = 0;
    cmd.alpha = 0.5f;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    EXPECT_TRUE(vgfx3d_d3d11_choose_blend_mode(&cmd) == VGFX3D_D3D11_BLEND_ALPHA,
                "Legacy translucent materials use alpha blending");
    cmd.alpha = 1.0f;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    EXPECT_TRUE(vgfx3d_d3d11_choose_blend_mode(&cmd) == VGFX3D_D3D11_BLEND_ALPHA,
                "Legacy explicit blend materials use alpha blending");
    cmd.alpha = 0.25f;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_MASK;
    EXPECT_TRUE(vgfx3d_d3d11_choose_blend_mode(&cmd) == VGFX3D_D3D11_BLEND_OPAQUE,
                "Legacy masked materials stay on the opaque render path");

    EXPECT_TRUE(vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SCENE) ==
                    VGFX3D_D3D11_COLOR_FORMAT_HDR16F,
                "Scene rendering uses the HDR color format class");
    EXPECT_TRUE(vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_SWAPCHAIN) ==
                        VGFX3D_D3D11_COLOR_FORMAT_UNORM8 &&
                    vgfx3d_d3d11_choose_color_format(VGFX3D_D3D11_TARGET_OVERLAY) ==
                        VGFX3D_D3D11_COLOR_FORMAT_UNORM8,
                "Swapchain and overlay targets use UNORM8 output");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_color_format(VGFX3D_D3D11_COLOR_FORMAT_UNORM8) == 1 &&
                    vgfx3d_d3d11_is_valid_color_format(VGFX3D_D3D11_COLOR_FORMAT_HDR16F) == 1,
                "D3D11 target validation accepts both internal color formats");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_color_format(99) == 0,
                "D3D11 target validation rejects unknown color formats");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_bloom_mip_count(1) == 1 &&
                    vgfx3d_d3d11_is_valid_bloom_mip_count(VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX) == 1,
                "Bloom validation accepts every fixed-array mip count");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_bloom_mip_count(0) == 0 &&
                    vgfx3d_d3d11_is_valid_bloom_mip_count(VGFX3D_D3D11_BLOOM_MIP_COUNT_MAX + 1) ==
                        0,
                "Bloom validation rejects empty and out-of-bounds cached chains");
    EXPECT_TRUE(vgfx3d_d3d11_srv_is_ready(1, 0) == 1,
                "Completed D3D11 SRVs advertise their authored material feature");
    EXPECT_TRUE(vgfx3d_d3d11_srv_is_ready(1, 1) == 0 && vgfx3d_d3d11_srv_is_ready(0, 0) == 0,
                "Missing and streaming-fallback SRVs do not masquerade as resident maps");
    EXPECT_TRUE(vgfx3d_d3d11_has_complete_splat(1, 1, 1, 1, 1, 1) == 1,
                "Splatting is enabled when the weight map and all four layers are bound");
    EXPECT_TRUE(vgfx3d_d3d11_has_complete_splat(1, 1, 1, 0, 1, 1) == 0,
                "Splatting is disabled when any layer texture is missing");
    EXPECT_TRUE(vgfx3d_d3d11_has_complete_splat(0, 1, 1, 1, 1, 1) == 0,
                "Splatting respects the draw command enable flag");
}

static void test_capacity_and_mip_helpers(void) {
    size_t bytes = 0;
    size_t capacity = 0;
    uint32_t byte_width = 0;
    uint32_t row_pitch = 0;
    int32_t mip_extent = 0;
    int32_t bloom_w = 0;
    int32_t bloom_h = 0;

    EXPECT_TRUE(vgfx3d_d3d11_compute_mip_count(1, 1) == 1, "1x1 textures use a single mip level");
    EXPECT_TRUE(vgfx3d_d3d11_compute_mip_count(4, 2) == 3,
                "Mip-count helper follows the full downsample chain");
    EXPECT_TRUE(vgfx3d_d3d11_compute_mip_count(0, 2) == 0,
                "Invalid texture dimensions fail closed instead of publishing a mip");
    EXPECT_TRUE(vgfx3d_d3d11_next_capacity(0, 65, 64) == 128,
                "Capacity helper grows fixed caches beyond the old hard cap");
    EXPECT_TRUE(vgfx3d_d3d11_next_capacity(16, 8, 16) == 16,
                "Capacity helper keeps existing storage when it is already large enough");
    EXPECT_TRUE(vgfx3d_d3d11_next_capacity(0, 0, 0) == 1,
                "Capacity helper never returns a non-positive capacity");
    EXPECT_TRUE(vgfx3d_d3d11_next_capacity(INT_MAX / 2 + 1, INT_MAX - 1, 64) == INT_MAX - 1,
                "Capacity helper saturates without overflowing signed int");
    EXPECT_TRUE(vgfx3d_d3d11_compute_row_bytes(7, 4, &bytes) == 1 && bytes == 28u,
                "Row-byte helper computes tightly packed RGBA8 rows");
    EXPECT_TRUE(vgfx3d_d3d11_compute_row_bytes(7, 8, &bytes) == 1 && bytes == 56u,
                "Row-byte helper computes tightly packed RGBA16F rows");
    EXPECT_TRUE(vgfx3d_d3d11_compute_row_bytes(0, 4, &bytes) == 0 && bytes == 0u,
                "Row-byte helper rejects non-positive widths");
    EXPECT_TRUE(vgfx3d_d3d11_compute_buffer_byte_width(64u, &byte_width) == 1 && byte_width == 64u,
                "Buffer ByteWidth helper preserves valid non-zero spans");
    EXPECT_TRUE(vgfx3d_d3d11_compute_buffer_byte_width(0u, &byte_width) == 0 && byte_width == 0u,
                "Buffer ByteWidth helper rejects zero-byte buffers");
    EXPECT_TRUE(vgfx3d_d3d11_compute_buffer_byte_width((size_t)UINT_MAX + 1u, &byte_width) == 0 &&
                    byte_width == 0u,
                "Buffer ByteWidth helper rejects spans beyond D3D11 UINT fields");
    EXPECT_TRUE(vgfx3d_d3d11_compute_constant_buffer_byte_width(1u, &byte_width) == 1 &&
                    byte_width == 16u,
                "Constant-buffer ByteWidth helper aligns small structs to sixteen bytes");
    EXPECT_TRUE(vgfx3d_d3d11_compute_constant_buffer_byte_width(16u, &byte_width) == 1 &&
                    byte_width == 16u,
                "Constant-buffer ByteWidth helper preserves already-aligned structs");
    EXPECT_TRUE(vgfx3d_d3d11_compute_constant_buffer_byte_width(17u, &byte_width) == 1 &&
                    byte_width == 32u,
                "Constant-buffer ByteWidth helper rounds up partial float4 slots");
    EXPECT_TRUE(vgfx3d_d3d11_compute_constant_buffer_byte_width(
                    VGFX3D_D3D11_MAX_CONSTANT_BUFFER_BYTES, &byte_width) == 1 &&
                    byte_width == VGFX3D_D3D11_MAX_CONSTANT_BUFFER_BYTES,
                "Constant-buffer ByteWidth helper accepts the D3D11 64 KiB limit");
    EXPECT_TRUE(vgfx3d_d3d11_compute_constant_buffer_byte_width(
                    VGFX3D_D3D11_MAX_CONSTANT_BUFFER_BYTES + 1u, &byte_width) == 0 &&
                    byte_width == 0u,
                "Constant-buffer ByteWidth helper rejects oversized cbuffers");
    EXPECT_TRUE(vgfx3d_d3d11_constant_buffer_desc_is_usable(32u, 1, 1, 1, 0u, 0u) == 1,
                "Constant-buffer descriptor validation accepts the dynamic write contract");
    EXPECT_TRUE(vgfx3d_d3d11_constant_buffer_desc_is_usable(31u, 1, 1, 1, 0u, 0u) == 0,
                "Constant-buffer descriptor validation rejects unaligned storage");
    EXPECT_TRUE(vgfx3d_d3d11_constant_buffer_desc_is_usable(32u, 0, 1, 1, 0u, 0u) == 0,
                "Constant-buffer descriptor validation rejects non-dynamic buffers");
    EXPECT_TRUE(vgfx3d_d3d11_constant_buffer_desc_is_usable(32u, 1, 0, 1, 0u, 0u) == 0,
                "Constant-buffer descriptor validation rejects the wrong bind class");
    EXPECT_TRUE(vgfx3d_d3d11_constant_buffer_desc_is_usable(32u, 1, 1, 0, 0u, 0u) == 0,
                "Constant-buffer descriptor validation requires CPU write access");
    EXPECT_TRUE(vgfx3d_d3d11_constant_buffer_desc_is_usable(32u, 1, 1, 1, 1u, 0u) == 0,
                "Constant-buffer descriptor validation rejects incompatible misc flags");
    EXPECT_TRUE(vgfx3d_d3d11_dynamic_buffer_desc_is_usable(4096u, 4096u, 1024u, 1, 1, 1, 0u, 0u) ==
                    1,
                "Dynamic-buffer descriptor validation accepts matching mapped storage");
    EXPECT_TRUE(vgfx3d_d3d11_dynamic_buffer_desc_is_usable(1024u, 4096u, 2048u, 1, 1, 1, 0u, 0u) ==
                    0,
                "Dynamic-buffer descriptor validation rejects overstated cached capacity");
    EXPECT_TRUE(vgfx3d_d3d11_dynamic_buffer_desc_is_usable(4096u, 4096u, 4097u, 1, 1, 1, 0u, 0u) ==
                    0,
                "Dynamic-buffer descriptor validation rejects uploads beyond ByteWidth");
    EXPECT_TRUE(vgfx3d_d3d11_dynamic_buffer_desc_is_usable(4096u, 4096u, 1024u, 1, 0, 1, 0u, 0u) ==
                    0,
                "Dynamic-buffer descriptor validation rejects a mismatched bind class");
    EXPECT_TRUE(vgfx3d_d3d11_float_srv_buffer_desc_is_usable(4096u, 1024u, 512u, 1, 1, 1, 0u, 0u) ==
                    1,
                "Float-SRV buffer validation accepts matching typed storage");
    EXPECT_TRUE(vgfx3d_d3d11_float_srv_buffer_desc_is_usable(1024u, 1024u, 512u, 1, 1, 1, 0u, 0u) ==
                    0,
                "Float-SRV buffer validation rejects overstated cached capacity");
    EXPECT_TRUE(
        vgfx3d_d3d11_float_srv_buffer_desc_is_usable(4096u, 1024u, 1025u, 1, 1, 1, 0u, 0u) == 0,
        "Float-SRV buffer validation rejects uploads beyond capacity");
    EXPECT_TRUE(vgfx3d_d3d11_float_srv_view_desc_is_usable(1024u, 1, 1, 0u, 1024u) == 1,
                "Float-SRV view validation accepts an exact full-buffer view");
    EXPECT_TRUE(vgfx3d_d3d11_float_srv_view_desc_is_usable(1024u, 1, 1, 1u, 1023u) == 0,
                "Float-SRV view validation rejects offset and narrowed views");
    EXPECT_TRUE(vgfx3d_d3d11_compute_rgba8_upload_pitch(3, &row_pitch) == 1 && row_pitch == 12u,
                "RGBA8 upload-pitch helper computes tightly packed rows");
    EXPECT_TRUE(vgfx3d_d3d11_compute_rgba8_upload_pitch(0, &row_pitch) == 0 && row_pitch == 0u,
                "RGBA8 upload-pitch helper rejects invalid widths");
    EXPECT_TRUE(vgfx3d_d3d11_compute_rgba8_upload_pitch(INT_MAX, &row_pitch) == 0 &&
                    row_pitch == 0u,
                "RGBA8 upload-pitch helper rejects pitches beyond D3D11 UINT fields");
    EXPECT_TRUE(vgfx3d_d3d11_expected_square_mip_extent(8, 0, &mip_extent) == 1 && mip_extent == 8,
                "Square mip extent helper preserves level zero");
    EXPECT_TRUE(vgfx3d_d3d11_expected_square_mip_extent(8, 3, &mip_extent) == 1 && mip_extent == 1,
                "Square mip extent helper reaches the tail mip");
    EXPECT_TRUE(vgfx3d_d3d11_expected_square_mip_extent(8, 4, &mip_extent) == 0 && mip_extent == 0,
                "Square mip extent helper rejects levels beyond the chain");
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_mip_extent(8, 1, 4, 4) == 1,
                "IBL mip validation accepts the expected destination extent");
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_mip_extent(8, 1, 4, 2) == 0,
                "IBL mip validation rejects non-square payloads");
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_mip_extent(8, 4, 1, 1) == 0,
                "IBL mip validation rejects levels beyond the cubemap chain");
    EXPECT_TRUE(vgfx3d_d3d11_compute_bloom_mip_extent(1, 1, 0, &bloom_w, &bloom_h) == 1 &&
                    bloom_w == 1 && bloom_h == 1,
                "Bloom mip helper keeps tiny valid viewports allocatable");
    EXPECT_TRUE(vgfx3d_d3d11_compute_bloom_mip_extent(17, 9, 0, &bloom_w, &bloom_h) == 1 &&
                    bloom_w == 8 && bloom_h == 4,
                "Bloom mip helper computes the first half-res target with floor division");
    EXPECT_TRUE(vgfx3d_d3d11_compute_bloom_mip_extent(128, 64, 1, &bloom_w, &bloom_h) == 1 &&
                    bloom_w == 32 && bloom_h == 16,
                "Bloom mip helper halves deeper levels while the next mip stays useful");
    EXPECT_TRUE(vgfx3d_d3d11_compute_bloom_mip_extent(17, 9, 1, &bloom_w, &bloom_h) == 0 &&
                    bloom_w == 0 && bloom_h == 0,
                "Bloom mip helper stops before deeper levels collapse below the bloom floor");
    EXPECT_TRUE(vgfx3d_d3d11_compute_bloom_mip_extent(0, 64, 0, &bloom_w, &bloom_h) == 0 &&
                    bloom_w == 0 && bloom_h == 0,
                "Bloom mip helper rejects invalid base extents");
    EXPECT_TRUE(vgfx3d_d3d11_compute_instance_upload_bytes(
                    3, sizeof(vgfx3d_d3d11_instance_data_t), &bytes) == 1 &&
                    bytes == 3u * sizeof(vgfx3d_d3d11_instance_data_t),
                "Instance upload helper computes the D3D11 vertex-buffer byte span");
    EXPECT_TRUE(vgfx3d_d3d11_compute_instance_upload_bytes(
                    0, sizeof(vgfx3d_d3d11_instance_data_t), &bytes) == 0 &&
                    bytes == 0u,
                "Instance upload helper rejects non-positive instance counts");
    EXPECT_TRUE(vgfx3d_d3d11_compute_instance_upload_bytes(
                    INT_MAX, sizeof(vgfx3d_d3d11_instance_data_t), &bytes) == 0 &&
                    bytes == 0u,
                "Instance upload helper rejects spans larger than a D3D11 UINT ByteWidth");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_update_bytes(3, 8, &bytes) == 1 && bytes == 12u,
                "Float-SRV update helper covers only live elements, not total capacity");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_update_bytes(9, 8, &bytes) == 0 && bytes == 0u,
                "Float-SRV update helper rejects spans beyond allocated capacity");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_update_bytes(
                    1u, (size_t)VGFX3D_D3D11_MAX_BUFFER_TEXELS + 1u, &bytes) == 0 &&
                    bytes == 0u,
                "Float-SRV update helper rejects a corrupted oversized capacity");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_float_srv_element_count(VGFX3D_D3D11_MAX_BUFFER_TEXELS) == 1,
                "Float-SRV element validation accepts the D3D11 typed-buffer limit");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_float_srv_element_count(
                    (size_t)VGFX3D_D3D11_MAX_BUFFER_TEXELS + 1u) == 0,
                "Float-SRV element validation rejects descriptors beyond the typed-buffer limit");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_capacity(0u, 3u, &capacity) == 1 &&
                    capacity == VGFX3D_D3D11_MIN_FLOAT_SRV_CAPACITY,
                "Float-SRV capacity starts at a reusable minimum allocation");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_capacity(256u, 300u, &capacity) == 1 &&
                    capacity == 512u,
                "Float-SRV capacity grows geometrically instead of reallocating every shape");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_capacity(512u, 300u, &capacity) == 1 &&
                    capacity == 512u,
                "Float-SRV capacity preserves sufficient existing storage");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_capacity(
                    (size_t)VGFX3D_D3D11_MAX_BUFFER_TEXELS + 1u, 3u, &capacity) == 1 &&
                    capacity == VGFX3D_D3D11_MIN_FLOAT_SRV_CAPACITY,
                "Float-SRV capacity recovers from invalid cached metadata");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_capacity(
                    0u, (size_t)VGFX3D_D3D11_MAX_BUFFER_TEXELS + 1u, &capacity) == 0 &&
                    capacity == 0u,
                "Float-SRV capacity rejects growth beyond the typed-buffer limit");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_update_bytes((size_t)UINT_MAX / sizeof(float) + 1u,
                                                            (size_t)UINT_MAX / sizeof(float) + 1u,
                                                            &bytes) == 0 &&
                    bytes == 0u,
                "Float-SRV update helper rejects ranges wider than D3D11 update boxes");
    EXPECT_TRUE(vgfx3d_d3d11_compute_float_srv_update_bytes(
                    SIZE_MAX / sizeof(float) + 1u, SIZE_MAX, &bytes) == 0 &&
                    bytes == 0u,
                "Float-SRV update helper rejects byte-size overflow");
    EXPECT_TRUE(vgfx3d_d3d11_validate_rgba8_destination(3, 2, 12, &bytes) == 1 && bytes == 24u,
                "RGBA8 destination validation returns the full writable span");
    EXPECT_TRUE(vgfx3d_d3d11_validate_rgba8_destination(3, 2, 12, NULL) == 1,
                "RGBA8 destination validation still checks spans without an out parameter");
    EXPECT_TRUE(vgfx3d_d3d11_validate_rgba8_destination(3, 2, 8, &bytes) == 0,
                "RGBA8 destination validation rejects short strides");
    EXPECT_TRUE(vgfx3d_d3d11_validate_row_span(16, 4, 8) == 1,
                "Row-span validation accepts a contained upload band");
    EXPECT_TRUE(vgfx3d_d3d11_validate_row_span(16, 15, 1) == 1,
                "Row-span validation accepts the final texture row");
    EXPECT_TRUE(vgfx3d_d3d11_validate_row_span(16, 15, 2) == 0,
                "Row-span validation rejects bands past the texture extent");
    EXPECT_TRUE(vgfx3d_d3d11_row_upload_cursor_is_valid(16, 0) == 1,
                "Row-upload cursor accepts the first row");
    EXPECT_TRUE(vgfx3d_d3d11_row_upload_cursor_is_valid(16, 15) == 1,
                "Row-upload cursor accepts the final pending row");
    EXPECT_TRUE(vgfx3d_d3d11_row_upload_cursor_is_valid(16, 16) == 0,
                "Row-upload cursor rejects completed state mislabeled as pending");
    EXPECT_TRUE(vgfx3d_d3d11_row_upload_cursor_is_valid(16, -1) == 0,
                "Row-upload cursor rejects negative state instead of rewinding it");
    EXPECT_TRUE(vgfx3d_d3d11_cubemap_upload_cursor_is_valid(16, 5, 15) == 1,
                "Cubemap cursor accepts the final face and row");
    EXPECT_TRUE(vgfx3d_d3d11_cubemap_upload_cursor_is_valid(16, 6, 0) == 0,
                "Cubemap cursor rejects a face beyond the cube");
    EXPECT_TRUE(vgfx3d_d3d11_native_upload_cursor_is_valid(4, 2, 8u, 7) == 1,
                "Native upload cursor accepts an in-range mip block row");
    EXPECT_TRUE(vgfx3d_d3d11_native_upload_cursor_is_valid(4, 4, 8u, 0) == 0,
                "Native upload cursor rejects a completed mip chain marked pending");
    EXPECT_TRUE(vgfx3d_d3d11_native_upload_cursor_is_valid(4, 2, 8u, 8) == 0,
                "Native upload cursor rejects an exhausted block-row cursor");
    EXPECT_TRUE(vgfx3d_d3d11_validate_row_span(16, -1, 1) == 0,
                "Row-span validation rejects negative row cursors");
}

static void test_cluster_ibl_and_upload_budget_validation(void) {
    vgfx3d_cluster_table_t table;
    int32_t ibl_level = -1;

    memset(&table, 0, sizeof(table));
    table.lights_revision = 7;
    table.global_light_count = 1;
    table.binned_light_count = 2;
    table.znear = 0.1f;
    table.zfar = 1000.0f;
    for (int32_t i = 1; i <= VGFX3D_CLUSTER_COUNT; i++)
        table.offsets[i] = 2;
    table.indices[0] = 1;
    table.indices[1] = 2;

    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 1,
                "Cluster validation accepts a bounded revision-matched table");
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(NULL, 7, 3) == 0,
                "Cluster validation rejects a missing table");
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 8, 3) == 0,
                "Cluster validation rejects a stale light revision");
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 0, 3) == 0,
                "Cluster validation rejects an unknown expected revision");

    table.global_light_count = 4;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects a global prefix beyond uploaded lights");
    table.global_light_count = 1;
    table.binned_light_count = 3;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects an oversized binned-light count");
    table.binned_light_count = 1;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects an incomplete binned-light suffix");
    table.binned_light_count = 2;
    table.overflow_count = -1;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects negative overflow telemetry");
    table.overflow_count = 0;
    table.zfar = table.znear;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects a degenerate depth range");
    table.zfar = 1000.0f;
    table.znear = NAN;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects a non-finite near plane");
    table.znear = 0.1f;
    table.zfar = NAN;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects a non-finite far plane");
    table.zfar = 1000.0f;

    table.offsets[0] = 1;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation requires the prefix sum to start at zero");
    table.offsets[0] = 0;
    table.offsets[2] = 1;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects descending offsets");
    table.offsets[2] = 2;
    table.offsets[VGFX3D_CLUSTER_COUNT] = VGFX3D_MAX_CLUSTER_LIGHT_INDICES + 1;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects offsets beyond the packed index buffer");
    table.offsets[VGFX3D_CLUSTER_COUNT] = 2;
    table.indices[0] = 0;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects duplicated global-prefix indices");
    table.indices[0] = 3;
    EXPECT_TRUE(vgfx3d_d3d11_cluster_table_is_usable(&table, 7, 3) == 0,
                "Cluster validation rejects indices beyond uploaded lights");

    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_layout(256, 64, 3, 6, &ibl_level) == 1,
                "IBL validation accepts a reachable bounded mip chain");
    EXPECT_TRUE(ibl_level == 2, "IBL validation reports the destination base mip");
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_layout(256, 64, 7, 6, &ibl_level) == 0,
                "IBL validation rejects counts beyond the fixed payload array");
    ibl_level = 99;
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_layout(256, 48, 2, 6, &ibl_level) == 0,
                "IBL validation rejects a base size absent from the destination chain");
    EXPECT_TRUE(ibl_level == 0, "Rejected IBL layouts clear the destination base mip");
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_layout(8, 4, 4, 6, &ibl_level) == 0,
                "IBL validation rejects a chain longer than remaining destination mips");
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_layout(256, 64, INT_MAX, 6, &ibl_level) == 0,
                "IBL validation rejects huge counts without signed overflow");
    EXPECT_TRUE(vgfx3d_d3d11_validate_ibl_layout(256, 64, 2, 6, NULL) == 0,
                "IBL validation requires an output base-mip destination");

    EXPECT_TRUE(vgfx3d_d3d11_upload_budget_allows(UINT64_MAX, UINT64_MAX, 4096) == 1,
                "Unlimited upload budgets admit complete temporary resources");
    EXPECT_TRUE(vgfx3d_d3d11_upload_budget_allows(1024, 256, 768) == 1,
                "Upload budget admits a resource that exactly fits the remainder");
    EXPECT_TRUE(vgfx3d_d3d11_upload_budget_allows(1024, 256, 769) == 0,
                "Upload budget rejects a resource larger than the remainder");
    EXPECT_TRUE(vgfx3d_d3d11_upload_budget_allows(0, 0, 1) == 0,
                "Zero upload budget pauses new temporary resources");
    EXPECT_TRUE(vgfx3d_d3d11_upload_budget_allows(1024, 1024, 1) == 0,
                "Exhausted upload budget pauses new temporary resources");
    EXPECT_TRUE(vgfx3d_d3d11_upload_budget_allows(UINT64_MAX - 1, UINT64_MAX - 2, 2) == 0,
                "Bounded upload budget subtraction does not wrap near UINT64_MAX");
    EXPECT_TRUE(vgfx3d_d3d11_upload_budget_allows(0, UINT64_MAX, 0) == 1,
                "Zero-byte upload work is always admissible");
    EXPECT_TRUE(vgfx3d_d3d11_cached_pending_texture_bytes(1, 4096, -1, -1, -1, 1) == 4096,
                "Native texture telemetry uses its cache-owned byte count");
    EXPECT_TRUE(vgfx3d_d3d11_cached_pending_texture_bytes(1, 4096, -1, -1, -1, 0) == 0,
                "Completed native texture telemetry clears stale cached bytes");
    EXPECT_TRUE(vgfx3d_d3d11_cached_pending_texture_bytes(0, 999, 8, 4, 3, 1) == 32,
                "RGBA texture telemetry derives the remaining row bytes");
}

static void test_d3d11_sanitization_helpers(void) {
    size_t elements = 0;
    float znear = 0.0f;
    float zfar = 0.0f;
    float fog_near = 0.0f;
    float fog_far = 0.0f;

    EXPECT_TRUE(vgfx3d_d3d11_sanitize_bool_flag(0) == 0, "Boolean flag sanitizer preserves false");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_bool_flag(-7) == 1,
                "Boolean flag sanitizer maps non-zero values to true");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_texture_uv_set(-4) == 0,
                "UV-set sanitizer maps negative selectors to uv0");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_texture_uv_set(0) == 0,
                "UV-set sanitizer preserves uv0 selectors");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_texture_uv_set(7) == 1,
                "UV-set sanitizer maps all positive selectors to uv1");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_int_param(5, 2, 8) == 5,
                "Integer parameter clamp preserves in-range values");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_int_param(99, 2, 8) == 8,
                "Integer parameter clamp caps high values");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_int_param(-4, 2, 8) == 2,
                "Integer parameter clamp raises low values");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_int_param(5, 8, 2) == 5,
                "Integer parameter clamp tolerates inverted bounds");
    EXPECT_NEAR(vgfx3d_d3d11_finite_or(2.5f, 1.0f),
                2.5f,
                1e-6f,
                "Finite float sanitizer preserves finite parameters");
    EXPECT_NEAR(vgfx3d_d3d11_finite_or(HUGE_VALF, 1.0f),
                1.0f,
                1e-6f,
                "Finite float sanitizer replaces positive infinity");
    EXPECT_NEAR(vgfx3d_d3d11_finite_or(-HUGE_VALF, -2.0f),
                -2.0f,
                1e-6f,
                "Finite float sanitizer replaces negative infinity");
    EXPECT_NEAR(vgfx3d_d3d11_finite_or(HUGE_VALF, HUGE_VALF),
                0.0f,
                1e-6f,
                "Finite float sanitizer never returns a non-finite fallback");
    EXPECT_NEAR(vgfx3d_d3d11_clamp_float_param(0.5f, 0.0f, 1.0f, 0.25f),
                0.5f,
                1e-6f,
                "Float parameter clamp preserves in-range values");
    EXPECT_NEAR(vgfx3d_d3d11_clamp_float_param(-1.0f, 0.0f, 1.0f, 0.25f),
                0.0f,
                1e-6f,
                "Float parameter clamp raises low values");
    EXPECT_NEAR(vgfx3d_d3d11_clamp_float_param(2.0f, 0.0f, 1.0f, 0.25f),
                1.0f,
                1e-6f,
                "Float parameter clamp caps high values");
    EXPECT_NEAR(vgfx3d_d3d11_clamp_float_param(HUGE_VALF, 0.0f, 1.0f, 0.25f),
                0.25f,
                1e-6f,
                "Float parameter clamp applies fallback for non-finite values");
    EXPECT_NEAR(vgfx3d_d3d11_clamp_float_param(HUGE_VALF, 0.0f, 1.0f, HUGE_VALF),
                0.0f,
                1e-6f,
                "Float parameter clamp sanitizes a non-finite fallback before clamping");
    EXPECT_NEAR(vgfx3d_d3d11_clamp_float_param(0.5f, 1.0f, 0.0f, 0.25f),
                0.5f,
                1e-6f,
                "Float parameter clamp tolerates inverted bounds");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(1.25f),
                1.25f,
                1e-6f,
                "Slope-bias sanitizer preserves finite values");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(HUGE_VALF),
                0.0f,
                1e-6f,
                "Slope-bias sanitizer clears non-finite values before D3D state creation");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(
                    VGFX3D_D3D11_MAX_SLOPE_SCALED_DEPTH_BIAS * 4.0f),
                VGFX3D_D3D11_MAX_SLOPE_SCALED_DEPTH_BIAS,
                1e-6f,
                "Slope-bias sanitizer clamps oversized positive finite values");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_slope_scaled_depth_bias(
                    -VGFX3D_D3D11_MAX_SLOPE_SCALED_DEPTH_BIAS * 4.0f),
                -VGFX3D_D3D11_MAX_SLOPE_SCALED_DEPTH_BIAS,
                1e-6f,
                "Slope-bias sanitizer clamps oversized negative finite values");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_material_workflow(RT_MATERIAL3D_WORKFLOW_PBR) ==
                    RT_MATERIAL3D_WORKFLOW_PBR,
                "Material workflow sanitizer preserves PBR");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_material_workflow(99) == RT_MATERIAL3D_WORKFLOW_LEGACY,
                "Material workflow sanitizer falls back to legacy for invalid values");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_alpha_mode(RT_MATERIAL3D_ALPHA_MODE_MASK) ==
                    RT_MATERIAL3D_ALPHA_MODE_MASK,
                "Alpha-mode sanitizer preserves masked materials");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_alpha_mode(-1) == RT_MATERIAL3D_ALPHA_MODE_OPAQUE,
                "Alpha-mode sanitizer falls back to opaque below range");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_alpha_mode(99) == RT_MATERIAL3D_ALPHA_MODE_OPAQUE,
                "Alpha-mode sanitizer falls back to opaque above range");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shading_model(VGFX3D_D3D11_SHADING_MODEL_MAX) ==
                    VGFX3D_D3D11_SHADING_MODEL_MAX,
                "Shading-model sanitizer preserves the highest valid model");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shading_model(VGFX3D_D3D11_SHADING_MODEL_MAX + 1) == 0,
                "Shading-model sanitizer falls back to Blinn-Phong above range");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_tonemap_mode(VGFX3D_D3D11_TONEMAP_MODE_MAX) ==
                    VGFX3D_D3D11_TONEMAP_MODE_MAX,
                "Tonemap sanitizer preserves the highest valid mode");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_tonemap_mode(99) == 0,
                "Tonemap sanitizer disables invalid modes");
    vgfx3d_d3d11_sanitize_fog_range(2.0f, 32.0f, &fog_near, &fog_far);
    EXPECT_NEAR(fog_near, 2.0f, 1e-6f, "Fog sanitizer preserves finite near distances");
    EXPECT_NEAR(fog_far, 32.0f, 1e-6f, "Fog sanitizer preserves ordered finite far distances");
    vgfx3d_d3d11_sanitize_fog_range(HUGE_VALF, -HUGE_VALF, &fog_near, &fog_far);
    EXPECT_NEAR(fog_near, 10.0f, 1e-6f, "Fog sanitizer replaces non-finite near distances");
    EXPECT_NEAR(fog_far, 50.0f, 1e-6f, "Fog sanitizer replaces non-finite far distances");
    vgfx3d_d3d11_sanitize_fog_range(VGFX3D_D3D11_FOG_DISTANCE_MAX, 1.0f, &fog_near, &fog_far);
    EXPECT_NEAR(fog_near, 10.0f, 1e-6f, "Fog sanitizer falls back when far cannot exceed near");
    EXPECT_NEAR(fog_far, 50.0f, 1e-6f, "Fog sanitizer restores an ordered fallback range");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_shadow_bias(0.125f),
                0.125f,
                1e-6f,
                "Shadow-bias sanitizer preserves small finite values");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_shadow_bias(HUGE_VALF),
                0.0f,
                1e-6f,
                "Shadow-bias sanitizer replaces non-finite values");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_shadow_bias(VGFX3D_D3D11_SHADOW_BIAS_MAX * 2.0f),
                VGFX3D_D3D11_SHADOW_BIAS_MAX,
                1e-6f,
                "Shadow-bias sanitizer clamps oversized positive values");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_shadow_bias(-VGFX3D_D3D11_SHADOW_BIAS_MAX * 2.0f),
                -VGFX3D_D3D11_SHADOW_BIAS_MAX,
                1e-6f,
                "Shadow-bias sanitizer clamps oversized negative values");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_cluster_global_count(-1, 8) == -1,
                "Cluster global-count sanitizer preserves flat-loop sentinel");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_cluster_global_count(4, 8) == 4,
                "Cluster global-count sanitizer preserves valid prefixes");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_cluster_global_count(12, 8) == 8,
                "Cluster global-count sanitizer clamps to uploaded light count");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_cluster_global_count(
                    VGFX3D_MAX_LIGHTS + 8, VGFX3D_MAX_LIGHTS + 4) == VGFX3D_MAX_LIGHTS,
                "Cluster global-count sanitizer clamps to shader light capacity");
    vgfx3d_d3d11_sanitize_cluster_depth_range(0.25f, 500.0f, &znear, &zfar);
    EXPECT_NEAR(znear, 0.25f, 1e-6f, "Cluster depth sanitizer preserves valid near planes");
    EXPECT_NEAR(zfar, 500.0f, 1e-6f, "Cluster depth sanitizer preserves valid far planes");
    vgfx3d_d3d11_sanitize_cluster_depth_range(HUGE_VALF, -1.0f, &znear, &zfar);
    EXPECT_NEAR(znear,
                VGFX3D_D3D11_CLUSTER_ZNEAR_FALLBACK,
                1e-6f,
                "Cluster depth sanitizer replaces non-finite near planes");
    EXPECT_TRUE(zfar > znear, "Cluster depth sanitizer restores ordered depth bounds");
    EXPECT_TRUE(vgfx3d_d3d11_should_upload_bone_palette(0, 0) == 0,
                "Bone upload helper skips unskinned draws");
    EXPECT_TRUE(vgfx3d_d3d11_should_upload_bone_palette(1, 0) == 1,
                "Bone upload helper accepts current skinning");
    EXPECT_TRUE(vgfx3d_d3d11_should_upload_bone_palette(0, 1) == 1,
                "Bone upload helper accepts previous-frame skinning");
    EXPECT_TRUE(vgfx3d_d3d11_saturating_add_u64(10u, 20u) == 30u,
                "Saturating add preserves normal sums");
    EXPECT_TRUE(vgfx3d_d3d11_saturating_add_u64(UINT64_MAX - 3u, 4u) == UINT64_MAX,
                "Saturating add clamps overflow instead of wrapping");
    EXPECT_TRUE(vgfx3d_d3d11_compute_morph_float_count(2u, 3, &elements) == 1 && elements == 18u,
                "Morph float-count helper computes shape * vertex * xyz floats");
    EXPECT_TRUE(vgfx3d_d3d11_compute_morph_float_count(1024u, 64, &elements) == 1 &&
                    elements == (size_t)VGFX3D_D3D11_MAX_MORPH_SHAPES * 1024u * 3u,
                "Morph float-count helper uses the clamped D3D11 shape count");
    EXPECT_TRUE(vgfx3d_d3d11_compute_morph_float_count((uint32_t)INT_MAX, 1, &elements) == 0 &&
                    elements == 0u,
                "Morph float-count helper rejects shader-index overflow spans");
    EXPECT_TRUE(vgfx3d_d3d11_compute_morph_float_count(1u << 28, 8, &elements) == 0 &&
                    elements == 0u,
                "Morph float-count helper rejects SRV ByteWidth overflow spans");
    EXPECT_TRUE(vgfx3d_d3d11_compute_morph_float_count(
                    VGFX3D_D3D11_MAX_BUFFER_TEXELS / 3u + 1u, 1, &elements) == 0 &&
                    elements == 0u,
                "Morph float-count helper rejects typed-SRV texel overflow before allocation");
}

static void test_d3d11_light_upload_sanitization_helpers(void) {
    float inner = 0.0f;
    float outer = 0.0f;
    float splits[4];
    float bad_splits[4] = {20.0f, 10.0f, HUGE_VALF, VGFX3D_D3D11_POSTFX_SCALAR_MAX * 2.0f};
    float negative_splits[4] = {-4.0f, 0.5f, 0.25f, 1.0f};

    EXPECT_TRUE(vgfx3d_d3d11_sanitize_light_type(0) == 0,
                "Light-type sanitizer preserves directional lights");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_light_type(3) == 3,
                "Light-type sanitizer preserves spot lights");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_light_type(4) == 4 &&
                    vgfx3d_d3d11_sanitize_light_type(5) == 5 &&
                    vgfx3d_d3d11_sanitize_light_type(6) == 6,
                "Light-type sanitizer preserves native area and volume lights");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_light_type(-1) == 0,
                "Light-type sanitizer falls back below range");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_light_type(99) == 0,
                "Light-type sanitizer falls back above range");

    EXPECT_TRUE(
        vgfx3d_d3d11_sanitize_shadow_projection_type(0, VGFX3D_SHADOW_PROJECTION_PERSPECTIVE) ==
            VGFX3D_SHADOW_PROJECTION_PERSPECTIVE,
        "Shadow projection sanitizer preserves perspective for shadowed lights");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_projection_type(0, VGFX3D_SHADOW_PROJECTION_CUBE) ==
                    VGFX3D_SHADOW_PROJECTION_CUBE,
                "Shadow projection sanitizer preserves cube projection for point lights");
    EXPECT_TRUE(
        vgfx3d_d3d11_sanitize_shadow_projection_type(-1, VGFX3D_SHADOW_PROJECTION_PERSPECTIVE) ==
            VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC,
        "Shadow projection sanitizer disables perspective for unshadowed lights");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_projection_type(0, 99) ==
                    VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC,
                "Shadow projection sanitizer rejects invalid projection ids");

    vgfx3d_d3d11_sanitize_spot_cone(-0.75f, 0.25f, &inner, &outer);
    EXPECT_NEAR(inner, 0.25f, 1e-6f, "Spot-cone sanitizer orders inner above outer");
    EXPECT_NEAR(outer, -0.75f, 1e-6f, "Spot-cone sanitizer keeps the lower outer cosine");
    vgfx3d_d3d11_sanitize_spot_cone(HUGE_VALF, -HUGE_VALF, &inner, &outer);
    EXPECT_NEAR(inner, 1.0f, 1e-6f, "Spot-cone sanitizer replaces invalid inner cones");
    EXPECT_NEAR(outer, 0.0f, 1e-6f, "Spot-cone sanitizer replaces invalid outer cones");

    memset(splits, 0xCD, sizeof(splits));
    vgfx3d_d3d11_sanitize_shadow_cascade_splits(splits, bad_splits, 4u);
    EXPECT_NEAR(splits[0], 20.0f, 1e-6f, "Cascade split sanitizer preserves valid first split");
    EXPECT_NEAR(splits[1], 20.0f, 1e-6f, "Cascade split sanitizer enforces monotonic order");
    EXPECT_NEAR(splits[2], 20.0f, 1e-6f, "Cascade split sanitizer replaces non-finite splits");
    EXPECT_NEAR(splits[3],
                VGFX3D_D3D11_POSTFX_SCALAR_MAX,
                1e-6f,
                "Cascade split sanitizer clamps oversized splits");

    vgfx3d_d3d11_sanitize_shadow_cascade_splits(splits, negative_splits, 4u);
    EXPECT_NEAR(splits[0], 0.0f, 1e-6f, "Cascade split sanitizer floors negative distances");
    EXPECT_NEAR(splits[1], 0.5f, 1e-6f, "Cascade split sanitizer preserves ordered distances");
    EXPECT_NEAR(splits[2], 0.5f, 1e-6f, "Cascade split sanitizer raises regressing distances");
    EXPECT_NEAR(splits[3], 1.0f, 1e-6f, "Cascade split sanitizer preserves later valid splits");

    splits[0] = 7.0f;
    vgfx3d_d3d11_sanitize_shadow_cascade_splits(splits, NULL, 1u);
    EXPECT_NEAR(splits[0], 0.0f, 1e-6f, "Cascade split sanitizer handles missing source arrays");
}

static void test_sampler_anisotropy_helpers(void) {
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_anisotropy(0) == 1,
                "D3D11 sampler anisotropy clamps zero to one");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_anisotropy(64) == 16,
                "D3D11 sampler anisotropy clamps high values to sixteen");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_anisotropy(8) == 8,
                "D3D11 sampler anisotropy preserves valid values");
    EXPECT_TRUE(vgfx3d_d3d11_sampler_anisotropy_index(1) == 0,
                "D3D11 sampler anisotropy index starts at zero");
    EXPECT_TRUE(vgfx3d_d3d11_sampler_anisotropy_index(16) == 15,
                "D3D11 sampler anisotropy index covers the final cache slot");
}

static void test_d3d11_limits_and_prune_helpers(void) {
    uint64_t gpu_time_us = UINT64_MAX;

    EXPECT_TRUE(vgfx3d_d3d11_is_valid_texture2d_extent(1, 1) == 1,
                "Texture extent helper accepts the smallest valid texture");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_texture2d_extent(VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION,
                                                       VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION) == 1,
                "Texture extent helper accepts the D3D11 maximum");
    EXPECT_TRUE(
        vgfx3d_d3d11_is_valid_texture2d_extent(VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION + 1, 16) == 0,
        "Texture extent helper rejects oversized widths before D3D allocation");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_cubemap_extent(0) == 0,
                "Cubemap extent helper rejects non-positive face sizes");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_cubemap_extent(VGFX3D_D3D11_MAX_CUBEMAP_DIMENSION + 1) == 0,
                "Cubemap extent helper rejects oversized faces");
    EXPECT_TRUE(vgfx3d_d3d11_is_single_subresource_texture2d(64, 32, 1, 1, 1, 0) == 1,
                "Texture-copy descriptor validation accepts one single-sampled subresource");
    EXPECT_TRUE(vgfx3d_d3d11_is_single_subresource_texture2d(64, 32, 2, 1, 1, 0) == 0,
                "Texture-copy descriptor validation rejects mip chains");
    EXPECT_TRUE(vgfx3d_d3d11_is_single_subresource_texture2d(64, 32, 1, 2, 1, 0) == 0,
                "Texture-copy descriptor validation rejects array textures");
    EXPECT_TRUE(vgfx3d_d3d11_is_single_subresource_texture2d(64, 32, 1, 1, 4, 0) == 0,
                "Texture-copy descriptor validation rejects multisampled sources");
    EXPECT_TRUE(vgfx3d_d3d11_is_single_subresource_texture2d(64, 32, 1, 1, 1, 1) == 0,
                "Texture-copy descriptor validation rejects incompatible sample quality");
    EXPECT_TRUE(vgfx3d_d3d11_is_single_subresource_texture2d(
                    VGFX3D_D3D11_MAX_TEXTURE2D_DIMENSION + 1u, 32, 1, 1, 1, 0) == 0,
                "Texture-copy descriptor validation rejects oversized unsigned extents");

    EXPECT_TRUE(vgfx3d_d3d11_compute_gpu_time_us(0, 1000000u, 10u, 25u, &gpu_time_us) == 1 &&
                    gpu_time_us == 15u,
                "GPU timestamp conversion reports rounded microseconds");
    EXPECT_TRUE(vgfx3d_d3d11_compute_gpu_time_us(1, 1000000u, 10u, 25u, &gpu_time_us) == 0 &&
                    gpu_time_us == 0u,
                "GPU timestamp conversion clears disjoint telemetry");
    EXPECT_TRUE(vgfx3d_d3d11_compute_gpu_time_us(0, 0u, 10u, 25u, &gpu_time_us) == 0 &&
                    gpu_time_us == 0u,
                "GPU timestamp conversion rejects a zero-frequency query");
    EXPECT_TRUE(vgfx3d_d3d11_compute_gpu_time_us(0, 1000000u, 25u, 10u, &gpu_time_us) == 0 &&
                    gpu_time_us == 0u,
                "GPU timestamp conversion rejects reversed timestamp pairs");
    EXPECT_TRUE(vgfx3d_d3d11_should_abandon_frame_timing(
                    VGFX3D_D3D11_FRAME_TIMING_PENDING_POLL_LIMIT - 1u) == 0,
                "GPU timing keeps polling below the bounded stale-query threshold");
    EXPECT_TRUE(
        vgfx3d_d3d11_should_abandon_frame_timing(VGFX3D_D3D11_FRAME_TIMING_PENDING_POLL_LIMIT) == 1,
        "GPU timing abandons a query that remains busy for the full poll budget");
    EXPECT_TRUE(vgfx3d_d3d11_should_abandon_depth_probe(
                    VGFX3D_D3D11_DEPTH_PROBE_PENDING_POLL_LIMIT - 1u) == 0,
                "Depth probes keep polling below the bounded stale-copy threshold");
    EXPECT_TRUE(
        vgfx3d_d3d11_should_abandon_depth_probe(VGFX3D_D3D11_DEPTH_PROBE_PENDING_POLL_LIMIT) == 1,
        "Depth probes abandon a staging copy that stays busy for the full poll budget");

    EXPECT_TRUE(vgfx3d_d3d11_clamp_morph_shape_count(1024u, 64) == VGFX3D_D3D11_MAX_MORPH_SHAPES,
                "Morph shape helper applies the backend shape cap");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_morph_shape_count((uint32_t)INT_MAX, 1) == 0,
                "Morph shape helper prevents signed shader-index overflow");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_morph_shape_count(1u << 28, 8) == 2,
                "Morph shape helper clamps by the maximum HLSL int buffer index");

    EXPECT_TRUE(vgfx3d_d3d11_should_prune_cache_entry(100, 0, 0, 1000, 64, 240) == 1,
                "Cache prune helper evicts aged entries while above the resident floor");
    EXPECT_TRUE(vgfx3d_d3d11_should_prune_cache_entry(100, 0, 36, 1000, 64, 240) == 0,
                "Cache prune helper keeps enough entries to preserve the resident floor");
    EXPECT_TRUE(vgfx3d_d3d11_should_prune_cache_entry(100, 0, 0, 12, 64, 240) == 0,
                "Cache prune helper keeps recently used entries");
    EXPECT_TRUE(vgfx3d_d3d11_should_prune_cache_entry(10, 11, 0, 1000, 0, 240) == 0,
                "Cache prune helper rejects impossible kept counts");
    EXPECT_TRUE(vgfx3d_d3d11_should_prune_cache_entry(
                    INT_MAX, INT_MAX - 1, 1, UINT64_MAX, INT_MAX - 1, 0) == 1,
                "Cache prune helper avoids signed overflow when preserving the resident floor");
}

static void test_mapped_copy_and_native_mip_validation_helpers(void) {
    uint8_t mip0_data[64] = {0};
    uint8_t mip1_data[32] = {0};
    vgfx3d_native_texture_mip_t mip0;
    vgfx3d_native_texture_mip_t mip1;
    size_t src_row_bytes = 0;
    size_t dst_row_bytes = 0;
    int32_t block_width = 0;
    int32_t block_height = 0;
    int32_t block_bytes = 0;

    EXPECT_TRUE(vgfx3d_d3d11_validate_mapped_texture_copy(
                    3, 12, 32, 8, &src_row_bytes, &dst_row_bytes) == 1 &&
                    src_row_bytes == 24u && dst_row_bytes == 12u,
                "Mapped readback validation accepts padded source rows");
    EXPECT_TRUE(vgfx3d_d3d11_validate_mapped_texture_copy(
                    3, 12, 16, 8, &src_row_bytes, &dst_row_bytes) == 0 &&
                    src_row_bytes == 0u && dst_row_bytes == 0u,
                "Mapped readback validation rejects short source row pitch");
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_mapped_texture_copy(3, 8, 32, 8, &src_row_bytes, &dst_row_bytes) == 0,
        "Mapped readback validation rejects short RGBA8 destination rows");
    EXPECT_TRUE(vgfx3d_d3d11_validate_mapped_texture_copy(
                    0, 12, 32, 8, &src_row_bytes, &dst_row_bytes) == 0,
                "Mapped readback validation rejects invalid copy widths");

    memset(&mip0, 0, sizeof(mip0));
    mip0.data = mip0_data;
    mip0.bytes = 32;
    mip0.width = 8;
    mip0.height = 4;
    mip0.block_width = 4;
    mip0.block_height = 4;
    mip0.block_bytes = 16;
    mip0.format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7;
    memset(&mip1, 0, sizeof(mip1));
    mip1.data = mip1_data;
    mip1.bytes = 16;
    mip1.width = 4;
    mip1.height = 2;
    mip1.block_width = 4;
    mip1.block_height = 4;
    mip1.block_bytes = 16;
    mip1.format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7;

    EXPECT_TRUE(
        vgfx3d_d3d11_native_format_block_layout(
            RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1, &block_width, &block_height, &block_bytes) == 1 &&
            block_width == 4 && block_height == 4 && block_bytes == 8,
        "Native format layout helper reports D3D11 BC1 block geometry");
    EXPECT_TRUE(
        vgfx3d_d3d11_native_format_block_layout(
            RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7, &block_width, &block_height, &block_bytes) == 1 &&
            block_width == 4 && block_height == 4 && block_bytes == 16,
        "Native format layout helper reports D3D11 BC7 block geometry");
    block_width = 13;
    block_height = 13;
    block_bytes = 13;
    EXPECT_TRUE(
        vgfx3d_d3d11_native_format_block_layout(
            RT_TEXTUREASSET3D_NATIVE_FORMAT_ASTC, &block_width, &block_height, &block_bytes) == 0 &&
            block_width == 0 && block_height == 0 && block_bytes == 0,
        "Native format layout helper rejects non-D3D11-native ASTC blocks");
    EXPECT_TRUE(vgfx3d_d3d11_native_mip_row_bytes(&mip0) == 32u,
                "Native mip row-byte helper counts BC7 block columns");
    EXPECT_TRUE(vgfx3d_d3d11_native_mip_block_rows(&mip0) == 1u,
                "Native mip block-row helper counts BC7 block rows");
    EXPECT_TRUE(vgfx3d_d3d11_native_mip_required_bytes(&mip0) == 32u,
                "Native mip required-byte helper counts BC7 block rows");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_native_mip_count(8, 4, 4) == 1,
                "Native mip-count validation accepts the full D3D11 mip chain");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_native_mip_count(8, 4, 5) == 0,
                "Native mip-count validation rejects chains longer than the base extent");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_native_mip_count(8, 4, (int64_t)UINT_MAX + 1ll) == 0,
                "Native mip-count validation rejects values that overflow D3D11 MipLevels");
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip0, NULL, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            1,
        "Native mip validation accepts a complete first mip");
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip1, &mip0, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            1,
        "Native mip validation accepts the expected halved next mip");

    {
        vgfx3d_native_texture_mip_t bc1_mip = mip0;
        bc1_mip.bytes = 16;
        bc1_mip.block_bytes = 8;
        bc1_mip.format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1;
        EXPECT_TRUE(vgfx3d_d3d11_validate_native_mip_desc(&bc1_mip,
                                                          NULL,
                                                          RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1,
                                                          bc1_mip.block_width,
                                                          bc1_mip.block_height,
                                                          bc1_mip.block_bytes) == 1,
                    "Native mip validation accepts D3D11 BC1 block geometry");
        bc1_mip.block_bytes = 16;
        bc1_mip.bytes = 32;
        EXPECT_TRUE(vgfx3d_d3d11_validate_native_mip_desc(&bc1_mip,
                                                          NULL,
                                                          RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1,
                                                          bc1_mip.block_width,
                                                          bc1_mip.block_height,
                                                          8) == 0,
                    "Native mip validation rejects BC1 payloads with BC7-sized blocks");
    }
    {
        vgfx3d_native_texture_mip_t bad_bc7_mip = mip0;
        bad_bc7_mip.block_width = 8;
        EXPECT_TRUE(vgfx3d_d3d11_validate_native_mip_desc(
                        &bad_bc7_mip, NULL, mip0.format_id, 4, 4, mip0.block_bytes) == 0,
                    "Native mip validation rejects BC7 payloads with non-D3D11 block widths");
    }

    mip1.width = 5;
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip1, &mip0, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            0,
        "Native mip validation rejects dimensions that do not follow the mip chain");
    mip1.width = 4;
    mip1.format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_ASTC;
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip1, &mip0, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            0,
        "Native mip validation rejects format changes inside a D3D11 texture");
    mip1.format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_BC7;
    mip1.block_bytes = 8;
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip1, &mip0, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            0,
        "Native mip validation rejects block-layout changes inside a D3D11 texture");
    mip1.block_bytes = 16;
    mip1.bytes = 8;
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip1, &mip0, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            0,
        "Native mip validation rejects short compressed payloads");
    mip1.bytes = (uint64_t)UINT_MAX + 1u;
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip1, &mip0, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            0,
        "Native mip validation rejects payloads that cannot fit D3D11 UINT upload fields");
    mip1.bytes = 16;
    mip1.block_height = 0;
    EXPECT_TRUE(vgfx3d_d3d11_native_mip_block_rows(&mip1) == 0u,
                "Native block-row helper rejects invalid block heights");
    EXPECT_TRUE(vgfx3d_d3d11_native_mip_required_bytes(&mip1) == 0u,
                "Native required-byte helper rejects invalid block heights");
    EXPECT_TRUE(
        vgfx3d_d3d11_validate_native_mip_desc(
            &mip1, &mip0, mip0.format_id, mip0.block_width, mip0.block_height, mip0.block_bytes) ==
            0,
        "Native mip validation rejects invalid block heights before upload math");
    mip1.block_height = 4;
    {
        vgfx3d_native_texture_mip_t corrupt_previous = mip0;
        corrupt_previous.block_width = 0;
        EXPECT_TRUE(vgfx3d_d3d11_validate_native_mip_desc(&mip1,
                                                          &corrupt_previous,
                                                          mip0.format_id,
                                                          mip0.block_width,
                                                          mip0.block_height,
                                                          mip0.block_bytes) == 0,
                    "Native mip validation rejects corrupt previous-mip descriptors");
    }
    {
        vgfx3d_native_texture_mip_t corrupt_previous = mip0;
        corrupt_previous.format_id = RT_TEXTUREASSET3D_NATIVE_FORMAT_BC1;
        EXPECT_TRUE(vgfx3d_d3d11_validate_native_mip_desc(&mip1,
                                                          &corrupt_previous,
                                                          mip0.format_id,
                                                          mip0.block_width,
                                                          mip0.block_height,
                                                          mip0.block_bytes) == 0,
                    "Native mip validation rejects previous-mip format changes");
    }
    {
        vgfx3d_native_texture_mip_t corrupt_previous = mip0;
        corrupt_previous.bytes = 8;
        EXPECT_TRUE(vgfx3d_d3d11_validate_native_mip_desc(&mip1,
                                                          &corrupt_previous,
                                                          mip0.format_id,
                                                          mip0.block_width,
                                                          mip0.block_height,
                                                          mip0.block_bytes) == 0,
                    "Native mip validation rejects undersized previous-mip payloads");
    }
}

static void test_target_fallback_helper(void) {
    EXPECT_TRUE(vgfx3d_d3d11_resolve_available_target(VGFX3D_D3D11_TARGET_SCENE, 1, 0, 0) ==
                    VGFX3D_D3D11_TARGET_SCENE,
                "Scene target stays selected when scene resources exist");
    EXPECT_TRUE(vgfx3d_d3d11_resolve_available_target(VGFX3D_D3D11_TARGET_SCENE, 0, 0, 0) ==
                    VGFX3D_D3D11_TARGET_SWAPCHAIN,
                "Scene target falls back to swapchain when allocation failed");
    EXPECT_TRUE(vgfx3d_d3d11_resolve_available_target(VGFX3D_D3D11_TARGET_OVERLAY, 1, 0, 0) ==
                    VGFX3D_D3D11_TARGET_SCENE,
                "Missing overlay target preserves the existing scene color target");
    EXPECT_TRUE(vgfx3d_d3d11_resolve_available_target(VGFX3D_D3D11_TARGET_OVERLAY, 0, 0, 0) ==
                    VGFX3D_D3D11_TARGET_SWAPCHAIN,
                "Missing overlay and scene targets fall back to swapchain");
    EXPECT_TRUE(vgfx3d_d3d11_resolve_available_target(VGFX3D_D3D11_TARGET_RTT, 1, 1, 0) ==
                    VGFX3D_D3D11_TARGET_SWAPCHAIN,
                "Missing RTT resources do not leave the backend targeting stale RTVs");
    EXPECT_TRUE(vgfx3d_d3d11_resolve_available_target((vgfx3d_d3d11_target_kind_t)99, 1, 1, 1) ==
                    VGFX3D_D3D11_TARGET_SWAPCHAIN,
                "Invalid target kinds fall back to the swapchain");
}

static void test_morph_cache_reuse_helper(void) {
    vgfx3d_draw_cmd_t cmd;
    float deltas[9] = {0};
    float weights[3] = {0};
    float normal_deltas[9] = {0};

    memset(&cmd, 0, sizeof(cmd));
    cmd.morph_key = &cmd;
    cmd.morph_revision = 7;
    cmd.morph_deltas = deltas;
    cmd.morph_weights = weights;
    cmd.morph_shape_count = 3;
    cmd.vertex_count = 1;

    EXPECT_TRUE(vgfx3d_d3d11_should_reuse_morph_cache(cmd.morph_key, 7, 3, 1, 0, &cmd) == 1,
                "Morph cache reuse accepts matching position-only payloads");
    EXPECT_TRUE(vgfx3d_d3d11_should_reuse_morph_cache(cmd.morph_key, 6, 3, 1, 0, &cmd) == 0,
                "Morph cache reuse rejects stale revisions");
    EXPECT_TRUE(vgfx3d_d3d11_should_reuse_morph_cache(cmd.morph_key, 7, 2, 1, 0, &cmd) == 0,
                "Morph cache reuse rejects mismatched shape counts");
    cmd.morph_normal_deltas = normal_deltas;
    EXPECT_TRUE(vgfx3d_d3d11_should_reuse_morph_cache(cmd.morph_key, 7, 3, 1, 0, &cmd) == 0,
                "Morph cache reuse includes normal-delta presence");
    EXPECT_TRUE(vgfx3d_d3d11_should_reuse_morph_cache(cmd.morph_key, 7, 3, 1, 1, &cmd) == 1,
                "Morph cache reuse accepts matching normal-delta payloads");
    cmd.morph_shape_count = VGFX3D_D3D11_MAX_MORPH_SHAPES + 4;
    EXPECT_TRUE(vgfx3d_d3d11_should_reuse_morph_cache(
                    cmd.morph_key, 7, VGFX3D_D3D11_MAX_MORPH_SHAPES, 1, 1, &cmd) == 1,
                "Morph cache reuse compares against the clamped D3D11 shape count");
}

static void test_shadow_and_rtt_policy_helpers(void) {
    int complete_none[VGFX3D_MAX_SHADOW_LIGHTS] = {0};
    int complete_first[VGFX3D_MAX_SHADOW_LIGHTS] = {0};
    int complete_sparse[VGFX3D_MAX_SHADOW_LIGHTS] = {0};
    int complete_all[VGFX3D_MAX_SHADOW_LIGHTS] = {0};
    int complete_negative[VGFX3D_MAX_SHADOW_LIGHTS] = {0};

    complete_first[0] = 1;
    complete_negative[0] = -1;
    if (VGFX3D_MAX_SHADOW_LIGHTS > 1)
        complete_sparse[1] = 1;
    for (int i = 0; i < VGFX3D_MAX_SHADOW_LIGHTS; i++)
        complete_all[i] = 1;

    EXPECT_TRUE(vgfx3d_d3d11_compute_shadow_count(VGFX3D_MAX_SHADOW_LIGHTS, complete_none) == 0,
                "Shadow count helper reports no advertised slots when slot zero is absent");
    EXPECT_TRUE(vgfx3d_d3d11_compute_shadow_count(VGFX3D_MAX_SHADOW_LIGHTS, complete_first) == 1,
                "Shadow count helper advertises the first complete slot");
    EXPECT_TRUE(vgfx3d_d3d11_compute_shadow_count(VGFX3D_MAX_SHADOW_LIGHTS, complete_sparse) == 0,
                "Shadow count helper rejects sparse shadow slots");
    EXPECT_TRUE(vgfx3d_d3d11_compute_shadow_count(VGFX3D_MAX_SHADOW_LIGHTS, complete_negative) == 0,
                "Shadow count helper treats negative sentinels as incomplete slots");
    EXPECT_TRUE(vgfx3d_d3d11_compute_shadow_count(VGFX3D_MAX_SHADOW_LIGHTS, complete_all) ==
                    VGFX3D_MAX_SHADOW_LIGHTS,
                "Shadow count helper advertises only a contiguous complete prefix");

    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_index(0, 1) == 0,
                "Shadow index sanitizer preserves valid advertised slots");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_index(1, 1) == -1,
                "Shadow index sanitizer disables slots outside the advertised range");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_index(-1, 1) == -1,
                "Shadow index sanitizer keeps negative slots unshadowed");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_shadow_count(VGFX3D_MAX_SHADOW_LIGHTS + 4) ==
                    VGFX3D_MAX_SHADOW_LIGHTS,
                "Shadow count clamping prevents shader-invisible shadow slots");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_index(VGFX3D_MAX_SHADOW_LIGHTS,
                                                   VGFX3D_MAX_SHADOW_LIGHTS + 4) == -1,
                "Shadow index sanitizer clamps oversized advertised counts before validation");
    EXPECT_TRUE(
        vgfx3d_d3d11_sanitize_shadow_index_for_projection(2, 8, VGFX3D_SHADOW_PROJECTION_CUBE) == 2,
        "Cube shadow validation preserves a complete six-face span");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_index_for_projection(
                    3, 8, VGFX3D_SHADOW_PROJECTION_CUBE) == -1,
                "Cube shadow validation disables an incomplete six-face span");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_index_for_projection(
                    3, 4, VGFX3D_SHADOW_PROJECTION_PERSPECTIVE) == 3,
                "Single-map projected shadows do not require six slots");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_cascade_count(
                    3, 0, 3, VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC) == 3,
                "Shadow cascade sanitizer preserves complete advertised cascade ranges");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_cascade_count(
                    4, 2, 3, VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC) == 1,
                "Shadow cascade sanitizer clamps cascades to the remaining advertised slots");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_cascade_count(VGFX3D_MAX_SHADOW_LIGHTS,
                                                           0,
                                                           VGFX3D_MAX_SHADOW_LIGHTS,
                                                           VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC) ==
                    VGFX3D_CSM_SLOTS,
                "Shadow cascade sanitizer cannot exceed the shader float4 split payload");
    EXPECT_TRUE(
        vgfx3d_d3d11_sanitize_shadow_cascade_count(4, 0, 6, VGFX3D_SHADOW_PROJECTION_CUBE) == 1,
        "Cube shadows select one face and never fan out as cascades");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_cascade_count(
                    0, 0, 3, VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC) == 1,
                "Shadow cascade sanitizer normalizes invalid cascade counts to one");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_shadow_cascade_count(
                    2, -1, 3, VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC) == 1,
                "Shadow cascade sanitizer disables cascade fan-out for unshadowed lights");

    EXPECT_TRUE(vgfx3d_d3d11_choose_depth_probe_target(1, 0, 1, 1, 1) == VGFX3D_D3D11_TARGET_RTT,
                "Depth probes prioritize the active RTT depth texture");
    EXPECT_TRUE(vgfx3d_d3d11_choose_depth_probe_target(1, 1, 0, 1, 1) == VGFX3D_D3D11_TARGET_NONE,
                "Depth probes do not leak to another target when active RTT depth is missing");
    EXPECT_TRUE(vgfx3d_d3d11_choose_depth_probe_target(0, 1, 0, 1, 1) == VGFX3D_D3D11_TARGET_SCENE,
                "Depth probes use offscreen scene depth on the post-FX route");
    EXPECT_TRUE(vgfx3d_d3d11_choose_depth_probe_target(0, 0, 0, 1, 1) ==
                    VGFX3D_D3D11_TARGET_SWAPCHAIN,
                "Depth probes avoid stale scene targets on the direct swapchain route");
    EXPECT_TRUE(vgfx3d_d3d11_choose_depth_probe_target(0, 0, 0, 0, 0) == VGFX3D_D3D11_TARGET_NONE,
                "Depth probes reject a frame with no complete depth source");

    EXPECT_TRUE(vgfx3d_d3d11_should_mark_rtt_dirty(1, 1, 1, 1, 1, 1, 1) == 1,
                "RTT dirty helper accepts complete active RTT state");
    EXPECT_TRUE(vgfx3d_d3d11_should_mark_rtt_dirty(1, 1, 1, 0, 1, 1, 1) == 0,
                "RTT dirty helper rejects partial color target state");
    EXPECT_TRUE(vgfx3d_d3d11_should_mark_rtt_dirty(0, 1, 1, 1, 1, 1, 1) == 0,
                "RTT dirty helper ignores inactive RTT state");
}

static void test_depth_probe_bias_instancing_and_rtt_guards(void) {
    int32_t pixel = -1;

    EXPECT_TRUE(vgfx3d_d3d11_ndc_to_pixel(-1.0f, 100, 0, &pixel) == 1 && pixel == 0,
                "NDC conversion maps the left edge to pixel zero");
    EXPECT_TRUE(vgfx3d_d3d11_ndc_to_pixel(1.0f, 100, 0, &pixel) == 1 && pixel == 99,
                "NDC conversion clamps the right edge inside the texture");
    EXPECT_TRUE(vgfx3d_d3d11_ndc_to_pixel(1.0e30f, 100, 1, &pixel) == 1 && pixel == 0,
                "NDC conversion clamps huge finite inverted coordinates before narrowing");
    pixel = 42;
    EXPECT_TRUE(vgfx3d_d3d11_ndc_to_pixel(NAN, 100, 0, &pixel) == 0 && pixel == 0,
                "NDC conversion rejects non-finite input and clears stale output");
    EXPECT_TRUE(vgfx3d_d3d11_ndc_to_pixel(0.0f, 0, 0, &pixel) == 0,
                "NDC conversion rejects empty texture extents");

    EXPECT_NEAR(vgfx3d_d3d11_sanitize_depth_probe_result(0.25f),
                0.75f,
                1e-6f,
                "Depth probes convert reversed-Z storage to canonical depth");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_depth_probe_result(-2.0f),
                1.0f,
                1e-6f,
                "Depth probes clamp values above canonical far depth");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_depth_probe_result(2.0f),
                0.0f,
                1e-6f,
                "Depth probes clamp values below canonical near depth");
    EXPECT_NEAR(vgfx3d_d3d11_sanitize_depth_probe_result(NAN),
                -1.0f,
                1e-6f,
                "Depth probes publish the invalid sentinel for non-finite samples");

    EXPECT_TRUE(vgfx3d_d3d11_depth_bias_units(0.01f, 0) > 0,
                "Standard-Z shadow bias preserves the renderer sign");
    EXPECT_TRUE(vgfx3d_d3d11_depth_bias_units(0.01f, 1) < 0,
                "Reversed-Z scene bias flips the renderer sign");
    EXPECT_TRUE(vgfx3d_d3d11_depth_bias_units(HUGE_VALF, 1) == 0,
                "Depth-bias conversion clears non-finite values");
    EXPECT_NEAR(vgfx3d_d3d11_depth_slope_bias(2.0f, 0),
                2.0f,
                1e-6f,
                "Standard-Z shadow slope bias preserves its sign");
    EXPECT_NEAR(vgfx3d_d3d11_depth_slope_bias(2.0f, 1),
                -2.0f,
                1e-6f,
                "Reversed-Z scene slope bias flips its sign");

    EXPECT_TRUE(vgfx3d_d3d11_sanitize_instance_bone_stride(16, 64, 4) == 16,
                "Instanced palette validation accepts exact packed layouts");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_instance_bone_stride(16, 63, 4) == 0,
                "Instanced palette validation rejects truncated packed layouts");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_instance_bone_stride(16, 80, 4) == 0,
                "Instanced palette validation rejects surplus packed layouts");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_instance_bone_stride(INT_MAX, 64, 4) == 0,
                "Instanced palette validation rejects overflowing strides");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_instance_bone_stride(64, 320, 5) == 0,
                "Instanced palette validation rejects layouts beyond the shader palette");

    EXPECT_TRUE(vgfx3d_d3d11_sanitize_ssr_steps(1) == VGFX3D_D3D11_SSR_STEPS_MIN,
                "SSR step sanitization matches the shader minimum");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_ssr_steps(256) == VGFX3D_D3D11_SSR_STEPS_MAX,
                "SSR step sanitization matches the shader maximum");
    EXPECT_TRUE(vgfx3d_d3d11_sanitize_ssr_steps(VGFX3D_D3D11_SSR_STEPS_DEFAULT) ==
                    VGFX3D_D3D11_SSR_STEPS_DEFAULT,
                "SSR step sanitization preserves the documented default");

    EXPECT_TRUE(vgfx3d_d3d11_rtt_readback_state_matches(64, 32, 1, 64, 32, 1) == 1,
                "RTT readback metadata accepts matching target and resource state");
    EXPECT_TRUE(vgfx3d_d3d11_rtt_readback_state_matches(64, 32, 1, 32, 32, 1) == 0,
                "RTT readback metadata rejects width drift");
    EXPECT_TRUE(vgfx3d_d3d11_rtt_readback_state_matches(64, 32, 1, 64, 32, 0) == 0,
                "RTT readback metadata rejects format drift");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_rtt_color_format(0) == 1 &&
                    vgfx3d_d3d11_is_valid_rtt_color_format(1) == 1,
                "RTT color-format validation accepts UNORM8 and HDR16F");
    EXPECT_TRUE(vgfx3d_d3d11_is_valid_rtt_color_format(2) == 0,
                "RTT color-format validation rejects unknown discriminators");
    EXPECT_TRUE(vgfx3d_d3d11_rtt_readback_state_matches(64, 32, 99, 64, 32, 99) == 0,
                "RTT readback rejects matching but invalid format metadata");
}

static void test_frame_protocol_submission_guards(void) {
    EXPECT_TRUE(vgfx3d_d3d11_frame_state_is_idle(0, 0) == 1,
                "D3D11 frame state is idle only before BeginFrame or after Present");
    EXPECT_TRUE(vgfx3d_d3d11_frame_state_is_idle(1, 0) == 0,
                "D3D11 frame mutation is blocked while drawing");
    EXPECT_TRUE(vgfx3d_d3d11_frame_state_is_idle(0, 1) == 0,
                "D3D11 frame mutation is blocked between EndFrame and Present");
    EXPECT_TRUE(vgfx3d_d3d11_frame_state_is_idle(1, 1) == 0,
                "D3D11 rejects contradictory active and pending state as busy");

    EXPECT_TRUE(vgfx3d_d3d11_draw_submission_is_ready(1, -1, 1, 1, 1, 640, 480) == 1,
                "D3D11 draw submission accepts a complete active color target");
    EXPECT_TRUE(vgfx3d_d3d11_draw_submission_is_ready(0, -1, 1, 1, 1, 640, 480) == 0,
                "D3D11 draw submission rejects calls outside a frame");
    EXPECT_TRUE(vgfx3d_d3d11_draw_submission_is_ready(1, 0, 1, 1, 1, 640, 480) == 0,
                "D3D11 ordinary draws cannot overwrite an active shadow pass");
    EXPECT_TRUE(vgfx3d_d3d11_draw_submission_is_ready(1, -1, 0, 1, 1, 640, 480) == 0,
                "D3D11 draw submission requires a device context");
    EXPECT_TRUE(vgfx3d_d3d11_draw_submission_is_ready(1, -1, 1, 0, 1, 640, 480) == 0 &&
                    vgfx3d_d3d11_draw_submission_is_ready(1, -1, 1, 1, 0, 640, 480) == 0,
                "D3D11 draw submission requires a published primary render target");
    EXPECT_TRUE(vgfx3d_d3d11_draw_submission_is_ready(1, -1, 1, 1, 1, 0, 480) == 0 &&
                    vgfx3d_d3d11_draw_submission_is_ready(1, -1, 1, 1, 1, 640, -1) == 0,
                "D3D11 draw submission rejects invalid viewport extents");
}

static void test_postfx_readback_policy_helpers(void) {
    vgfx3d_postfx_effect_desc_t effect;
    vgfx3d_postfx_chain_t chain;

    memset(&effect, 0, sizeof(effect));
    memset(&chain, 0, sizeof(chain));
    chain.enabled = 1;
    chain.effect_count = 1;
    chain.effect_capacity = 1;
    chain.effects = &effect;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_chain_is_usable(&chain) == 1,
                "PostFX chain validator accepts enabled chains with enough storage");
    effect.type = (int32_t)VGFX3D_POSTFX_EFFECT_TAA;
    effect.snapshot.taa_enabled = 0;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_effect_is_active(&effect) == 0,
                "PostFX active-effect helper ignores disabled TAA entries");
    EXPECT_TRUE(
        vgfx3d_d3d11_postfx_chain_has_active_effect(&chain, (int32_t)VGFX3D_POSTFX_EFFECT_TAA) == 0,
        "PostFX active-chain helper ignores disabled TAA entries");
    effect.snapshot.taa_enabled = 1;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_effect_is_active(&effect) == 1,
                "PostFX active-effect helper accepts enabled TAA entries");
    EXPECT_TRUE(
        vgfx3d_d3d11_postfx_chain_has_active_effect(&chain, (int32_t)VGFX3D_POSTFX_EFFECT_TAA) == 1,
        "PostFX active-chain helper accepts enabled TAA entries");
    EXPECT_TRUE(vgfx3d_d3d11_postfx_chain_has_active_effects(&chain) == 1,
                "PostFX active-chain helper reports any enabled effect");
    effect.snapshot.taa_enabled = 0;
    effect.type = (int32_t)VGFX3D_POSTFX_EFFECT_TONEMAP;
    effect.snapshot.tonemap_explicit = 1;
    effect.snapshot.tonemap_mode = 0;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_effect_is_active(&effect) == 1,
                "PostFX active-effect helper keeps explicit tonemap mode-zero entries active");
    effect.snapshot.tonemap_explicit = 0;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_chain_has_active_effects(&chain) == 0,
                "PostFX active-chain helper rejects structurally valid but inert chains");
    chain.effect_capacity = 0;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_chain_is_usable(&chain) == 0,
                "PostFX chain validator rejects effect counts beyond capacity");
    chain.effect_capacity = 1;
    chain.effects = NULL;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_chain_is_usable(&chain) == 0,
                "PostFX chain validator rejects missing effect storage");
    chain.effects = &effect;
    chain.enabled = 0;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_chain_is_usable(&chain) == 0,
                "PostFX chain validator rejects disabled chains");
    chain.enabled = 1;
    effect.type = INT32_MAX;
    EXPECT_TRUE(vgfx3d_d3d11_postfx_chain_is_usable(&chain) == 0,
                "PostFX chain validator rejects unknown effect discriminators");

    EXPECT_TRUE(vgfx3d_d3d11_should_composite_to_swapchain(0, 1, 1, 0) == 1,
                "D3D11 composites an unpresented GPU-postfx scene to the swapchain");
    EXPECT_TRUE(vgfx3d_d3d11_should_composite_to_swapchain(0, 1, 1, 1) == 0,
                "D3D11 skips duplicate postfx composites once the swapchain is current");
    EXPECT_TRUE(vgfx3d_d3d11_should_composite_to_swapchain(1, 1, 1, 0) == 0,
                "RTT rendering bypasses window-swapchain compositing");
    EXPECT_TRUE(vgfx3d_d3d11_should_composite_to_swapchain(0, 0, 1, 0) == 0,
                "Disabled GPU postfx never requests a scene composite");
    EXPECT_TRUE(vgfx3d_d3d11_should_composite_to_swapchain(0, 1, 0, 0) == 0,
                "Missing scene targets never request a scene composite");
    EXPECT_TRUE(vgfx3d_d3d11_should_reset_composited_swapchain_for_frame(0, 0) == 1,
                "A new main scene frame invalidates the prior apply_postfx swapchain image");
    EXPECT_TRUE(vgfx3d_d3d11_should_reset_composited_swapchain_for_frame(0, 1) == 0,
                "A load-existing overlay pass can keep drawing over the composited swapchain");
    EXPECT_TRUE(vgfx3d_d3d11_should_reset_composited_swapchain_for_frame(1, 1) == 1,
                "RTT begin-frame state never preserves a window-swapchain composite");
    EXPECT_TRUE(vgfx3d_d3d11_should_reset_composited_swapchain_for_postfx_update(1, 1) == 0,
                "Reapplying the same GPU postfx state preserves the composited swapchain");
    EXPECT_TRUE(vgfx3d_d3d11_should_reset_composited_swapchain_for_postfx_update(1, 0) == 1,
                "Disabling GPU postfx invalidates the composited swapchain");
    EXPECT_TRUE(vgfx3d_d3d11_should_reset_composited_swapchain_for_postfx_update(0, 1) == 1,
                "Enabling GPU postfx invalidates any direct-swapchain contents");
    EXPECT_TRUE(vgfx3d_d3d11_should_treat_begin_frame_as_overlay(VGFX3D_D3D11_TARGET_OVERLAY, 1) ==
                    1,
                "Separate overlay passes preserve scene temporal history");
    EXPECT_TRUE(
        vgfx3d_d3d11_should_treat_begin_frame_as_overlay(VGFX3D_D3D11_TARGET_SWAPCHAIN, 1) == 1,
        "Final overlays after apply_postfx preserve scene temporal history");
    EXPECT_TRUE(vgfx3d_d3d11_should_treat_begin_frame_as_overlay(VGFX3D_D3D11_TARGET_RTT, 1) == 0,
                "RTT load-existing passes do not preserve scene temporal history");
    EXPECT_TRUE(vgfx3d_d3d11_should_treat_begin_frame_as_overlay(VGFX3D_D3D11_TARGET_SCENE, 0) == 0,
                "Main scene passes refresh scene temporal history");
    EXPECT_TRUE(
        vgfx3d_d3d11_should_treat_begin_frame_as_overlay((vgfx3d_d3d11_target_kind_t)99, 1) == 0,
        "Invalid target kinds do not preserve scene temporal history as overlays");
    EXPECT_TRUE(vgfx3d_d3d11_uses_separate_overlay_target(VGFX3D_D3D11_TARGET_OVERLAY, 1) == 1,
                "Overlay target passes mark the separate overlay target as used");
    EXPECT_TRUE(vgfx3d_d3d11_uses_separate_overlay_target(VGFX3D_D3D11_TARGET_SWAPCHAIN, 1) == 0,
                "Direct swapchain overlays do not reuse stale separate overlay target state");
    EXPECT_TRUE(vgfx3d_d3d11_uses_separate_overlay_target(VGFX3D_D3D11_TARGET_OVERLAY, 0) == 0,
                "Incomplete overlay resources do not mark a separate overlay target as used");
    EXPECT_TRUE(vgfx3d_d3d11_should_keep_presented_snapshot(1, 1) == 1,
                "Presented snapshot stays valid only after a successful snapshot and Present");
    EXPECT_TRUE(vgfx3d_d3d11_should_keep_presented_snapshot(1, 0) == 0,
                "Presented snapshot is invalidated when Present fails");
    EXPECT_TRUE(vgfx3d_d3d11_should_keep_presented_snapshot(0, 1) == 0,
                "Presented snapshot is invalidated when the pre-present copy fails");
    EXPECT_TRUE(vgfx3d_d3d11_present_status_confirms_display(0) == 1,
                "Exact S_OK confirms a displayed D3D11 frame");
    EXPECT_TRUE(vgfx3d_d3d11_present_status_confirms_display(1) == 0,
                "Informational DXGI success statuses do not count as displayed frames");
    EXPECT_TRUE(vgfx3d_d3d11_present_status_confirms_display(-1) == 0,
                "Failed DXGI status values do not count as displayed frames");

    EXPECT_TRUE(
        vgfx3d_d3d11_choose_readback_kind(1, 1, 0, 1, 1, 1, 1, 1, 1, VGFX3D_D3D11_TARGET_SCENE) ==
            VGFX3D_D3D11_READBACK_PRESENTED_SNAPSHOT,
        "Readback uses the pre-present snapshot after a finalized swapchain frame");
    EXPECT_TRUE(
        vgfx3d_d3d11_choose_readback_kind(1, 0, 0, 0, 0, 0, 0, 0, 1, VGFX3D_D3D11_TARGET_SCENE) ==
            VGFX3D_D3D11_READBACK_SCENE_COLOR,
        "Readback ignores a stale presented snapshot flag when no snapshot texture exists");
    EXPECT_TRUE(
        vgfx3d_d3d11_choose_readback_kind(0, 0, 1, 1, 1, 1, 1, 1, 1, VGFX3D_D3D11_TARGET_SCENE) ==
            VGFX3D_D3D11_READBACK_BACKBUFFER,
        "Readback uses the visible backbuffer after apply_postfx already composited");
    EXPECT_TRUE(
        vgfx3d_d3d11_choose_readback_kind(0, 0, 0, 1, 1, 1, 2, 1, 1, VGFX3D_D3D11_TARGET_SCENE) ==
            VGFX3D_D3D11_READBACK_POSTFX_COMPOSITE,
        "Readback replays a valid GPU postfx chain when the scene is not composited");
    EXPECT_TRUE(
        vgfx3d_d3d11_choose_readback_kind(0, 0, 0, 1, 1, 1, 2, 0, 1, VGFX3D_D3D11_TARGET_SCENE) ==
            VGFX3D_D3D11_READBACK_SCENE_COLOR,
        "Readback avoids replaying malformed postfx snapshots without effects storage");
    EXPECT_TRUE(vgfx3d_d3d11_choose_readback_kind(
                    0, 0, 0, 1, 1, 1, 2, 1, 0, VGFX3D_D3D11_TARGET_SWAPCHAIN) ==
                    VGFX3D_D3D11_READBACK_BACKBUFFER,
                "Readback avoids replaying postfx snapshots when scene targets are unavailable");
    EXPECT_TRUE(
        vgfx3d_d3d11_choose_readback_kind(0, 0, 0, 0, 0, 0, 0, 0, 1, VGFX3D_D3D11_TARGET_SCENE) ==
            VGFX3D_D3D11_READBACK_SCENE_COLOR,
        "Readback can source the offscreen scene when it is still the active target");
    EXPECT_TRUE(
        vgfx3d_d3d11_choose_readback_kind(0, 0, 0, 0, 0, 0, 0, 0, 1, VGFX3D_D3D11_TARGET_OVERLAY) ==
            VGFX3D_D3D11_READBACK_SCENE_COLOR,
        "Readback can source the composed scene when the overlay target is active");
    EXPECT_TRUE(vgfx3d_d3d11_choose_readback_kind(
                    0, 0, 0, 0, 0, 0, 0, 0, 1, VGFX3D_D3D11_TARGET_SWAPCHAIN) ==
                    VGFX3D_D3D11_READBACK_BACKBUFFER,
                "Readback falls back to the swapchain when the current target is the backbuffer");
    EXPECT_TRUE(vgfx3d_d3d11_choose_readback_kind(
                    0, 0, 0, 0, 0, 0, 0, 0, 1, (vgfx3d_d3d11_target_kind_t)99) ==
                    VGFX3D_D3D11_READBACK_BACKBUFFER,
                "Readback treats invalid target kinds as backbuffer state");
}

static void test_shadow_projection_helper_handles_all_projection_modes(void) {
    float m[16];
    float world[3] = {0.25f, -0.25f, 2.0f};
    float uv_depth[3];

    memset(m, 0, sizeof(m));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 2.0f;
    EXPECT_TRUE(vgfx3d_d3d11_project_shadow_coord(
                    m, VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC, world, uv_depth) == 1,
                "D3D11 orthographic shadow projection accepts finite coordinates");
    EXPECT_NEAR(uv_depth[0], 0.625f, 1e-6f, "D3D11 orthographic shadow UV does not divide by W");
    EXPECT_NEAR(uv_depth[1], 0.625f, 1e-6f, "D3D11 orthographic shadow UV flips Y");

    memset(m, 0, sizeof(m));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 0.5f;
    m[14] = 1.0f;
    EXPECT_TRUE(vgfx3d_d3d11_project_shadow_coord(
                    m, VGFX3D_SHADOW_PROJECTION_PERSPECTIVE, world, uv_depth) == 1,
                "D3D11 perspective shadow projection accepts positive W");
    EXPECT_NEAR(uv_depth[0], 0.5625f, 1e-6f, "D3D11 perspective shadow UV divides by W");
    EXPECT_NEAR(uv_depth[1], 0.5625f, 1e-6f, "D3D11 perspective shadow UV flips Y after divide");
    EXPECT_NEAR(uv_depth[2], 0.75f, 1e-6f, "D3D11 perspective shadow depth divides by W");

    EXPECT_TRUE(
        vgfx3d_d3d11_project_shadow_coord(m, VGFX3D_SHADOW_PROJECTION_CUBE, world, uv_depth) == 1,
        "D3D11 cube-face shadow projection accepts positive W");
    EXPECT_NEAR(uv_depth[0], 0.5625f, 1e-6f, "D3D11 cube-face shadow UV divides by W");
    EXPECT_NEAR(uv_depth[2], 0.75f, 1e-6f, "D3D11 cube-face shadow depth divides by W");

    EXPECT_TRUE(vgfx3d_d3d11_project_shadow_coord(m, 99, world, uv_depth) == 0,
                "D3D11 shadow projection rejects unknown projection modes");
    EXPECT_NEAR(uv_depth[0], 0.0f, 1e-6f, "Invalid shadow modes clear stale projected output");

    world[2] = 0.0f;
    uv_depth[0] = 9.0f;
    uv_depth[1] = 9.0f;
    uv_depth[2] = 9.0f;
    EXPECT_TRUE(vgfx3d_d3d11_project_shadow_coord(
                    m, VGFX3D_SHADOW_PROJECTION_PERSPECTIVE, world, uv_depth) == 0,
                "D3D11 perspective shadow projection rejects non-positive W");
    EXPECT_NEAR(uv_depth[0], 0.0f, 1e-6f, "Failed shadow projection clears stale U");
    EXPECT_NEAR(uv_depth[1], 0.0f, 1e-6f, "Failed shadow projection clears stale V");
    EXPECT_NEAR(uv_depth[2], 0.0f, 1e-6f, "Failed shadow projection clears stale depth");
}

static void test_d3d11_shader_sources_keep_numeric_guards(void) {
    const char *fog_write;

    EXPECT_TRUE(contains_text(d3d11_shader_source, "len2 > 1e-12 && len2 < 1e20"),
                "Main D3D11 shader bounds safeNormalize before rsqrt");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "bool mainFinite3(float3 v)"),
                "Main D3D11 shader exposes reusable finite-vector validation");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "idx = clamp(idx, 0, 63);"),
                "Main D3D11 shader exposes all 64 packed morph weights");
    EXPECT_TRUE(!contains_text(d3d11_shader_source, "idx = clamp(idx, 0, 31);"),
                "Main D3D11 shader no longer aliases upper morph weights to slot 31");
    EXPECT_TRUE(count_text(d3d11_shader_source, "if (abs(w) > 0.0001 && abs(w) < 1e20)") == 2,
                "Position and normal morph loops reject non-finite weights");
    EXPECT_TRUE(count_text(d3d11_shader_source, "if (!mainFinite3(delta))") == 2,
                "Position and normal morph loops reject malformed delta payloads");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "float3 positionCandidate = pos + delta * w;"),
                "Position morphing validates each accumulated candidate");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "float3 normalCandidate = nrm + delta * w;"),
                "Normal morphing validates each accumulated candidate");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "uint skinPaletteIndex(uint paletteBase, uint boneIndex)"),
                "Skinning uses overflow-safe palette indexing");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "maxOffset = min(maxOffset, (uint)(instanceBoneStride - 1));"),
                "Instanced skinning confines bone indices to one instance palette");
    EXPECT_TRUE(count_text(d3d11_shader_source, "if (!(w > 0.0001 && w < 1e20))") == 2,
                "Position and vector skinning reject invalid bone weights");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "if (!mainFinite4(transformed))"),
                "Position skinning skips invalid matrix products");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "if (!mainFinite3(transformed))"),
                "Vector skinning skips invalid matrix products");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "if (!mainFinite4(lc) || (light.shadowProjectionType != 0 &&"),
                "Shadow sampling rejects malformed homogeneous coordinates");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "if (!mainFinite3(ndc))"),
                "Shadow sampling rejects invalid projected coordinates before texture lookup");
    EXPECT_TRUE(
        contains_text(d3d11_shader_source,
                      "return shadowIndex >= 0 && shadowIndex < shadowCount ? shadowIndex : -1;"),
        "Cube shadows return their selected face without entering cascade selection");
    EXPECT_TRUE(
        count_text(d3d11_shader_source, "clamp(light.shadowCascadeCount, 1, kShadowCsmSlots)") == 2,
        "Both shadow paths cap cascades to the four uploaded split lanes");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "float2 halfTexel = 0.5 / tileSize;\n"
                              "        float2 localUv = clamp(uv, halfTexel, 1.0 - halfTexel);"),
                "Shadow-atlas comparison footprints stay inside their owning tile");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "float2(atlasWidth, atlasHeight) /\n"
                              "                  float2(kShadowAtlasColumns, kShadowAtlasRows)"),
                "Shadow-atlas PCF derives texels from the per-tile dimensions");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "int tile = shadowIndex - kShadowCsmSlots;") &&
                    contains_text(d3d11_shader_source, "tile % kShadowAtlasColumns"),
                "Shadow HLSL consumes the shared CSM and atlas-grid constants");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "float sampleShadowAt(int shadowIndex, float2 uv, float depth, "
                              "float bias) {\n    float result = 1.0;"),
                "Shadow sampling initializes one result across every texture branch");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "PS_OUTPUT PSMain(PS_INPUT input, bool isFront : SV_IsFrontFace)"),
                "Main D3D11 shader source remains available to the compile path");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "if (!isFront) N = -N;"),
                "D3D11 back faces of cull-off geometry shade with the outward normal (ZB-21)");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "evalNativeLight") &&
                    contains_text(d3d11_shader_source, "nativeLightDecay"),
                "D3D11 shader retains native area/volume evaluation and FBX decay");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "int evalNativeLight(Light light, float3 worldPos, out float3 L, "
                              "out float atten) {\n    int result = 0;"),
                "Native-light evaluation initializes its return value on every path");
    EXPECT_TRUE(contains_text(d3d11_shader_source, "float4 basisU") &&
                    contains_text(d3d11_shader_source, "float4 basisV") &&
                    contains_text(d3d11_shader_source, "float4 shape"),
                "D3D11 light layout retains emitter basis and dimensions");
    EXPECT_TRUE(contains_text(d3d11_skybox_shader_source, "len2 > 1e-12 && len2 < 1e20"),
                "Skybox D3D11 shader bounds safeNormalize before rsqrt");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source,
                              "!postFinite4(world) || !(abs(world.w) > 0.0001 && "
                              "abs(world.w) < 1e20)"),
                "PostFX rejects invalid homogeneous world reconstruction");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source,
                              "bool reconstructWorld(float2 uv, float depth, out float3 worldOut)"),
                "PostFX reports failed world reconstruction to its callers");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source, "worldOut = world.xyz / world.w;"),
                "PostFX divides only by a finite non-zero signed homogeneous W");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source,
                              "return postFinite3(color) ? color : float3(0.0, 0.0, 0.0);"),
                "PostFX isolates invalid neighboring scene samples");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source,
                              "postFinite1(depth) && depth >= 0.0 && depth <= 1.0"),
                "PostFX rejects invalid depth samples before reconstruction");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source, "if (!postFinite4(motion))"),
                "Motion blur rejects malformed motion payloads");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source,
                              "color = max(color * tonemapExposure, float3(0.0, 0.0, 0.0));"),
                "Tonemapping prevents negative HDR values from reaching rational curves");
    EXPECT_TRUE(contains_text(d3d11_bloom_shader_source, "PSBloomDown"),
                "Bloom D3D11 shader source remains available to the compile path");
    EXPECT_TRUE(contains_text(d3d11_bloom_shader_source, "clamp(c, 0.0, 65504.0)"),
                "Bloom clamps finite values to the RGBA16F storage range");
    EXPECT_TRUE(contains_text(d3d11_bloom_shader_source, "1.0 + max(bloomLuma(c), 0.0)"),
                "Bloom Karis weighting keeps its denominator positive");
    EXPECT_TRUE(
        contains_text(d3d11_bloom_shader_source, "bloomFinite3(c) ? clamp(c, 0.0, 65504.0)"),
        "Bloom replaces NaN and infinity before filtering");
    EXPECT_TRUE(contains_text(d3d11_taa_shader_source, "PSTAA"),
                "TAA D3D11 shader source remains available to the compile path");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source, "len2 > 1e-12 && len2 < 1e20"),
                "SSR D3D11 shader bounds safeNormalize before rsqrt");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source, "bool refineValid = true;"),
                "SSR D3D11 shader tracks invalid binary-search samples");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source,
                              "!ssrFinite4(mc) || !(mc.w > 0.0001 && mc.w < 1e20)"),
                "SSR rejects invalid refined clip coordinates and samples behind the camera");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source, "float3 mndc = mc.xyz / mc.w;"),
                "SSR D3D11 shader divides by guarded mid-sample W");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source, "float3 hndc = hc.xyz / hc.w;"),
                "SSR D3D11 shader divides by guarded hit-sample W");
    EXPECT_TRUE(
        contains_text(d3d11_shader_source, "uint clusterOffsetAt(int i) {\n    i = clamp(i, 0, "),
        "Cluster offset reads clamp their packed-buffer index");
    EXPECT_TRUE(
        contains_text(d3d11_shader_source, "uint clusterIndexAt(int i) {\n    i = clamp(i, 0, "),
        "Cluster light-index reads clamp their packed-buffer index");
    EXPECT_TRUE(
        count_text(d3d11_shader_source, "clusterStart = clamp(int(clusterOffsetAt(cidx)), 0,") == 2,
        "Both lighting workflows clamp cluster starts");
    EXPECT_TRUE(count_text(d3d11_shader_source,
                           "int clusterEnd = clamp(int(clusterOffsetAt(cidx + 1)), "
                           "clusterStart,") == 2,
                "Both lighting workflows clamp and order cluster ends");
    EXPECT_TRUE(count_text(d3d11_shader_source,
                           "lightLoopCount = min(lightCount, clusterGlobalCount + clusterEnd - "
                           "clusterStart);") == 2,
                "Both clustered loops stay within uploaded lights");
    EXPECT_TRUE(count_text(d3d11_shader_source, "if (i < 0 || i >= lightCount)") == 2,
                "Both clustered loops reject malformed light indices");
    EXPECT_TRUE(contains_text(
                    d3d11_shader_source,
                    "float3 tangentProjected = input.tangent.xyz - N * dot(input.tangent.xyz, N);"),
                "Normal mapping retains the projected tangent before normalization");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "float tangentLengthSq = dot(tangentProjected, tangentProjected);"),
                "Normal mapping measures the unfallback tangent");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "if (tangentLengthSq > 0.0001 && tangentLengthSq < 1e20)"),
                "Normal mapping rejects degenerate and unbounded tangents");
    EXPECT_TRUE(
        !contains_text(d3d11_shader_source, "float3 T = safeNormalize3(input.tangent.xyz - N *"),
        "Normal mapping no longer promotes a missing tangent to a fallback basis");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "float tangentLengthSqA = dot(tangentProjectedA, "
                              "tangentProjectedA);"),
                "Anisotropy measures the projected tangent before normalization");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "if (tangentLengthSqA > 0.0001 && tangentLengthSqA < 1e20)"),
                "Anisotropy skips degenerate and non-finite tangent bases");
    EXPECT_TRUE(!contains_text(d3d11_shader_source, "float3 TnA = normalize("),
                "Anisotropy no longer uses unsafe normalize on authored tangents");
    EXPECT_TRUE(contains_text(d3d11_shader_source,
                              "float opticalDepth = min(hd * max(viewDistance, 0.0), 80.0);"),
                "Height fog bounds optical depth");
    fog_write = strstr(d3d11_shader_source, "result = lerp(result, fogTint, fogFactor);");
    EXPECT_TRUE(fog_write && strstr(fog_write, "if (any(isnan(result)) || any(isinf(result)))"),
                "Scene shader validates results after height-fog evaluation");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source,
                              "float3 accum = float3(0.0, 0.0, 0.0);\n"
                              "    for (int i = 0; i < taps; i++)"),
                "Motion blur samples a centered full kernel");
    EXPECT_TRUE(
        contains_text(d3d11_shader_source, "if (any(isnan(velocity)) || any(isinf(velocity)))"),
        "Main D3D11 shader prevents invalid motion vectors from reaching TAA");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source, "if (!(nlen2 > 1e-12 && nlen2 < 1e20))"),
                "SSAO rejects non-finite reconstructed normals");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source, "color = sourceColor;"),
                "PostFX falls back to its finite source color after numeric failure");
    EXPECT_TRUE(contains_text(d3d11_taa_shader_source, "bool taaFinite3(float3 v)"),
                "TAA validates current, history, and resolved colors");
    EXPECT_TRUE(contains_text(d3d11_taa_shader_source, "if (!taaFinite4(motion))"),
                "TAA rejects malformed motion payloads");
    EXPECT_TRUE(
        contains_text(d3d11_taa_shader_source, "!taaFinite1(depth) || depth < 0.0 || depth > 1.0"),
        "TAA rejects malformed depth before world reconstruction");
    EXPECT_TRUE(
        contains_text(d3d11_taa_shader_source, "!taaFinite4(world) || !(abs(world.w) > 0.0001"),
        "TAA validates homogeneous world coordinates before division");
    EXPECT_TRUE(contains_text(d3d11_taa_shader_source, "taaFinite3(resolved) ? resolved : curr"),
                "TAA falls back to current color when resolve math is invalid");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source, "bool ssrFinite3(float3 v)"),
                "SSR validates source and reflected colors");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source,
                              "if (!ssrFinite1(mask) || !ssrDepthAt(input.uv, depth))"),
                "SSR rejects malformed masks and primary depth samples");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source, "if (!ssrDepthAt(uv2, sceneD))"),
                "SSR rejects invalid ray-march depth samples");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source,
                              "if (!ssrFinite4(pc) || !(pc.w > 0.0001 && pc.w < 1e20))"),
                "SSR validates projected ray coordinates before sampling");
    EXPECT_TRUE(contains_text(d3d11_ssr_shader_source, "ssrFinite3(reflected) ? reflected : base"),
                "SSR falls back to the source pixel when reflection math is invalid");
    EXPECT_TRUE(!contains_text(d3d11_postfx_shader_source,
                               "float3 accum = color;\n    for (int i = 1; i < taps; i++)"),
                "Motion blur no longer double-weights its input color");
    EXPECT_TRUE(
        contains_text(d3d11_postfx_shader_source,
                      "float blur = clamp(abs(dist - dofFocusDistance) * max(dofAperture, 0.0) * "
                      "0.02, 0.0, max(dofMaxBlur, 0.0));"),
        "DOF uses aperture as blur strength and honors zero aperture");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source, "float2 radius = invResolution * blur;"),
                "DOF maximum blur directly bounds its sample radius");
    EXPECT_TRUE(!contains_text(d3d11_postfx_shader_source, "* blur * 8.0"),
                "DOF does not multiply the documented maximum radius");
    EXPECT_TRUE(
        contains_text(d3d11_postfx_shader_source, "abs(luminance(n) + luminance(s) - 2.0 * lumaM)"),
        "FXAA detects symmetric horizontal edges against the center");
    EXPECT_TRUE(
        contains_text(d3d11_postfx_shader_source, "abs(luminance(e) + luminance(w) - 2.0 * lumaM)"),
        "FXAA detects symmetric vertical edges against the center");
    EXPECT_TRUE(contains_text(d3d11_postfx_shader_source,
                              "return lerp(color, (n + s + e + w) * 0.25, 0.5);"),
                "FXAA blends the center with the neighbor average");
}

static void test_d3d11_shader_chunk_join_validation(void) {
    void *volatile cache = NULL;
    void *volatile bad_cache = NULL;
    void *volatile empty_cache = NULL;
    void *volatile preset_cache = (void *)"already-published";
    const char *chunks[] = {"alpha", "", "omega"};
    const char *bad_chunks[] = {"alpha", NULL};
    const char *joined = d3d11_join_shader_chunks(&cache, chunks, 3);

    EXPECT_TRUE(joined && strcmp(joined, "alphaomega") == 0,
                "Shader chunk join preserves order and empty chunks");
    EXPECT_TRUE(d3d11_join_shader_chunks(&cache, chunks, 3) == joined,
                "Shader chunk join reuses its cache");
    EXPECT_TRUE(d3d11_join_shader_chunks(NULL, chunks, 3) == NULL,
                "Shader chunk join rejects a missing cache");
    EXPECT_TRUE(d3d11_join_shader_chunks(&bad_cache, NULL, 1) == NULL,
                "Shader chunk join rejects missing chunks");
    EXPECT_TRUE(d3d11_join_shader_chunks(&bad_cache, bad_chunks, 2) == NULL && !bad_cache,
                "Shader chunk join rejects null chunk entries atomically");
    EXPECT_TRUE(strcmp(d3d11_join_shader_chunks(&empty_cache, NULL, 0), "") == 0,
                "Shader chunk join accepts an empty source");
    EXPECT_TRUE(strcmp(d3d11_join_shader_chunks(&preset_cache, chunks, 3), "already-published") ==
                    0,
                "Shader chunk join observes an already-published atomic winner");
    free((void *)cache);
    free((void *)empty_cache);
}

static char *read_text_file(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *text;

    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    text = (char *)malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return NULL;
    }
    if (fread(text, 1u, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[size] = '\0';
    fclose(file);
    return text;
}

static char *read_d3d11_backend_sources(void) {
    static const char *k_parts[] = {
        "vgfx3d_backend_d3d11.c",
        "vgfx3d_backend_d3d11_context.inc",
        "vgfx3d_backend_d3d11_draw.inc",
        "vgfx3d_backend_d3d11_present.inc",
        "vgfx3d_backend_d3d11_targets.inc",
        "vgfx3d_backend_d3d11_texture.inc",
    };
    char path[1024];
    char *combined = NULL;
    size_t combined_length = 0;

    for (size_t i = 0; i < sizeof(k_parts) / sizeof(k_parts[0]); i++) {
        char *part;
        char *grown;
        size_t part_length;
        snprintf(path,
                 sizeof(path),
                 "%s/src/runtime/graphics/3d/backend/%s",
                 ZANNA_SOURCE_DIR,
                 k_parts[i]);
        part = read_text_file(path);
        if (!part) {
            free(combined);
            return NULL;
        }
        part_length = strlen(part);
        grown = (char *)realloc(combined, combined_length + part_length + 1u);
        if (!grown) {
            free(part);
            free(combined);
            return NULL;
        }
        combined = grown;
        memcpy(combined + combined_length, part, part_length + 1u);
        combined_length += part_length;
        free(part);
    }
    return combined;
}

static void test_d3d11_backend_source_contracts(void) {
    char *source = read_d3d11_backend_sources();

    EXPECT_TRUE(source != NULL, "D3D11 backend source chunks are readable for contract checks");
    if (!source)
        return;
    EXPECT_TRUE(strstr(source, "vgfx3d_sanitize_draw_command(cmd, &safe_cmd)") != NULL,
                "D3D11 sanitizes draw snapshots before native upload");
    EXPECT_TRUE(count_text(source, "vgfx3d_d3d11_draw_submission_is_ready(") >= 4,
                "D3D11 gates regular, instanced, skybox, and begin-frame target submission");
    EXPECT_TRUE(count_text(source, "vgfx3d_d3d11_frame_state_is_idle(") >= 1,
                "D3D11 blocks render-scale mutation while a completed frame awaits Present");
    EXPECT_TRUE(strstr(source, "CopyResource(presentedSnapshot)") != NULL &&
                    strstr(source, "CopySubresourceRegion(depthProbeStaging)") != NULL,
                "D3D11 checks device health before publishing asynchronous copy results");
    EXPECT_TRUE(count_text(source, "d3d11_device_status_after_void_command(") >= 10 &&
                    strstr(source, "CopyResource(opaqueDepth)") != NULL &&
                    strstr(source, "GenerateMips(streamed texture)") != NULL &&
                    strstr(source, "GenerateMips(streamed cubemap)") != NULL,
                "D3D11 validates device health before publishing void upload and copy commands");
    EXPECT_TRUE(strstr(source, "Draw(post-FX pass)") != NULL &&
                    strstr(source, "Draw(bloom downsample)") != NULL &&
                    strstr(source, "Draw(bloom upsample)") != NULL &&
                    strstr(source, "Draw(TAA resolve)") != NULL &&
                    strstr(source, "Draw(SSR resolve)") != NULL &&
                    strstr(source, "Draw(overlay composite)") != NULL,
                "D3D11 validates device health after every post-effect draw family");
    EXPECT_TRUE(strstr(source, "frame_submission_failed") != NULL &&
                    strstr(source, "Clear(frame targets)") != NULL &&
                    strstr(source, "DrawIndexed(main)") != NULL &&
                    strstr(source, "DrawIndexedInstanced(main)") != NULL &&
                    strstr(source, "DrawIndexed(shadow)") != NULL &&
                    strstr(source, "Begin(shadow atlas pass)") != NULL,
                "D3D11 latches device failures across clear and indexed draw families");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "if (ctx->frame_submission_failed) {",
                                            "ctx->frame_pending_present = 0;",
                                            "return;"),
                "D3D11 discards a failed frame before publishing it for Present");
    EXPECT_TRUE(strstr(source, "ID3D11DeviceContext_RSSetViewports(ctx->ctx, 0, NULL)") != NULL,
                "D3D11 clears stale viewport state when the active target extent is invalid");
    EXPECT_TRUE(strstr(source, "if (!ctx->postfx_current_bloom_srv)") != NULL &&
                    strstr(source, "if (!resolved) {") != NULL &&
                    strstr(source, "if (!reflected) {") != NULL,
                "D3D11 propagates bloom, TAA, and SSR encoder failures to the chain caller");
    EXPECT_TRUE(strstr(source, "SyncRTTBeforeUnbind") != NULL &&
                    strstr(source, "CreateTexture2D(rttStagingRecovery)") != NULL,
                "D3D11 preserves dirty RTT data and replaces failed staging resources");
    EXPECT_TRUE(
        strstr(source, "ctx->shadow_pass_failed = 1") != NULL &&
            strstr(source, "ctx->shadow_slot_complete[slot] = ctx->shadow_pass_failed ? 0") != NULL,
        "D3D11 never publishes a shadow slot after a failed shadow draw");
    EXPECT_TRUE(strstr(source, "vgfx3d_sanitize_postfx_snapshot") != NULL,
                "D3D11 sanitizes every post-FX snapshot before constant-buffer upload");
    EXPECT_TRUE(strstr(source, "vgfx3d_d3d11_cluster_table_is_usable") != NULL,
                "D3D11 clustered-light uploads validate revision and offset/index metadata");
    EXPECT_TRUE(strstr(source, "vgfx3d_d3d11_validate_ibl_layout") != NULL &&
                    strstr(source, "entry->applied_ibl_identity == env_cm->ibl_identity") != NULL,
                "D3D11 enables IBL only after the exact validated overlay is resident");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "entry->failed_ibl_identity == cubemap->ibl_identity",
                                            "entry->pending_ibl_identity = 0;",
                                            "entry->pending_ibl_bytes = 0;") &&
                    strstr(source, "entry->failed_ibl_identity == cubemap->ibl_identity ||") !=
                        NULL,
                "D3D11 retires failed IBL uploads instead of reporting them as pending forever");
    EXPECT_TRUE(strstr(source, "target->width > INT32_MAX / 4") != NULL,
                "D3D11 HDR readback rejects a float-stride narrowing overflow");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_readback_staging_texture",
                                            "&new_staging);",
                                            "d3d11_release_readback_staging_texture(ctx);"),
                "Readback staging allocates before releasing the cached surface");
    EXPECT_TRUE(count_text(source, "d3d11_texture2d_desc_matches(") >= 13u,
                "D3D11 validates created and cached texture descriptors before use");
    EXPECT_TRUE(count_text(source, "d3d11_view_resource_matches(") >= 4u &&
                    strstr(source, "d3d11_rtv_matches_texture2d") != NULL &&
                    strstr(source, "d3d11_dsv_matches_texture2d") != NULL &&
                    strstr(source, "d3d11_srv_matches_texturecube") != NULL,
                "D3D11 validates view shape and backing-resource identity");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_readback_staging_texture",
                                            "d3d11_texture2d_desc_matches(ctx->readback_staging",
                                            "return S_OK;"),
                "Readback staging metadata cannot hide a malformed cached native texture");
    EXPECT_TRUE(strstr(source, "d3d11_query_desc_matches(ctx->frame_time_disjoint_query") != NULL &&
                    count_text(source, "d3d11_query_desc_matches(") >= 4u,
                "D3D11 timestamp queries validate their native types before publication");
    EXPECT_TRUE(count_text(source, "d3d11_sampler_desc_matches(") >= 6u &&
                    strstr(source, "D3D11_DECODE_IS_ANISOTROPIC_FILTER") != NULL &&
                    strstr(source, "D3D11_DECODE_IS_COMPARISON_FILTER") != NULL &&
                    strstr(source, "D3D11_TEXTURE_ADDRESS_BORDER") != NULL &&
                    strstr(source, "SAFE_RELEASE(*cached);") != NULL,
                "D3D11 samplers validate behavior-bearing fields and evict stale cache entries");
    EXPECT_TRUE(count_text(source, "d3d11_buffer_desc_matches(") >= 4u,
                "D3D11 constant and immutable buffers validate native descriptors");
    EXPECT_TRUE(strstr(source, "ID3D11Texture2D_GetDesc(depth, &depth_desc)") != NULL &&
                    strstr(source, "depth_desc.Format != DXGI_FORMAT_R32_TYPELESS") != NULL,
                "Opaque-depth copies validate the source resource contract first");
    EXPECT_TRUE(count_text(source, "vgfx3d_d3d11_saturating_add_u64(") >= 3u &&
                    count_text(source, "texture_fallback_binds++") == 0u,
                "D3D11 texture telemetry cannot wrap after long-running fallback use");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_presented_snapshot_texture",
                                            "ID3D11Texture2D_GetDesc(ctx->presented_color_tex",
                                            "ID3D11Device_CreateTexture2D"),
                "Presented snapshots validate cached native descriptors before reuse");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_presented_snapshot_texture",
                                            "&new_texture);",
                                            "SAFE_RELEASE(ctx->presented_color_tex);"),
                "Presented snapshots allocate before replacing the cached texture");
    EXPECT_TRUE(strstr(source, "vgfx3d_d3d11_dynamic_buffer_desc_is_usable") != NULL,
                "Dynamic VB/IB reuse validates native descriptors against tracked capacity");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_float_srv_buffer",
                                            "d3d11_float_srv_pair_is_usable(*buffer",
                                            "ID3D11Device_CreateBuffer"),
                "Float-SRV caches validate the backing buffer and view before reuse");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_float_srv_buffer",
                                            "ID3D11Device_CreateShaderResourceView",
                                            "d3d11_float_srv_pair_is_usable("),
                "Float-SRV replacements validate native descriptors before publication");
    EXPECT_TRUE(count_text(source, "return d3d11_required_output_result") == 0u,
                "Direct D3D11 creation helpers release partial COM outputs on failure");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_scene_targets",
                                            "&new_depth_srv);",
                                            "d3d11_destroy_scene_route_targets(ctx);"),
                "Scene targets stage every color/depth view before publication");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_overlay_target",
                                            "&new_srv);",
                                            "SAFE_RELEASE(ctx->overlay_color_srv);"),
                "Overlay replacement preserves the old target until allocation succeeds");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_postfx_target",
                                            "&new_srv);",
                                            "SAFE_RELEASE(ctx->postfx_color_srv);"),
                "PostFX replacement stages a complete target first");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_postfx_scratch_target",
                                            "&new_srv);",
                                            "SAFE_RELEASE(ctx->postfx_scratch_srv);"),
                "PostFX scratch replacement stages a complete target first");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_bloom_targets",
                                            "&new_srv[i]);",
                                            "d3d11_destroy_bloom_targets(ctx);"),
                "Bloom builds its complete mip chain before publishing it");
    EXPECT_TRUE(
        text_appears_in_order_after(
            source, "d3d11_ensure_taa_targets", "&new_srv[i]);", "d3d11_destroy_taa_targets(ctx);"),
        "TAA builds both history targets before publishing them");
    EXPECT_TRUE(
        text_appears_in_order_after(
            source, "d3d11_ensure_ssr_target", "&new_srv);", "d3d11_destroy_ssr_target(ctx);"),
        "SSR stages its replacement before releasing the cached target");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_start_native_texture_upload",
                                            "&new_tex, &new_srv);",
                                            "d3d11_stage_texture_replacement(entry);"),
                "Native texture uploads allocate before evicting a good cache entry");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_start_texture_upload",
                                            "&new_tex, &new_srv);",
                                            "d3d11_stage_texture_replacement(entry);"),
                "RGBA texture uploads allocate before evicting a good cache entry");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_start_cubemap_upload",
                                            "&new_tex, &new_srv);",
                                            "d3d11_stage_cubemap_replacement(entry);"),
                "Cubemap uploads allocate before evicting a good cache entry");
    EXPECT_TRUE(
        strstr(source, "d3d11_discard_texture_candidate(entry);") != NULL &&
            strstr(source, "d3d11_discard_cubemap_candidate(entry);") != NULL &&
            strstr(source, "d3d11_previous_texture_or_white_srv(ctx, entry)") != NULL &&
            strstr(source,
                   "d3d11_previous_cubemap_or_white_srv(ctx, "
                   "&ctx->cubemap_cache[i])") != NULL,
        "D3D11 keeps previous complete textures visible across pending and failed replacements");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_acquire_mesh_buffers",
                                            "&new_ib);",
                                            "d3d11_release_mesh_cache_entry(slot);"),
                "Static meshes stage both immutable buffers before cache eviction");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_prepare_anim_resources",
                                            "&replacement.normal_element_count",
                                            "d3d11_release_morph_cache_entry(slot);"),
                "Morph caches stage position and normal resources before publication");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_resolve_opaque_targets",
                                            "&new_srv);",
                                            "d3d11_destroy_opaque_depth_target(ctx);"),
                "Opaque-depth replacement preserves the previous complete target on failure");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_rtt_targets",
                                            "&new_staging);",
                                            "d3d11_destroy_rtt_targets(ctx);"),
                "RTT replacement stages color, depth, and readback resources together");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_shadow_targets",
                                            "&new_srv);",
                                            "d3d11_release_shadow_slot(ctx, slot);"),
                "Shadow-slot resizing preserves the previous complete target on failure");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_ensure_shadow_atlas",
                                            "&new_srv);",
                                            "SAFE_RELEASE(ctx->shadow_atlas_srv);"),
                "Shadow-atlas resizing stages a complete replacement before release");
    EXPECT_TRUE(strstr(source, "FAILED(hr) || !*out_tex") != NULL &&
                    strstr(source,
                           "d3d11_device_child_result(hr, (ID3D11DeviceChild *)back_buffer, "
                           "ctx->device)") != NULL,
                "D3D11 target helpers reject missing or foreign COM outputs");
    EXPECT_TRUE(strstr(source, "static HRESULT d3d11_required_output_result") != NULL &&
                    strstr(source, "static HRESULT d3d11_device_child_result") != NULL &&
                    count_text(source, "d3d11_device_child_result(") >= 35u &&
                    strstr(source, "ID3D11DeviceChild_GetDevice") != NULL &&
                    strstr(source, "VGFX3D_IID_IUnknown") != NULL &&
                    strstr(source, "VGFX3D_IID_ID3D11Device") != NULL &&
                    strstr(source, "&IID_IUnknown") == NULL &&
                    strstr(source, "&IID_ID3D11Device") == NULL,
                "Required resources, states, shaders, and layouts validate output and device "
                "identity without external GUID data imports");
    EXPECT_TRUE(
        strstr(source, "d3d11_com_identity_matches((IUnknown *)actual, (IUnknown *)expected)") !=
                NULL &&
            strstr(source,
                   "d3d11_com_identity_matches((IUnknown *)view_resource, "
                   "(IUnknown *)buffer)") != NULL &&
            strstr(source, "view_resource ==") == NULL,
        "D3D11 view/resource pairing uses controlling COM identity");
    EXPECT_TRUE(strstr(source, "d3d11_depth_stencil_state_result") != NULL &&
                    strstr(source, "ID3D11DepthStencilState_GetDesc") != NULL &&
                    strstr(source, "d3d11_blend_state_result") != NULL &&
                    strstr(source, "ID3D11BlendState_GetDesc") != NULL &&
                    strstr(source, "d3d11_rasterizer_state_result") != NULL &&
                    strstr(source, "ID3D11RasterizerState_GetDesc") != NULL,
                "D3D11 pipeline states validate behavior-bearing native descriptors");
    EXPECT_TRUE(strstr(source, "(!ctx->swap_chain || !ctx->device || !ctx->ctx)") != NULL,
                "Device creation rejects a successful HRESULT with any missing core interface");
    EXPECT_TRUE(
        strstr(source, "d3d11_device_context_belongs_to_device(ctx->ctx, ctx->device)") != NULL &&
            strstr(source, "d3d11_swap_chain_belongs_to_device(ctx->swap_chain, ctx->device)") !=
                NULL,
        "Device creation validates immediate-context and swapchain ownership");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "d3d11_get_depth_biased_rasterizer",
                                            "d3d11_rasterizer_desc_matches",
                                            "SAFE_RELEASE(ctx->rs_depth_biased_cached)"),
                "Depth-biased rasterizer cache hits revalidate native state before reuse");
    EXPECT_TRUE(strstr(source, "static HRESULT d3d11_bind_main_pipeline") != NULL &&
                    strstr(source, "Bind(main pipeline)") != NULL,
                "Main pipeline binding reports state and device-health failures to draw callers");
    EXPECT_TRUE(strstr(source, "(void)d3d11_get_depth_biased_rasterizer") == NULL &&
                    strstr(source, "CreateRasterizerState(main depth bias)") != NULL &&
                    strstr(source, "CreateRasterizerState(shadow depth bias)") != NULL,
                "Depth-biased scene and shadow draws cannot ignore rasterizer creation failures");
    EXPECT_TRUE(strstr(source, "SelectRasterizerState(main)") != NULL &&
                    strstr(source, "SelectRasterizerState(shadow)") != NULL &&
                    strstr(source, "Bind(shadow rasterizer)") != NULL,
                "Scene and shadow draws require a concrete, healthy rasterizer state");
    EXPECT_TRUE(count_text(source, "d3d11_stats_count(&ctx->stats.dropped_draws);") >= 12u &&
                    strstr(source, "Draw(skybox)\")))\n        d3d11_stats_count") != NULL,
                "Bind, post-draw, and skybox device failures contribute to dropped-draw telemetry");
    EXPECT_TRUE(strstr(source, "D3D11_CREATE_DEVICE_BGRA_SUPPORT") != NULL &&
                    strstr(source, "created_feature_level != requested_feature_level") != NULL,
                "Device creation requests interop support and confirms Direct3D 11.0");
    EXPECT_TRUE(strstr(source, "D3D_DRIVER_TYPE_HARDWARE") != NULL &&
                    strstr(source, "D3D_DRIVER_TYPE_WARP") != NULL &&
                    strstr(source, "d3d11_release_device_attempt(ctx)") != NULL,
                "Device creation retries transactionally with WARP for remote and virtual hosts");
    EXPECT_TRUE(strstr(source, "Begin(frame timestamp)") != NULL &&
                    strstr(source, "End(frame timestamp)") != NULL,
                "Timestamp query publication is gated by device-health checks");
    EXPECT_TRUE(strstr(source, "ctx->fallback_white_tex && ctx->fallback_white_srv") != NULL &&
                    strstr(source, "ctx->brdf_lut_tex && ctx->brdf_lut_srv") != NULL &&
                    strstr(source, "new_fallback_white_tex") != NULL,
                "Fallback textures and the BRDF table publish as one complete resource set");
    EXPECT_TRUE(strstr(source, "invalid rtt staging descriptor") != NULL &&
                    strstr(source, "invalid rtt staging payload") != NULL,
                "Malformed cached RTT staging surfaces are evicted before retry");
    EXPECT_TRUE(strstr(source, "d3d11_format_log_message") != NULL,
                "D3D11 diagnostics remain initialized when CRT formatting fails");
    EXPECT_TRUE(strstr(source, "if (hr != S_OK)") != NULL,
                "Present status codes do not publish an unconfirmed displayed-frame snapshot");
    EXPECT_TRUE(strstr(source,
                       "static void d3d11_present_swapchain_classified(d3d11_context_t *ctx, int "
                       "offscreen) {\n"
                       "    if (!ctx)\n"
                       "        return;\n"
                       "    ctx->stats.present_path = 0;\n"
                       "    if (!ctx->swap_chain)\n"
                       "        return;\n"
                       "    if (!d3d11_present_swapchain(ctx))") != NULL,
                "D3D11 clears stale present-path telemetry before every swapchain availability or "
                "status check");
    EXPECT_TRUE(
        text_appears_in_order_after(source,
                                    "if (!cmd->geometry_key || cmd->geometry_revision == 0)",
                                    "D3D11_INITIAL_DYNAMIC_IB_SIZE))",
                                    "&ctx->stats.mesh_stream_uploads"),
        "D3D11 stream-upload telemetry commits after both buffer uploads succeed");
    EXPECT_TRUE(count_text(source,
                           "if (SUCCEEDED(d3d11_record_frame_device_status(ctx, "
                           "\"DrawIndexed") >= 2u,
                "D3D11 draw telemetry commits only while the device remains healthy");
    EXPECT_TRUE(strstr(source,
                       "out_stats->default_framebuffer_writable = "
                       "ctx->swap_chain && ctx->rtv ? 1 : 0;") != NULL,
                "D3D11 reports whether its default framebuffer is currently writable");
    EXPECT_TRUE(strstr(source, "D3D11_TEXTURE_CACHE_MAX_ENTRIES 4096") != NULL &&
                    strstr(source, "D3D11_CUBEMAP_CACHE_MAX_ENTRIES 256") != NULL,
                "D3D11 bounds one-frame texture and cubemap cache-table growth");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "static void d3d11_resize(",
                                            "IDXGISwapChain_ResizeBuffers",
                                            "d3d11_destroy_scene_targets(ctx);"),
                "D3D11 preserves independent scene targets until DXGI accepts a resize");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "static void d3d11_resize(",
                                            "IDXGISwapChain_ResizeBuffers",
                                            "ctx->frame_pending_present = 0;"),
                "D3D11 retires a superseded pending frame only after DXGI accepts a resize");
    EXPECT_TRUE(strstr(source, "!ctx->frame_pending_present") != NULL &&
                    strstr(source, "vgfx3d_d3d11_frame_state_is_idle") != NULL &&
                    strstr(source, "!ctx || !ctx->frame_active") != NULL &&
                    strstr(source, "ctx->frame_pending_present = prior_pending_present;") != NULL,
                "D3D11 rejects nested/out-of-order operations and preserves failed continuations");
    EXPECT_TRUE(
        strstr(source, "need_scratch_target = force_offscreen_final ? chain->effect_count > 1") !=
                NULL &&
            strstr(source,
                   "!final_rtv || "
                   "!vgfx3d_d3d11_is_valid_texture2d_extent(final_width,") != NULL,
        "D3D11 validates post-FX output first and allocates only required ping-pong targets");
    EXPECT_TRUE(text_appears_in_order_after(source,
                                            "static int8_t d3d11_set_render_scale(",
                                            "d3d11_ensure_overlay_target",
                                            "d3d11_ensure_scene_targets"),
                "Render-scale changes make the native overlay ready before publishing a scene");
    EXPECT_TRUE(strstr(source, "d3d11_destroy_scene_route_targets(ctx);") != NULL,
                "Scene replacement preserves the independent native-size overlay target");
    free(source);
}

int main(void) {
    test_pack_bone_palette_identity_pads_unused_bones();
    test_pack_bone_palette_identity_pads_empty_source();
    test_pack_bone_palette_keeps_highest_supported_bone();
    test_pack_bone_palette_identity_pads_invalid_bones();
    test_bone_palette_upload_size_covers_supported_bone_count();
    test_pack_scalar_array4_matches_hlsl_layout();
    test_finite_copy_helpers();
    test_constant_buffer_struct_sizes_match_expected_layout();
    test_fill_instance_data_uses_previous_or_current_matrices();
    test_frame_history_preserves_scene_state_across_overlay_passes();
    test_upload_status_helpers_drop_stale_state();
    test_target_kind_blend_and_color_format_helpers();
    test_capacity_and_mip_helpers();
    test_cluster_ibl_and_upload_budget_validation();
    test_d3d11_sanitization_helpers();
    test_d3d11_light_upload_sanitization_helpers();
    test_sampler_anisotropy_helpers();
    test_d3d11_limits_and_prune_helpers();
    test_mapped_copy_and_native_mip_validation_helpers();
    test_target_fallback_helper();
    test_morph_cache_reuse_helper();
    test_shadow_and_rtt_policy_helpers();
    test_depth_probe_bias_instancing_and_rtt_guards();
    test_frame_protocol_submission_guards();
    test_postfx_readback_policy_helpers();
    test_shadow_projection_helper_handles_all_projection_modes();
    test_d3d11_shader_chunk_join_validation();
    test_d3d11_shader_sources_keep_numeric_guards();
    test_d3d11_backend_source_contracts();

    printf("vgfx3d d3d11 shared tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
