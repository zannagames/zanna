//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_canvas3d_production.cpp
// Purpose: Headless production-readiness checks for Canvas3D runtime behavior.
// Key invariants:
//   - Production software-renderer scenes remain byte-stable across backend changes.
//   - Backend capability reporting stays script-facing and deterministic.
// Ownership/Lifetime:
//   - Tests allocate runtime objects directly and release them before returning.
//   - Environment overrides are restored by scoped guards.
// Links: src/runtime/graphics/3d/backend/vgfx3d_backend_sw.c,
//        docs/zannalib/graphics/rendering3d.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_string.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_vec3.h"
#include "vgfx3d_backend.h"

int64_t vgfx3d_software_backend_thread_count_for_test(const void *ctx);
float vgfx3d_software_backend_clamp01_for_test(float value);
float vgfx3d_software_backend_material_pow_for_test(float value, float power);
int32_t vgfx3d_software_backend_wrap_index_for_test(int64_t index, int32_t size, int32_t mode);
int32_t vgfx3d_software_backend_unit_coord_index_for_test(float value, int32_t extent);
int32_t vgfx3d_software_backend_perspective_weights_for_test(float b0,
                                                             float b1,
                                                             float b2,
                                                             float inv_w0,
                                                             float inv_w1,
                                                             float inv_w2,
                                                             float *out_b0,
                                                             float *out_b1,
                                                             float *out_b2);
int32_t vgfx3d_software_backend_rgba_surface_valid_for_test(const uint8_t *pixels,
                                                            int32_t width,
                                                            int32_t height,
                                                            int32_t stride);
void *rt_pixels_new(int64_t width, int64_t height);
void rt_pixels_set_rgba(void *pixels, int64_t x, int64_t y, int64_t rgba);
}

static int tests_passed = 0;
static int tests_run = 0;

struct RenderTargetReleaseProbe {
    int calls;
    bool saw_live_shell;
};

static void render_target_release_probe(void *userdata, vgfx3d_rendertarget_t *target) {
    auto *probe = static_cast<RenderTargetReleaseProbe *>(userdata);
    if (!probe)
        return;
    probe->calls++;
    probe->saw_live_shell = target && target->width == 8 && target->height == 6 &&
                            target->cache_identity != 0 && target->estimated_bytes != 0;
}

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_EQ_I64(a, b, msg)                                                                   \
    do {                                                                                           \
        tests_run++;                                                                               \
        if ((int64_t)(a) != (int64_t)(b))                                                          \
            std::fprintf(stderr,                                                                   \
                         "FAIL: %s (expected %lld, got %lld)\n",                                   \
                         msg,                                                                      \
                         (long long)(b),                                                           \
                         (long long)(a));                                                          \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (std::fabs((double)(a) - (double)(b)) > (eps))                                          \
            std::fprintf(                                                                          \
                stderr, "FAIL: %s (expected %f, got %f)\n", msg, (double)(b), (double)(a));        \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_EQ_U64(a, b, msg)                                                                   \
    do {                                                                                           \
        tests_run++;                                                                               \
        if ((uint64_t)(a) != (uint64_t)(b))                                                        \
            std::fprintf(stderr,                                                                   \
                         "FAIL: %s (expected 0x%016llx, got 0x%016llx)\n",                         \
                         msg,                                                                      \
                         (unsigned long long)(b),                                                  \
                         (unsigned long long)(a));                                                 \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static int set_process_env_var(const char *name, const char *value) {
    if (!name || !*name)
        return 0;
#if RT_PLATFORM_WINDOWS
    return _putenv_s(name, value ? value : "") == 0 ? 1 : 0;
#else
    return value ? (setenv(name, value, 1) == 0 ? 1 : 0) : (unsetenv(name) == 0 ? 1 : 0);
#endif
}

class ScopedEnvVar {
  public:
    explicit ScopedEnvVar(const char *name) : name_(name ? name : ""), had_value_(false) {
        const char *value = std::getenv(name_.c_str());
        if (value) {
            had_value_ = true;
            old_value_ = value;
        }
    }

    ~ScopedEnvVar() {
        if (had_value_)
            (void)set_process_env_var(name_.c_str(), old_value_.c_str());
        else
            (void)set_process_env_var(name_.c_str(), nullptr);
    }

    int set(const char *value) {
        return set_process_env_var(name_.c_str(), value);
    }

    int unset() {
        return set_process_env_var(name_.c_str(), nullptr);
    }

  private:
    std::string name_;
    bool had_value_;
    std::string old_value_;
};

struct SoftwareSceneRenderResult {
    uint64_t hash = 0;
    float shadow_luma = 0.0f;
    float lit_left_luma = 0.0f;
    float lit_right_luma = 0.0f;
    int64_t worker_count = 1;
    std::vector<uint8_t> rgba;
};

static void set_identity_matrix(double *m) {
    std::memset(m, 0, sizeof(double) * 16u);
    m[0] = 1.0;
    m[5] = 1.0;
    m[10] = 1.0;
    m[15] = 1.0;
}

static void set_translation_matrix(double *m, double x, double y, double z) {
    set_identity_matrix(m);
    m[3] = x;
    m[7] = y;
    m[11] = z;
}

static int project_world_to_pixel(const rt_canvas3d *canvas,
                                  float wx,
                                  float wy,
                                  float wz,
                                  int32_t width,
                                  int32_t height,
                                  int32_t *out_x,
                                  int32_t *out_y) {
    const float *m = canvas ? canvas->cached_vp : nullptr;
    float cx;
    float cy;
    float cw;
    float ndc_x;
    float ndc_y;

    if (!m || !out_x || !out_y || width <= 0 || height <= 0)
        return 0;
    cx = wx * m[0] + wy * m[1] + wz * m[2] + m[3];
    cy = wx * m[4] + wy * m[5] + wz * m[6] + m[7];
    cw = wx * m[12] + wy * m[13] + wz * m[14] + m[15];
    if (!std::isfinite(cw) || cw <= 1e-6f)
        return 0;
    ndc_x = cx / cw;
    ndc_y = cy / cw;
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y))
        return 0;
    *out_x = (int32_t)((ndc_x + 1.0f) * 0.5f * (float)width);
    *out_y = (int32_t)((1.0f - ndc_y) * 0.5f * (float)height);
    return *out_x >= 0 && *out_x < width && *out_y >= 0 && *out_y < height;
}

