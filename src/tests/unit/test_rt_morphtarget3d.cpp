//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_morphtarget3d.cpp
// Purpose: Unit tests for MorphTarget3D — shape creation, delta application,
//   weight blending, packed payloads, temporal state, and storage repair.
//
// Key invariants:
//   - Private allocation identities bound every mutable pointer/count mirror.
//   - No-op/implicit-zero edits do not allocate or invalidate GPU payloads.
// Ownership/Lifetime:
//   - Runtime fixtures are process-owned unless a test explicitly finalizes one.
// Links: rt_morphtarget3d.h, plans/3d/16-morph-targets.md
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_canvas3d.h"
#include "rt_internal.h"
#include "rt_morphtarget3d.h"
#include "rt_string.h"
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" {
extern rt_string rt_const_cstr(const char *s);
extern void *rt_mesh3d_new_box(double w, double h, double d);
extern void *rt_mesh3d_clone(void *obj);
extern void rt_mesh3d_clear(void *obj);
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
}

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (fabs((double)(a) - (double)(b)) > (eps)) {                                             \
            fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

struct MorphTarget3DShapeTest {
    char name[64];
    float *pos_deltas;
    float *nrm_deltas;
    float *tan_deltas;
    float *owned_pos_deltas;
    float *owned_nrm_deltas;
    float *owned_tan_deltas;
};

struct MorphTarget3DTestLayout {
    void *vptr;
    MorphTarget3DShapeTest *shapes;
    float *weights;
    float *prev_weights;
    float *motion_weight_snapshot;
    float *packed_pos_deltas;
    float *packed_nrm_deltas;
    uint64_t payload_generation;
    uint64_t max_delta_generation;
    double max_position_delta_cache;
    int32_t shape_count;
    int32_t shape_capacity;
    int32_t vertex_count;
    int64_t last_motion_frame;
    int32_t name_lookup_memo;
    int8_t has_prev_weights;
    int8_t packed_dirty;
    MorphTarget3DShapeTest *owned_shapes;
    float *owned_weights;
    float *owned_prev_weights;
    float *owned_motion_weight_snapshot;
    float *owned_packed_pos_deltas;
    float *owned_packed_nrm_deltas;
    int32_t allocation_shape_capacity;
    int32_t allocation_vertex_count;
    int32_t initialized_shape_count;
    int8_t motion_frame_initialized;
};

extern "C" {
int32_t vgfx3d_metal_clamp_morph_shape_count(uint32_t vertex_count, int32_t requested_shape_count);
int32_t vgfx3d_opengl_clamp_morph_shape_count(uint32_t vertex_count, int32_t requested_shape_count);
int32_t vgfx3d_d3d11_clamp_morph_shape_count(uint32_t vertex_count, int32_t requested_shape_count);
}

static void test_create() {
    void *mt = rt_morphtarget3d_new(10);
    EXPECT_TRUE(mt != nullptr, "MorphTarget3D.New returns non-null");
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 0, "Initial shape count = 0");
}

static void test_add_shape() {
    void *mt = rt_morphtarget3d_new(4);
    int64_t idx = rt_morphtarget3d_add_shape(mt, rt_const_cstr("smile"));
    EXPECT_TRUE(idx == 0, "First shape index = 0");
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 1, "Shape count = 1");

    int64_t idx2 = rt_morphtarget3d_add_shape(mt, rt_const_cstr("frown"));
    EXPECT_TRUE(idx2 == 1, "Second shape index = 1");
}

static void test_weight_zero() {
    void *mt = rt_morphtarget3d_new(4);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("test"));
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(mt, 0, 0.0);

    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0), 0.0, 0.001, "Weight = 0.0");
}

static void test_weight_set_get() {
    void *mt = rt_morphtarget3d_new(4);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("test"));
    rt_morphtarget3d_set_weight(mt, 0, 0.75);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0), 0.75, 0.001, "Weight = 0.75");
}

static void test_weight_by_name() {
    void *mt = rt_morphtarget3d_new(4);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("blink"));
    rt_morphtarget3d_set_weight_by_name(mt, rt_const_cstr("blink"), 0.5);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0), 0.5, 0.001, "SetWeightByName works");
}

