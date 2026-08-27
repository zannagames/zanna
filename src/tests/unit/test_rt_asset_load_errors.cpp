//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_asset_load_errors.cpp
// Purpose: Unit tests for recoverable runtime content-loader diagnostics and
//   their bounded, machine-readable reporting contract.
// Key invariants:
//   - Bad or missing content returns NULL and records a queryable error.
//   - Optional material texture loss records warnings without failing the parent load.
//   - Scope misuse, truncation, suppression, and hostile warning bytes cannot
//     corrupt diagnostic state or the JSON import report.
// Ownership/Lifetime:
//   - Tests release any GC-managed runtime objects they receive.
//   - Temporary files are created under /tmp and removed by each test.
// Links: rt_asset_error.h, docs/zannalib/graphics/rendering3d.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_asset_error.h"
#include "rt_gltf_json.h"
#include "rt_string.h"
#include "tests/TestHarness.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
void *rt_model3d_load(rt_string path);
void *rt_fbx_load(rt_string path);
int64_t rt_fbx_mesh_count(void *asset);
void *rt_gltf_load(rt_string path);
void *rt_mesh3d_from_obj(rt_string path);
void *rt_mesh3d_from_stl(rt_string path);
void *rt_scene3d_load(rt_string path);
void *rt_pixels_load(void *path);
void *rt_game3d_world_new(rt_string title, int64_t width, int64_t height);
void *rt_game3d_world_stream_new(void *world_obj);
void rt_game3d_world_stream_mount_cells(void *obj, rt_string manifest_path);
int64_t rt_game3d_world_stream_get_cell_count(void *obj);
void rt_game3d_world_destroy(void *obj);
rt_string rt_const_cstr(const char *str);
int64_t rt_obj_release_check0(void *obj);
void rt_obj_free(void *obj);
}