static float sample_luminance_box(const vgfx3d_rendertarget_t *target,
                                  int32_t cx,
                                  int32_t cy,
                                  int32_t radius) {
    float sum = 0.0f;
    int32_t count = 0;

    if (!target || !target->color_buf || target->width <= 0 || target->height <= 0 ||
        target->stride < target->width * 4)
        return 0.0f;
    for (int32_t y = cy - radius; y <= cy + radius; y++) {
        if (y < 0 || y >= target->height)
            continue;
        for (int32_t x = cx - radius; x <= cx + radius; x++) {
            const uint8_t *px;
            float r;
            float g;
            float b;

            if (x < 0 || x >= target->width)
                continue;
            px = &target->color_buf[y * target->stride + x * 4];
            r = (float)px[0] / 255.0f;
            g = (float)px[1] / 255.0f;
            b = (float)px[2] / 255.0f;
            sum += r * 0.2126f + g * 0.7152f + b * 0.0722f;
            count++;
        }
    }
    return count > 0 ? sum / (float)count : 0.0f;
}

static uint64_t hash_render_target_rgba(const vgfx3d_rendertarget_t *target) {
    uint64_t hash = 1469598103934665603ull;

    if (!target || !target->color_buf || target->width <= 0 || target->height <= 0 ||
        target->stride < target->width * 4)
        return 0;
    for (int32_t y = 0; y < target->height; y++) {
        const uint8_t *row = &target->color_buf[y * target->stride];
        for (int32_t x = 0; x < target->width * 4; x++) {
            hash ^= row[x];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static float g_clear_r = -1.0f;
static float g_clear_g = -1.0f;
static float g_clear_b = -1.0f;

static void fake_clear(void *, vgfx_window_t, float r, float g, float b) {
    g_clear_r = r;
    g_clear_g = g;
    g_clear_b = b;
}

static void fake_set_render_target(void *, vgfx3d_rendertarget_t *) {}

static void fake_shadow_begin(void *, int32_t, float *, int32_t, int32_t, const float *) {}

static void fake_shadow_draw(void *, const vgfx3d_draw_cmd_t *) {}

static void fake_shadow_end(void *, int32_t, float) {}

static void fake_draw_skybox(void *, const void *) {}

static void fake_submit_draw_instanced(void *,
                                       vgfx_window_t,
                                       const vgfx3d_draw_cmd_t *,
                                       const float *,
                                       int32_t,
                                       const vgfx3d_light_params_t *,
                                       int32_t,
                                       const float *,
                                       int8_t,
                                       int8_t) {}

static int fake_readback_rgba(void *, uint8_t *, int32_t, int32_t, int32_t) {
    return 0;
}

static void fake_submit_draw(void *,
                             vgfx_window_t,
                             const vgfx3d_draw_cmd_t *,
                             const vgfx3d_light_params_t *,
                             int32_t,
                             const float *,
                             int8_t,
                             int8_t) {}

static void fake_present(void *) {}

static void fake_present_postfx(void *, const vgfx3d_postfx_chain_t *) {}

static void fake_apply_postfx(void *, const vgfx3d_postfx_chain_t *) {}

static int64_t fake_native_texture_caps(void *) {
    return RT_CANVAS3D_BACKEND_CAP_BC7 | RT_CANVAS3D_BACKEND_CAP_ASTC |
           RT_CANVAS3D_BACKEND_CAP_ETC2 | RT_CANVAS3D_BACKEND_CAP_ANISOTROPY |
           RT_CANVAS3D_BACKEND_CAP_BC1 | RT_CANVAS3D_BACKEND_CAP_BC3 | RT_CANVAS3D_BACKEND_CAP_BC4 |
           RT_CANVAS3D_BACKEND_CAP_BC5;
}

static vgfx3d_backend_t make_fake_gpu_backend() {
    vgfx3d_backend_t backend = {};
    backend.name = "testgpu";
    backend.clear = fake_clear;
    backend.submit_draw = fake_submit_draw;
    backend.set_render_target = fake_set_render_target;
    backend.shadow_begin = fake_shadow_begin;
    backend.shadow_draw = fake_shadow_draw;
    backend.shadow_end = fake_shadow_end;
    backend.draw_skybox = fake_draw_skybox;
    backend.submit_draw_instanced = fake_submit_draw_instanced;
    backend.readback_rgba = fake_readback_rgba;
    backend.present = fake_present;
    backend.present_postfx = fake_present_postfx;
    backend.apply_postfx = fake_apply_postfx;
    return backend;
}

static int backend_supports(rt_canvas3d *canvas, const char *name) {
    rt_string s = rt_const_cstr(name);
    int supported = rt_canvas3d_backend_supports(canvas, s);
    rt_string_unref(s);
    return supported;
}

static void test_null_canvas_has_no_capabilities() {
    EXPECT_EQ_I64(rt_canvas3d_get_backend_capabilities(nullptr),
                  0,
                  "null canvas reports no backend capabilities");
    EXPECT_TRUE(!backend_supports(nullptr, "shadows"),
                "null canvas does not support named capabilities");
}

static void test_software_backend_reports_canvas_fallback_features() {
    rt_canvas3d canvas = {};
    canvas.backend = &vgfx3d_software_backend;

    int64_t caps = rt_canvas3d_get_backend_capabilities(&canvas);
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SOFTWARE) != 0,
                "software backend advertises software mode");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_GPU) == 0,
                "software backend does not advertise GPU mode");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET) != 0,
                "software backend supports render targets");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_WINDOW_READBACK) != 0,
                "software backend supports window readback");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SHADOWS) != 0,
                "software backend supports shadow maps");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SKYBOX) != 0,
                "software backend supports Canvas3D skybox fallback");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_POSTFX) != 0,
                "software backend supports CPU post effects");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX) == 0,
                "software backend does not advertise GPU post effects");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_POSTFX_OVERLAY) != 0,
                "software backend advertises final overlays after CPU post effects");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_FINAL_SCREENSHOT) != 0,
                "software backend advertises final-frame screenshots");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX_OVERLAY) == 0,
                "software backend does not advertise GPU postfx overlay composition");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_HARDWARE_INSTANCING) == 0,
                "software backend does not advertise hardware instancing");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_HLOD) != 0,
                "software backend advertises runtime HLOD/impostor proxies");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_OCCLUSION) != 0,
                "software backend advertises CPU occlusion culling");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_PBR) != 0,
                "software backend advertises material PBR workflow support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_NORMAL_MAPS) != 0,
                "software backend advertises normal-map material support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ALPHA_MASK) != 0,
                "software backend advertises alpha-mask material support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_MORPH_TARGETS) != 0,
                "software backend advertises CPU morph-target support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SKINNING) != 0,
                "software backend advertises CPU skinning support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_TERRAIN_SPLAT) != 0,
                "software backend advertises terrain splat support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC1) == 0,
                "software backend does not advertise BC1 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC3) == 0,
                "software backend does not advertise BC3 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC4) == 0,
                "software backend does not advertise BC4 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC5) == 0,
                "software backend does not advertise BC5 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC7) == 0,
                "software backend does not advertise BC7 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ASTC) == 0,
                "software backend does not advertise ASTC compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ETC2) == 0,
                "software backend does not advertise ETC2 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ANISOTROPY) == 0,
                "software backend accepts but does not advertise anisotropy");

    EXPECT_TRUE(backend_supports(&canvas, "postfx-overlay"),
                "BackendSupports accepts postfx-overlay alias");
    EXPECT_TRUE(backend_supports(&canvas, "final_screenshot"),
                "BackendSupports accepts final_screenshot alias");
    EXPECT_TRUE(backend_supports(&canvas, "hlod"), "BackendSupports accepts hlod capability alias");
    EXPECT_TRUE(backend_supports(&canvas, "occlusion"),
                "BackendSupports accepts occlusion capability alias");
    EXPECT_TRUE(backend_supports(&canvas, "pbr"), "BackendSupports accepts pbr capability alias");
    EXPECT_TRUE(backend_supports(&canvas, "normal-maps"),
                "BackendSupports accepts normal-maps capability alias");
    EXPECT_TRUE(backend_supports(&canvas, "alpha-mask"),
                "BackendSupports accepts alpha-mask capability alias");
    EXPECT_TRUE(backend_supports(&canvas, "morph-targets"),
                "BackendSupports accepts morph-targets capability alias");
    EXPECT_TRUE(backend_supports(&canvas, "skinning"),
                "BackendSupports accepts skinning capability alias");
    EXPECT_TRUE(backend_supports(&canvas, "terrain-splat"),
                "BackendSupports accepts terrain-splat capability alias");
    EXPECT_TRUE(!backend_supports(&canvas, "bc1"),
                "BackendSupports reports bc1 unsupported until backend upload exists");
    EXPECT_TRUE(!backend_supports(&canvas, "bc3"),
                "BackendSupports reports bc3 unsupported until backend upload exists");
    EXPECT_TRUE(!backend_supports(&canvas, "bc4"),
                "BackendSupports reports bc4 unsupported until backend upload exists");
    EXPECT_TRUE(!backend_supports(&canvas, "bc5"),
                "BackendSupports reports bc5 unsupported until backend upload exists");
    EXPECT_TRUE(!backend_supports(&canvas, "bc7"),
                "BackendSupports reports bc7 unsupported until backend upload exists");
    EXPECT_TRUE(!backend_supports(&canvas, "astc"),
                "BackendSupports reports astc unsupported until backend upload exists");
    EXPECT_TRUE(!backend_supports(&canvas, "etc2"),
                "BackendSupports reports etc2 unsupported until backend upload exists");
    EXPECT_TRUE(!backend_supports(&canvas, "anisotropy"),
                "BackendSupports reports software anisotropy unsupported");
}

