//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_mesh_simplify.cpp
// Purpose: Focused regressions for topology-safe Mesh3D simplification, complete/partial
//   diagnostics, and exact remapping of retained vertex/animation/material side streams.
//
// Key invariants:
//   - Simplification uses subset placement and never invents an attribute payload.
//   - Complete and valid partial results report exact requested/achieved triangle counts.
//   - Non-manifold, bow-tie, duplicate, degenerate, and inverted collapse outcomes are rejected.
//
// Ownership/Lifetime:
//   - Every runtime object created by a case is released through the ordinary object runtime.
//   - Test-only private layout views are read-only and used solely to inspect remapped channels.
//
// Links: rt_mesh_simplify.c, rt_morphtarget3d.c,
//   docs/adr/0173-graphics3d-transactional-hardening-and-retained-work.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

extern "C" {
#include "rt_canvas3d_internal.h"
#include "rt_mat4.h"
#include "rt_mesh_simplify.h"
#include "rt_morphtarget3d.h"
#include "rt_object.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define EXPECT_TRUE(condition, message)                                                            \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL: %s\n", message);                                           \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(actual, expected, message)                                                       \
    do {                                                                                           \
        const long long actual_value = static_cast<long long>(actual);                             \
        const long long expected_value = static_cast<long long>(expected);                         \
        ++g_checks;                                                                                \
        if (actual_value != expected_value) {                                                      \
            ++g_failures;                                                                          \
            std::fprintf(stderr,                                                                   \
                         "FAIL: %s (got %lld, expected %lld)\n",                                   \
                         message,                                                                  \
                         actual_value,                                                             \
                         expected_value);                                                          \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(actual, expected, epsilon, message)                                            \
    do {                                                                                           \
        const double actual_value = static_cast<double>(actual);                                   \
        const double expected_value = static_cast<double>(expected);                               \
        ++g_checks;                                                                                \
        if (!std::isfinite(actual_value) ||                                                        \
            std::fabs(actual_value - expected_value) > static_cast<double>(epsilon)) {             \
            ++g_failures;                                                                          \
            std::fprintf(stderr,                                                                   \
                         "FAIL: %s (got %.9g, expected %.9g)\n",                                   \
                         message,                                                                  \
                         actual_value,                                                             \
                         expected_value);                                                          \
        }                                                                                          \
    } while (0)

/// @brief Fail the focused executable if a runtime operation unexpectedly traps.
/// @details Simplification fixtures are valid by construction, so reaching this embedding hook is
///   always a test failure. The process abort keeps the original trap site visible to CTest.
/// @param message Borrowed diagnostic text supplied by the runtime, or NULL.
extern "C" void vm_trap(const char *message) {
    std::fprintf(stderr, "unexpected runtime trap: %s\n", message ? message : "(null)");
    std::abort();
}

/** @brief Test-only mirror of one private MorphTarget3D shape record. */
struct TestMorphShape {
    char name[64];
    float *pos_deltas;
    float *nrm_deltas;
    float *tan_deltas;
};

/**
 * @brief Read-only test mirror of MorphTarget3D's private payload.
 *
 * Product modules use the owning API rather than this layout. The focused regression needs direct
 * access only because tangent deltas intentionally have no public packed GPU getter: their
 * presence routes morphing through the CPU path.
 */
struct TestMorphLayout {
    void *vptr;
    TestMorphShape *shapes;
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
};

/**
 * @brief Release one test-owned runtime object reference.
 * @param object Runtime payload pointer; `nullptr` is accepted.
 */
static void release_object(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/**
 * @brief Create an exact-size Mesh3D from position and triangle arrays.
 *
 * Every fixed vertex channel receives a deterministic source-index marker so subset-placement
 * output can be mapped back to its source without relying on geometric coordinate equality.
 *
 * @param positions Flat XYZ source coordinates.
 * @param indices Triangle-list indices.
 * @return New runtime-owned Mesh3D, or `nullptr` on allocation failure.
 */
static rt_mesh3d *make_mesh(const std::vector<float> &positions,
                            const std::vector<uint32_t> &indices) {
    const uint32_t vertex_count = static_cast<uint32_t>(positions.size() / 3u);
    auto *mesh = static_cast<rt_mesh3d *>(rt_mesh3d_new_empty_storage());
    if (!mesh || positions.size() % 3u != 0u || indices.size() % 3u != 0u)
        return mesh;
    mesh->vertices =
        static_cast<vgfx3d_vertex_t *>(std::calloc(vertex_count, sizeof(vgfx3d_vertex_t)));
    mesh->indices = static_cast<uint32_t *>(std::malloc(indices.size() * sizeof(uint32_t)));
    if (!mesh->vertices || !mesh->indices) {
        release_object(mesh);
        return nullptr;
    }
    mesh->vertex_count = vertex_count;
    mesh->vertex_capacity = vertex_count;
    mesh->index_count = static_cast<uint32_t>(indices.size());
    mesh->index_capacity = mesh->index_count;
    std::memcpy(mesh->indices, indices.data(), indices.size() * sizeof(uint32_t));
    for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
        vgfx3d_vertex_t &record = mesh->vertices[vertex];
        record.pos[0] = positions[(size_t)vertex * 3u + 0u];
        record.pos[1] = positions[(size_t)vertex * 3u + 1u];
        record.pos[2] = positions[(size_t)vertex * 3u + 2u];
        record.normal[0] = 0.1f + static_cast<float>(vertex) * 0.01f;
        record.normal[1] = 0.2f + static_cast<float>(vertex) * 0.01f;
        record.normal[2] = 0.9f;
        record.uv[0] = static_cast<float>(vertex) * 0.07f;
        record.uv[1] = static_cast<float>(vertex) * 0.09f;
        record.uv1[0] = static_cast<float>(vertex) * 0.11f;
        record.uv1[1] = static_cast<float>(vertex) * 0.13f;
        record.color[0] = static_cast<float>(vertex + 1u) / 32.0f;
        record.color[1] = 0.25f;
        record.color[2] = 0.5f;
        record.color[3] = 1.0f;
        record.tangent[0] = 0.8f;
        record.tangent[1] = static_cast<float>(vertex) * 0.01f;
        record.tangent[2] = 0.1f;
        record.tangent[3] = (vertex & 1u) ? -1.0f : 1.0f;
        record.bone_indices[0] = 0;
        record.bone_weights[0] = 1.0f;
    }
    rt_mesh3d_touch_geometry(mesh);
    return mesh;
}

/**
 * @brief Return the source vertex represented by an exact subset output record.
 * @param source Original mesh.
 * @param output_vertex Simplified fixed vertex record.
 * @return Source index, or `UINT32_MAX` when the simplifier invented/modified attributes.
 */
static uint32_t source_vertex_for_record(const rt_mesh3d *source,
                                         const vgfx3d_vertex_t &output_vertex) {
    for (uint32_t vertex = 0; vertex < source->vertex_count; ++vertex) {
        if (std::memcmp(&source->vertices[vertex], &output_vertex, sizeof(output_vertex)) == 0)
            return vertex;
    }
    return UINT32_MAX;
}

/**
 * @brief Compute a unit face normal from one mesh's current index/position data.
 * @param mesh Mesh containing @p face.
 * @param face Triangle ordinal.
 * @param out_normal Receives the unit normal.
 * @return Non-zero for a finite non-degenerate triangle.
 */
static bool face_normal(const rt_mesh3d *mesh, uint32_t face, double out_normal[3]) {
    const uint32_t i0 = mesh->indices[(size_t)face * 3u + 0u];
    const uint32_t i1 = mesh->indices[(size_t)face * 3u + 1u];
    const uint32_t i2 = mesh->indices[(size_t)face * 3u + 2u];
    const float *p0 = mesh->vertices[i0].pos;
    const float *p1 = mesh->vertices[i1].pos;
    const float *p2 = mesh->vertices[i2].pos;
    const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    out_normal[0] = e1[1] * e2[2] - e1[2] * e2[1];
    out_normal[1] = e1[2] * e2[0] - e1[0] * e2[2];
    out_normal[2] = e1[0] * e2[1] - e1[1] * e2[0];
    const double length = std::hypot(std::hypot(out_normal[0], out_normal[1]), out_normal[2]);
    if (!std::isfinite(length) || length <= 1e-12)
        return false;
    for (int axis = 0; axis < 3; ++axis)
        out_normal[axis] /= length;
    return true;
}

/**
 * @brief Validate index bounds, non-degenerate triangles, and duplicate-free output.
 * @param mesh Simplified mesh to inspect.
 */
static void expect_valid_triangle_output(const rt_mesh3d *mesh) {
    EXPECT_TRUE(mesh != nullptr, "simplifier returns a mesh");
    if (!mesh)
        return;
    EXPECT_TRUE(mesh->index_count % 3u == 0u, "output index count is triangle aligned");
    std::vector<std::array<uint32_t, 3>> canonical;
    for (uint32_t index = 0; index < mesh->index_count; ++index)
        EXPECT_TRUE(mesh->indices[index] < mesh->vertex_count, "output index is in range");
    for (uint32_t face = 0; face < mesh->index_count / 3u; ++face) {
        double normal[3];
        EXPECT_TRUE(face_normal(mesh, face, normal), "output triangle is non-degenerate");
        std::array<uint32_t, 3> tri = {mesh->indices[(size_t)face * 3u + 0u],
                                       mesh->indices[(size_t)face * 3u + 1u],
                                       mesh->indices[(size_t)face * 3u + 2u]};
        std::sort(tri.begin(), tri.end());
        EXPECT_TRUE(std::find(canonical.begin(), canonical.end(), tri) == canonical.end(),
                    "output contains no duplicate unordered face");
        canonical.push_back(tri);
    }
}

/**
 * @brief Exercise a real collapse while checking every retained/remapped side stream.
 */
static void test_attribute_animation_and_range_remap() {
    const std::vector<float> positions = {
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        -1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        -1.0f,
        0.0f,
        -1.0f,
        0.0f,
    };
    const std::vector<uint32_t> indices = {
        0, 2, 1, 0, 3, 2, 0, 4, 3, 0, 1, 4, 5, 1, 2, 5, 2, 3, 5, 3, 4, 5, 4, 1,
    };
    rt_mesh3d *source = make_mesh(positions, indices);
    EXPECT_TRUE(source != nullptr, "attribute-rich octahedron allocates");
    if (!source)
        return;

    source->positions64 =
        static_cast<double *>(std::malloc((size_t)source->vertex_count * 3u * sizeof(double)));
    source->extra_influences = static_cast<vgfx3d_extra_influences_t *>(
        std::calloc(source->vertex_count, sizeof(vgfx3d_extra_influences_t)));
    source->submesh_ranges = static_cast<rt_mesh3d_submesh_range *>(
        std::calloc(source->index_count / 3u, sizeof(rt_mesh3d_submesh_range)));
    EXPECT_TRUE(source->positions64 && source->extra_influences && source->submesh_ranges,
                "attribute-rich side streams allocate");
    if (!source->positions64 || !source->extra_influences || !source->submesh_ranges) {
        release_object(source);
        return;
    }
    source->submesh_range_count = source->index_count / 3u;
    source->submesh_range_capacity = source->submesh_range_count;
    for (uint32_t vertex = 0; vertex < source->vertex_count; ++vertex) {
        for (int axis = 0; axis < 3; ++axis) {
            source->positions64[(size_t)vertex * 3u + (size_t)axis] =
                static_cast<double>(source->vertices[vertex].pos[axis]) +
                static_cast<double>(vertex + 1u) * 1e-9;
        }
        source->extra_influences[vertex].indices[0] = static_cast<uint16_t>(vertex + 20u);
        source->extra_influences[vertex].weights[0] = 0.125f + vertex * 0.01f;
    }
    for (uint32_t face = 0; face < source->index_count / 3u; ++face) {
        source->submesh_ranges[face] = {face * 3u, 3u, static_cast<int32_t>(100u + face)};
    }
    source->compact_streams = 1;

    void *skeleton = rt_skeleton3d_new();
    void *identity = rt_mat4_identity();
    rt_string root_name = rt_const_cstr("root");
    EXPECT_EQ(rt_skeleton3d_add_bone(skeleton, root_name, -1, identity),
              0,
              "test skeleton receives one root bone");
    rt_string_unref(root_name);
    release_object(identity);
    rt_mesh3d_set_skeleton(source, skeleton);
    source->bone_map = static_cast<int32_t *>(std::malloc(sizeof(int32_t)));
    EXPECT_TRUE(source->bone_map != nullptr, "source bone map allocates");
    if (source->bone_map)
        source->bone_map[0] = 0;

    void *morph = rt_morphtarget3d_new(source->vertex_count);
    rt_string shape_name = rt_const_cstr("pulse");
    const int64_t shape = rt_morphtarget3d_add_shape(morph, shape_name);
    rt_string_unref(shape_name);
    EXPECT_EQ(shape, 0, "test morph receives one shape");
    for (uint32_t vertex = 0; vertex < source->vertex_count; ++vertex) {
        const double base = static_cast<double>(vertex + 1u);
        rt_morphtarget3d_set_delta(morph, shape, vertex, base, base + 0.1, base + 0.2);
        rt_morphtarget3d_set_normal_delta(morph, shape, vertex, base + 1.0, base + 1.1, base + 1.2);
        rt_morphtarget3d_set_tangent_delta(
            morph, shape, vertex, base + 2.0, base + 2.1, base + 2.2);
    }
    rt_morphtarget3d_set_weight(morph, shape, 0.75);
    rt_mesh3d_set_morph_targets(source, morph);
    source->tangents_ready = 1;
    source->tangent_revision = source->geometry_revision;
    release_object(skeleton);
    release_object(morph);

    rt_mesh3d *output = static_cast<rt_mesh3d *>(rt_mesh3d_simplify(source, 6));
    expect_valid_triangle_output(output);
    if (!output) {
        release_object(source);
        return;
    }
    EXPECT_EQ(rt_mesh3d_get_simplify_requested_triangles(output),
              6,
              "requested triangle diagnostic is exact");
    EXPECT_EQ(rt_mesh3d_get_simplify_achieved_triangles(output),
              output->index_count / 3u,
              "achieved triangle diagnostic matches the output buffer");
    EXPECT_TRUE(rt_mesh3d_get_simplify_status(output) == RT_MESH3D_SIMPLIFY_STATUS_COMPLETE ||
                    rt_mesh3d_get_simplify_status(output) == RT_MESH3D_SIMPLIFY_STATUS_PARTIAL,
                "simplified output carries an explicit completion status");
    EXPECT_TRUE(output->vertex_count < source->vertex_count,
                "octahedron simplification performs at least one legal collapse");
    EXPECT_TRUE(output->positions64 != nullptr, "double-position stream survives simplification");
    EXPECT_TRUE(output->extra_influences != nullptr,
                "influences 5-8 stream survives simplification");
    EXPECT_TRUE(output->bone_map != nullptr && output->bone_map[0] == 0,
                "bone palette remap survives simplification");
    EXPECT_TRUE(output->skeleton_ref == source->skeleton_ref,
                "simplified mesh retains the source skeleton object");
    EXPECT_TRUE(output->morph_targets_ref != nullptr &&
                    output->morph_targets_ref != source->morph_targets_ref,
                "simplified mesh owns an independently remapped morph container");
    EXPECT_EQ(output->compact_streams, 1, "compact-stream preference survives simplification");
    EXPECT_EQ(output->tangents_ready, 1, "authored tangent readiness survives simplification");

    const float *output_pos_deltas = rt_morphtarget3d_get_packed_deltas(output->morph_targets_ref);
    const float *output_nrm_deltas =
        rt_morphtarget3d_get_packed_normal_deltas(output->morph_targets_ref);
    auto *output_morph = static_cast<TestMorphLayout *>(output->morph_targets_ref);
    EXPECT_EQ(output_morph->vertex_count,
              output->vertex_count,
              "remapped morph vertex count matches simplified mesh");
    EXPECT_TRUE(output_morph->shapes && output_morph->shapes[0].tan_deltas,
                "optional morph tangent channel survives remapping");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(output->morph_targets_ref, 0),
                0.75,
                1e-6,
                "morph weight survives remapping");
    for (uint32_t output_vertex = 0; output_vertex < output->vertex_count; ++output_vertex) {
        const uint32_t source_vertex =
            source_vertex_for_record(source, output->vertices[output_vertex]);
        EXPECT_TRUE(source_vertex != UINT32_MAX,
                    "simplification preserves an exact fixed vertex record");
        if (source_vertex == UINT32_MAX)
            continue;
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_NEAR(output->positions64[(size_t)output_vertex * 3u + (size_t)axis],
                        source->positions64[(size_t)source_vertex * 3u + (size_t)axis],
                        1e-15,
                        "authoritative double position follows the subset map");
            EXPECT_NEAR(output_pos_deltas[(size_t)output_vertex * 3u + (size_t)axis],
                        static_cast<double>(source_vertex + 1u) + axis * 0.1,
                        1e-5,
                        "morph position delta follows the subset map");
            EXPECT_NEAR(output_nrm_deltas[(size_t)output_vertex * 3u + (size_t)axis],
                        static_cast<double>(source_vertex + 2u) + axis * 0.1,
                        1e-5,
                        "morph normal delta follows the subset map");
            EXPECT_NEAR(
                output_morph->shapes[0].tan_deltas[(size_t)output_vertex * 3u + (size_t)axis],
                static_cast<double>(source_vertex + 3u) + axis * 0.1,
                1e-5,
                "morph tangent delta follows the subset map");
        }
        EXPECT_EQ(output->extra_influences[output_vertex].indices[0],
                  source_vertex + 20u,
                  "extra joint index follows the subset map");
        EXPECT_NEAR(output->extra_influences[output_vertex].weights[0],
                    0.125 + source_vertex * 0.01,
                    1e-6,
                    "extra joint weight follows the subset map");
    }

    for (uint32_t range_index = 0; range_index < output->submesh_range_count; ++range_index) {
        const rt_mesh3d_submesh_range &range = output->submesh_ranges[range_index];
        EXPECT_TRUE(range.first_index % 3u == 0u && range.index_count % 3u == 0u,
                    "simplified material range remains triangle aligned");
        EXPECT_TRUE((uint64_t)range.first_index + range.index_count <= output->index_count,
                    "simplified material range remains inside the output index buffer");
        EXPECT_TRUE(range.material_slot >= 100 && range.material_slot < 108,
                    "simplified material range retains its source slot");
        for (uint32_t output_face = range.first_index / 3u;
             output_face < (range.first_index + range.index_count) / 3u;
             ++output_face) {
            const uint32_t source_face = static_cast<uint32_t>(range.material_slot - 100);
            double source_normal[3];
            double output_normal[3];
            EXPECT_TRUE(face_normal(source, source_face, source_normal) &&
                            face_normal(output, output_face, output_normal),
                        "source/output material face normals are finite");
            const double dot = source_normal[0] * output_normal[0] +
                               source_normal[1] * output_normal[1] +
                               source_normal[2] * output_normal[2];
            EXPECT_TRUE(dot > 0.0, "surviving material face never inverts");
        }
    }

    void *retained_skeleton = output->skeleton_ref;
    release_object(source);
    EXPECT_TRUE(rt_g3d_has_class(retained_skeleton, RT_G3D_SKELETON3D_CLASS_ID),
                "output keeps the skeleton alive after the source is released");
    EXPECT_EQ(rt_morphtarget3d_get_shape_count(output->morph_targets_ref),
              1,
              "output keeps its remapped morph alive after source release");
    release_object(output);
}

/**
 * @brief Verify exact partial diagnostics and material-range coalescing on an unreachable target.
 */
static void test_partial_status_duplicate_guard_and_range_coalescing() {
    const std::vector<float> positions = {
        1.0f,
        1.0f,
        1.0f,
        -1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        1.0f,
        -1.0f,
        -1.0f,
    };
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2};
    rt_mesh3d *source = make_mesh(positions, indices);
    EXPECT_TRUE(source != nullptr, "tetrahedron allocates");
    if (!source)
        return;
    source->submesh_ranges =
        static_cast<rt_mesh3d_submesh_range *>(std::malloc(3u * sizeof(rt_mesh3d_submesh_range)));
    source->submesh_range_count = source->submesh_range_capacity = 3;
    source->submesh_ranges[0] = {0u, 3u, 7};
    source->submesh_ranges[1] = {3u, 3u, 7};
    source->submesh_ranges[2] = {6u, 6u, 8};

    rt_mesh3d *partial = static_cast<rt_mesh3d *>(rt_mesh3d_simplify(source, 1));
    expect_valid_triangle_output(partial);
    EXPECT_EQ(rt_mesh3d_get_simplify_requested_triangles(partial),
              1,
              "unreachable request remains exact");
    EXPECT_EQ(rt_mesh3d_get_simplify_achieved_triangles(partial),
              4,
              "tetrahedron duplicate guard reports exact achieved count");
    EXPECT_EQ(rt_mesh3d_get_simplify_status(partial),
              RT_MESH3D_SIMPLIFY_STATUS_PARTIAL,
              "unreachable topology returns valid partial status");
    EXPECT_EQ(
        partial->submesh_range_count, 2, "adjacent same-material source spans coalesce in output");
    EXPECT_EQ(
        partial->submesh_ranges[0].first_index, 0, "coalesced first material range starts at zero");
    EXPECT_EQ(partial->submesh_ranges[0].index_count,
              6,
              "coalesced first material range covers two faces");
    EXPECT_EQ(partial->submesh_ranges[0].material_slot,
              7,
              "coalesced first material range preserves its slot");

    rt_mesh3d *complete = static_cast<rt_mesh3d *>(rt_mesh3d_simplify(source, 100));
    expect_valid_triangle_output(complete);
    EXPECT_EQ(rt_mesh3d_get_simplify_requested_triangles(complete),
              100,
              "no-op simplification records the caller's target");
    EXPECT_EQ(rt_mesh3d_get_simplify_achieved_triangles(complete),
              4,
              "no-op simplification reports its exact source triangle count");
    EXPECT_EQ(rt_mesh3d_get_simplify_status(complete),
              RT_MESH3D_SIMPLIFY_STATUS_COMPLETE,
              "budget above source count is complete");
    rt_mesh3d_touch_geometry(complete);
    EXPECT_EQ(rt_mesh3d_get_simplify_status(complete),
              RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN,
              "later geometry mutation invalidates stale simplification status");
    EXPECT_EQ(rt_mesh3d_get_simplify_requested_triangles(complete),
              0,
              "later geometry mutation clears the stale requested diagnostic");

    release_object(complete);
    release_object(partial);
    release_object(source);
}