extern "C" void vm_trap(const char *msg) {
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

using LoaderFn = void *(*)(rt_string path);

static std::string tmp_path(const char *name) {
    return std::string("/tmp/zanna_asset_error_") + name;
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *f = std::fopen(path, "wb");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ(std::fwrite(data, 1, size, f), size);
    std::fclose(f);
}

static void write_text(const char *path, const char *text) {
    write_bytes(path, text, std::strlen(text));
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void expect_error_recorded(const char *context) {
    const char *message = rt_asset_error_get_message();
    EXPECT_NE(rt_asset_error_get_code(), RT_ASSET_ERROR_NONE);
    EXPECT_TRUE(message != nullptr);
    EXPECT_GT(std::strlen(message), (size_t)0);
    (void)context;
}

static void expect_null_with_error(LoaderFn loader, const char *path, const char *context) {
    void *obj = loader(rt_const_cstr(path));
    EXPECT_EQ(obj, nullptr);
    expect_error_recorded(context);
}

static void expect_null_pixels_with_error(const char *path, const char *context) {
    void *obj = rt_pixels_load(rt_const_cstr(path));
    EXPECT_EQ(obj, nullptr);
    expect_error_recorded(context);
}

TEST(AssetLoadErrors, MissingFilesReturnNullAndSetError) {
    std::string model = tmp_path("missing_model.fbx");
    std::remove(model.c_str());
    expect_null_with_error(rt_model3d_load, model.c_str(), "SceneAsset.Load missing");

    std::string fbx = tmp_path("missing_direct.fbx");
    std::remove(fbx.c_str());
    expect_null_with_error(rt_fbx_load, fbx.c_str(), "FBX.Load missing");

    std::string gltf = tmp_path("missing_scene.gltf");
    std::remove(gltf.c_str());
    expect_null_with_error(rt_gltf_load, gltf.c_str(), "GLTF.Load missing");

    std::string obj = tmp_path("missing_mesh.obj");
    std::remove(obj.c_str());
    expect_null_with_error(rt_mesh3d_from_obj, obj.c_str(), "Mesh3D.FromOBJ missing");

    std::string stl = tmp_path("missing_mesh.stl");
    std::remove(stl.c_str());
    expect_null_with_error(rt_mesh3d_from_stl, stl.c_str(), "Mesh3D.FromSTL missing");

    std::string vscn = tmp_path("missing_scene.vscn");
    std::remove(vscn.c_str());
    expect_null_with_error(rt_scene3d_load, vscn.c_str(), "SceneGraph.Load missing");

    std::string png = tmp_path("missing_pixels.png");
    std::remove(png.c_str());
    expect_null_pixels_with_error(png.c_str(), "Pixels.Load missing");
}

TEST(AssetLoadErrors, TruncatedFilesReturnNullAndSetError) {
    const uint8_t png_header[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    std::string model = tmp_path("truncated_model.fbx");
    write_text(model.c_str(), "K");
    expect_null_with_error(rt_model3d_load, model.c_str(), "SceneAsset.Load truncated");
    std::remove(model.c_str());

    std::string fbx = tmp_path("truncated_direct.fbx");
    write_text(fbx.c_str(), "K");
    expect_null_with_error(rt_fbx_load, fbx.c_str(), "FBX.Load truncated");
    std::remove(fbx.c_str());

    std::string gltf = tmp_path("truncated_scene.gltf");
    write_text(gltf.c_str(), "{");
    expect_null_with_error(rt_gltf_load, gltf.c_str(), "GLTF.Load truncated");
    std::remove(gltf.c_str());

    std::string obj = tmp_path("truncated_mesh.obj");
    write_text(obj.c_str(), "v 0 0");
    expect_null_with_error(rt_mesh3d_from_obj, obj.c_str(), "Mesh3D.FromOBJ truncated");
    std::remove(obj.c_str());

    std::string stl = tmp_path("truncated_mesh.stl");
    write_text(stl.c_str(), "so");
    expect_null_with_error(rt_mesh3d_from_stl, stl.c_str(), "Mesh3D.FromSTL truncated");
    std::remove(stl.c_str());

    std::string vscn = tmp_path("truncated_scene.vscn");
    write_text(vscn.c_str(), "{");
    expect_null_with_error(rt_scene3d_load, vscn.c_str(), "SceneGraph.Load truncated");
    std::remove(vscn.c_str());

    std::string png = tmp_path("truncated_pixels.png");
    write_bytes(png.c_str(), png_header, sizeof(png_header));
    expect_null_pixels_with_error(png.c_str(), "Pixels.Load truncated");
    std::remove(png.c_str());
}

TEST(AssetLoadErrors, WrongMagicFilesReturnNullAndSetError) {
    std::string model = tmp_path("wrong_model.fbx");
    write_text(model.c_str(), "not an fbx");
    expect_null_with_error(rt_model3d_load, model.c_str(), "SceneAsset.Load wrong magic");
    std::remove(model.c_str());

    std::string fbx = tmp_path("wrong_direct.fbx");
    write_text(fbx.c_str(), "not an fbx");
    expect_null_with_error(rt_fbx_load, fbx.c_str(), "FBX.Load wrong magic");
    std::remove(fbx.c_str());

    std::string gltf = tmp_path("wrong_scene.gltf");
    write_text(gltf.c_str(), "not json");
    expect_null_with_error(rt_gltf_load, gltf.c_str(), "GLTF.Load wrong magic");
    std::remove(gltf.c_str());

    std::string obj = tmp_path("wrong_mesh.obj");
    write_text(obj.c_str(), "not obj");
    expect_null_with_error(rt_mesh3d_from_obj, obj.c_str(), "Mesh3D.FromOBJ wrong magic");
    std::remove(obj.c_str());

    std::string stl = tmp_path("wrong_mesh.stl");
    write_text(stl.c_str(), "not stl");
    expect_null_with_error(rt_mesh3d_from_stl, stl.c_str(), "Mesh3D.FromSTL wrong magic");
    std::remove(stl.c_str());

    std::string vscn = tmp_path("wrong_scene.vscn");
    write_text(vscn.c_str(), "not json");
    expect_null_with_error(rt_scene3d_load, vscn.c_str(), "SceneGraph.Load wrong magic");
    std::remove(vscn.c_str());

    std::string img = tmp_path("wrong_pixels.img");
    write_text(img.c_str(), "not image");
    expect_null_pixels_with_error(img.c_str(), "Pixels.Load wrong magic");
    std::remove(img.c_str());
}

TEST(AssetLoadErrors, UntrustedCountsReturnNullAndSetError) {
    std::string vscn = tmp_path("huge_vertex_count.vscn");
    write_text(vscn.c_str(),
               "{\"format\":\"vscn\",\"version\":1,\"meshes\":[{\"vertexFormat\":"
               "\"vgfx3d_vertex_le_v2\",\"vertexCount\":2147483648,\"indexCount\":0,"
               "\"boneCount\":0,\"verticesBase64\":\"\",\"indicesBase64\":\"\"}],"
               "\"nodes\":[]}");
    expect_null_with_error(rt_scene3d_load, vscn.c_str(), "SceneGraph.Load huge vertex count");
    std::remove(vscn.c_str());

    const uint8_t truncated_glb[] = {'g', 'l', 'T', 'F', 2, 0, 0,   0,   32,  0,
                                     0,   0,   4,   0,   0, 0, 'J', 'S', 'O', 'N'};
    std::string glb = tmp_path("truncated_count.glb");
    write_bytes(glb.c_str(), truncated_glb, sizeof(truncated_glb));
    expect_null_with_error(rt_gltf_load, glb.c_str(), "GLTF.Load truncated GLB");
    std::remove(glb.c_str());

    std::string obj = tmp_path("degenerate_face_token.obj");
    write_text(obj.c_str(),
               "v 0 0 0\n"
               "v 1 0 0\n"
               "v 0 1 0\n"
               "f //\n");
    expect_null_with_error(rt_mesh3d_from_obj, obj.c_str(), "Mesh3D.FromOBJ degenerate face");
    std::remove(obj.c_str());

    std::string fbx = tmp_path("degenerate_triangle.fbx");
    write_text(fbx.c_str(),
               "Objects:  {\n"
               "    Geometry: 1, \"Geometry::Degenerate\", \"Mesh\" {\n"
               "        Vertices: *9 { a: 0,0,0, 0,0,0, 0,0,0 }\n"
               "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
               "    }\n"
               "}\n");
    expect_null_with_error(rt_fbx_load, fbx.c_str(), "FBX.Load degenerate triangle");
    std::remove(fbx.c_str());

    std::string huge_fbx = tmp_path("huge_ascii_position.fbx");
    write_text(huge_fbx.c_str(),
               "Objects:  {\n"
               "    Geometry: 1, \"Geometry::Huge\", \"Mesh\" {\n"
               "        Vertices: *9 { a: 1e39,0,0, 1e39,0,0, 1e39,0,0 }\n"
               "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
               "    }\n"
               "}\n");
    expect_null_with_error(rt_fbx_load, huge_fbx.c_str(), "FBX.Load huge ASCII position");
    std::remove(huge_fbx.c_str());
}

TEST(AssetLoadErrors, StreamManifestDeclaredCountIsBounded) {
    std::string manifest = tmp_path("huge_stream_manifest.json");
    write_text(manifest.c_str(), "{\"cellCount\":1000000000,\"cells\":[]}");

    rt_asset_error_clear();
    void *world = rt_game3d_world_new(rt_const_cstr("manifest-count-test"), 16, 16);
    ASSERT_NE(world, nullptr);
    void *stream = rt_game3d_world_stream_new(world);
    ASSERT_NE(stream, nullptr);

    rt_game3d_world_stream_mount_cells(stream, rt_const_cstr(manifest.c_str()));
    EXPECT_EQ(rt_game3d_world_stream_get_cell_count(stream), INT64_C(0));
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_TOO_LARGE);
    expect_error_recorded("Game3D.WorldStream3D huge manifest count");

    release_obj(stream);
    rt_game3d_world_destroy(world);
    release_obj(world);
    std::remove(manifest.c_str());
}

TEST(AssetLoadErrors, AsciiFbxHeaderLoadsGeometryThroughAsciiPath) {
    /* ASCII FBX with the standard comment signature loads its geometry subset
     * instead of being rejected (fps plan 09 / E31). */
    std::string fbx = tmp_path("ascii_header.fbx");
    write_text(fbx.c_str(),
               "; FBX 7.4.0 project file\n"
               "; ----------------------------------------------------\n"
               "Objects:  {\n"
               "  Geometry: 1, \"Geometry::AsciiMesh\", \"Mesh\" {\n"
               "    Vertices: *9 { a: 0,0,0, 1,0,0, 0,1,0 }\n"
               "    PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
               "  }\n"
               "}\n");

    void *asset = rt_fbx_load(rt_const_cstr(fbx.c_str()));
    EXPECT_NE(asset, nullptr);
    EXPECT_EQ(rt_fbx_mesh_count(asset), 1);

    std::remove(fbx.c_str());
}

TEST(AssetLoadErrors, OversizedConcaveFbxPolygonIsRejectedInsteadOfFanned) {
    constexpr int kVertexCount = 1025;
    std::string text = "; FBX 7.4.0 project file\nObjects: {\n"
                       " Geometry: 1, \"Geometry::ConcaveMegaFace\", \"Mesh\" {\n"
                       "  Vertices: *" +
                       std::to_string(kVertexCount * 3) + " { a: ";
    for (int i = 0; i < kVertexCount; i++) {
        double angle = 2.0 * 3.14159265358979323846 * (double)i / (double)kVertexCount;
        double radius = i == kVertexCount / 2 ? 0.1 : 1.0;
        if (i)
            text += ",";
        text += std::to_string(radius * std::cos(angle)) + "," +
                std::to_string(radius * std::sin(angle)) + ",0";
    }
    text += " }\n  PolygonVertexIndex: *" + std::to_string(kVertexCount) + " { a: ";
    for (int i = 0; i < kVertexCount; i++) {
        if (i)
            text += ",";
        text += std::to_string(i + 1 == kVertexCount ? -(i + 1) : i);
    }
    text += " }\n }\n}\n";

    std::string fbx = tmp_path("concave_mega_face.fbx");
    write_text(fbx.c_str(), text.c_str());
    expect_null_with_error(rt_fbx_load, fbx.c_str(), "FBX.Load oversized concave polygon");
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_CORRUPT);
    std::remove(fbx.c_str());
}

TEST(AssetLoadErrors, AsciiFbxWithoutObjectGraphReportsCorrupt) {
    /* A header-only file claims the supported ASCII FBX format but lacks the
     * required typed Objects/Connections graph, so it is malformed rather than
     * an unsupported format or optional FBX feature. */
    std::string fbx = tmp_path("ascii_empty.fbx");
    write_text(fbx.c_str(),
               "; FBX 7.4.0 project file\n"
               "FBXHeaderExtension:  {\n"
               "  FBXVersion: 7400\n"
               "}\n");

    void *asset = rt_fbx_load(rt_const_cstr(fbx.c_str()));
    EXPECT_EQ(asset, nullptr);
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_CORRUPT);
    EXPECT_EQ(std::string(rt_asset_error_get_message()),
              std::string("FBX.Load: invalid Objects or Connections graph"));

    std::remove(fbx.c_str());
}