static void test_gpu_backend_capability_bits_and_names() {
    vgfx3d_backend_t fake_gpu_backend = make_fake_gpu_backend();
    rt_canvas3d canvas = {};
    canvas.backend = &fake_gpu_backend;

    int64_t caps = rt_canvas3d_get_backend_capabilities(&canvas);
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_GPU) != 0, "GPU backend advertises GPU mode");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SOFTWARE) == 0,
                "GPU backend does not advertise software mode");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_RENDER_TARGET) != 0,
                "GPU backend advertises render targets");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_WINDOW_READBACK) != 0,
                "GPU backend advertises window readback when hook exists");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SHADOWS) != 0,
                "GPU backend advertises shadows only when all hooks exist");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SKYBOX) != 0,
                "GPU backend advertises skybox when draw hook exists");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_HARDWARE_INSTANCING) != 0,
                "GPU backend advertises hardware instancing when hook exists");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_POSTFX) != 0,
                "GPU backend advertises post effects when present_postfx exists");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX) != 0,
                "GPU backend advertises GPU post effects when present_postfx exists");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_POSTFX_OVERLAY) != 0,
                "GPU backend advertises final overlay after split GPU postfx");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_FINAL_SCREENSHOT) != 0,
                "GPU backend advertises final screenshots when readback hook exists");
    EXPECT_TRUE(
        (caps & RT_CANVAS3D_BACKEND_CAP_GPU_POSTFX_OVERLAY) != 0,
        "GPU backend advertises GPU postfx overlay composition when split apply/present exists");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_HLOD) != 0,
                "GPU backend advertises runtime HLOD/impostor proxies");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_PBR) != 0,
                "GPU backend advertises material PBR workflow support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_NORMAL_MAPS) != 0,
                "GPU backend advertises normal-map material support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ALPHA_MASK) != 0,
                "GPU backend advertises alpha-mask material support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_MORPH_TARGETS) != 0,
                "GPU backend advertises morph-target support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_SKINNING) != 0,
                "GPU backend advertises skinning support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_TERRAIN_SPLAT) != 0,
                "GPU backend advertises terrain splat support");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC1) == 0,
                "generic GPU backend does not imply BC1 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC3) == 0,
                "generic GPU backend does not imply BC3 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC4) == 0,
                "generic GPU backend does not imply BC4 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC5) == 0,
                "generic GPU backend does not imply BC5 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC7) == 0,
                "generic GPU backend does not imply BC7 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ASTC) == 0,
                "generic GPU backend does not imply ASTC compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ETC2) == 0,
                "generic GPU backend does not imply ETC2 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ANISOTROPY) == 0,
                "generic GPU backend does not imply anisotropic filtering");

    EXPECT_TRUE(backend_supports(&canvas, "hardware_instancing"),
                "BackendSupports accepts hardware_instancing");
    EXPECT_TRUE(backend_supports(&canvas, "instancing"),
                "BackendSupports accepts instancing alias");
    EXPECT_TRUE(backend_supports(&canvas, "readback"), "BackendSupports accepts readback alias");
    EXPECT_TRUE(backend_supports(&canvas, "gpu_post_fx"),
                "BackendSupports accepts gpu_post_fx alias");
    EXPECT_TRUE(backend_supports(&canvas, "final-screenshot"),
                "BackendSupports accepts final-screenshot alias");
    EXPECT_TRUE(
        backend_supports(&canvas, "gpu-postfx-overlay"),
        "BackendSupports reports GPU postfx overlay support when split apply/present exists");
    EXPECT_TRUE(backend_supports(&canvas, "impostor"),
                "BackendSupports accepts impostor alias for HLOD proxies");
    EXPECT_TRUE(backend_supports(&canvas, "physically_based"),
                "BackendSupports accepts physically_based alias for PBR");
    EXPECT_TRUE(backend_supports(&canvas, "normalmap"), "BackendSupports accepts normalmap alias");
    EXPECT_TRUE(backend_supports(&canvas, "masked-alpha"),
                "BackendSupports accepts masked-alpha alias");
    EXPECT_TRUE(backend_supports(&canvas, "morphing"), "BackendSupports accepts morphing alias");
    EXPECT_TRUE(backend_supports(&canvas, "skeletal-animation"),
                "BackendSupports accepts skeletal-animation alias");
    EXPECT_TRUE(backend_supports(&canvas, "terrain_splatting"),
                "BackendSupports accepts terrain_splatting alias");
    EXPECT_TRUE(!backend_supports(&canvas, "bc1"),
                "BackendSupports accepts bc1 as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "bc3"),
                "BackendSupports accepts bc3 as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "bc4"),
                "BackendSupports accepts bc4 as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "bc5"),
                "BackendSupports accepts bc5 as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "bc7"),
                "BackendSupports accepts bc7 as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "astc"),
                "BackendSupports accepts astc as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "etc2"),
                "BackendSupports accepts etc2 as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "anisotropy"),
                "BackendSupports accepts anisotropy as a false capability name");
    EXPECT_TRUE(!backend_supports(&canvas, "missing_feature"),
                "BackendSupports rejects unknown capability names");
}