static void test_weight_by_name_uses_canonical_long_names() {
    void *mt = rt_morphtarget3d_new(4);
    char long_name[128];
    std::memset(long_name, 'c', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';

    EXPECT_TRUE(rt_morphtarget3d_add_shape(mt, rt_const_cstr(long_name)) == 0,
                "MorphTarget3D accepts long shape names");
    rt_morphtarget3d_set_weight_by_name(mt, rt_const_cstr(long_name), 0.5);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0),
                0.5,
                0.001,
                "MorphTarget3D.SetWeightByName canonicalizes long names");
}

static void test_weight_by_name_clamps_like_indexed_set_weight() {
    void *mt = rt_morphtarget3d_new(4);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("blink"));
    rt_morphtarget3d_set_weight_by_name(mt, rt_const_cstr("blink"), 4.0);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0),
                1.0,
                0.001,
                "SetWeightByName clamps weights to the same unit range");
}

static void test_rejects_wrong_string_handles() {
    void *mt = rt_morphtarget3d_new(4);
    void *wrong_name = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_name);
    rt_string fake_name = reinterpret_cast<rt_string>(wrong_name);

    EXPECT_TRUE(rt_morphtarget3d_add_shape(mt, fake_name) == -1,
                "MorphTarget3D.AddShape rejects wrong-class string handles");
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 0,
                "fake shape name does not allocate a shape");
    EXPECT_TRUE(rt_morphtarget3d_add_shape(mt, rt_const_cstr("blink")) == 0,
                "valid shape fixture still inserts after rejecting fake names");
    rt_morphtarget3d_set_weight_by_name(mt, fake_name, 0.75);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0),
                0.0,
                0.001,
                "MorphTarget3D.SetWeightByName rejects wrong-class string handles");
    EXPECT_TRUE(rt_obj_release_check0(wrong_name) == 0,
                "MorphTarget3D fake-name guards do not release wrong-class handles");
    if (rt_obj_release_check0(wrong_name))
        rt_obj_free(wrong_name);
}

static void test_negative_weight() {
    void *mt = rt_morphtarget3d_new(4);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("test"));
    rt_morphtarget3d_set_weight(mt, 0, -0.5);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0), -0.5, 0.001, "Negative weight = -0.5");
}

/* Audit fix #9 — set_weight clamps to [-1, 1] so over-range values don't
 * silently over-extrude geometry past the target mesh. */
static void test_weight_clamped_to_unit_range() {
    void *mt = rt_morphtarget3d_new(4);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("test"));

    rt_morphtarget3d_set_weight(mt, 0, 2.5);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0), 1.0, 0.001, "Weight > 1.0 clamps to 1.0");

    rt_morphtarget3d_set_weight(mt, 0, -3.0);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0), -1.0, 0.001, "Weight < -1.0 clamps to -1.0");

    rt_morphtarget3d_set_weight(mt, 0, 0.5);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0), 0.5, 0.001, "In-range weight unchanged");
}

static void test_bounds_checks() {
    void *mt = rt_morphtarget3d_new(4);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("test"));

    /* Out-of-bounds shape index — should be no-op */
    rt_morphtarget3d_set_weight(mt, 5, 1.0);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 5), 0.0, 0.001, "Out of bounds returns 0");

    /* Out-of-bounds vertex — should be no-op */
    rt_morphtarget3d_set_delta(mt, 0, 100, 1.0, 2.0, 3.0); /* vertex 100 > 4 */
    EXPECT_TRUE(1, "Out-of-bounds vertex delta is no-op (no crash)");
}

static void test_null_safety() {
    rt_morphtarget3d_set_weight(NULL, 0, 1.0);
    EXPECT_NEAR(rt_morphtarget3d_get_weight(NULL, 0), 0.0, 0.001, "Null safety");
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(NULL) == 0, "Null shape count = 0");
}