/**
 * @brief Verify non-manifold edges and bow-tie center vertices cannot be collapsed.
 */
static void test_nonmanifold_and_bowtie_guards() {
    const std::vector<float> nonmanifold_positions = {
        -1.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        -1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    const std::vector<uint32_t> nonmanifold_indices = {0, 1, 2, 1, 0, 3, 0, 1, 4};
    rt_mesh3d *nonmanifold = make_mesh(nonmanifold_positions, nonmanifold_indices);
    rt_mesh3d *nonmanifold_output = static_cast<rt_mesh3d *>(rt_mesh3d_simplify(nonmanifold, 1));
    expect_valid_triangle_output(nonmanifold_output);
    EXPECT_EQ(rt_mesh3d_get_simplify_achieved_triangles(nonmanifold_output),
              3,
              "three-face non-manifold edge is left intact");
    EXPECT_EQ(rt_mesh3d_get_simplify_status(nonmanifold_output),
              RT_MESH3D_SIMPLIFY_STATUS_PARTIAL,
              "non-manifold fixture reports partial rather than corrupting topology");

    const std::vector<float> bowtie_positions = {
        0.0f,
        0.0f,
        0.0f,
        -2.0f,
        0.0f,
        0.0f,
        -1.0f,
        1.0f,
        0.0f,
        2.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
    };
    const std::vector<uint32_t> bowtie_indices = {0, 1, 2, 0, 3, 4};
    rt_mesh3d *bowtie = make_mesh(bowtie_positions, bowtie_indices);
    rt_mesh3d *bowtie_output = static_cast<rt_mesh3d *>(rt_mesh3d_simplify(bowtie, 1));
    expect_valid_triangle_output(bowtie_output);
    bool center_survived = false;
    for (uint32_t vertex = 0; bowtie_output && vertex < bowtie_output->vertex_count; ++vertex) {
        center_survived |= source_vertex_for_record(bowtie, bowtie_output->vertices[vertex]) == 0u;
    }
    EXPECT_TRUE(center_survived, "disconnected bow-tie center is never selected for collapse");

    const std::vector<float> duplicate_positions = {
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
    };
    const std::vector<uint32_t> duplicate_indices = {0, 1, 2, 0, 1, 2};
    rt_mesh3d *duplicate = make_mesh(duplicate_positions, duplicate_indices);
    rt_mesh3d *duplicate_output = static_cast<rt_mesh3d *>(rt_mesh3d_simplify(duplicate, 1));
    EXPECT_TRUE(duplicate_output != nullptr, "duplicate-face fixture returns a valid partial mesh");
    EXPECT_EQ(rt_mesh3d_get_simplify_achieved_triangles(duplicate_output),
              2,
              "duplicate source faces are not collapsed into invalid topology");
    EXPECT_EQ(rt_mesh3d_get_simplify_status(duplicate_output),
              RT_MESH3D_SIMPLIFY_STATUS_PARTIAL,
              "duplicate-face fixture reports partial");

    release_object(duplicate_output);
    release_object(duplicate);
    release_object(bowtie_output);
    release_object(bowtie);
    release_object(nonmanifold_output);
    release_object(nonmanifold);
}

/**
 * @brief Build a planar two-chart grid whose shared column is duplicated per chart.
 *
 * Models a UV-atlas seam after exact-record welding: each chart indexes its own
 * copy of the seam column (records differ through the index-keyed attribute
 * markers), so the seam is a pair of open boundary polylines. All faces are
 * wound counter-clockwise for a +Z normal.
 *
 * @param out_seam_sources Receives the source indices of both seam copies.
 * @return New runtime-owned mesh, or `nullptr` on allocation failure.
 */
static rt_mesh3d *make_two_chart_seam_grid(std::vector<uint32_t> &out_seam_sources) {
    const uint32_t cols = 4;
    const uint32_t rows = 4;
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    out_seam_sources.clear();
    for (uint32_t chart = 0; chart < 2; ++chart) {
        const float x0 = chart == 0 ? 0.0f : 3.0f;
        for (uint32_t y = 0; y < rows; ++y) {
            for (uint32_t x = 0; x < cols; ++x) {
                positions.push_back(x0 + static_cast<float>(x));
                positions.push_back(static_cast<float>(y));
                positions.push_back(0.0f);
            }
        }
    }
    for (uint32_t y = 0; y < rows; ++y) {
        out_seam_sources.push_back(y * cols + (cols - 1u)); /* chart A, x = 3 */
        out_seam_sources.push_back(rows * cols + y * cols); /* chart B, x = 3 */
    }
    for (uint32_t chart = 0; chart < 2; ++chart) {
        const uint32_t base = chart * rows * cols;
        for (uint32_t y = 0; y + 1u < rows; ++y) {
            for (uint32_t x = 0; x + 1u < cols; ++x) {
                const uint32_t v00 = base + y * cols + x;
                const uint32_t v10 = v00 + 1u;
                const uint32_t v01 = v00 + cols;
                const uint32_t v11 = v01 + 1u;
                indices.insert(indices.end(), {v00, v10, v11});
                indices.insert(indices.end(), {v00, v11, v01});
            }
        }
    }
    return make_mesh(positions, indices);
}

/**
 * @brief Seam locking keeps both copies of every boundary polyline vertex verbatim.
 */
static void test_seam_lock_preserves_boundary_polylines() {
    std::vector<uint32_t> seam_sources;
    rt_mesh3d *source = make_two_chart_seam_grid(seam_sources);
    EXPECT_TRUE(source != nullptr, "two-chart seam grid allocates");
    if (!source)
        return;
    const int64_t source_tris = static_cast<int64_t>(source->index_count / 3u);

    rt_mesh3d *locked = static_cast<rt_mesh3d *>(
        rt_mesh3d_simplify_ex(source, 4, RT_MESH3D_SIMPLIFY_FLAG_LOCK_BOUNDARIES, 0.0));
    EXPECT_TRUE(locked != nullptr, "seam-locked simplify returns a mesh");
    if (locked) {
        expect_valid_triangle_output(locked);
        EXPECT_EQ(rt_mesh3d_get_simplify_status(locked),
                  RT_MESH3D_SIMPLIFY_STATUS_PARTIAL,
                  "boundary-dominated grid reports partial under seam locking");
        EXPECT_TRUE(rt_mesh3d_get_simplify_achieved_triangles(locked) > 4,
                    "seam locking refuses to shatter down to the budget");
        EXPECT_TRUE(rt_mesh3d_get_simplify_achieved_triangles(locked) < source_tris,
                    "seam locking still decimates chart interiors");
        for (uint32_t seam_index = 0; seam_index < seam_sources.size(); ++seam_index) {
            const vgfx3d_vertex_t &record = source->vertices[seam_sources[seam_index]];
            bool found = false;
            for (uint32_t vertex = 0; vertex < locked->vertex_count && !found; ++vertex)
                found = std::memcmp(&locked->vertices[vertex], &record, sizeof(record)) == 0;
            EXPECT_TRUE(found, "both copies of every seam vertex survive seam locking");
        }
        for (uint32_t face = 0; face < locked->index_count / 3u; ++face) {
            double normal[3];
            if (face_normal(locked, face, normal))
                EXPECT_TRUE(normal[2] > 0.0, "seam-locked planar output keeps +Z orientation");
        }
    }

    rt_mesh3d *unlocked = static_cast<rt_mesh3d *>(rt_mesh3d_simplify(source, 4));
    EXPECT_TRUE(unlocked != nullptr, "legacy simplify still returns a mesh");
    if (unlocked) {
        expect_valid_triangle_output(unlocked);
        for (uint32_t face = 0; face < unlocked->index_count / 3u; ++face) {
            double normal[3];
            if (face_normal(unlocked, face, normal))
                EXPECT_TRUE(normal[2] > 0.0, "flip guard keeps planar output facing +Z");
        }
        if (locked)
            EXPECT_TRUE(rt_mesh3d_get_simplify_achieved_triangles(unlocked) <=
                            rt_mesh3d_get_simplify_achieved_triangles(locked),
                        "legacy path decimates at least as far as the locked path");
    }

    release_object(unlocked);
    release_object(locked);
    release_object(source);
}

/**
 * @brief A tight error ceiling stops collapsing early with an honest PARTIAL result.
 */
static void test_error_cap_stops_early() {
    const uint32_t cols = 5;
    const uint32_t rows = 5;
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = 0; x < cols; ++x) {
            positions.push_back(static_cast<float>(x));
            positions.push_back(static_cast<float>(y));
            positions.push_back(static_cast<float>((x * x * 3u + y * y * 5u + x * y * 7u) % 11u) * 0.05f);
        }
    }
    for (uint32_t y = 0; y + 1u < rows; ++y) {
        for (uint32_t x = 0; x + 1u < cols; ++x) {
            const uint32_t v00 = y * cols + x;
            const uint32_t v10 = v00 + 1u;
            const uint32_t v01 = v00 + cols;
            const uint32_t v11 = v01 + 1u;
            indices.insert(indices.end(), {v00, v10, v11});
            indices.insert(indices.end(), {v00, v11, v01});
        }
    }
    rt_mesh3d *source = make_mesh(positions, indices);
    EXPECT_TRUE(source != nullptr, "bumpy grid allocates");
    if (!source)
        return;
    const int64_t source_tris = static_cast<int64_t>(source->index_count / 3u);

    rt_mesh3d *capped = static_cast<rt_mesh3d *>(rt_mesh3d_simplify_ex(source, 2, 0, 1e-7));
    EXPECT_TRUE(capped != nullptr, "tightly capped simplify returns a mesh");
    if (capped) {
        expect_valid_triangle_output(capped);
        EXPECT_EQ(rt_mesh3d_get_simplify_status(capped),
                  RT_MESH3D_SIMPLIFY_STATUS_PARTIAL,
                  "a tight error ceiling reports partial");
        EXPECT_EQ(rt_mesh3d_get_simplify_requested_triangles(capped),
                  2,
                  "the capped result still records the requested budget");
        EXPECT_TRUE(rt_mesh3d_get_simplify_achieved_triangles(capped) >= source_tris - 4,
                    "a tight error ceiling permits at most a few zero-cost collapses");
    }

    rt_mesh3d *roomy = static_cast<rt_mesh3d *>(rt_mesh3d_simplify_ex(source, 2, 0, 0.9));
    EXPECT_TRUE(roomy != nullptr, "roomy capped simplify returns a mesh");
    if (roomy) {
        expect_valid_triangle_output(roomy);
        EXPECT_TRUE(rt_mesh3d_get_simplify_achieved_triangles(roomy) < source_tris,
                    "a roomy error ceiling permits real decimation");
        if (capped)
            EXPECT_TRUE(rt_mesh3d_get_simplify_achieved_triangles(roomy) <=
                            rt_mesh3d_get_simplify_achieved_triangles(capped),
                        "the ceiling only ever stops work earlier");
    }

    release_object(roomy);
    release_object(capped);
    release_object(source);
}

/**
 * @brief Run all focused simplifier regressions.
 * @return Process success when every assertion passed.
 */
int main() {
    test_attribute_animation_and_range_remap();
    test_partial_status_duplicate_guard_and_range_coalescing();
    test_nonmanifold_and_bowtie_guards();
    test_seam_lock_preserves_boundary_polylines();
    test_error_cap_stops_early();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