static void test_gpu_backend_native_texture_capability_hook() {
    vgfx3d_backend_t fake_gpu_backend = make_fake_gpu_backend();
    rt_canvas3d canvas = {};
    fake_gpu_backend.get_native_texture_caps = fake_native_texture_caps;
    canvas.backend = &fake_gpu_backend;

    int64_t caps = rt_canvas3d_get_backend_capabilities(&canvas);
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC1) != 0,
                "native texture hook advertises BC1 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC3) != 0,
                "native texture hook advertises BC3 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC4) != 0,
                "native texture hook advertises BC4 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC5) != 0,
                "native texture hook advertises BC5 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_BC7) != 0,
                "native texture hook advertises BC7 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ASTC) != 0,
                "native texture hook advertises ASTC compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ETC2) != 0,
                "native texture hook advertises ETC2 compressed upload");
    EXPECT_TRUE((caps & RT_CANVAS3D_BACKEND_CAP_ANISOTROPY) != 0,
                "native texture hook advertises anisotropic filtering");
    EXPECT_TRUE(backend_supports(&canvas, "bc1"), "BackendSupports reports hooked BC1 support");
    EXPECT_TRUE(backend_supports(&canvas, "bc3"), "BackendSupports reports hooked BC3 support");
    EXPECT_TRUE(backend_supports(&canvas, "bc4"), "BackendSupports reports hooked BC4 support");
    EXPECT_TRUE(backend_supports(&canvas, "bc5"), "BackendSupports reports hooked BC5 support");
    EXPECT_TRUE(backend_supports(&canvas, "bc7"), "BackendSupports reports hooked BC7 support");
    EXPECT_TRUE(backend_supports(&canvas, "astc"), "BackendSupports reports hooked ASTC support");
    EXPECT_TRUE(backend_supports(&canvas, "etc2"), "BackendSupports reports hooked ETC2 support");
    EXPECT_TRUE(backend_supports(&canvas, "anisotropic-filtering"),
                "BackendSupports reports hooked anisotropy support");
}

static void test_backend_runtime_fallback_is_queryable() {
    rt_canvas3d canvas = {};
    canvas.backend = &vgfx3d_software_backend;

    EXPECT_TRUE(!rt_canvas3d_get_backend_fallback(&canvas),
                "BackendFallback defaults false for a directly selected software backend");
    EXPECT_TRUE(!backend_supports(&canvas, "runtime-fallback"),
                "BackendSupports runtime-fallback defaults false");
    EXPECT_TRUE(!backend_supports(&canvas, "backend_fallback"),
                "BackendSupports backend_fallback alias defaults false");

    canvas.backend_requested_name = "opengl";
    canvas.backend_fallback = 1;
    EXPECT_TRUE(rt_canvas3d_get_backend_fallback(&canvas),
                "BackendFallback reports runtime software fallback");
    EXPECT_TRUE(backend_supports(&canvas, "runtime-fallback"),
                "BackendSupports accepts runtime-fallback alias");
    EXPECT_TRUE(backend_supports(&canvas, "backend_fallback"),
                "BackendSupports accepts backend_fallback alias");
    EXPECT_TRUE(backend_supports(&canvas, "software-fallback"),
                "BackendSupports accepts software-fallback alias");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_canvas3d_get_backend(&canvas)), "software") == 0,
                "Backend reports the active renderer after fallback");
}

