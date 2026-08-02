//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/runtime/TestStlLoad.cpp
// Purpose: Unit tests for the STL (Binary + ASCII) mesh loader.
// Key invariants:
//   - Content failures return NULL and populate asset-load diagnostics.
//   - Invalid programmer arguments still trap.
// Ownership/Lifetime:
//   - Test-scoped meshes are released before the case exits.
// Links: src/runtime/graphics/3d/render/rt_mesh3d_stl.inc
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "tests/TestHarness.hpp"

#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" {
#include "rt_asset_error.h"
#include "rt_canvas3d_internal.h"
void *rt_mesh3d_from_stl(void *path);
int64_t rt_mesh3d_get_vertex_count(void *obj);
int64_t rt_mesh3d_get_triangle_count(void *obj);
rt_string rt_const_cstr(const char *str);
}

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

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_last_trap = nullptr;                                                                     \
        g_expect_trap = true;                                                                      \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            g_expect_trap = false;                                                                 \
            EXPECT_TRUE(false);                                                                    \
        } else {                                                                                   \
            g_expect_trap = false;                                                                 \
            EXPECT_TRUE(g_last_trap != nullptr);                                                   \
        }                                                                                          \
    } while (0)

static void free_mesh(void *m) {
    if (m && rt_obj_release_check0(m))
        rt_obj_free(m);
}

static const char *write_temp(const char *name, const void *data, size_t len) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/zanna_test_%s", name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return nullptr;
    fwrite(data, 1, len, f);
    fclose(f);
    return path;
}

static void put_f32_le(uint8_t *dst, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    dst[0] = (uint8_t)(bits & 0xffu);
    dst[1] = (uint8_t)((bits >> 8) & 0xffu);
    dst[2] = (uint8_t)((bits >> 16) & 0xffu);
    dst[3] = (uint8_t)((bits >> 24) & 0xffu);
}

static void put_binary_triangle(
    uint8_t *dst, const float normal[3], const float v1[3], const float v2[3], const float v3[3]) {
    for (int i = 0; i < 3; i++)
        put_f32_le(dst + i * 4, normal[i]);
    for (int i = 0; i < 3; i++)
        put_f32_le(dst + 12 + i * 4, v1[i]);
    for (int i = 0; i < 3; i++)
        put_f32_le(dst + 24 + i * 4, v2[i]);
    for (int i = 0; i < 3; i++)
        put_f32_le(dst + 36 + i * 4, v3[i]);
}

static double first_triangle_normal_dot(const rt_mesh3d *mesh, const float expected[3]) {
    const uint32_t *idx = mesh->indices;
    const float *a = mesh->vertices[idx[0]].pos;
    const float *b = mesh->vertices[idx[1]].pos;
    const float *c = mesh->vertices[idx[2]].pos;
    double abx = (double)b[0] - (double)a[0];
    double aby = (double)b[1] - (double)a[1];
    double abz = (double)b[2] - (double)a[2];
    double acx = (double)c[0] - (double)a[0];
    double acy = (double)c[1] - (double)a[1];
    double acz = (double)c[2] - (double)a[2];
    double nx = aby * acz - abz * acy;
    double ny = abz * acx - abx * acz;
    double nz = abx * acy - aby * acx;
    return nx * expected[0] + ny * expected[1] + nz * expected[2];
}

TEST(StlLoadTest, RejectNull) {
    EXPECT_TRAP((void)rt_mesh3d_from_stl(nullptr));
}

TEST(StlLoadTest, RejectNonExistent) {
    void *rts = rt_const_cstr("/tmp/nonexistent_stl.stl");
    void *mesh = rt_mesh3d_from_stl(rts);
    EXPECT_EQ(mesh, nullptr);
    EXPECT_NE(rt_asset_error_get_code(), RT_ASSET_ERROR_NONE);
}

TEST(StlLoadTest, RejectGarbage) {
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03};
    const char *path = write_temp("garbage.stl", garbage, sizeof(garbage));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    EXPECT_EQ(mesh, nullptr);
    EXPECT_NE(rt_asset_error_get_code(), RT_ASSET_ERROR_NONE);
    remove(path);
}