static void test_packed_payload_generation_tracks_delta_edits_only() {
    void *mt = rt_morphtarget3d_new(2);
    uint64_t initial_generation = rt_morphtarget3d_get_payload_generation(mt);

    rt_morphtarget3d_add_shape(mt, rt_const_cstr("smile"));
    uint64_t after_shape_generation = rt_morphtarget3d_get_payload_generation(mt);
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    uint64_t after_delta_generation = rt_morphtarget3d_get_payload_generation(mt);
    rt_morphtarget3d_set_weight(mt, 0, 0.5);
    EXPECT_TRUE(after_shape_generation > initial_generation,
                "Adding a shape bumps the packed-payload generation");
    EXPECT_TRUE(after_delta_generation > after_shape_generation,
                "Editing morph deltas bumps the packed-payload generation");
    EXPECT_TRUE(rt_morphtarget3d_get_payload_generation(mt) == after_delta_generation,
                "Changing only morph weights does not bump the packed-payload generation");
}

static void test_payload_generation_repairs_zero_sentinel() {
    void *mt = rt_morphtarget3d_new(2);
    auto *bits = reinterpret_cast<MorphTarget3DTestLayout *>(mt);
    bits->payload_generation = 0;
    bits->max_delta_generation = 1;
    bits->max_position_delta_cache = 99.0;
    bits->packed_dirty = 0;

    EXPECT_TRUE(rt_morphtarget3d_get_payload_generation(mt) == 1,
                "MorphTarget3D repairs a corrupt zero payload generation");
    EXPECT_TRUE(bits->payload_generation == 1,
                "MorphTarget3D stores the repaired nonzero payload generation");
    EXPECT_TRUE(bits->packed_dirty == 1 && bits->max_delta_generation == 0,
                "Generation repair invalidates packed and maximum-delta caches");
}

static void test_packed_payload_exports_positions_and_normals() {
    void *mt = rt_morphtarget3d_new(2);
    const float *packed_pos;
    const float *packed_nrm;

    rt_morphtarget3d_add_shape(mt, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_delta(mt, 0, 1, 4.0, 5.0, 6.0);
    packed_pos = rt_morphtarget3d_get_packed_deltas(mt);
    EXPECT_TRUE(packed_pos != nullptr, "Packed morph positions export successfully");
    if (packed_pos) {
        EXPECT_NEAR(packed_pos[0], 1.0f, 1e-6f, "Packed morph positions keep vertex 0 X");
        EXPECT_NEAR(packed_pos[1], 2.0f, 1e-6f, "Packed morph positions keep vertex 0 Y");
        EXPECT_NEAR(packed_pos[2], 3.0f, 1e-6f, "Packed morph positions keep vertex 0 Z");
        EXPECT_NEAR(packed_pos[3], 4.0f, 1e-6f, "Packed morph positions keep vertex 1 X");
        EXPECT_NEAR(packed_pos[4], 5.0f, 1e-6f, "Packed morph positions keep vertex 1 Y");
        EXPECT_NEAR(packed_pos[5], 6.0f, 1e-6f, "Packed morph positions keep vertex 1 Z");
    }

    EXPECT_TRUE(rt_morphtarget3d_get_packed_normal_deltas(mt) == nullptr,
                "Packed morph normals stay null until normal deltas exist");

    rt_morphtarget3d_set_normal_delta(mt, 0, 0, 0.25, 0.5, 0.75);
    packed_nrm = rt_morphtarget3d_get_packed_normal_deltas(mt);
    EXPECT_TRUE(packed_nrm != nullptr, "Packed morph normals export successfully");
    if (packed_nrm) {
        EXPECT_NEAR(packed_nrm[0], 0.25f, 1e-6f, "Packed morph normals keep vertex 0 X");
        EXPECT_NEAR(packed_nrm[1], 0.5f, 1e-6f, "Packed morph normals keep vertex 0 Y");
        EXPECT_NEAR(packed_nrm[2], 0.75f, 1e-6f, "Packed morph normals keep vertex 0 Z");
        EXPECT_NEAR(packed_nrm[3], 0.0f, 1e-6f, "Packed morph normals zero-pad untouched vertices");
    }
}

static void test_add_shape_grows_beyond_32_entries() {
    void *mt = rt_morphtarget3d_new(1);
    int64_t last_index = -1;
    for (int i = 0; i < 40; i++)
        last_index = rt_morphtarget3d_add_shape(mt, rt_const_cstr("shape"));
    EXPECT_TRUE(last_index == 39, "MorphTarget3D.AddShape no longer traps at 32 shapes");
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 40,
                "MorphTarget3D tracks shape counts beyond the old 32-shape ceiling");
}