static void test_frustum_culling_aliases_share_state() {
    rt_canvas3d canvas = {};

    rt_canvas3d_set_frustum_culling(&canvas, 1);
    EXPECT_EQ_I64(canvas.frustum_culling, 1, "SetFrustumCulling enables frustum culling state");
    EXPECT_EQ_I64(canvas.occlusion_culling, 0, "SetFrustumCulling does not enable occlusion");

    rt_canvas3d_set_occlusion_culling(&canvas, 0);
    EXPECT_EQ_I64(
        canvas.frustum_culling, 1, "SetOcclusionCulling(false) leaves frustum culling alone");
    EXPECT_EQ_I64(canvas.occlusion_culling, 0, "SetOcclusionCulling(false) disables occlusion");

    rt_canvas3d_set_occlusion_culling(&canvas, -1);
    EXPECT_EQ_I64(canvas.frustum_culling, 1, "SetOcclusionCulling leaves frustum culling alone");
    EXPECT_EQ_I64(canvas.occlusion_culling, 1, "SetOcclusionCulling enables occlusion");

    rt_canvas3d_set_frustum_culling(&canvas, 0);
    EXPECT_EQ_I64(canvas.frustum_culling, 0, "SetFrustumCulling(false) disables frustum state");
    EXPECT_EQ_I64(
        canvas.occlusion_culling, 1, "SetFrustumCulling(false) leaves occlusion culling alone");

    rt_canvas3d_set_occlusion_culling(&canvas, 0);
    EXPECT_EQ_I64(canvas.occlusion_culling, 0, "SetOcclusionCulling(false) disables occlusion");

    rt_canvas3d_set_frustum_culling(nullptr, 1);
    rt_canvas3d_set_occlusion_culling(nullptr, 1);
    EXPECT_TRUE(true, "culling setters tolerate null canvas handles");
}

static void test_canvas_render_state_sanitizes_inputs() {
    vgfx3d_backend_t fake_gpu_backend = make_fake_gpu_backend();
    vgfx3d_rendertarget_t dummy_target = {};
    rt_canvas3d canvas = {};
    canvas.backend = &fake_gpu_backend;
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.render_target = &dummy_target;

    rt_canvas3d_clear(&canvas, NAN, -1.0, 2.0);
    EXPECT_NEAR(g_clear_r, 0.0, 0.001, "Clear clamps NaN red to zero");
    EXPECT_NEAR(g_clear_g, 0.0, 0.001, "Clear clamps negative green to zero");
    EXPECT_NEAR(g_clear_b, 1.0, 0.001, "Clear clamps blue to one");

    rt_canvas3d_set_ambient(&canvas, NAN, -2.0, 3.0);
    EXPECT_NEAR(canvas.ambient[0], 0.0, 0.001, "Ambient clamps NaN to zero");
    EXPECT_NEAR(canvas.ambient[1], 0.0, 0.001, "Ambient clamps negative values");
    EXPECT_NEAR(canvas.ambient[2], 1.0, 0.001, "Ambient clamps high values");

    canvas.delta_time_ms = 1000;
    canvas.dt_max_ms = 16;
    EXPECT_EQ_I64(rt_canvas3d_get_delta_time(&canvas), 16, "Delta time getter applies max clamp");
}

static void test_software_backend_numeric_guards() {
    float b0 = -1.0f;
    float b1 = -1.0f;
    float b2 = -1.0f;
    uint8_t rgba[16] = {};

    EXPECT_NEAR(vgfx3d_software_backend_clamp01_for_test(NAN),
                0.0f,
                0.0f,
                "Software UNORM clamp maps NaN to zero");
    EXPECT_NEAR(vgfx3d_software_backend_clamp01_for_test(INFINITY),
                1.0f,
                0.0f,
                "Software UNORM clamp maps positive infinity to one");
    EXPECT_NEAR(vgfx3d_software_backend_material_pow_for_test(NAN, 2.0f),
                0.0f,
                0.0f,
                "Software material exponent rejects a NaN base");
    EXPECT_NEAR(vgfx3d_software_backend_material_pow_for_test(0.5f, NAN),
                0.5f,
                1e-6f,
                "Software material exponent substitutes a finite exponent");
    EXPECT_EQ_I64(vgfx3d_software_backend_wrap_index_for_test(
                      (int64_t)INT32_MAX - 1, INT32_MAX, RT_MATERIAL3D_TEXTURE_WRAP_REPEAT),
                  INT32_MAX - 1,
                  "Software repeat wrapping avoids signed addition overflow");
    EXPECT_EQ_I64(vgfx3d_software_backend_wrap_index_for_test(
                      -2, INT32_MAX, RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT),
                  1,
                  "Software mirrored wrapping remains correct for very large dimensions");
    EXPECT_EQ_I64(vgfx3d_software_backend_unit_coord_index_for_test(FLT_MAX, 7),
                  6,
                  "Software normalized coordinate conversion clamps huge finite values");
    EXPECT_TRUE(vgfx3d_software_backend_perspective_weights_for_test(
                    0.25f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, &b0, &b1, &b2),
                "Software perspective weights accept a valid projected triangle");
    EXPECT_NEAR(b0, 1.0f / 11.0f, 1e-6f, "Perspective correction weights vertex zero");
    EXPECT_NEAR(b1, 2.0f / 11.0f, 1e-6f, "Perspective correction weights vertex one");
    EXPECT_NEAR(b2, 8.0f / 11.0f, 1e-6f, "Perspective correction weights vertex two");
    EXPECT_TRUE(!vgfx3d_software_backend_perspective_weights_for_test(
                    0.25f, 0.25f, 0.5f, 1.0f, NAN, 4.0f, &b0, &b1, &b2),
                "Software perspective weights reject non-finite reciprocal W");
    EXPECT_TRUE(vgfx3d_software_backend_rgba_surface_valid_for_test(rgba, 2, 2, 8),
                "Software framebuffer layout accepts complete RGBA rows");
    EXPECT_TRUE(!vgfx3d_software_backend_rgba_surface_valid_for_test(rgba, 2, 2, 7),
                "Software framebuffer layout rejects a short row stride");
}

static void test_software_depth_probe_uses_active_render_target() {
    void *ctx = vgfx3d_software_backend.create_ctx(nullptr, 4, 4);
    void *target_obj = rt_rendertarget3d_new(2, 2);
    auto *target_owner = static_cast<rt_rendertarget3d *>(target_obj);
    vgfx3d_rendertarget_t *target = target_owner ? target_owner->target : nullptr;

    EXPECT_TRUE(ctx != nullptr && target != nullptr,
                "Software depth-probe render-target fixture allocates");
    if (!ctx || !target)
        goto cleanup;
    vgfx3d_software_backend.set_render_target(ctx, target);
    EXPECT_TRUE(target->depth_buf != nullptr, "Binding a software render target allocates depth");
    if (!target->depth_buf)
        goto cleanup;

    /* NDC (0, 0) maps to the lower-right sample of this 2x2 target. An NDC depth
     * of -0.5 converts to canonical window depth 0.25. */
    target->depth_buf[3] = -0.5f;
    {
        const int32_t slot = vgfx3d_software_backend.queue_depth_probe(ctx, 0.0f, 0.0f);
        EXPECT_TRUE(slot >= 0, "Software depth probe queues against an active render target");
        EXPECT_NEAR(vgfx3d_software_backend.read_depth_probe(ctx, slot),
                    0.25f,
                    1e-6f,
                    "Software depth probe samples the active RTT depth instead of window depth");
    }

cleanup:
    if (ctx) {
        vgfx3d_software_backend.set_render_target(ctx, nullptr);
        vgfx3d_software_backend.destroy_ctx(ctx);
    }
    release_obj(target_obj);
}