TEST(StlLoadTest, BinaryOneTriangle) {
    // Construct a minimal binary STL: 80-byte header + 1 triangle (134 bytes total)
    uint8_t stl[134];
    memset(stl, 0, sizeof(stl));

    // Header: 80 bytes (already zero)
    // Triangle count: 1
    stl[80] = 1;
    stl[81] = stl[82] = stl[83] = 0;

    // Triangle: normal (0, 1, 0) + 3 vertices forming an XZ triangle
    float normal[] = {0.0f, 1.0f, 0.0f};
    float v1[] = {0.0f, 0.0f, 0.0f};
    float v2[] = {1.0f, 0.0f, 0.0f};
    float v3[] = {0.0f, 0.0f, 1.0f};

    put_binary_triangle(stl + 84, normal, v1, v2, v3);
    // Attribute bytes: stl[132..133] = 0 (already zero)

    const char *path = write_temp("one_tri.stl", stl, sizeof(stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    ASSERT_TRUE(mesh != nullptr);

    EXPECT_EQ(rt_mesh3d_get_vertex_count(mesh), 3);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(mesh), 1);
    float expected[] = {0.0f, 1.0f, 0.0f};
    EXPECT_GT(first_triangle_normal_dot((const rt_mesh3d *)mesh, expected), 0.0);

    free_mesh(mesh);
    remove(path);
}

TEST(StlLoadTest, AsciiOneTriangle) {
    const char *ascii_stl = "solid test\n"
                            "  facet normal 0 1 0\n"
                            "    outer loop\n"
                            "      vertex 0 0 0\n"
                            "      vertex 1 0 0\n"
                            "      vertex 0 0 1\n"
                            "    endloop\n"
                            "  endfacet\n"
                            "endsolid test\n";

    const char *path = write_temp("one_tri_ascii.stl", ascii_stl, strlen(ascii_stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    ASSERT_TRUE(mesh != nullptr);

    EXPECT_EQ(rt_mesh3d_get_vertex_count(mesh), 3);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(mesh), 1);
    float expected[] = {0.0f, 1.0f, 0.0f};
    EXPECT_GT(first_triangle_normal_dot((const rt_mesh3d *)mesh, expected), 0.0);

    free_mesh(mesh);
    remove(path);
}

TEST(StlLoadTest, BinarySkipsDegenerateTriangles) {
    uint8_t stl[184];
    memset(stl, 0, sizeof(stl));
    stl[80] = 2;

    float normal[] = {0.0f, 1.0f, 0.0f};
    float a[] = {0.0f, 0.0f, 0.0f};
    float b[] = {1.0f, 0.0f, 0.0f};
    float c[] = {0.0f, 0.0f, 1.0f};
    put_binary_triangle(stl + 84, normal, a, a, a);
    put_binary_triangle(stl + 134, normal, a, b, c);

    const char *path = write_temp("degenerate_binary.stl", stl, sizeof(stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    ASSERT_TRUE(mesh != nullptr);
    EXPECT_EQ(rt_mesh3d_get_vertex_count(mesh), 3);
    EXPECT_EQ(rt_mesh3d_get_triangle_count(mesh), 1);

    free_mesh(mesh);
    remove(path);
}

TEST(StlLoadTest, RejectAsciiVertexOutsideFacet) {
    const char *ascii_stl = "solid bad\n"
                            "  vertex 0 0 0\n"
                            "  vertex 1 0 0\n"
                            "  vertex 0 0 1\n"
                            "endsolid bad\n";

    const char *path = write_temp("vertex_outside_facet.stl", ascii_stl, strlen(ascii_stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    EXPECT_EQ(mesh, nullptr);
    remove(path);
}

TEST(StlLoadTest, RejectMalformedAsciiVertexNumber) {
    const char *ascii_stl = "solid bad\n"
                            "  facet normal 0 1 0\n"
                            "    outer loop\n"
                            "      vertex 0 0 0\n"
                            "      vertex 1 nope 0\n"
                            "      vertex 0 0 1\n"
                            "    endloop\n"
                            "  endfacet\n"
                            "endsolid bad\n";

    const char *path = write_temp("bad_ascii_number.stl", ascii_stl, strlen(ascii_stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    EXPECT_EQ(mesh, nullptr);
    remove(path);
}

TEST(StlLoadTest, RejectAsciiVertexTrailingGarbage) {
    const char *ascii_stl = "solid bad\n"
                            "  facet normal 0 1 0\n"
                            "    outer loop\n"
                            "      vertex 0 0 0\n"
                            "      vertex 1 0 0junk\n"
                            "      vertex 0 0 1\n"
                            "    endloop\n"
                            "  endfacet\n"
                            "endsolid bad\n";

    const char *path = write_temp("bad_ascii_trailing.stl", ascii_stl, strlen(ascii_stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    EXPECT_EQ(mesh, nullptr);
    remove(path);
}

TEST(StlLoadTest, RejectIncompleteAsciiFacet) {
    const char *ascii_stl = "solid bad\n"
                            "  facet normal 0 1 0\n"
                            "    outer loop\n"
                            "      vertex 0 0 0\n"
                            "      vertex 1 0 0\n"
                            "    endloop\n"
                            "  endfacet\n"
                            "endsolid bad\n";

    const char *path = write_temp("incomplete_ascii.stl", ascii_stl, strlen(ascii_stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    EXPECT_EQ(mesh, nullptr);
    remove(path);
}

TEST(StlLoadTest, RejectBinaryWithNonFiniteVertex) {
    uint8_t stl[134];
    memset(stl, 0, sizeof(stl));
    stl[80] = 1;

    float normal[] = {0.0f, 1.0f, 0.0f};
    float v1[] = {0.0f, 0.0f, 0.0f};
    float v2[] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    float v3[] = {0.0f, 0.0f, 1.0f};

    put_binary_triangle(stl + 84, normal, v1, v2, v3);

    const char *path = write_temp("nan_binary.stl", stl, sizeof(stl));
    void *rts = rt_const_cstr(path);
    void *mesh = rt_mesh3d_from_stl(rts);
    EXPECT_EQ(mesh, nullptr);
    remove(path);
}

int main() {
    return zanna_test::run_all_tests();
}