static void test_packed_payload_keeps_shapes_beyond_32() {
    void *mt = rt_morphtarget3d_new(1);
    const float *packed_pos;
    const float *packed_nrm;
    for (int i = 0; i < 33; i++)
        rt_morphtarget3d_add_shape(mt, rt_const_cstr("shape"));
    rt_morphtarget3d_set_delta(mt, 32, 0, 7.0, 8.0, 9.0);
    rt_morphtarget3d_set_normal_delta(mt, 32, 0, 0.25, 0.5, 0.75);
    packed_pos = rt_morphtarget3d_get_packed_deltas(mt);
    packed_nrm = rt_morphtarget3d_get_packed_normal_deltas(mt);
    EXPECT_TRUE(packed_pos != nullptr, "Packed morph positions rebuild for shapes beyond slot 31");
    EXPECT_TRUE(packed_nrm != nullptr, "Packed morph normals rebuild for shapes beyond slot 31");
    if (packed_pos) {
        size_t offset = (size_t)32 * 3u;
        EXPECT_NEAR(packed_pos[offset + 0], 7.0f, 1e-6f, "Packed payload keeps shape 32 X");
        EXPECT_NEAR(packed_pos[offset + 1], 8.0f, 1e-6f, "Packed payload keeps shape 32 Y");
        EXPECT_NEAR(packed_pos[offset + 2], 9.0f, 1e-6f, "Packed payload keeps shape 32 Z");
    }
    if (packed_nrm) {
        size_t offset = (size_t)32 * 3u;
        EXPECT_NEAR(packed_nrm[offset + 0], 0.25f, 1e-6f, "Packed normal payload keeps shape 32 X");
        EXPECT_NEAR(packed_nrm[offset + 1], 0.5f, 1e-6f, "Packed normal payload keeps shape 32 Y");
        EXPECT_NEAR(packed_nrm[offset + 2], 0.75f, 1e-6f, "Packed normal payload keeps shape 32 Z");
    }
}

static void test_gpu_morph_budget_is_64() {
    /* All three GPU backends budget 64 shader-side morph weights; requests past
     * the budget clamp (the dispatch layer routes such meshes to CPU blending).
     * ARKit-scale 52-blendshape rigs must stay inside the GPU budget. */
    EXPECT_TRUE(vgfx3d_metal_clamp_morph_shape_count(100, 52) == 52,
                "Metal keeps 52-shape rigs on the GPU path");
    EXPECT_TRUE(vgfx3d_metal_clamp_morph_shape_count(100, 128) == 64,
                "Metal clamps morph shapes at the 64-slot budget");
    EXPECT_TRUE(vgfx3d_opengl_clamp_morph_shape_count(100, 52) == 52,
                "OpenGL keeps 52-shape rigs on the GPU path");
    EXPECT_TRUE(vgfx3d_opengl_clamp_morph_shape_count(100, 128) == 64,
                "OpenGL clamps morph shapes at the 64-slot budget");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_morph_shape_count(100, 52) == 52,
                "D3D11 keeps 52-shape rigs on the GPU path");
    EXPECT_TRUE(vgfx3d_d3d11_clamp_morph_shape_count(100, 128) == 64,
                "D3D11 clamps morph shapes at the 64-slot budget");
}