TEST(AssetLoadErrors, MissingObjMaterialTextureRecordsOneWarning) {
    std::string obj_path = tmp_path("textured_missing.obj");
    std::string mtl_path = tmp_path("textured_missing.mtl");
    write_text(mtl_path.c_str(), "newmtl bark\nmap_Kd missing_albedo.png\n");
    write_text(obj_path.c_str(),
               "mtllib zanna_asset_error_textured_missing.mtl\n"
               "v 0 0 0\n"
               "v 1 0 0\n"
               "v 0 1 0\n"
               "vt 0 0\n"
               "vt 1 0\n"
               "vt 0 1\n"
               "usemtl bark\n"
               "f 1/1 2/2 3/3\n");

    void *model = rt_model3d_load(rt_const_cstr(obj_path.c_str()));
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_NONE);
    EXPECT_EQ(rt_asset_error_get_warning_count(), INT64_C(1));
    EXPECT_CONTAINS(rt_asset_error_get_warning(0), "missing_albedo.png");

    release_obj(model);
    std::remove(obj_path.c_str());
    std::remove(mtl_path.c_str());
}

TEST(AssetLoadErrors, DiagnosticCodesAndScopeStateRemainCanonical) {
    rt_asset_error_clear();
    rt_asset_error_set(static_cast<rt_asset_error_code>(-17), "invalid negative code");
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_CORRUPT);
    EXPECT_CONTAINS(rt_asset_error_get_message(), "invalid negative code");

    rt_asset_error_set(static_cast<rt_asset_error_code>(999), "invalid positive code");
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_CORRUPT);
    EXPECT_CONTAINS(rt_asset_error_get_message(), "invalid positive code");

    rt_asset_error_setf(static_cast<rt_asset_error_code>(-99), "formatted invalid %d", 7);
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_CORRUPT);
    EXPECT_CONTAINS(rt_asset_error_get_message(), "formatted invalid 7");

    rt_asset_error_clear();
    rt_asset_error_setf_if_empty(static_cast<rt_asset_error_code>(700), "first invalid");
    rt_asset_error_set_if_empty(RT_ASSET_ERROR_UNREADABLE, "must not replace first");
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_CORRUPT);
    EXPECT_EQ(std::string(rt_asset_error_get_message()), std::string("first invalid"));

    rt_asset_error_set(RT_ASSET_ERROR_NONE, "stale text");
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_NONE);
    EXPECT_EQ(std::string(rt_asset_error_get_message()), std::string());
    EXPECT_EQ(rt_asset_error_get_message_was_truncated(), 0);

    rt_asset_error_set(RT_ASSET_ERROR_CORRUPT, "must survive unmatched success");
    rt_asset_error_end_load_success();
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_CORRUPT);
    EXPECT_CONTAINS(rt_asset_error_get_message(), "must survive unmatched success");

    EXPECT_EQ(rt_asset_error_begin_load(), 1);
    rt_asset_error_set(RT_ASSET_ERROR_UNREADABLE, "nested failure");
    EXPECT_EQ(rt_asset_error_begin_load(), 0);
    rt_asset_error_end_load_success();
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_UNREADABLE);
    rt_asset_error_end_load_failure();
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_UNREADABLE);

    EXPECT_EQ(rt_asset_error_begin_load(), 1);
    EXPECT_EQ(rt_asset_error_get_code(), RT_ASSET_ERROR_NONE);
    rt_asset_error_end_load_success();
}