static int render_software_spot_light_shadow_scene(SoftwareSceneRenderResult *result) {
    const int32_t width = 96;
    const int32_t height = 96;
    rt_canvas3d canvas = {};
    void *target_obj = nullptr;
    void *camera = nullptr;
    void *eye = nullptr;
    void *look = nullptr;
    void *up = nullptr;
    void *light_pos = nullptr;
    void *light_dir = nullptr;
    void *spot = nullptr;
    void *plane = nullptr;
    void *sphere = nullptr;
    void *plane_mat = nullptr;
    void *sphere_mat = nullptr;
    double plane_model[16];
    double sphere_model[16];
    int32_t shadow_x = 0;
    int32_t shadow_y = 0;
    int32_t lit_left_x = 0;
    int32_t lit_left_y = 0;
    int32_t lit_right_x = 0;
    int32_t lit_right_y = 0;
    float shadow_luma;
    float lit_left_luma;
    float lit_right_luma;
    uint64_t hash = 0;
    rt_rendertarget3d *target_owner;
    vgfx3d_rendertarget_t *target;

    canvas.backend = &vgfx3d_software_backend;
    canvas.backend_ctx = vgfx3d_software_backend.create_ctx(nullptr, width, height);
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = width;
    canvas.height = height;
    canvas.framebuffer_width = width;
    canvas.framebuffer_height = height;
    canvas.shadow_cascade_count = 1;
    EXPECT_TRUE(canvas.backend_ctx != nullptr, "Software backend context is available");
    if (!canvas.backend_ctx)
        return 0;

    target_obj = rt_rendertarget3d_new(width, height);
    camera = rt_camera3d_new(55.0, 1.0, 0.1, 20.0);
    eye = rt_vec3_new(0.0, 2.4, 5.0);
    look = rt_vec3_new(0.0, 0.25, 0.0);
    up = rt_vec3_new(0.0, 1.0, 0.0);
    light_pos = rt_vec3_new(-3.0, 5.0, 4.0);
    light_dir = rt_vec3_new(3.0, -5.0, -4.0);
    spot = rt_light3d_new_spot(light_pos, light_dir, 1.0, 0.96, 0.88, 1.0 / 64.0, 18.0, 34.0);
    plane = rt_mesh3d_new_plane(6.0, 6.0);
    sphere = rt_mesh3d_new_sphere(0.55, 24);
    plane_mat = rt_material3d_new_color(0.76, 0.76, 0.72);
    sphere_mat = rt_material3d_new_color(0.82, 0.82, 0.86);

    EXPECT_TRUE(target_obj && camera && eye && look && up && light_pos && light_dir && spot &&
                    plane && sphere && plane_mat && sphere_mat,
                "Spot-shadow render test allocates scene resources");
    if (!target_obj || !camera || !eye || !look || !up || !light_pos || !light_dir || !spot ||
        !plane || !sphere || !plane_mat || !sphere_mat)
        goto cleanup;

    rt_camera3d_look_at(camera, eye, look, up);
    rt_light3d_set_intensity(spot, 5.5);
    rt_light3d_set_casts_shadows(spot, 1);
    rt_canvas3d_set_render_target(&canvas, target_obj);
    rt_canvas3d_enable_shadows(&canvas, 128);
    rt_canvas3d_set_shadow_bias(&canvas, 0.0012);
    rt_canvas3d_set_ambient(&canvas, 0.035, 0.035, 0.04);
    rt_canvas3d_set_light(&canvas, 0, spot);

    set_identity_matrix(plane_model);
    set_translation_matrix(sphere_model, 0.0, 0.65, 0.0);

    rt_canvas3d_clear(&canvas, 0.018, 0.020, 0.024);
    rt_canvas3d_begin(&canvas, camera);
    EXPECT_TRUE(
        project_world_to_pixel(&canvas, 0.45f, 0.0f, -0.55f, width, height, &shadow_x, &shadow_y),
        "Predicted spot-shadow core projects inside the render target");
    EXPECT_TRUE(project_world_to_pixel(
                    &canvas, -1.05f, 0.0f, -0.55f, width, height, &lit_left_x, &lit_left_y),
                "Lit plane sample before the penumbra projects inside the render target");
    EXPECT_TRUE(project_world_to_pixel(
                    &canvas, 1.45f, 0.0f, -0.55f, width, height, &lit_right_x, &lit_right_y),
                "Lit plane sample after the penumbra projects inside the render target");
    rt_canvas3d_draw_mesh_matrix(&canvas, plane, plane_model, plane_mat);
    rt_canvas3d_draw_mesh_matrix(&canvas, sphere, sphere_model, sphere_mat);
    rt_canvas3d_end(&canvas);

    target_owner = (rt_rendertarget3d *)target_obj;
    target = target_owner->target;
    hash = hash_render_target_rgba(target);
    shadow_luma = sample_luminance_box(target, shadow_x, shadow_y, 2);
    lit_left_luma = sample_luminance_box(target, lit_left_x, lit_left_y, 2);
    lit_right_luma = sample_luminance_box(target, lit_right_x, lit_right_y, 2);
    if (result) {
        result->hash = hash;
        result->shadow_luma = shadow_luma;
        result->lit_left_luma = lit_left_luma;
        result->lit_right_luma = lit_right_luma;
        result->worker_count = vgfx3d_software_backend_thread_count_for_test(canvas.backend_ctx);
        result->rgba.assign((size_t)width * (size_t)height * 4u, 0u);
        for (int32_t y = 0; y < height; y++) {
            std::memcpy(&result->rgba[(size_t)y * (size_t)width * 4u],
                        &target->color_buf[(size_t)y * (size_t)target->stride],
                        (size_t)width * 4u);
        }
    }

cleanup:
    if (canvas.in_frame)
        rt_canvas3d_end(&canvas);
    rt_canvas3d_clear_lights(&canvas);
    rt_canvas3d_disable_shadows(&canvas);
    rt_canvas3d_reset_render_target(&canvas);
    if (canvas.backend && canvas.backend_ctx && canvas.backend->destroy_ctx) {
        canvas.backend->destroy_ctx(canvas.backend_ctx);
        canvas.backend_ctx = nullptr;
    }
    release_obj(sphere_mat);
    release_obj(plane_mat);
    release_obj(sphere);
    release_obj(plane);
    release_obj(spot);
    release_obj(light_dir);
    release_obj(light_pos);
    release_obj(up);
    release_obj(look);
    release_obj(eye);
    release_obj(camera);
    release_obj(target_obj);
    return hash != 0;
}