static void test_clone_copies_delta_payloads_and_weights() {
    void *mt = rt_morphtarget3d_new(2);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_normal_delta(mt, 0, 0, 0.25, 0.5, 0.75);
    rt_morphtarget3d_set_weight(mt, 0, 0.6);

    void *clone = rt_morphtarget3d_clone(mt);
    EXPECT_TRUE(clone != nullptr, "MorphTarget3D.Clone returns a complete clone");
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(clone) == 1,
                "MorphTarget3D.Clone copies shape count");
    EXPECT_NEAR(
        rt_morphtarget3d_get_weight(clone, 0), 0.6, 0.001, "MorphTarget3D.Clone copies weights");

    const float *clone_pos = rt_morphtarget3d_get_packed_deltas(clone);
    const float *clone_nrm = rt_morphtarget3d_get_packed_normal_deltas(clone);
    EXPECT_TRUE(clone_pos != nullptr, "MorphTarget3D.Clone copies position deltas");
    EXPECT_TRUE(clone_nrm != nullptr, "MorphTarget3D.Clone copies normal deltas");
    if (clone_pos)
        EXPECT_NEAR(clone_pos[0], 1.0f, 1e-6f, "Clone keeps position delta X");
    if (clone_nrm)
        EXPECT_NEAR(clone_nrm[2], 0.75f, 1e-6f, "Clone keeps normal delta Z");
}

static void test_shape_count_repair_ignores_unallocated_slots() {
    void *mt = rt_morphtarget3d_new(2);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("raise"));
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    rt_morphtarget3d_set_weight(mt, 0, 0.5);

    auto *bits = reinterpret_cast<MorphTarget3DTestLayout *>(mt);
    bits->shape_count = INT32_MAX;
    bits->shape_capacity = 1;

    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 1,
                "MorphTarget3D clamps a corrupted shape count to live shapes");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 0),
                0.5,
                0.001,
                "MorphTarget3D keeps live shape weight after count repair");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mt, 1),
                0.0,
                0.001,
                "MorphTarget3D hides unallocated slots after count repair");

    const float *packed = rt_morphtarget3d_get_packed_deltas(mt);
    EXPECT_TRUE(packed != nullptr, "MorphTarget3D packs only live shapes after count repair");
    if (packed) {
        EXPECT_NEAR(packed[0], 1.0f, 1e-6f, "Repaired packed payload keeps vertex 0 X");
        EXPECT_NEAR(packed[3], 0.0f, 1e-6f, "Repaired packed payload stops after live shape");
    }

    void *clone = rt_morphtarget3d_clone(mt);
    EXPECT_TRUE(clone != nullptr, "MorphTarget3D.Clone succeeds after source count repair");
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(clone) == 1,
                "MorphTarget3D.Clone copies only live shapes after count repair");
}

static void test_max_position_delta_handles_large_finite_values() {
    void *mt = rt_morphtarget3d_new(1);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("huge"));
    rt_morphtarget3d_set_delta(mt, 0, 0, std::numeric_limits<float>::max(), 0.0, 0.0);

    double max_delta = rt_morphtarget3d_get_max_position_delta(mt);
    EXPECT_TRUE(std::isfinite(max_delta) && max_delta > 1.0e38,
                "MorphTarget3D max delta uses overflow-safe length for huge finite deltas");
}

static void test_noop_delta_edits_preserve_sparse_storage_and_payload_generation() {
    void *mt = rt_morphtarget3d_new(2);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("idle"));
    auto *bits = reinterpret_cast<MorphTarget3DTestLayout *>(mt);
    auto *shape = &bits->owned_shapes[0];
    uint64_t initial_generation = rt_morphtarget3d_get_payload_generation(mt);

    rt_morphtarget3d_set_delta(mt, 0, 0, 0.0, 0.0, 0.0);
    rt_morphtarget3d_set_normal_delta(mt, 0, 0, 0.0, 0.0, 0.0);
    rt_morphtarget3d_set_tangent_delta(mt, 0, 0, 0.0, 0.0, 0.0);
    EXPECT_TRUE(rt_morphtarget3d_get_payload_generation(mt) == initial_generation,
                "Implicit-zero edits do not invalidate an unchanged packed payload");
    EXPECT_TRUE(shape->owned_nrm_deltas == nullptr && shape->owned_tan_deltas == nullptr,
                "Implicit-zero normal/tangent edits retain sparse channel storage");
    EXPECT_TRUE(rt_morphtarget3d_get_packed_normal_deltas(mt) == nullptr &&
                    rt_morphtarget3d_has_tangent_deltas(mt) == 0,
                "Implicit-zero edits do not force GPU normals or the CPU tangent path");

    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    uint64_t changed_generation = rt_morphtarget3d_get_payload_generation(mt);
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    EXPECT_TRUE(rt_morphtarget3d_get_payload_generation(mt) == changed_generation,
                "Repeating an identical position edit does not invalidate the payload");
    rt_morphtarget3d_set_normal_delta(mt, 0, 0, 0.25, 0.5, 0.75);
    uint64_t normal_generation = rt_morphtarget3d_get_payload_generation(mt);
    rt_morphtarget3d_set_normal_delta(mt, 0, 0, 0.25, 0.5, 0.75);
    EXPECT_TRUE(rt_morphtarget3d_get_payload_generation(mt) == normal_generation,
                "Repeating an identical normal edit does not invalidate the payload");
}