TEST(AssetLoadErrors, WarningTruncationAndSuppressionAccountingAreExact) {
    rt_asset_error_clear();
    std::string oversized(600, 'w');
    rt_asset_error_add_warningf("formatted:%s", oversized.c_str());
    ASSERT_EQ(rt_asset_error_get_warning_count(), INT64_C(1));
    EXPECT_EQ(rt_asset_error_get_warning_was_truncated(0), 1);
    EXPECT_EQ(std::strlen(rt_asset_error_get_warning(0)), (size_t)255);

    rt_asset_error_clear();
    for (int i = 0; i < 17; ++i) {
        std::string warning = "warning-" + std::to_string(i);
        rt_asset_error_add_warning(warning.c_str());
    }
    ASSERT_EQ(rt_asset_error_get_warning_count(), INT64_C(16));
    for (int i = 0; i < 15; ++i) {
        std::string expected = "warning-" + std::to_string(i);
        EXPECT_EQ(std::string(rt_asset_error_get_warning(i)), expected);
    }
    EXPECT_EQ(rt_asset_error_get_warning_suppressed_count(), INT64_C(2));
    EXPECT_EQ(std::string(rt_asset_error_get_warning(15)), std::string("2 more suppressed"));
    EXPECT_EQ(rt_asset_error_get_warning_was_truncated(15), 0);

    rt_asset_error_add_warning("warning-17");
    EXPECT_EQ(rt_asset_error_get_warning_suppressed_count(), INT64_C(3));
    EXPECT_EQ(std::string(rt_asset_error_get_warning(15)), std::string("3 more suppressed"));
}