static int rgba_equal(const SoftwareSceneRenderResult &a, const SoftwareSceneRenderResult &b) {
    return a.rgba.size() == b.rgba.size() &&
           (a.rgba.empty() || std::memcmp(a.rgba.data(), b.rgba.data(), a.rgba.size()) == 0);
}

static void test_software_spot_light_shadow_render_is_stable() {
    /* Rebaselined by the July 2026 backend audit after perspective-correct fragment/shadow
     * interpolation, per-slot shadow bias, and single-path lighting fixes. MSVC deliberately
     * uses the semantic assertions below plus the exact same-compiler cross-thread comparisons:
     * its permitted floating contraction has already produced multiple byte-exact images for
     * this software-only scene across toolset revisions. */
    const uint64_t expected_hash = 0xfd116402b1c198d8ull;
    SoftwareSceneRenderResult result;

    EXPECT_TRUE(render_software_spot_light_shadow_scene(&result),
                "Software spot-shadow render scene completes");
    if (result.hash == 0)
        return;

    EXPECT_TRUE(result.lit_left_luma > result.shadow_luma * 1.25f + 0.015f,
                "Spot shadow darkens the plane relative to the lit side before the penumbra");
    EXPECT_TRUE(result.lit_right_luma > result.shadow_luma * 1.25f + 0.015f,
                "Spot shadow darkens the plane relative to the lit side after the penumbra");
#if RT_COMPILER_MSVC
    EXPECT_TRUE(result.rgba.size() == (size_t)96 * 96 * 4,
                "Software spot-shadow render fills the complete MSVC target");
#else
    EXPECT_EQ_U64(
        result.hash, expected_hash, "Software spot-shadow render snapshot remains stable");
#endif
}

static void test_software_tiled_raster_threads_are_deterministic() {
    ScopedEnvVar threads_env("ZANNA_3D_SW_THREADS");
    SoftwareSceneRenderResult one;
    SoftwareSceneRenderResult four;
    SoftwareSceneRenderResult automatic;

    EXPECT_TRUE(threads_env.set("1"), "Test can force single-threaded software rasterization");
    EXPECT_TRUE(render_software_spot_light_shadow_scene(&one),
                "Single-threaded software raster render completes");
    if (one.hash == 0)
        return;
    EXPECT_EQ_I64(one.worker_count, 1, "ZANNA_3D_SW_THREADS=1 uses the serial worker count");

    EXPECT_TRUE(threads_env.set("4"), "Test can force four software raster workers");
    EXPECT_TRUE(render_software_spot_light_shadow_scene(&four),
                "Four-worker software raster render completes");
    if (four.hash == 0)
        return;
    EXPECT_EQ_I64(four.worker_count, 4, "ZANNA_3D_SW_THREADS=4 creates four workers");
    EXPECT_EQ_U64(four.hash, one.hash, "Four-worker software raster hash matches serial");
    EXPECT_TRUE(rgba_equal(one, four), "Four-worker software raster pixels match serial");

    EXPECT_TRUE(threads_env.unset(), "Test can restore unset software raster worker override");
    EXPECT_TRUE(render_software_spot_light_shadow_scene(&automatic),
                "Default software raster render completes");
    if (automatic.hash == 0)
        return;
    EXPECT_TRUE(automatic.worker_count >= 1 && automatic.worker_count <= 16,
                "Default software raster worker count is clamped to 1..16");
    EXPECT_EQ_U64(automatic.hash, one.hash, "Default software raster hash matches serial");
    EXPECT_TRUE(rgba_equal(one, automatic), "Default software raster pixels match serial");
}

static void test_render_target_notifies_native_cache_before_finalization() {
    RenderTargetReleaseProbe probe{};
    void *target_obj = rt_rendertarget3d_new(8, 6);
    EXPECT_TRUE(target_obj != nullptr, "RenderTarget3D release-hook fixture allocates");
    if (!target_obj)
        return;
    auto *owner = static_cast<rt_rendertarget3d *>(target_obj);
    owner->target->release_backend = render_target_release_probe;
    owner->target->release_backend_userdata = &probe;
    release_obj(target_obj);
    EXPECT_EQ_I64(probe.calls, 1, "RenderTarget3D finalization notifies its native cache once");
    EXPECT_TRUE(probe.saw_live_shell,
                "RenderTarget3D native-cache notification runs before shell fields are freed");
}

/// Build a solid-color square Pixels face for the IBL environment cubemap.
static void *ibl_test_face(int size, int64_t rgba) {
    void *p = rt_pixels_new(size, size);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            rt_pixels_set_rgba(p, x, y, rgba);
    return p;
}