static void test_owned_storage_repairs_pointer_count_vertex_and_name_corruption() {
    void *mt = rt_morphtarget3d_new(2);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("first"));
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("second"));
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 2.0, 3.0);
    const float *initial_packed = rt_morphtarget3d_get_packed_deltas(mt);
    auto *bits = reinterpret_cast<MorphTarget3DTestLayout *>(mt);
    MorphTarget3DShapeTest foreign_shapes[1] = {};
    float foreign_weights[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    float foreign_deltas[6] = {8.0f, 8.0f, 8.0f, 8.0f, 8.0f, 8.0f};

    bits->shapes = foreign_shapes;
    bits->weights = foreign_weights;
    bits->prev_weights = foreign_weights;
    bits->motion_weight_snapshot = foreign_weights;
    bits->packed_pos_deltas = foreign_deltas;
    bits->packed_nrm_deltas = foreign_deltas;
    bits->shape_count = INT32_MAX;
    bits->shape_capacity = INT32_MAX;
    bits->vertex_count = INT32_MAX;
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 2,
                "MorphTarget3D derives the live shape prefix from allocation authority");
    EXPECT_TRUE(bits->shapes == bits->owned_shapes && bits->weights == bits->owned_weights &&
                    bits->prev_weights == bits->owned_prev_weights &&
                    bits->motion_weight_snapshot == bits->owned_motion_weight_snapshot,
                "MorphTarget3D restores all four aligned top-level array mirrors");
    EXPECT_TRUE(bits->shape_capacity == bits->allocation_shape_capacity &&
                    bits->vertex_count == bits->allocation_vertex_count,
                "MorphTarget3D restores mutable capacity and vertex metadata");
    bits->shape_count = 0;
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 2,
                "MorphTarget3D restores a corrupt downward shape count without losing shapes");

    auto *shape = &bits->owned_shapes[0];
    std::memset(shape->name, 'n', sizeof(shape->name));
    shape->pos_deltas = foreign_deltas;
    shape->nrm_deltas = foreign_deltas;
    shape->tan_deltas = foreign_deltas;
    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mt) == 2,
                "MorphTarget3D repairs per-shape channel mirrors before traversal");
    EXPECT_TRUE(shape->pos_deltas == shape->owned_pos_deltas &&
                    shape->nrm_deltas == shape->owned_nrm_deltas &&
                    shape->tan_deltas == shape->owned_tan_deltas,
                "MorphTarget3D follows only owned per-shape channel identities");
    EXPECT_TRUE(shape->name[63] == '\0',
                "MorphTarget3D repairs unterminated fixed-size shape names");
    EXPECT_TRUE(rt_morphtarget3d_get_packed_deltas(mt) == initial_packed,
                "MorphTarget3D restores the owned packed payload mirror when still clean");

    bits->vertex_count = INT32_MAX;
    void *clone = rt_morphtarget3d_clone(mt);
    EXPECT_TRUE(clone != nullptr,
                "MorphTarget3D.Clone repairs source vertex metadata before sizing copies");
    if (clone) {
        auto *clone_bits = reinterpret_cast<MorphTarget3DTestLayout *>(clone);
        EXPECT_TRUE(clone_bits->allocation_vertex_count == 2,
                    "MorphTarget3D.Clone retains the real allocation vertex count");
    }
}