TEST(AssetLoadErrors, WorstCaseImportReportIsCompleteStrictJson) {
    rt_asset_error_clear();
    std::string controls(255, '\x01');
    for (int i = 0; i < 16; ++i)
        rt_asset_error_add_warning(controls.c_str());

    rt_string report = rt_assets3d_get_import_report();
    ASSERT_NE(report, nullptr);
    const char *text = rt_string_cstr(report);
    const size_t length = (size_t)rt_str_len(report);
    EXPECT_TRUE(length > 20000u);
    EXPECT_TRUE(gltf_json_validate_document(text, length) != 0);
    EXPECT_TRUE(length >= 2u && text[length - 2u] == ']' && text[length - 1u] == '}');
    rt_string_unref(report);

    rt_asset_error_clear();
    const char invalid_utf8[] = "a\xC0\xAFz";
    rt_asset_error_add_warning(invalid_utf8);
    report = rt_assets3d_get_import_report();
    ASSERT_NE(report, nullptr);
    text = rt_string_cstr(report);
    EXPECT_CONTAINS(text, "a\\u00c0\\u00afz");
    EXPECT_TRUE(gltf_json_validate_document(text, (size_t)rt_str_len(report)) != 0);
    rt_string_unref(report);

    rt_asset_error_clear();
    const char valid_utf8[] = "caf\xC3\xA9";
    rt_asset_error_add_warning(valid_utf8);
    report = rt_assets3d_get_import_report();
    ASSERT_NE(report, nullptr);
    text = rt_string_cstr(report);
    EXPECT_CONTAINS(text, valid_utf8);
    EXPECT_TRUE(gltf_json_validate_document(text, (size_t)rt_str_len(report)) != 0);
    rt_string_unref(report);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
