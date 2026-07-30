//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_vgfx3d_backend_opengl_shared.c
// Purpose: Unit tests for OpenGL backend shared helper functions.
//
// Key invariants:
//   - Shared helper math and cache policies are deterministic without an OpenGL context.
//   - Render-target, blend, and readback policies stay consistent with backend code.
//   - Sampler-state cache indices stay clamped to their fixed array bounds.
//
// Ownership/Lifetime:
//   - Tests allocate only stack-local fixtures and process-local scratch buffers.
//
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_shared.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "vgfx3d_backend_opengl_shared.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZANNA_SOURCE_DIR
#define ZANNA_SOURCE_DIR "."
#endif

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

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (fabs((double)(a) - (double)(b)) > (eps))                                               \
            fprintf(stderr, "FAIL: %s (got %.6f expected %.6f)\n", msg, (double)(a), (double)(b)); \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

static size_t count_text(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t needle_length;
    const char *cursor;

    if (!haystack || !needle || needle[0] == '\0')
        return 0;
    needle_length = strlen(needle);
    cursor = haystack;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_length;
    }
    return count;
}

static void set_identity4x4(float *m) {
    memset(m, 0, sizeof(float) * 16u);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void test_frame_history_preserves_scene_state_across_overlay_passes(void) {
    vgfx3d_opengl_frame_history_t history;
    float scene_vp0[16];
    float scene_vp1[16];
    float overlay_vp[16];
    float inv0[16];
    float inv1[16];
    float overlay_inv[16];
    float cam0[3] = {1.0f, 2.0f, 3.0f};
    float cam1[3] = {4.0f, 5.0f, 6.0f};

    memset(&history, 0, sizeof(history));
    for (int i = 0; i < 16; i++) {
        scene_vp0[i] = (float)(i + 1);
        scene_vp1[i] = (float)(i + 21);
        overlay_vp[i] = (float)(i + 41);
        inv0[i] = (float)(i + 61);
        inv1[i] = (float)(i + 81);
        overlay_inv[i] = (float)(i + 101);
    }

    vgfx3d_opengl_update_frame_history(&history, scene_vp0, inv0, cam0, 0);
    EXPECT_TRUE(history.scene_history_valid == 1,
                "Main-pass history becomes valid after the first scene");
    EXPECT_NEAR(history.scene_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "First scene seeds prevViewProjection from the current scene");
    EXPECT_NEAR(history.draw_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "First scene uses the current VP as draw-time history");

    vgfx3d_opengl_update_frame_history(&history, scene_vp1, inv1, cam1, 0);
    EXPECT_NEAR(history.scene_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "Second scene preserves the previous scene VP");
    EXPECT_NEAR(
        history.scene_vp[0], scene_vp1[0], 1e-6f, "Second scene updates the current scene VP");
    EXPECT_NEAR(history.draw_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "Second scene draws against the prior scene VP");
    EXPECT_NEAR(
        history.scene_inv_vp[0], inv1[0], 1e-6f, "Second scene updates the scene inverse VP");
    EXPECT_NEAR(
        history.scene_cam_pos[0], cam1[0], 1e-6f, "Second scene updates the scene camera position");

    vgfx3d_opengl_update_frame_history(&history, overlay_vp, overlay_inv, cam0, 1);
    EXPECT_NEAR(history.scene_vp[0],
                scene_vp1[0],
                1e-6f,
                "Overlay passes preserve the main-scene VP for later postfx");
    EXPECT_NEAR(history.scene_prev_vp[0],
                scene_vp0[0],
                1e-6f,
                "Overlay passes preserve the previous main-scene VP");
    EXPECT_NEAR(history.scene_inv_vp[0],
                inv1[0],
                1e-6f,
                "Overlay passes preserve the main-scene inverse VP");
    EXPECT_NEAR(history.draw_prev_vp[0],
                overlay_vp[0],
                1e-6f,
                "Overlay passes use their own VP for draw-time history");
}

static void test_frame_history_sanitizes_nonfinite_scene_inputs(void) {
    vgfx3d_opengl_frame_history_t history;
    float vp[16];
    float inv_vp[16];
    float overlay_vp[16];
    float cam[3] = {NAN, 2.0f, INFINITY};

    set_identity4x4(vp);
    set_identity4x4(inv_vp);
    set_identity4x4(overlay_vp);
    vp[3] = NAN;
    inv_vp[7] = INFINITY;
    overlay_vp[11] = NAN;
    memset(&history, 0, sizeof(history));

    vgfx3d_opengl_update_frame_history(&history, vp, inv_vp, cam, 0);
    EXPECT_NEAR(history.scene_vp[0], 1.0f, 0.0f, "Invalid OpenGL scene VP becomes identity");
    EXPECT_NEAR(history.scene_vp[3], 0.0f, 0.0f, "Invalid OpenGL scene VP lane is cleared");
    EXPECT_NEAR(history.scene_inv_vp[7],
                0.0f,
                0.0f,
                "Invalid OpenGL inverse VP cannot poison post-processing");
    EXPECT_NEAR(
        history.scene_cam_pos[0], 0.0f, 0.0f, "Invalid OpenGL camera position lane uses zero");
    EXPECT_NEAR(
        history.scene_cam_pos[1], 2.0f, 0.0f, "Finite OpenGL camera position lane is preserved");
    EXPECT_NEAR(
        history.scene_cam_pos[2], 0.0f, 0.0f, "Infinite OpenGL camera position lane uses zero");

    vgfx3d_opengl_update_frame_history(&history, overlay_vp, inv_vp, cam, 1);
    EXPECT_NEAR(history.draw_prev_vp[0],
                1.0f,
                0.0f,
                "Invalid OpenGL overlay VP falls back to identity history");
    EXPECT_NEAR(history.draw_prev_vp[11], 0.0f, 0.0f, "Invalid OpenGL overlay VP lane is cleared");
}

static void test_target_blend_motion_and_readback_helpers(void) {
    vgfx3d_draw_cmd_t cmd;

    memset(&cmd, 0, sizeof(cmd));
    EXPECT_TRUE(vgfx3d_opengl_choose_target_kind(1, 1) == VGFX3D_OPENGL_TARGET_RTT,
                "RTT rendering always chooses the RTT target kind");
    EXPECT_TRUE(vgfx3d_opengl_choose_target_kind(0, 0) == VGFX3D_OPENGL_TARGET_SWAPCHAIN,
                "Without GPU postfx the backend renders directly to the swapchain");
    EXPECT_TRUE(vgfx3d_opengl_choose_target_kind(0, 1) == VGFX3D_OPENGL_TARGET_SCENE,
                "GPU postfx main passes render into the scene target");

    EXPECT_TRUE(vgfx3d_opengl_choose_color_format(VGFX3D_OPENGL_TARGET_SCENE) ==
                    VGFX3D_OPENGL_COLOR_FORMAT_HDR16F,
                "Scene rendering uses the HDR color target");
    EXPECT_TRUE(vgfx3d_opengl_choose_color_format(VGFX3D_OPENGL_TARGET_SWAPCHAIN) ==
                        VGFX3D_OPENGL_COLOR_FORMAT_UNORM8 &&
                    vgfx3d_opengl_choose_color_format(VGFX3D_OPENGL_TARGET_RTT) ==
                        VGFX3D_OPENGL_COLOR_FORMAT_UNORM8,
                "Swapchain and RTT targets use UNORM8 output");

    cmd.workflow = RT_MATERIAL3D_WORKFLOW_PBR;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    EXPECT_TRUE(vgfx3d_opengl_choose_blend_mode(&cmd) == VGFX3D_OPENGL_BLEND_ALPHA,
                "Blend materials use alpha blending");
    EXPECT_TRUE(vgfx3d_opengl_choose_motion_attachment_mode(VGFX3D_OPENGL_TARGET_SCENE, &cmd) ==
                    VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_ONLY,
                "Alpha-blended scene draws disable the motion attachment");
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_MASK;
    EXPECT_TRUE(vgfx3d_opengl_choose_blend_mode(&cmd) == VGFX3D_OPENGL_BLEND_OPAQUE,
                "Mask materials keep opaque render-target writes");
    EXPECT_TRUE(vgfx3d_opengl_choose_motion_attachment_mode(VGFX3D_OPENGL_TARGET_SCENE, &cmd) ==
                    VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_AND_MOTION,
                "Opaque scene draws keep the motion attachment enabled");
    cmd.disable_depth_test = 1;
    EXPECT_TRUE(vgfx3d_opengl_choose_motion_attachment_mode(VGFX3D_OPENGL_TARGET_SCENE, &cmd) ==
                    VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_ONLY,
                "Depth-disabled OpenGL draws cannot publish authoritative motion");
    cmd.disable_depth_test = 0;
    EXPECT_TRUE(vgfx3d_opengl_choose_motion_attachment_mode(VGFX3D_OPENGL_TARGET_SWAPCHAIN, &cmd) ==
                    VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_ONLY,
                "Swapchain draws never target a scene-motion attachment");

    memset(&cmd, 0, sizeof(cmd));
    cmd.workflow = RT_MATERIAL3D_WORKFLOW_LEGACY;
    cmd.alpha = 0.5f;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    EXPECT_TRUE(vgfx3d_opengl_choose_blend_mode(&cmd) == VGFX3D_OPENGL_BLEND_ALPHA,
                "Legacy translucent materials use alpha blending");
    cmd.alpha = 1.0f;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    EXPECT_TRUE(vgfx3d_opengl_choose_blend_mode(&cmd) == VGFX3D_OPENGL_BLEND_ALPHA,
                "Legacy explicit blend materials use alpha blending");
    cmd.alpha = 0.25f;
    cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_MASK;
    EXPECT_TRUE(vgfx3d_opengl_choose_blend_mode(&cmd) == VGFX3D_OPENGL_BLEND_OPAQUE,
                "Legacy masked materials stay on the opaque render path");

    EXPECT_TRUE(vgfx3d_opengl_choose_readback_kind(0) == VGFX3D_OPENGL_READBACK_BACKBUFFER,
                "Direct rendering reads back the backbuffer");
    EXPECT_TRUE(vgfx3d_opengl_choose_readback_kind(1) == VGFX3D_OPENGL_READBACK_POSTFX_COMPOSITE,
                "GPU postfx readback uses the composited postfx path");
    EXPECT_TRUE(vgfx3d_opengl_sanitize_shadow_index(1, 2) == 1,
                "Shadow index helper preserves completed shadow slots");
    EXPECT_TRUE(vgfx3d_opengl_sanitize_shadow_index(2, 2) == -1,
                "Shadow index helper rejects slots beyond the completed count");
    EXPECT_TRUE(vgfx3d_opengl_sanitize_shadow_index(0, 0) == -1,
                "Shadow index helper rejects all slots when no shadow maps completed");
    EXPECT_TRUE(vgfx3d_opengl_sanitize_shadow_index(3, 99) == -1,
                "Shadow index helper still clamps to the backend maximum slot count");
}

static void test_capacity_and_cache_helpers(void) {
    vgfx3d_draw_cmd_t cmd;
    size_t bytes = 0;
    size_t capacity = 0;

    EXPECT_TRUE(vgfx3d_opengl_compute_mip_count(1, 1) == 1, "1x1 textures use a single mip level");
    EXPECT_TRUE(vgfx3d_opengl_compute_mip_count(4, 2) == 3,
                "Mip-count helper follows the full downsample chain");
    EXPECT_TRUE(vgfx3d_opengl_next_capacity(0, 65, 64) == 128,
                "Capacity helper grows beyond the old fixed cache size");
    EXPECT_TRUE(vgfx3d_opengl_next_capacity(16, 8, 16) == 16,
                "Capacity helper keeps existing storage when it is already large enough");
    EXPECT_TRUE(vgfx3d_opengl_next_capacity(0, 0, 0) == 1,
                "Capacity helper never returns a non-positive allocation size");
    EXPECT_TRUE(vgfx3d_opengl_compute_buffer_capacity(0, 65, 64, &capacity) == 1 && capacity == 128,
                "Size_t capacity helper grows without narrowing through int32_t");
    EXPECT_TRUE(vgfx3d_opengl_compute_buffer_capacity(SIZE_MAX / 2 + 1, SIZE_MAX, 64, &capacity) ==
                        1 &&
                    capacity == SIZE_MAX,
                "Size_t capacity helper reaches exact oversized needs without doubling overflow");
    EXPECT_TRUE(vgfx3d_opengl_validate_rgba8_destination(3, 2, 12, &bytes) == 1 && bytes == 24,
                "RGBA8 destination validation accepts a tight destination");
    EXPECT_TRUE(vgfx3d_opengl_validate_rgba8_destination(3, 2, 8, &bytes) == 0,
                "RGBA8 destination validation rejects short strides");
    EXPECT_TRUE(vgfx3d_opengl_clamp_morph_shape_count(1024u, 64) == VGFX3D_OPENGL_MAX_MORPH_SHAPES,
                "Morph count helper clamps to the shader-visible shape limit");
    EXPECT_TRUE(vgfx3d_opengl_clamp_morph_shape_count((uint32_t)INT_MAX, 1) == 0,
                "Morph count helper rejects vertex counts that would overflow int indexing");
    EXPECT_TRUE(vgfx3d_opengl_clamp_morph_shape_count(1u << 28, 8) == 2,
                "Morph count helper clamps to the signed shader index range");
    EXPECT_TRUE(vgfx3d_opengl_texture_buffer_accepts_r32f_payload(64u, 16) == 1,
                "R32F texture-buffer payload accepts exact texel-limit fits");
    EXPECT_TRUE(vgfx3d_opengl_texture_buffer_accepts_r32f_payload(68u, 16) == 0,
                "R32F texture-buffer payload rejects values beyond the texel limit");
    EXPECT_TRUE(vgfx3d_opengl_texture_buffer_accepts_r32f_payload(66u, 64) == 0,
                "R32F texture-buffer payload rejects non-float-aligned byte sizes");
    EXPECT_TRUE(vgfx3d_opengl_texture_buffer_accepts_r32f_payload(64u, 0) == 1,
                "R32F texture-buffer payload treats unknown GL limits as unbounded");
    EXPECT_TRUE(vgfx3d_opengl_has_complete_splat(1, 1, 1, 1, 1, 1) == 1,
                "Terrain splat helper accepts a complete control map and four layers");
    EXPECT_TRUE(vgfx3d_opengl_has_complete_splat(1, 1, 1, 0, 1, 1) == 0,
                "Terrain splat helper rejects partial layer bindings");
    EXPECT_TRUE(vgfx3d_opengl_should_prune_cache_entry(300, 10, 240) == 1,
                "Old cache entries become prune candidates");
    EXPECT_TRUE(vgfx3d_opengl_should_prune_cache_entry(200, 10, 240) == 0,
                "Recently used cache entries stay resident");
    EXPECT_TRUE(vgfx3d_opengl_sanitize_anisotropy(0) == 1,
                "OpenGL sampler anisotropy clamps zero to one");
    EXPECT_TRUE(vgfx3d_opengl_sanitize_anisotropy(64) == 16,
                "OpenGL sampler anisotropy clamps high values to sixteen");
    EXPECT_TRUE(vgfx3d_opengl_sanitize_anisotropy(8) == 8,
                "OpenGL sampler anisotropy preserves valid values");
    EXPECT_TRUE(vgfx3d_opengl_sampler_anisotropy_index(1) == 0,
                "OpenGL sampler anisotropy index starts at zero");
    EXPECT_TRUE(vgfx3d_opengl_sampler_anisotropy_index(16) == 15,
                "OpenGL sampler anisotropy index covers the final cache slot");

    memset(&cmd, 0, sizeof(cmd));
    cmd.morph_key = &cmd;
    cmd.morph_revision = 4;
    cmd.morph_deltas = (const float *)&cmd;
    cmd.morph_weights = (const float *)&cmd;
    cmd.morph_shape_count = 3;
    cmd.vertex_count = 128;
    cmd.morph_normal_deltas = NULL;
    EXPECT_TRUE(vgfx3d_opengl_should_reuse_morph_cache(
                    cmd.morph_key, cmd.morph_revision, 3, 128, 0, &cmd) == 1,
                "Morph cache entries reuse matching key/revision payloads");
    cmd.morph_revision = 5;
    EXPECT_TRUE(vgfx3d_opengl_should_reuse_morph_cache(cmd.morph_key, 4, 3, 128, 0, &cmd) == 0,
                "Morph cache entries reject stale revisions");
    cmd.morph_revision = 4;
    cmd.morph_normal_deltas = (const float *)&tests_run;
    EXPECT_TRUE(vgfx3d_opengl_should_reuse_morph_cache(cmd.morph_key, 4, 3, 128, 0, &cmd) == 0,
                "Morph cache entries include normal-delta presence in the cache key");
    cmd.morph_normal_deltas = (const float *)&tests_run;
    cmd.morph_shape_count = VGFX3D_OPENGL_MAX_MORPH_SHAPES + 4;
    EXPECT_TRUE(vgfx3d_opengl_should_reuse_morph_cache(
                    cmd.morph_key, 4, VGFX3D_OPENGL_MAX_MORPH_SHAPES, 128, 1, &cmd) == 1,
                "Morph cache reuse compares against the clamped OpenGL shape count");
}

static void test_shadow_projection_helper_handles_orthographic_and_perspective(void) {
    float m[16];
    float world[3] = {0.25f, -0.25f, 2.0f};
    float uv_depth[3];

    memset(m, 0, sizeof(m));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 2.0f;
    EXPECT_TRUE(vgfx3d_opengl_project_shadow_coord(
                    m, VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC, world, uv_depth) == 1,
                "OpenGL orthographic shadow projection accepts finite coordinates");
    EXPECT_NEAR(uv_depth[0], 0.625f, 1e-6f, "OpenGL orthographic shadow UV does not divide by W");
    EXPECT_NEAR(uv_depth[1], 0.375f, 1e-6f, "OpenGL orthographic shadow UV preserves GL Y");

    memset(m, 0, sizeof(m));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 0.5f;
    m[14] = 1.0f;
    EXPECT_TRUE(vgfx3d_opengl_project_shadow_coord(
                    m, VGFX3D_SHADOW_PROJECTION_PERSPECTIVE, world, uv_depth) == 1,
                "OpenGL perspective shadow projection accepts positive W");
    EXPECT_NEAR(uv_depth[0], 0.5625f, 1e-6f, "OpenGL perspective shadow UV divides by W");
    EXPECT_NEAR(uv_depth[1], 0.4375f, 1e-6f, "OpenGL perspective shadow UV preserves GL Y");
    EXPECT_NEAR(uv_depth[2], 0.75f, 1e-6f, "OpenGL perspective shadow depth divides by W");

    world[2] = 0.0f;
    EXPECT_TRUE(vgfx3d_opengl_project_shadow_coord(
                    m, VGFX3D_SHADOW_PROJECTION_PERSPECTIVE, world, uv_depth) == 0,
                "OpenGL perspective shadow projection rejects non-positive W");

    world[2] = 2.0f;
    EXPECT_TRUE(
        vgfx3d_opengl_project_shadow_coord(m, VGFX3D_SHADOW_PROJECTION_CUBE, world, uv_depth) == 1,
        "OpenGL cube shadow projection uses perspective division");
    EXPECT_NEAR(uv_depth[0], 0.5625f, 1e-6f, "OpenGL cube shadow UV divides by W");
    uv_depth[0] = 9.0f;
    uv_depth[1] = 9.0f;
    uv_depth[2] = 9.0f;
    EXPECT_TRUE(vgfx3d_opengl_project_shadow_coord(m, 99, world, uv_depth) == 0,
                "OpenGL shadow projection rejects unknown projection types");
    EXPECT_TRUE(uv_depth[0] == 0.0f && uv_depth[1] == 0.0f && uv_depth[2] == 0.0f,
                "OpenGL shadow projection clears output on failure");
    memset(m, 0, sizeof(m));
    EXPECT_TRUE(vgfx3d_opengl_project_shadow_coord(
                    m, VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC, world, uv_depth) == 0,
                "OpenGL shadow projection rejects an all-zero matrix");
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

static char *read_opengl_backend_sources(void) {
    static const char *kParts[] = {
        "vgfx3d_backend_opengl.c",
        "vgfx3d_backend_opengl_context.inc",
        "vgfx3d_backend_opengl_shaders.inc",
        "vgfx3d_backend_opengl_mesh.inc",
        "vgfx3d_backend_opengl_material.inc",
        "vgfx3d_backend_opengl_frame.inc",
        "vgfx3d_backend_opengl_targets.inc",
        "vgfx3d_backend_opengl_texture.inc",
    };
    char path[1024];
    char *combined = NULL;
    size_t combined_length = 0;

    for (size_t i = 0; i < sizeof(kParts) / sizeof(kParts[0]); i++) {
        char *part;
        char *grown;
        size_t part_length;
        snprintf(path,
                 sizeof(path),
                 "%s/src/runtime/graphics/3d/backend/%s",
                 ZANNA_SOURCE_DIR,
                 kParts[i]);
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

static void test_opengl_source_contracts_are_context_safe(void) {
    char *source = read_opengl_backend_sources();
    const char *sync_target_decl;
    const char *target_switch;

    EXPECT_TRUE(source != NULL, "OpenGL backend source chunks are readable for contract checks");
    if (!source)
        return;
    sync_target_decl = strstr(source,
                              "static int gl_sync_render_target_color(void *ctx_ptr, "
                              "vgfx3d_rendertarget_t *rt);");
    target_switch = strstr(source, "static void gl_set_render_target");
    EXPECT_TRUE(sync_target_decl && target_switch && sync_target_decl < target_switch,
                "OpenGL render-target synchronization is declared before frame target switching");
    EXPECT_TRUE(strstr(source, "static int gl_zero_to_one_clip") == NULL,
                "OpenGL clip convention is not shared mutable process state");
    EXPECT_TRUE(strstr(source, "gl_compile_context_shaders(&shaders, ctx->zero_to_one_clip)") !=
                    NULL,
                "OpenGL shader compilation receives the owning context's clip convention");
    EXPECT_TRUE(strstr(source, "char log[1024] = {0};") != NULL,
                "OpenGL shader and linker diagnostics start NUL-terminated");
    EXPECT_TRUE(strstr(source, "vec4(mat3(model) * localTangent, aTangent.w)") != NULL,
                "OpenGL tangents use the model linear transform under non-uniform scale");
    EXPECT_TRUE(strstr(source, "vgfx3d_copy_mat4_finite_or_identity(safe_matrix") != NULL,
                "OpenGL bone uploads sanitize each matrix before transposition");
    EXPECT_TRUE(strstr(source, "vgfx3d_shadow_matrix_is_usable(light_vp)") != NULL,
                "OpenGL shadow begin/reuse reject unusable light matrices");
    EXPECT_TRUE(strstr(source,
                       "return (len2 > 1e-12 && len2 < 1e20) ? v * inversesqrt(len2) : "
                       "fallback;") != NULL,
                "OpenGL safe normalization rejects huge and non-finite vector lengths");
    EXPECT_TRUE(strstr(source, "skyboxSafeNormalize3") != NULL,
                "OpenGL skybox normalization has a finite fallback path");
    EXPECT_TRUE(strstr(source, " normalize(") == NULL,
                "OpenGL shaders do not retain raw zero/overflow-prone normalize calls");
    EXPECT_TRUE(
        strstr(source, "vec3 safeHdrColor(vec3 c)") != NULL &&
            strstr(source, "FragColor = vec4(safeHdrColor(result), safeAlpha(finalAlpha));") !=
                NULL,
        "OpenGL material outputs contain non-finite HDR values before post-processing");
    EXPECT_TRUE(strstr(source, "vec3 bloomSrcAt(vec2 uv)") != NULL &&
                    strstr(source, "bloomSafeColor(col * scale)") != NULL &&
                    strstr(source, "bloomSafeColor(col * (1.0 / 16.0))") != NULL,
                "OpenGL bloom contains invalid HDR samples through its complete mip chain");
    EXPECT_TRUE(strstr(source, "vgfx3d_sanitize_reversed_depth_probe_result(mapped[i])") != NULL,
                "OpenGL depth-probe publication rejects non-finite GPU samples");
    EXPECT_TRUE(strstr(source, "ctx->depth_probe_result_count = 0;") != NULL,
                "OpenGL invalidates stale probe results before mapping new PBO data");
    EXPECT_TRUE(strstr(source, "vgfx3d_sanitize_postfx_snapshot(snapshot, &safe_snapshot)") != NULL,
                "OpenGL post-FX uniforms sanitize malformed snapshots");
    EXPECT_TRUE(strstr(source, "vgfx3d_cluster_table_is_usable(table, expected_revision") != NULL,
                "OpenGL clustered-light uploads validate revision and offset/index metadata");
    EXPECT_TRUE(strstr(source, "gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)") != NULL &&
                    strstr(source, "gl.ClientWaitSync(ctx->depth_probe_fence, 0, 0)") != NULL &&
                    strstr(source, "gl.DeleteSync(ctx->depth_probe_fence)") != NULL,
                "OpenGL depth-probe PBOs use an explicit GPU fence lifecycle");
    EXPECT_TRUE(strstr(source, "ctx->shadow_pass_failed = 1") != NULL &&
                    strstr(source, "!ctx->shadow_pass_failed && ctx->shadow_depth_tex[slot]") !=
                        NULL,
                "OpenGL never publishes a shadow slot after a failed shadow draw");
    EXPECT_TRUE(strstr(source, "GL_UPLOAD_FAILED = -1") != NULL &&
                    strstr(source, "entry->failed_generation == generation") != NULL,
                "OpenGL memoizes terminal texture-upload failures separately from budget pauses");
    EXPECT_TRUE(strstr(source, "gl_cache_age_scratch(ctx, ctx->texture_cache_count)") != NULL &&
                    strstr(source, "free(ctx->cache_age_scratch)") != NULL,
                "OpenGL cache pruning reuses and releases context-owned age scratch storage");
    EXPECT_TRUE(strstr(source, "vgfx3d_validate_cubemap_ibl_layout") != NULL &&
                    strstr(source, "entry->applied_ibl_identity == env_cm->ibl_identity") != NULL,
                "OpenGL enables IBL only after the exact validated overlay is resident");
    EXPECT_TRUE(
        strstr(source,
               "entry->fallback_tex ? entry->fallback_tex : gl_fallback_white_cubemap(ctx)") !=
                NULL &&
            strstr(source, "if (entry->fallback_tex)") != NULL,
        "OpenGL keeps the previous complete cubemap visible during replacement and failure");
    EXPECT_TRUE(strstr(source,
                       "gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, "
                       "&state->draw_framebuffer)") != NULL &&
                    strstr(source,
                           "gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, "
                           "&state->read_framebuffer)") != NULL,
                "OpenGL helper passes preserve distinct draw and read framebuffer bindings");
    EXPECT_TRUE(strstr(source, "gl.GetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture)") !=
                        NULL &&
                    strstr(source,
                           "gl.GetIntegerv(GL_TEXTURE_BINDING_2D, "
                           "&state->texture_binding_2d)") != NULL &&
                    strstr(source,
                           "gl.GetIntegerv(GL_RENDERBUFFER_BINDING, "
                           "&state->renderbuffer)") != NULL,
                "OpenGL target helpers preserve texture-unit, texture, and renderbuffer state");
    EXPECT_TRUE(strstr(source, "gl.GetIntegerv(GL_SCISSOR_TEST, &state->scissor_test)") != NULL &&
                    strstr(source, "if (state->scissor_test)") != NULL &&
                    strstr(source, "gl.Enable(GL_SCISSOR_TEST);") != NULL,
                "OpenGL helper passes preserve an active caller scissor test");
    EXPECT_TRUE(strstr(source, "static int gl_target_extent_is_valid") != NULL &&
                    strstr(source, "width <= ctx->max_texture_size") != NULL,
                "OpenGL rejects target dimensions beyond the live device limit");
    EXPECT_TRUE(strstr(source, "face_size > ctx->max_texture_size") != NULL,
                "OpenGL rejects cubemap faces beyond the live device limit");
    EXPECT_TRUE(strstr(source, "GLuint scene_postfx_tex;") != NULL &&
                    strstr(source,
                           "source_is_scene_color ? GL_COLOR_ATTACHMENT2 : "
                           "GL_COLOR_ATTACHMENT0") != NULL,
                "OpenGL in-scene post-FX uses a dedicated same-precision attachment");
    EXPECT_TRUE(strstr(source,
                       "source_is_scene_color ? ctx->scene_postfx_tex : "
                       "ctx->scene_color_tex") != NULL,
                "OpenGL post-FX never overwrites motion vectors while sampling them");
    EXPECT_TRUE(strstr(source, "static int gl_ensure_color_target") != NULL &&
                    strstr(source, "previous_fbo = *io_fbo;") != NULL,
                "OpenGL color targets publish only after candidate framebuffer validation");
    EXPECT_TRUE(strstr(source, "gl_scene_target_set_t candidate = {0};") != NULL &&
                    strstr(source, "gl_release_scene_target_set(&previous);") != NULL &&
                    strstr(source, "const GLint color_format = hdr ? GL_RGBA16F : GL_RGBA8;") !=
                        NULL &&
                    strstr(source, "allocation_ok = gl_upload_ok();") != NULL,
                "OpenGL scene resize and HDR fallback publish a complete sized-format set");
    EXPECT_TRUE(strstr(source, "gl_encode_bloom_chain(\n                ctx, source_tex") != NULL,
                "OpenGL bloom consumes the current ordered-chain source");
    EXPECT_TRUE(strstr(source, "if (!resolved) {") != NULL &&
                    strstr(source, "if (!reflected) {") != NULL,
                "OpenGL propagates TAA and SSR encoder failures");
    EXPECT_TRUE(count_text(source, "gl_ensure_readback_scratch(ctx,") >= 3u,
                "OpenGL reuses context-owned scratch for window and RTT readback");
    EXPECT_TRUE(strstr(source, "while (capacity < bytes)") != NULL &&
                    strstr(source, "ctx->readback_scratch_bytes = capacity;") != NULL,
                "OpenGL readback scratch grows geometrically instead of reallocating per pixel");
    EXPECT_TRUE(
        strstr(source, "needs_bloom && ensure_bloom_targets") != NULL &&
            strstr(source, "This fallback must not mutate scene color before discovering") != NULL,
        "OpenGL preflights deterministic in-scene fallback requirements before drawing");
    EXPECT_TRUE(strstr(source, "ctx->morph_cache_count < GL_MORPH_CACHE_MAX_RESIDENT") != NULL &&
                    strstr(source, "gl_morph_cache_entry_t candidate;") != NULL,
                "OpenGL morph cache is hard-bounded and replacement is transactional");
    EXPECT_TRUE(
        strstr(source, "Keep the dirty target and its") != NULL &&
            strstr(source,
                   "if (!ctx->rtt_fbo || !ctx->rtt_color_tex || !ctx->rtt_depth_rbo || "
                   "!ctx->rtt_target)") != NULL,
        "OpenGL failed RTT unbind and replacement requests preserve a recoverable dirty target");
    free(source);
}

int main(void) {
    test_frame_history_preserves_scene_state_across_overlay_passes();
    test_frame_history_sanitizes_nonfinite_scene_inputs();
    test_target_blend_motion_and_readback_helpers();
    test_capacity_and_cache_helpers();
    test_shadow_projection_helper_handles_orthographic_and_perspective();
    test_opengl_source_contracts_are_context_safe();

    printf("vgfx3d opengl shared tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