/// End-to-end software render: a lightless PBR sphere under a top-lit IBL
/// environment must pick up directional ambient (bright top, dark bottom),
/// while the same scene without IBL stays black.
static void test_software_ibl_environment_lights_pbr_sphere() {
    const int32_t width = 96;
    const int32_t height = 96;
    rt_canvas3d canvas;
    std::memset(&canvas, 0, sizeof(canvas));

    canvas.backend = &vgfx3d_software_backend;
    canvas.backend_ctx = vgfx3d_software_backend.create_ctx(nullptr, width, height);
    canvas.gfx_win = (vgfx_window_t)1;
    canvas.width = width;
    canvas.height = height;
    canvas.framebuffer_width = width;
    canvas.framebuffer_height = height;
    EXPECT_TRUE(canvas.backend_ctx != nullptr, "Software backend context for IBL test");
    if (!canvas.backend_ctx)
        return;

    void *target_obj = rt_rendertarget3d_new(width, height);
    void *camera = rt_camera3d_new(55.0, 1.0, 0.1, 20.0);
    void *eye = rt_vec3_new(0.0, 0.0, 4.0);
    void *look = rt_vec3_new(0.0, 0.0, 0.0);
    void *up = rt_vec3_new(0.0, 1.0, 0.0);
    void *sphere = rt_mesh3d_new_sphere(1.0, 32);
    void *mat = rt_material3d_new_pbr(0.85, 0.85, 0.85);
    /* Top-lit environment: white +Y, black -Y, mid gray sides. */
    void *faces[6];
    for (int i = 0; i < 6; i++)
        faces[i] = ibl_test_face(16, 0x808080FF);
    faces[2] = ibl_test_face(16, 0xFFFFFFFFll); /* +Y */
    faces[3] = ibl_test_face(16, 0x000000FFll); /* -Y */
    void *sky = rt_cubemap3d_new(faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
    double model[16];
    int32_t top_x = 0;
    int32_t top_y = 0;
    int32_t bottom_x = 0;
    int32_t bottom_y = 0;

    EXPECT_TRUE(target_obj && camera && sphere && mat && sky, "IBL test scene resources");
    if (!target_obj || !camera || !sphere || !mat || !sky)
        return;

    rt_camera3d_look_at(camera, eye, look, up);
    rt_canvas3d_set_render_target(&canvas, target_obj);
    rt_canvas3d_set_ambient(&canvas, 0.0, 0.0, 0.0);
    set_identity_matrix(model);

    rt_rendertarget3d *target_owner = (rt_rendertarget3d *)target_obj;

    /* Frame A: no IBL — a lightless, zero-ambient PBR sphere renders black. */
    rt_canvas3d_clear(&canvas, 0.0, 0.0, 0.0);
    rt_canvas3d_begin(&canvas, camera);
    EXPECT_TRUE(project_world_to_pixel(&canvas, 0.0f, 0.72f, 0.65f, width, height, &top_x, &top_y),
                "Sphere upper-front point projects inside the IBL render target");
    EXPECT_TRUE(
        project_world_to_pixel(&canvas, 0.0f, -0.72f, 0.65f, width, height, &bottom_x, &bottom_y),
        "Sphere lower-front point projects inside the IBL render target");
    rt_canvas3d_draw_mesh_matrix(&canvas, sphere, model, mat);
    rt_canvas3d_end(&canvas);
    float dark_top = sample_luminance_box(target_owner->target, top_x, top_y, 2);
    float dark_bottom = sample_luminance_box(target_owner->target, bottom_x, bottom_y, 2);
    EXPECT_TRUE(dark_top < 0.03f && dark_bottom < 0.03f,
                "Without IBL a lightless zero-ambient PBR sphere stays black");

    /* Frame B: skybox + IBL — SH irradiance lights the top, bottom stays dark. */
    rt_canvas3d_set_skybox(&canvas, sky);
    rt_canvas3d_set_ibl_enabled(&canvas, 1);
    rt_canvas3d_set_ibl_intensity(&canvas, 1.0);
    EXPECT_TRUE(rt_canvas3d_get_ibl_enabled(&canvas) == 1, "IblEnabled reads back");
    EXPECT_TRUE(((rt_cubemap3d *)sky)->ibl_ready == 0,
                "Enabling IBL defers skybox payload work until the next frame");
    rt_canvas3d_clear(&canvas, 0.0, 0.0, 0.0);
    rt_canvas3d_begin(&canvas, camera);
    EXPECT_TRUE(((rt_cubemap3d *)sky)->ibl_ready == 1,
                "First IBL frame prepares the skybox payload lazily");
    rt_canvas3d_draw_mesh_matrix(&canvas, sphere, model, mat);
    rt_canvas3d_end(&canvas);
    float lit_top = sample_luminance_box(target_owner->target, top_x, top_y, 2);
    float lit_bottom = sample_luminance_box(target_owner->target, bottom_x, bottom_y, 2);
    EXPECT_TRUE(lit_top > 0.18f, "IBL lights the sphere's upward-facing surface");
    EXPECT_TRUE(lit_top > lit_bottom * 2.0f + 0.05f,
                "IBL irradiance is directional: top clearly brighter than bottom");
    EXPECT_TRUE(lit_top > dark_top + 0.15f, "IBL adds energy over the no-IBL frame");

    /* Intensity scales the contribution down. */
    rt_canvas3d_set_ibl_intensity(&canvas, 0.25);
    rt_canvas3d_clear(&canvas, 0.0, 0.0, 0.0);
    rt_canvas3d_begin(&canvas, camera);
    rt_canvas3d_draw_mesh_matrix(&canvas, sphere, model, mat);
    rt_canvas3d_end(&canvas);
    float dim_top = sample_luminance_box(target_owner->target, top_x, top_y, 2);
    EXPECT_TRUE(dim_top < lit_top - 0.05f, "IblIntensity scales the environment contribution");

    rt_canvas3d_clear_skybox(&canvas);
    rt_canvas3d_reset_render_target(&canvas);
    if (canvas.backend && canvas.backend_ctx && canvas.backend->destroy_ctx) {
        canvas.backend->destroy_ctx(canvas.backend_ctx);
        canvas.backend_ctx = nullptr;
    }
    release_obj(sky);
    for (int i = 0; i < 6; i++)
        release_obj(faces[i]);
    release_obj(mat);
    release_obj(sphere);
    release_obj(up);
    release_obj(look);
    release_obj(eye);
    release_obj(camera);
    release_obj(target_obj);
}

int main() {
    test_null_canvas_has_no_capabilities();
    test_software_backend_reports_canvas_fallback_features();
    test_gpu_backend_capability_bits_and_names();
    test_gpu_backend_native_texture_capability_hook();
    test_backend_runtime_fallback_is_queryable();
    test_frustum_culling_aliases_share_state();
    test_canvas_render_state_sanitizes_inputs();
    test_software_backend_numeric_guards();
    test_software_depth_probe_uses_active_render_target();
    test_render_target_notifies_native_cache_before_finalization();
    test_software_tiled_raster_threads_are_deterministic();
    test_software_spot_light_shadow_render_is_stable();
    test_software_ibl_environment_lights_pbr_sphere();

    if (tests_passed != tests_run) {
        std::fprintf(
            stderr, "test_rt_canvas3d_production: %d/%d checks passed\n", tests_passed, tests_run);
        return 1;
    }

    std::printf("test_rt_canvas3d_production: %d/%d checks passed\n", tests_passed, tests_run);
    return 0;
}
