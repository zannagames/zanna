//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_quickhull3d.cpp
// Purpose: Property tests for the from-scratch quickhull used by physics
//   collider reduction: exact hull extraction (interior points removed),
//   containment of the input cloud, outward face orientation, degenerate
//   input rejection, and farthest-point reduction behavior.
// Key invariants:
//   - Cube-with-interior-noise reduces to exactly the 8 corners.
//   - Every input point lies on or inside the produced hull (plane epsilon).
//   - Face normals point away from the hull centroid.
// Ownership/Lifetime: outputs from rt_quickhull3d_build are free()'d locally.
// Links: src/runtime/graphics/3d/physics/rt_quickhull3d.c
//
//===----------------------------------------------------------------------===//

#include "../TestHarness.hpp"

extern "C" {
#include "rt_quickhull3d.h"
}

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

/// @brief Deterministic xorshift so the "random" cloud is identical everywhere.
uint64_t rng_state = 0x9E3779B97F4A7C15ull;

double next_unit() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state % 1000000ull) / 1000000.0;
}

/// @brief Max signed distance of any input point outside any hull face.
double max_outside_distance(const std::vector<double> &cloud,
                            const double *verts,
                            const int32_t *idx,
                            int32_t index_count) {
    double worst = -1e30;
    for (int32_t f = 0; f < index_count; f += 3) {
        const double *a = verts + idx[f] * 3;
        const double *b = verts + idx[f + 1] * 3;
        const double *c = verts + idx[f + 2] * 3;
        double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        double e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                       e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0]};
        double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 0.0)
            continue;
        double off = (n[0] * a[0] + n[1] * a[1] + n[2] * a[2]) / len;
        for (size_t p = 0; p + 2 < cloud.size(); p += 3) {
            double d = (n[0] * cloud[p] + n[1] * cloud[p + 1] + n[2] * cloud[p + 2]) / len - off;
            if (d > worst)
                worst = d;
        }
    }
    return worst;
}

} // namespace

TEST(Quickhull3D, CubeWithInteriorNoiseYieldsEightCorners) {
    std::vector<double> cloud;
    for (int x = 0; x <= 1; x++)
        for (int y = 0; y <= 1; y++)
            for (int z = 0; z <= 1; z++) {
                cloud.push_back(x ? 1.0 : -1.0);
                cloud.push_back(y ? 1.0 : -1.0);
                cloud.push_back(z ? 1.0 : -1.0);
            }
    for (int i = 0; i < 500; i++) {
        cloud.push_back(next_unit() * 1.8 - 0.9);
        cloud.push_back(next_unit() * 1.8 - 0.9);
        cloud.push_back(next_unit() * 1.8 - 0.9);
    }

    double *verts = nullptr;
    int32_t vert_count = 0;
    int32_t *idx = nullptr;
    int32_t index_count = 0;
    ASSERT_TRUE(
        rt_quickhull3d_build(
            cloud.data(), (int32_t)(cloud.size() / 3), &verts, &vert_count, &idx, &index_count) ==
        1);
    EXPECT_EQ(vert_count, 8);
    EXPECT_EQ(index_count, 36); // 12 triangles for a cube hull

    // Containment: no input point may lie meaningfully outside any face.
    EXPECT_TRUE(max_outside_distance(cloud, verts, idx, index_count) < 1e-7);

    free(verts);
    free(idx);
}

TEST(Quickhull3D, RandomCloudHullContainsEveryPoint) {
    std::vector<double> cloud;
    for (int i = 0; i < 800; i++) {
        cloud.push_back(next_unit() * 4.0 - 2.0);
        cloud.push_back(next_unit() * 2.0 - 1.0);
        cloud.push_back(next_unit() * 6.0 - 3.0);
    }
    double *verts = nullptr;
    int32_t vert_count = 0;
    int32_t *idx = nullptr;
    int32_t index_count = 0;
    ASSERT_TRUE(
        rt_quickhull3d_build(
            cloud.data(), (int32_t)(cloud.size() / 3), &verts, &vert_count, &idx, &index_count) ==
        1);
    EXPECT_GT(vert_count, 3);
    EXPECT_LT(vert_count, 800);
    EXPECT_TRUE(max_outside_distance(cloud, verts, idx, index_count) < 1e-7);
    free(verts);
    free(idx);
}

TEST(Quickhull3D, DegenerateInputsFailCleanly) {
    // Coplanar square (flat cloud).
    double flat[12] = {0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1};
    double *verts = nullptr;
    int32_t vert_count = 0;
    EXPECT_EQ(rt_quickhull3d_build(flat, 4, &verts, &vert_count, nullptr, nullptr), 0);
    EXPECT_TRUE(verts == nullptr);

    // Collinear points.
    double line[12] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3};
    EXPECT_EQ(rt_quickhull3d_build(line, 4, &verts, &vert_count, nullptr, nullptr), 0);

    // Too few points.
    EXPECT_EQ(rt_quickhull3d_build(flat, 3, &verts, &vert_count, nullptr, nullptr), 0);
}