static void test_morphtarget_finalizer_uses_owned_identities_only() {
    void *mt = rt_morphtarget3d_new(1);
    rt_morphtarget3d_add_shape(mt, rt_const_cstr("owned"));
    rt_morphtarget3d_set_delta(mt, 0, 0, 1.0, 0.0, 0.0);
    (void)rt_morphtarget3d_get_packed_deltas(mt);
    auto *bits = reinterpret_cast<MorphTarget3DTestLayout *>(mt);
    MorphTarget3DShapeTest foreign_shapes[1] = {};
    float foreign_storage[3] = {11.0f, 12.0f, 13.0f};
    bits->owned_shapes[0].pos_deltas = foreign_storage;
    bits->shapes = foreign_shapes;
    bits->weights = foreign_storage;
    bits->prev_weights = foreign_storage;
    bits->motion_weight_snapshot = foreign_storage;
    bits->packed_pos_deltas = foreign_storage;
    bits->packed_nrm_deltas = foreign_storage;
    if (rt_obj_release_check0(mt))
        rt_obj_free(mt);
    EXPECT_NEAR(foreign_storage[0],
                11.0,
                0.0,
                "MorphTarget3D finalization never frees or mutates corrupt pointer mirrors");
}

namespace {
static int g_morph_release_count = 0;
} // namespace

extern "C" void tracked_morph_finalizer(void *obj) {
    (void)obj;
    g_morph_release_count++;
}

static void test_mesh_clone_deep_copy_releases_original_morph_target_on_clear() {
    void *mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *mt = rt_morphtarget3d_new(24);
    void *clone;
    assert(mesh != nullptr && mt != nullptr);

    rt_morphtarget3d_add_shape(mt, rt_const_cstr("raise"));
    g_morph_release_count = 0;
    rt_obj_set_finalizer(mt, tracked_morph_finalizer);

    rt_mesh3d_set_morph_targets(mesh, mt);
    clone = rt_mesh3d_clone(mesh);
    EXPECT_TRUE(clone != nullptr, "Mesh3D.Clone succeeds with attached morph targets");

    if (rt_obj_release_check0(mt))
        rt_obj_free(mt);
    EXPECT_TRUE(g_morph_release_count == 0,
                "Mesh3D retains attached morph targets across user-side releases");

    rt_mesh3d_clear(mesh);
    EXPECT_TRUE(g_morph_release_count == 1,
                "Clearing the source mesh releases its original morph target after deep clone");

    if (rt_obj_release_check0(clone))
        rt_obj_free(clone);
    EXPECT_TRUE(g_morph_release_count == 1,
                "Destroying the clone does not release the source morph target a second time");
}

int main() {
    test_create();
    test_gpu_morph_budget_is_64();
    test_add_shape();
    test_weight_zero();
    test_weight_set_get();
    test_weight_by_name();
    test_weight_by_name_uses_canonical_long_names();
    test_weight_by_name_clamps_like_indexed_set_weight();
    test_rejects_wrong_string_handles();
    test_negative_weight();
    test_weight_clamped_to_unit_range();
    test_bounds_checks();
    test_null_safety();
    test_packed_payload_generation_tracks_delta_edits_only();
    test_payload_generation_repairs_zero_sentinel();
    test_packed_payload_exports_positions_and_normals();
    test_add_shape_grows_beyond_32_entries();
    test_packed_payload_keeps_shapes_beyond_32();
    test_clone_copies_delta_payloads_and_weights();
    test_shape_count_repair_ignores_unallocated_slots();
    test_max_position_delta_handles_large_finite_values();
    test_noop_delta_edits_preserve_sparse_storage_and_payload_generation();
    test_owned_storage_repairs_pointer_count_vertex_and_name_corruption();
    test_morphtarget_finalizer_uses_owned_identities_only();
    test_mesh_clone_deep_copy_releases_original_morph_target_on_clear();

    printf("MorphTarget3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