TEST(Quickhull3D, ExtremeFiniteCoordinatesDoNotOverflowConstruction) {
    const double extent = std::numeric_limits<double>::max() / 4.0;
    std::vector<double> cloud;
    for (int x = 0; x <= 1; ++x)
        for (int y = 0; y <= 1; ++y)
            for (int z = 0; z <= 1; ++z) {
                cloud.push_back(x ? extent : -extent);
                cloud.push_back(y ? extent : -extent);
                cloud.push_back(z ? extent : -extent);
            }
    cloud.insert(cloud.end(), {0.0, 0.0, 0.0, extent / 2.0, 0.0, 0.0});

    double *verts = nullptr;
    int32_t vert_count = 0;
    int32_t *idx = nullptr;
    int32_t index_count = 0;
    ASSERT_TRUE(rt_quickhull3d_build(cloud.data(),
                                     static_cast<int32_t>(cloud.size() / 3u),
                                     &verts,
                                     &vert_count,
                                     &idx,
                                     &index_count) == 1);
    EXPECT_EQ(vert_count, 8);
    EXPECT_EQ(index_count, 36);
    for (int32_t index = 0; index < vert_count * 3; ++index)
        EXPECT_TRUE(std::isfinite(verts[index]));
    free(verts);
    free(idx);
}

TEST(Quickhull3D, LargeConflictCloudBuildsDeterministically) {
    std::vector<double> cloud;
    cloud.reserve(4096u * 3u);
    for (int index = 0; index < 4096; ++index) {
        const double u = next_unit() * 2.0 - 1.0;
        const double angle = next_unit() * 6.283185307179586;
        const double radius = std::sqrt(std::fmax(0.0, 1.0 - u * u));
        cloud.push_back(radius * std::cos(angle));
        cloud.push_back(radius * std::sin(angle));
        cloud.push_back(u);
    }
    double *first_vertices = nullptr;
    double *second_vertices = nullptr;
    int32_t first_vertex_count = 0;
    int32_t second_vertex_count = 0;
    int32_t *first_indices = nullptr;
    int32_t *second_indices = nullptr;
    int32_t first_index_count = 0;
    int32_t second_index_count = 0;
    ASSERT_TRUE(rt_quickhull3d_build(cloud.data(),
                                     static_cast<int32_t>(cloud.size() / 3u),
                                     &first_vertices,
                                     &first_vertex_count,
                                     &first_indices,
                                     &first_index_count) == 1);
    ASSERT_TRUE(rt_quickhull3d_build(cloud.data(),
                                     static_cast<int32_t>(cloud.size() / 3u),
                                     &second_vertices,
                                     &second_vertex_count,
                                     &second_indices,
                                     &second_index_count) == 1);
    EXPECT_EQ(first_vertex_count, second_vertex_count);
    EXPECT_EQ(first_index_count, second_index_count);
    EXPECT_TRUE(std::memcmp(first_vertices,
                            second_vertices,
                            static_cast<size_t>(first_vertex_count) * 3u * sizeof(double)) == 0);
    EXPECT_TRUE(std::memcmp(first_indices,
                            second_indices,
                            static_cast<size_t>(first_index_count) * sizeof(int32_t)) == 0);
    free(first_vertices);
    free(second_vertices);
    free(first_indices);
    free(second_indices);
}

TEST(Quickhull3D, ReductionKeepsAxisExtremesAndBudget) {
    std::vector<double> cloud;
    for (int i = 0; i < 300; i++) {
        cloud.push_back(next_unit() * 10.0 - 5.0);
        cloud.push_back(next_unit() * 10.0 - 5.0);
        cloud.push_back(next_unit() * 10.0 - 5.0);
    }
    // Plant known extremes.
    double extremes[6][3] = {
        {-20, 0, 0}, {20, 0, 0}, {0, -20, 0}, {0, 20, 0}, {0, 0, -20}, {0, 0, 20}};
    for (auto &e : extremes) {
        cloud.push_back(e[0]);
        cloud.push_back(e[1]);
        cloud.push_back(e[2]);
    }

    std::vector<double> reduced(32 * 3);
    int32_t count =
        rt_quickhull3d_reduce(cloud.data(), (int32_t)(cloud.size() / 3), 32, reduced.data());
    EXPECT_EQ(count, 32);

    // Every planted extreme must survive the reduction (seeded selection).
    for (auto &e : extremes) {
        bool found = false;
        for (int32_t i = 0; i < count; i++) {
            if (reduced[i * 3] == e[0] && reduced[i * 3 + 1] == e[1] && reduced[i * 3 + 2] == e[2])
                found = true;
        }
        EXPECT_TRUE(found);
    }
}

TEST(Quickhull3D, ReductionHandlesExtremeFiniteDistances) {
    const double extent = std::numeric_limits<double>::max() / 4.0;
    const double cloud[] = {-extent, 0.0, 0.0, extent,  0.0, 0.0, 0.0,    -extent, 0.0, 0.0, extent,
                            0.0,     0.0, 0.0, -extent, 0.0, 0.0, extent, 0.0,     0.0, 0.0};
    double reduced[18] = {};
    EXPECT_EQ(rt_quickhull3d_reduce(cloud, 7, 6, reduced), 6);
    for (double coordinate : reduced)
        EXPECT_TRUE(std::isfinite(coordinate));
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
