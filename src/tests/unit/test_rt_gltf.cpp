//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_gltf.cpp
// Purpose: Unit tests for the glTF/GLB runtime loader and imported asset surface.
// Key invariants:
//   - glTF content failures return null and record recoverable asset diagnostics.
//   - Imported meshes, materials, animations, textures, and scenes preserve authored data.
// Ownership/Lifetime:
//   - Temporary fixture files are owned by each test and removed where cleanup matters.
//   - Runtime handles are GC-managed for the duration of the test process.
// Links: rt_gltf.h, rt_asset_error.h, docs/zannalib/graphics/rendering3d.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_asset.h"
#include "rt_asset_error.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_gltf.h"
#include "rt_gltf_json.h"
#include "rt_morphtarget3d.h"
#include "rt_pixels.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
extern "C" {
#include "vgfx3d_skinning.h"
}
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ZpakWriter.hpp"

extern "C" {
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern rt_string rt_const_cstr(const char *s);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern double rt_quat_x(void *q);
extern double rt_quat_y(void *q);
extern double rt_quat_z(void *q);
extern double rt_quat_w(void *q);
}

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (std::fabs((a) - (b)) > (eps))                                                          \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

static std::string base64_encode(const uint8_t *data, size_t len) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out(((len + 2) / 3) * 4, '=');
    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        uint32_t a = data[i++];
        uint32_t b = (i < len) ? data[i++] : 0;
        uint32_t c = (i < len) ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = chars[(triple >> 18) & 0x3F];
        out[j++] = chars[(triple >> 12) & 0x3F];
        out[j++] = chars[(triple >> 6) & 0x3F];
        out[j++] = chars[triple & 0x3F];
    }
    size_t padding = (3 - (len % 3)) % 3;
    for (size_t p = 0; p < padding; p++)
        out[out.size() - 1 - p] = '=';
    return out;
}

static bool read_file_bytes(const char *path, std::vector<uint8_t> &out) {
    out.clear();
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long size = std::ftell(f);
    if (size < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)size);
    bool ok = size == 0 || std::fread(out.data(), 1, (size_t)size, f) == (size_t)size;
    std::fclose(f);
    return ok;
}

static void *gltf_test_texture_pixels(void *texture) {
    void *pixels = texture ? rt_textureasset3d_get_pixels(texture) : nullptr;
    return pixels ? pixels : texture;
}

static bool gltf_test_texture_source_equals_bytes(void *texture,
                                                  const char *expected_kind,
                                                  const uint8_t *expected,
                                                  size_t expected_size) {
    const uint8_t *data = nullptr;
    uint64_t size = 0;
    const char *kind = nullptr;
    return texture && expected_kind &&
           rt_textureasset3d_get_source_container(texture, &data, &size, &kind) && data && kind &&
           std::strcmp(kind, expected_kind) == 0 && size == expected_size &&
           std::memcmp(data, expected, expected_size) == 0;
}

static bool gltf_test_texture_source_equals(void *texture,
                                            const char *expected_kind,
                                            const std::vector<uint8_t> &expected) {
    return gltf_test_texture_source_equals_bytes(
        texture, expected_kind, expected.data(), expected.size());
}

static bool write_text_file(const char *path, const std::string &text) {
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

static bool write_binary_file(const char *path, const std::vector<uint8_t> &bytes) {
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool load_warnings_contain(const char *needle) {
    int64_t count = rt_asset_error_get_warning_count();
    for (int64_t i = 0; i < count; i++) {
        const char *warning = rt_asset_error_get_warning(i);
        if (warning && std::strstr(warning, needle) != nullptr)
            return true;
    }
    return false;
}

static bool import_report_contains(const char *needle) {
    rt_string report = rt_assets3d_get_import_report();
    const char *text = report ? rt_string_cstr(report) : nullptr;
    bool contains = text && std::strstr(text, needle) != nullptr;
    rt_string_unref(report);
    return contains;
}

static void test_gltf_accessors_reject_wrong_handles() {
    void *fake = rt_obj_new_i64(0, 8);
    EXPECT_TRUE(rt_gltf_mesh_count(fake) == 0, "GLTF mesh count rejects wrong handles");
    EXPECT_TRUE(rt_gltf_material_count(fake) == 0, "GLTF material count rejects wrong handles");
    EXPECT_TRUE(rt_gltf_skeleton_count(fake) == 0, "GLTF skeleton count rejects wrong handles");
    EXPECT_TRUE(rt_gltf_animation_count(fake) == 0, "GLTF animation count rejects wrong handles");
    EXPECT_TRUE(rt_gltf_node_animation_count(fake) == 0,
                "GLTF node-animation count rejects wrong handles");
    EXPECT_TRUE(rt_gltf_node_count(fake) == 0, "GLTF node count rejects wrong handles");
    EXPECT_TRUE(rt_gltf_get_mesh(fake, 0) == nullptr, "GLTF mesh getter rejects wrong handles");
    EXPECT_TRUE(rt_gltf_get_material(fake, 0) == nullptr,
                "GLTF material getter rejects wrong handles");
    EXPECT_TRUE(rt_gltf_get_skeleton(fake, 0) == nullptr,
                "GLTF skeleton getter rejects wrong handles");
    EXPECT_TRUE(rt_gltf_get_animation(fake, 0) == nullptr,
                "GLTF animation getter rejects wrong handles");
    EXPECT_TRUE(rt_gltf_get_node_animation(fake, 0) == nullptr,
                "GLTF node-animation getter rejects wrong handles");
    EXPECT_TRUE(rt_gltf_get_scene_root(fake) == nullptr,
                "GLTF scene-root getter rejects wrong handles");
}

template <typename T> static void append_bytes(std::vector<uint8_t> &buf, const T &value) {
    size_t offset = buf.size();
    buf.resize(offset + sizeof(T));
    std::memcpy(buf.data() + offset, &value, sizeof(T));
}

static void append_u32_le(std::vector<uint8_t> &buf, uint32_t value) {
    buf.push_back((uint8_t)(value & 0xFFu));
    buf.push_back((uint8_t)((value >> 8) & 0xFFu));
    buf.push_back((uint8_t)((value >> 16) & 0xFFu));
    buf.push_back((uint8_t)((value >> 24) & 0xFFu));
}

static void append_u64_le(std::vector<uint8_t> &buf, uint64_t value) {
    append_u32_le(buf, (uint32_t)(value & 0xFFFFFFFFu));
    append_u32_le(buf, (uint32_t)(value >> 32));
}

static bool write_test_ktx2_rgba8(const char *path,
                                  uint32_t width,
                                  uint32_t height,
                                  const uint8_t *level0,
                                  uint64_t level0_bytes) {
    static const uint8_t identifier[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), identifier, identifier + sizeof(identifier));
    append_u32_le(bytes, 37u);
    append_u32_le(bytes, 1u);
    append_u32_le(bytes, width);
    append_u32_le(bytes, height);
    append_u32_le(bytes, 0u);
    append_u32_le(bytes, 0u);
    append_u32_le(bytes, 1u);
    append_u32_le(bytes, 1u);
    append_u32_le(bytes, 0u);
    while (bytes.size() < 80u)
        bytes.push_back(0u);
    append_u64_le(bytes, 104u);
    append_u64_le(bytes, level0_bytes);
    append_u64_le(bytes, level0_bytes);
    if (level0 && level0_bytes > 0)
        bytes.insert(bytes.end(), level0, level0 + (size_t)level0_bytes);
    return write_binary_file(path, bytes);
}

static void test_gltf_loads_data_uri_buffers_and_embedded_textures() {
    const char *png_path = "/tmp/zanna_gltf_embedded.png";
    const char *gltf_path = "/tmp/zanna_gltf_embedded.gltf";

    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x336699FFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "Pixels.SavePng creates a temporary embedded texture");

    std::vector<uint8_t> png_bytes;
    EXPECT_TRUE(read_file_bytes(png_path, png_bytes), "Embedded texture PNG can be read back");
    if (png_bytes.empty())
        return;

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float normals[9] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const float uvs[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : normals)
        append_bytes(gltf_buffer, v);
    for (float v : uvs)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string image_b64 = base64_encode(png_bytes.data(), png_bytes.size());

    std::string gltf_json =
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"buffers\": [{\"uri\": \"data:application/octet-stream;base64," +
        buffer_b64 + "\", \"byteLength\": " + std::to_string(gltf_buffer.size()) +
        "}],\n"
        "  \"bufferViews\": [\n"
        "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
        "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36},\n"
        "    {\"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 24},\n"
        "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 6}\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC2\"},\n"
        "    {\"bufferView\": 3, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\"}\n"
        "  ],\n"
        "  \"images\": [{\"uri\": \"data:image/png;base64," +
        image_b64 +
        "\"}],\n"
        "  \"textures\": [{\"source\": 0}],\n"
        "  \"materials\": [{\n"
        "    \"pbrMetallicRoughness\": {\n"
        "      \"baseColorFactor\": [1.0, 1.0, 1.0, 0.7],\n"
        "      \"baseColorTexture\": {\"index\": 0},\n"
        "      \"metallicFactor\": 0.75,\n"
        "      \"roughnessFactor\": 0.25,\n"
        "      \"metallicRoughnessTexture\": {\"index\": 0}\n"
        "    },\n"
        "    \"normalTexture\": {\"index\": 0, \"scale\": 0.6},\n"
        "    \"occlusionTexture\": {\"index\": 0, \"strength\": 0.5},\n"
        "    \"emissiveTexture\": {\"index\": 0},\n"
        "    \"emissiveFactor\": [1.0, 1.0, 1.0],\n"
        "    \"doubleSided\": true,\n"
        "    \"alphaMode\": \"BLEND\",\n"
        "    \"extensions\": {\"KHR_materials_emissive_strength\": {\"emissiveStrength\": 1.8}}\n"
        "  }],\n"
        "  \"meshes\": [{\"primitives\": [{\n"
        "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2},\n"
        "    \"indices\": 3,\n"
        "    \"material\": 0\n"
        "  }]}]\n"
        "}\n";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Embedded glTF file can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses data-uri assets");
    if (!asset)
        return;

    EXPECT_TRUE(rt_gltf_mesh_count(asset) == 1, "GLTF.Load extracts one mesh primitive");
    EXPECT_TRUE(rt_gltf_material_count(asset) == 1, "GLTF.Load extracts one material");

    rt_mesh3d *mesh = (rt_mesh3d *)rt_gltf_get_mesh(asset, 0);
    rt_material3d *material = (rt_material3d *)rt_gltf_get_material(asset, 0);
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "GLTF.Load decodes mesh buffers from data URIs");
    EXPECT_TRUE(material != nullptr, "GLTF.Load returns decoded materials");
    if (!mesh || !material)
        return;

    EXPECT_TRUE(mesh->vertices[1].pos[0] == 1.0f && mesh->vertices[2].uv[1] == 1.0f,
                "GLTF.Load preserves vertex attributes");
    EXPECT_TRUE(std::fabs(mesh->vertices[0].tangent[0]) > 0.9f &&
                    std::fabs(mesh->vertices[0].tangent[3]) > 0.9f,
                "GLTF.Load computes tangents when a normal map is present but tangents are absent");
    EXPECT_TRUE(material->workflow == RT_MATERIAL3D_WORKFLOW_PBR,
                "GLTF.Load preserves the PBR workflow");
    EXPECT_TRUE(material->texture != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 0) ==
                        0x336699FFll,
                "GLTF.Load wires base color textures into Material3D");
    EXPECT_TRUE(material->normal_map != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->normal_map), 0, 0) ==
                        0x336699FFll,
                "GLTF.Load wires normal textures into Material3D");
    EXPECT_TRUE(material->metallic_roughness_map != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->metallic_roughness_map),
                                  0,
                                  0) == 0x336699FFll,
                "GLTF.Load wires metallic-roughness textures into Material3D");
    EXPECT_TRUE(material->ao_map != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->ao_map), 0, 0) == 0x336699FFll,
                "GLTF.Load wires occlusion textures into Material3D");
    EXPECT_TRUE(material->emissive_map != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->emissive_map), 0, 0) ==
                        0x336699FFll,
                "GLTF.Load wires emissive textures into Material3D");
    EXPECT_TRUE(gltf_test_texture_source_equals(material->texture, "png", png_bytes),
                "GLTF.Load retains data-URI PNG bytes exactly");
    EXPECT_TRUE(material->texture == material->normal_map &&
                    material->texture == material->metallic_roughness_map &&
                    material->texture == material->ao_map &&
                    material->texture == material->emissive_map,
                "one glTF image preserves shared texture identity across material slots");
    EXPECT_NEAR(material->metallic, 0.75, 0.001, "GLTF.Load preserves metallicFactor");
    EXPECT_NEAR(material->roughness, 0.25, 0.001, "GLTF.Load preserves roughnessFactor");
    EXPECT_NEAR(material->ao, 0.5, 0.001, "GLTF.Load preserves occlusion strength");
    EXPECT_NEAR(material->normal_scale, 0.6, 0.001, "GLTF.Load preserves normal-map scale");
    EXPECT_NEAR(material->alpha, 0.7, 0.001, "GLTF.Load preserves base-color alpha");
    EXPECT_NEAR(material->emissive_intensity,
                1.8,
                0.001,
                "GLTF.Load preserves emissive strength extension");
    EXPECT_TRUE(material->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND,
                "GLTF.Load preserves alphaMode");
    EXPECT_TRUE(material->double_sided == 1, "GLTF.Load preserves doubleSided");
}

static void test_gltf_resolves_percent_encoded_external_buffers() {
    const char *bin_path = "/tmp/zanna gltf encoded buffer.bin";
    const char *gltf_path = "/tmp/zanna_gltf_encoded_external_buffer.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    FILE *bin = std::fopen(bin_path, "wb");
    EXPECT_TRUE(bin != nullptr, "External glTF buffer file can be created");
    if (!bin)
        return;
    std::fwrite(gltf_buffer.data(), 1, gltf_buffer.size(), bin);
    std::fclose(bin);

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"zanna%20gltf%20encoded%20buffer.bin\",\"byteLength\":" +
        std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]"
        "}";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "External-buffer glTF fixture can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load resolves percent-encoded external buffer URIs");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "GLTF.Load imports mesh data from an encoded external buffer path");
    if (!mesh)
        return;
    EXPECT_NEAR(mesh->vertices[1].pos[0], 2.0, 0.001, "External buffer vertex X is loaded");
    EXPECT_NEAR(mesh->vertices[2].pos[1], 3.0, 0.001, "External buffer vertex Y is loaded");
}

static void test_gltf_preload_bundle_supplies_external_buffers() {
    const char *bin_path = "/tmp/zanna_gltf_preload_external.bin";
    const char *gltf_path = "/tmp/zanna_gltf_preload_external.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    FILE *bin = std::fopen(bin_path, "wb");
    EXPECT_TRUE(bin != nullptr, "Preload external buffer file can be created");
    if (!bin)
        return;
    std::fwrite(gltf_buffer.data(), 1, gltf_buffer.size(), bin);
    std::fclose(bin);

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"zanna_gltf_preload_external.bin\",\"byteLength\":" +
        std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Preload glTF fixture can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload glTF root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload root byte copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can be built from root bytes");
    EXPECT_TRUE(error[0] == '\0', "Preload bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 2,
                "Preload bundle stages the external buffer plus decoded mesh POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                "Preload bundle worker-decodes the external-buffer static mesh to POD");
    if (!bundle)
        return;

    std::remove(bin_path);
    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr,
                "Preloaded bundle supplies external buffer after source deletion");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "Preloaded external buffer builds the expected mesh");
    if (!mesh)
        return;
    EXPECT_NEAR(mesh->vertices[1].pos[0], 4.0, 0.001, "Preloaded buffer vertex X is loaded");
    EXPECT_NEAR(mesh->vertices[2].pos[1], 5.0, 0.001, "Preloaded buffer vertex Y is loaded");
}

static void test_gltf_preload_bundle_rejects_missing_required_buffers() {
    const char *gltf_path = "/tmp/zanna_gltf_preload_missing_required_buffer.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"zanna_gltf_preload_missing_required_buffer.bin\","
        "\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Missing-buffer preload glTF fixture can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Missing-buffer root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Missing-buffer root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle == nullptr, "Preload rejects missing required external buffers on worker");
    EXPECT_TRUE(std::strstr(error, "failed to stage glTF dependency") != nullptr,
                "Missing required buffer reports a preload-stage error");
}

static void test_gltf_preload_bundle_rejects_short_glb_bin() {
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":4}]}";
    while ((json.size() & 3u) != 0)
        json.push_back(' ');

    std::vector<uint8_t> glb;
    glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
    append_u32_le(glb, 2);
    append_u32_le(glb, (uint32_t)(12 + 8 + json.size() + 8));
    append_u32_le(glb, (uint32_t)json.size());
    append_u32_le(glb, 0x4E4F534Au);
    glb.insert(glb.end(), json.begin(), json.end());
    append_u32_le(glb, 0);
    append_u32_le(glb, 0x004E4942u);

    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(glb.size()));
    EXPECT_TRUE(owned_root != nullptr, "Short-GLB preload root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, glb.data(), glb.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle =
        rt_gltf_preload_bundle_create(rt_const_cstr("/tmp/zanna_gltf_preload_short_bin.glb"),
                                      owned_root,
                                      glb.size(),
                                      0,
                                      error,
                                      sizeof(error));
    EXPECT_TRUE(bundle == nullptr, "Preload rejects GLB BIN chunks shorter than buffer byteLength");
    EXPECT_TRUE(std::strstr(error, "failed to stage glTF dependency") != nullptr,
                "Short GLB BIN preload reports a staging error");
}

static void test_gltf_preload_bundle_validates_accessor_ranges() {
    const char *gltf_path = "/tmp/zanna_gltf_preload_invalid_accessor_range.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 11.0f, 0.0f};
    for (float v : positions)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json = "{"
                            "\"asset\":{\"version\":\"2.0\"},"
                            "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
                            buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
                            "}],"
                            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":24}],"
                            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                            "\"type\":\"VEC3\"}],"
                            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]"
                            "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Invalid-accessor preload glTF fixture can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Invalid-accessor root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Invalid-accessor root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle == nullptr, "Preload validates accessor byte ranges on worker");
    EXPECT_TRUE(std::strstr(error, "invalid glTF accessor range") != nullptr,
                "Invalid accessor range reports a worker validation error");
}

static void test_gltf_preload_bundle_rejects_corrupt_required_image_payload() {
    const char *gltf_path = "/tmp/zanna_gltf_preload_corrupt_required_image.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"images\":[{\"uri\":\"data:image/png;base64,AAAA\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Corrupt required-image preload fixture can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Corrupt required-image root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Corrupt required-image root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle == nullptr, "Preload rejects corrupt required texture image payloads");
    EXPECT_TRUE(std::strstr(error, "invalid glTF image payload") != nullptr,
                "Corrupt required image reports a preload image error");
}

static void test_gltf_preload_bundle_stages_data_uri_buffers_and_images() {
    const char *png_path = "/tmp/zanna_gltf_preload_data_uri.png";
    const char *gltf_path = "/tmp/zanna_gltf_preload_data_uri.gltf";
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x5588CCFFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "Preload data-uri PNG can be written");

    std::vector<uint8_t> png_bytes;
    EXPECT_TRUE(read_file_bytes(png_path, png_bytes), "Preload data-uri PNG can be read");
    if (png_bytes.empty())
        return;

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f, 0.0f, 7.0f, 0.0f};
    const float uv0[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uv0)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string image_b64 = base64_encode(png_bytes.data(), png_bytes.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"images\":[{\"uri\":\"data:image/png;base64," +
        image_b64 +
        "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,"
        "\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Preload data-uri glTF can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload data-uri root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload data-uri root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage data-uri payloads");
    EXPECT_TRUE(error[0] == '\0', "Preload data-uri bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 4,
                "Preload bundle stages buffer, exact PNG source, decoded image, and mesh POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_count(bundle) == 1,
                "Preload bundle worker-decodes PNG image bytes to raw RGBA POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                "Preload bundle worker-decodes the data-uri static mesh to POD");
    if (!bundle)
        return;

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Preloaded data-uri bundle builds an asset");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    auto *material = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "Preloaded data-uri buffer builds the expected mesh");
    EXPECT_TRUE(material != nullptr && material->texture != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 0) ==
                        0x5588CCFFll,
                "Preloaded data-uri image builds the expected material texture");
    EXPECT_TRUE(gltf_test_texture_source_equals(material->texture, "png", png_bytes),
                "Preloaded data-URI image retains its exact PNG source");
    if (mesh) {
        EXPECT_NEAR(mesh->vertices[1].pos[0], 6.0, 0.001, "Data-uri preload vertex X loads");
        EXPECT_NEAR(mesh->vertices[2].pos[1], 7.0, 0.001, "Data-uri preload vertex Y loads");
    }
    std::remove(png_path);
}

static void test_gltf_preload_bundle_decodes_bmp_images_to_rgba_pod() {
    const char *bmp_path = "/tmp/zanna_gltf_preload_data_uri.bmp";
    const char *gltf_path = "/tmp/zanna_gltf_preload_data_uri_bmp.gltf";
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0xCC8844FFll);
    EXPECT_TRUE(rt_pixels_save_bmp(pixels, rt_const_cstr(bmp_path)) == 1,
                "Preload data-uri BMP can be written");

    std::vector<uint8_t> bmp_bytes;
    EXPECT_TRUE(read_file_bytes(bmp_path, bmp_bytes), "Preload data-uri BMP can be read");
    if (bmp_bytes.empty())
        return;

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 12.0f, 0.0f, 0.0f, 0.0f, 13.0f, 0.0f};
    const float uv0[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uv0)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string image_b64 = base64_encode(bmp_bytes.data(), bmp_bytes.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"images\":[{\"uri\":\"data:image/bmp;base64," +
        image_b64 +
        "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,"
        "\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Preload data-uri BMP glTF can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload data-uri BMP root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload data-uri BMP root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage BMP payloads");
    EXPECT_TRUE(error[0] == '\0', "Preload data-uri BMP bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 4,
                "Preload bundle stages buffer, exact BMP source, decoded image, and mesh POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_count(bundle) == 1,
                "Preload bundle worker-decodes BMP image bytes to raw RGBA POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                "Preload bundle worker-decodes the BMP fixture static mesh to POD");
    if (!bundle)
        return;

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Preloaded BMP data-uri bundle builds an asset");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    auto *material = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "Preloaded BMP data-uri buffer builds the expected mesh");
    EXPECT_TRUE(material != nullptr && material->texture != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 0) ==
                        0xCC8844FFll,
                "Preloaded worker-decoded BMP image builds the expected material texture");
    EXPECT_TRUE(gltf_test_texture_source_equals(material->texture, "bmp", bmp_bytes),
                "Preloaded data-URI image retains its exact BMP source");
    if (mesh) {
        EXPECT_NEAR(mesh->vertices[1].pos[0], 12.0, 0.001, "BMP preload vertex X loads");
        EXPECT_NEAR(mesh->vertices[2].pos[1], 13.0, 0.001, "BMP preload vertex Y loads");
    }
    std::remove(bmp_path);
}

static void test_gltf_preload_bundle_decodes_jpeg_images_to_rgba_pod() {
    const char *gltf_path = "/tmp/zanna_gltf_preload_data_uri_jpeg.gltf";
    const char *image_b64 =
        "/9j/4AAQSkZJRgABAQAASABIAAD/4QBMRXhpZgAATU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAA"
        "A6ABAAMAAAABAAEAAKACAAQAAAABAAAAAaADAAQAAAABAAAAAQAAAAD/7QA4UGhvdG9zaG9w"
        "IDMuMAA4QklNBAQAAAAAAAA4QklNBCUAAAAAABDUHYzZjwCyBOmACZjs+EJ+/8AAEQgAAQAB"
        "AwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMC"
        "BAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBka"
        "JSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqS"
        "k5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq"
        "8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQE"
        "AwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEX"
        "GBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJ"
        "ipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp"
        "6vLz9PX29/j5+v/bAEMAAgICAgICAwICAwUDAwMFBgUFBQUGCAYGBgYGCAoICAgICAgKCgoK"
        "CgoKCgwMDAwMDA4ODg4ODw8PDw8PDw8PD//bAEMBAgICBAQEBwQEBxALCQsQEBAQEBAQEBAQ"
        "EBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEP/dAAQAAf/aAAwDAQAC"
        "EQMRAD8A9Uooor+Sz+oD/9k=";

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 16.0f, 0.0f, 0.0f, 0.0f, 17.0f, 0.0f};
    const float uv0[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uv0)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"images\":[{\"uri\":\"data:image/jpeg;base64," +
        std::string(image_b64) +
        "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,"
        "\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Preload data-uri JPEG glTF can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload data-uri JPEG root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload data-uri JPEG root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage JPEG payloads");
    EXPECT_TRUE(error[0] == '\0', "Preload data-uri JPEG bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 4,
                "Preload bundle stages buffer, exact JPEG source, decoded image, and mesh POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_count(bundle) == 1,
                "Preload bundle worker-decodes JPEG image bytes to raw RGBA POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                "Preload bundle worker-decodes the JPEG fixture static mesh to POD");
    if (!bundle)
        return;

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Preloaded JPEG data-uri bundle builds an asset");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    auto *material = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "Preloaded JPEG data-uri buffer builds the expected mesh");
    int64_t rgba = material && material->texture
                       ? rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 0)
                       : 0;
    int red = (int)((rgba >> 24) & 0xFF);
    int green = (int)((rgba >> 16) & 0xFF);
    int blue = (int)((rgba >> 8) & 0xFF);
    int alpha = (int)(rgba & 0xFF);
    EXPECT_TRUE(material != nullptr && material->texture != nullptr && red >= 140 && green >= 60 &&
                    green <= 200 && blue <= 160 && alpha == 0xFF,
                "Preloaded worker-decoded JPEG image builds the expected material texture");
    {
        const uint8_t *source = nullptr;
        uint64_t source_size = 0;
        const char *source_kind = nullptr;
        EXPECT_TRUE(material && material->texture &&
                        rt_textureasset3d_get_source_container(
                            material->texture, &source, &source_size, &source_kind) &&
                        source_kind && std::strcmp(source_kind, "jpeg") == 0 &&
                        base64_encode(source, (size_t)source_size) == image_b64,
                    "Preloaded data-URI image retains its exact JPEG source");
    }
    if (mesh) {
        EXPECT_NEAR(mesh->vertices[1].pos[0], 16.0, 0.001, "JPEG preload vertex X loads");
        EXPECT_NEAR(mesh->vertices[2].pos[1], 17.0, 0.001, "JPEG preload vertex Y loads");
    }
}

static void test_gltf_preload_bundle_decodes_gif_images_to_rgba_pod() {
    const char *gltf_path = "/tmp/zanna_gltf_preload_data_uri_gif.gltf";
    const uint8_t gif_bytes[] = {'G',  'I',  'F',  '8',  '7',  'a',  0x01, 0x00, 0x01,
                                 0x00, 0x80, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
                                 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3B};

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 18.0f, 0.0f, 0.0f, 0.0f, 19.0f, 0.0f};
    const float uv0[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uv0)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string image_b64 = base64_encode(gif_bytes, sizeof(gif_bytes));
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"images\":[{\"uri\":\"data:image/gif;base64," +
        image_b64 +
        "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,"
        "\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Preload data-uri GIF glTF can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload data-uri GIF root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload data-uri GIF root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage GIF payloads");
    EXPECT_TRUE(error[0] == '\0', "Preload data-uri GIF bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 4,
                "Preload bundle stages buffer, exact GIF source, decoded image, and mesh POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_count(bundle) == 1,
                "Preload bundle worker-decodes GIF image bytes to raw RGBA POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                "Preload bundle worker-decodes the GIF fixture static mesh to POD");
    if (!bundle)
        return;

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Preloaded GIF data-uri bundle builds an asset");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    auto *material = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "Preloaded GIF data-uri buffer builds the expected mesh");
    EXPECT_TRUE(material != nullptr && material->texture != nullptr &&
                    rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 0) ==
                        0xFF0000FFll,
                "Preloaded worker-decoded GIF image builds the expected material texture");
    EXPECT_TRUE(gltf_test_texture_source_equals_bytes(
                    material->texture, "gif", gif_bytes, sizeof(gif_bytes)),
                "Preloaded data-URI image retains its exact GIF source");
    if (mesh) {
        EXPECT_NEAR(mesh->vertices[1].pos[0], 18.0, 0.001, "GIF preload vertex X loads");
        EXPECT_NEAR(mesh->vertices[2].pos[1], 19.0, 0.001, "GIF preload vertex Y loads");
    }
}

static void test_gltf_preload_bundle_decodes_static_mesh_to_pod() {
    const char *gltf_path = "/tmp/zanna_gltf_preload_static_mesh_pod.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 14.0f, 0.0f, 0.0f, 0.0f, 15.0f, 0.0f};
    const float normals[9] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const float uvs[6] = {0.0f, 0.0f, 1.0f, 0.25f, 0.5f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : normals)
        append_bytes(gltf_buffer, v);
    for (float v : uvs)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
        "\"TEXCOORD_0\":2},\"indices\":3}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Preload static-mesh POD glTF fixture can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload static-mesh POD root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload static-mesh POD root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage a static mesh POD payload");
    EXPECT_TRUE(error[0] == '\0', "Preload static-mesh POD bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 2,
                "Preload bundle stages the raw buffer plus decoded static mesh POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                "Preload bundle worker-decodes the static triangle mesh to POD");
    if (!bundle)
        return;

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Preloaded static-mesh POD bundle builds an asset");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "Preloaded static-mesh POD builds the expected mesh");
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_capacity == 3 && mesh->index_capacity == 3,
                "Preloaded static-mesh path commits the worker POD mesh directly");
    if (!mesh)
        return;
    EXPECT_NEAR(mesh->vertices[1].pos[0], 14.0, 0.001, "Static-mesh POD vertex X loads");
    EXPECT_NEAR(mesh->vertices[2].pos[1], 15.0, 0.001, "Static-mesh POD vertex Y loads");
    EXPECT_NEAR(mesh->vertices[0].normal[2], 1.0, 0.001, "Static-mesh POD normals load");
    EXPECT_NEAR(mesh->vertices[1].uv[1], 0.25, 0.001, "Static-mesh POD UVs load");
}

static void test_gltf_preload_bundle_decodes_strip_and_fan_without_normals_to_pod() {
    const char *gltf_path = "/tmp/zanna_gltf_preload_topology_mesh_pod.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float strip_positions[12] = {
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
    };
    const float fan_positions[12] = {
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
    };
    const float uvs[8] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};

    for (float v : strip_positions)
        append_bytes(gltf_buffer, v);
    for (float v : uvs)
        append_bytes(gltf_buffer, v);
    for (float v : fan_positions)
        append_bytes(gltf_buffer, v);
    for (float v : uvs)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":32},"
        "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":32}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"}"
        "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"mode\":5},"
        "{\"attributes\":{\"POSITION\":2,\"TEXCOORD_0\":3},\"mode\":6}"
        "]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Preload topology POD glTF fixture can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload topology POD root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload topology POD root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage strip/fan mesh POD payloads");
    EXPECT_TRUE(error[0] == '\0', "Preload topology POD bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 3,
                "Preload bundle stages the raw buffer plus two topology mesh PODs");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 2,
                "Preload bundle worker-decodes triangle strip and fan primitives to POD");
    if (!bundle)
        return;

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Preloaded topology POD bundle builds an asset");
    if (!asset)
        return;
    auto *strip_mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    auto *fan_mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 1));
    EXPECT_TRUE(strip_mesh != nullptr && strip_mesh->vertex_count == 4 &&
                    strip_mesh->index_count == 6,
                "Preloaded triangle-strip POD builds two triangles");
    EXPECT_TRUE(fan_mesh != nullptr && fan_mesh->vertex_count == 4 && fan_mesh->index_count == 6,
                "Preloaded triangle-fan POD builds two triangles");
    EXPECT_TRUE(strip_mesh != nullptr && strip_mesh->vertex_capacity == 4 &&
                    strip_mesh->index_capacity == 6,
                "Preloaded triangle-strip POD commits direct mesh storage");
    EXPECT_TRUE(fan_mesh != nullptr && fan_mesh->vertex_capacity == 4 &&
                    fan_mesh->index_capacity == 6,
                "Preloaded triangle-fan POD commits direct mesh storage");
    if (!strip_mesh || !fan_mesh)
        return;
    EXPECT_TRUE(strip_mesh->indices[0] == 0 && strip_mesh->indices[1] == 1 &&
                    strip_mesh->indices[2] == 2 && strip_mesh->indices[3] == 2 &&
                    strip_mesh->indices[4] == 1 && strip_mesh->indices[5] == 3,
                "Preloaded triangle-strip POD preserves alternating winding");
    EXPECT_TRUE(fan_mesh->indices[0] == 0 && fan_mesh->indices[1] == 1 &&
                    fan_mesh->indices[2] == 2 && fan_mesh->indices[3] == 0 &&
                    fan_mesh->indices[4] == 2 && fan_mesh->indices[5] == 3,
                "Preloaded triangle-fan POD preserves fan winding");
    EXPECT_NEAR(strip_mesh->vertices[0].normal[2],
                1.0,
                0.001,
                "Preloaded strip POD recalculates missing normals on commit");
    EXPECT_NEAR(fan_mesh->vertices[0].normal[2],
                1.0,
                0.001,
                "Preloaded fan POD recalculates missing normals on commit");
}

static void test_gltf_preload_bundle_stages_buffer_view_images() {
    const char *png_path = "/tmp/zanna_gltf_preload_bufferview_image.png";
    const char *gltf_path = "/tmp/zanna_gltf_preload_bufferview_image.gltf";
    void *pixels = rt_pixels_new(2, 2);
    rt_pixels_set(pixels, 0, 0, 0xAA6633FFll);
    rt_pixels_set(pixels, 1, 0, 0x11223344ll);
    rt_pixels_set(pixels, 0, 1, 0x55667788ll);
    rt_pixels_set(pixels, 1, 1, 0x99AABBCCll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "Preload bufferView PNG can be written");

    std::vector<uint8_t> png_bytes;
    EXPECT_TRUE(read_file_bytes(png_path, png_bytes), "Preload bufferView PNG can be read");
    if (png_bytes.empty())
        return;

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 8.0f, 0.0f, 0.0f, 0.0f, 9.0f, 0.0f};
    const float uv0[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uv0)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    size_t image_offset = gltf_buffer.size();
    gltf_buffer.insert(gltf_buffer.end(), png_bytes.begin(), png_bytes.end());

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(image_offset) + ",\"byteLength\":" + std::to_string(png_bytes.size()) +
        "}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,"
        "\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Preload bufferView glTF can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Preload bufferView root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Preload bufferView root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage bufferView image payloads");
    EXPECT_TRUE(error[0] == '\0', "Preload bufferView bundle build has no terminal error");
    EXPECT_TRUE(rt_gltf_preload_bundle_dependency_count(bundle) == 4,
                "Preload bundle stages buffer, exact bufferView PNG, decoded image, and mesh POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_count(bundle) == 1,
                "Preload bundle worker-decodes bufferView PNG image bytes to raw RGBA POD");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                "Preload bundle worker-decodes the bufferView-image fixture mesh to POD");
    if (!bundle)
        return;

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Preloaded bufferView bundle builds an asset");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    auto *material = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "Preloaded bufferView fixture builds the expected mesh");
    EXPECT_TRUE(
        material != nullptr && material->texture != nullptr &&
            rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 0) == 0xAA6633FFll &&
            rt_pixels_get(gltf_test_texture_pixels(material->texture), 1, 0) == 0x11223344ll &&
            rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 1) == 0x55667788ll &&
            rt_pixels_get(gltf_test_texture_pixels(material->texture), 1, 1) == 0x99AABBCCll,
        "Preloaded bufferView image builds the expected material texture");
    EXPECT_TRUE(material != nullptr && material->texture != nullptr &&
                    rt_pixels_generation(gltf_test_texture_pixels(material->texture)) <= 1,
                "Preloaded RGBA POD commits texture pixels through one bulk mutation");
    EXPECT_TRUE(gltf_test_texture_source_equals(material->texture, "png", png_bytes),
                "Preloaded bufferView image retains its exact PNG source");
    if (mesh) {
        EXPECT_NEAR(mesh->vertices[1].pos[0], 8.0, 0.001, "BufferView preload vertex X loads");
        EXPECT_NEAR(mesh->vertices[2].pos[1], 9.0, 0.001, "BufferView preload vertex Y loads");
    }
    std::remove(png_path);
}

static void test_gltf_preload_bundle_slices_decoded_image_commit() {
    const char *png_path = "/tmp/zanna_gltf_sliced_preload_texture.png";
    const char *gltf_path = "/tmp/zanna_gltf_sliced_preload_texture.gltf";

    void *pixels = rt_pixels_new(32, 32);
    EXPECT_TRUE(pixels != nullptr, "Sliced preload source Pixels can be allocated");
    if (!pixels)
        return;
    for (int64_t y = 0; y < 32; ++y) {
        for (int64_t x = 0; x < 32; ++x)
            rt_pixels_set(pixels, x, y, 0xAA7744FFll);
    }
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "Sliced preload PNG can be written");

    std::vector<uint8_t> png_bytes;
    EXPECT_TRUE(read_file_bytes(png_path, png_bytes), "Sliced preload PNG can be read");
    if (png_bytes.empty())
        return;

    std::string image_b64 = base64_encode(png_bytes.data(), png_bytes.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"images\":[{\"uri\":\"data:image/png;base64," +
        image_b64 +
        "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Sliced preload glTF fixture can be written");

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Sliced preload root bytes can be read");
    if (root.empty())
        return;
    uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
    EXPECT_TRUE(owned_root != nullptr, "Sliced preload root copy can be allocated");
    if (!owned_root)
        return;
    std::memcpy(owned_root, root.data(), root.size());

    char error[128] = {0};
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
        rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle != nullptr, "Sliced preload bundle can stage decoded image POD");
    EXPECT_TRUE(error[0] == '\0', "Sliced preload bundle build has no terminal error");
    if (!bundle)
        return;
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_count(bundle) == 1,
                "Sliced preload starts with one decoded RGBA image");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_bytes(bundle) == 32u * 32u * 4u,
                "Sliced preload decoded-byte estimate starts at full RGBA size");
    EXPECT_TRUE(rt_gltf_preload_bundle_next_decoded_image_slice_bytes(bundle, 256u) == 256u,
                "Sliced preload exposes a bounded first commit slice");

    size_t slices = 0;
    size_t prepared_total = 0;
    for (;;) {
        size_t prepared = rt_gltf_preload_bundle_prepare_decoded_image_slice(bundle, 256u);
        if (prepared == 0u)
            break;
        prepared_total += prepared;
        slices++;
    }
    EXPECT_TRUE(slices > 1, "Sliced preload decoded image requires multiple commit slices");
    EXPECT_TRUE(prepared_total == 32u * 32u * 4u,
                "Sliced preload prepares exactly the decoded RGBA payload");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_count(bundle) == 0,
                "Sliced preload converts decoded RGBA into a prepared Pixels dependency");
    EXPECT_TRUE(rt_gltf_preload_bundle_decoded_image_bytes(bundle) == 0,
                "Sliced preload has no remaining decoded RGBA bytes after preparation");

    void *asset = rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
    EXPECT_TRUE(asset != nullptr, "Sliced preload bundle builds an asset after sliced prep");
    if (asset) {
        auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
        EXPECT_TRUE(mat != nullptr && mat->texture != nullptr,
                    "Sliced preload material keeps the prepared Pixels texture");
        EXPECT_TRUE(mat != nullptr && mat->texture != nullptr &&
                        rt_pixels_get(gltf_test_texture_pixels(mat->texture), 17, 13) ==
                            0xAA7744FFll,
                    "Sliced preload prepared texture preserves pixel contents");
        EXPECT_TRUE(gltf_test_texture_source_equals(mat->texture, "png", png_bytes),
                    "Sliced preload keeps encoded PNG bytes beside prepared Pixels");
    }
    std::remove(gltf_path);
    std::remove(png_path);
}

static void test_gltf_load_asset_resolves_mounted_external_buffers() {
    const char *pack_path = "/tmp/zanna_gltf_asset_pack.zpak";
    const char *png_path = "/tmp/zanna_gltf_asset_texture.png";
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x224466FFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "Package texture PNG can be written");
    std::vector<uint8_t> png_bytes;
    EXPECT_TRUE(read_file_bytes(png_path, png_bytes), "Package texture PNG can be read");
    if (png_bytes.empty())
        return;

    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f};
    const float uv0[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uv0)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"buffers/tri.bin\",\"byteLength\":" +
        std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"images\":[{\"uri\":\"textures/albedo.png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,"
        "\"material\":0}]}]"
        "}";

    zanna::asset::ZpakWriter writer;
    writer.addEntry("assets/models/tri.gltf",
                    reinterpret_cast<const uint8_t *>(gltf_json.data()),
                    gltf_json.size(),
                    false);
    writer.addEntry("assets/models/buffers/tri.bin", gltf_buffer.data(), gltf_buffer.size(), false);
    writer.addEntry("assets/models/textures/albedo.png", png_bytes.data(), png_bytes.size(), false);
    std::string err;
    bool wrote_pack = writer.writeToFile(pack_path, err);
    EXPECT_TRUE(wrote_pack, "Mounted glTF asset pack can be written");
    if (!err.empty())
        std::fprintf(stderr, "ZPAK write detail: %s\n", err.c_str());
    if (!wrote_pack)
        return;

    bool mounted = rt_asset_mount(rt_const_cstr(pack_path)) == 1;
    EXPECT_TRUE(mounted, "Mounted glTF asset pack can mount");
    if (!mounted)
        return;
    void *asset = rt_gltf_load_asset(rt_const_cstr("asset://assets/models/tri.gltf"));
    EXPECT_TRUE(asset != nullptr, "GLTF.LoadAsset loads a root model from a mounted asset URI");
    if (asset) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
        auto *material = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                    "GLTF.LoadAsset imports geometry from a mounted external buffer");
        EXPECT_TRUE(material != nullptr && material->texture != nullptr &&
                        rt_pixels_get(gltf_test_texture_pixels(material->texture), 0, 0) ==
                            0x224466FFll,
                    "GLTF.LoadAsset imports package-relative external textures");
        EXPECT_TRUE(gltf_test_texture_source_equals(material->texture, "png", png_bytes),
                    "GLTF.LoadAsset retains exact packed external texture bytes");
        if (mesh) {
            EXPECT_NEAR(
                mesh->vertices[1].pos[0], 4.0, 0.001, "Mounted external buffer vertex X is loaded");
            EXPECT_NEAR(
                mesh->vertices[2].pos[1], 5.0, 0.001, "Mounted external buffer vertex Y is loaded");
        }
    }

    rt_asset_unmount(rt_const_cstr(pack_path));
    std::remove(png_path);
    std::remove(pack_path);
}

static std::vector<uint8_t> make_triangle_glb(float x1, float y2) {
    std::vector<uint8_t> bin;
    const float positions[9] = {0.0f, 0.0f, 0.0f, x1, 0.0f, 0.0f, 0.0f, y2, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(bin, v);
    for (uint16_t v : indices)
        append_bytes(bin, v);
    size_t gltf_byte_length = bin.size();
    while ((bin.size() & 3u) != 0)
        bin.push_back(0);

    std::string json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":" +
        std::to_string(gltf_byte_length) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]"
        "}";
    while ((json.size() & 3u) != 0)
        json.push_back(' ');

    std::vector<uint8_t> glb;
    glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
    append_u32_le(glb, 2);
    append_u32_le(glb, (uint32_t)(12 + 8 + json.size() + 8 + bin.size()));
    append_u32_le(glb, (uint32_t)json.size());
    append_u32_le(glb, 0x4E4F534Au);
    glb.insert(glb.end(), json.begin(), json.end());
    append_u32_le(glb, (uint32_t)bin.size());
    append_u32_le(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

static void test_gltf_load_asset_handles_glb_filesystem_and_mounted_package() {
    const char *glb_path = "/tmp/zanna_gltf_asset_triangle.glb";
    const char *pack_path = "/tmp/zanna_gltf_asset_glb_pack.zpak";
    std::vector<uint8_t> glb = make_triangle_glb(8.0f, 9.0f);

    FILE *f = std::fopen(glb_path, "wb");
    EXPECT_TRUE(f != nullptr, "GLB fixture can be written");
    if (!f)
        return;
    std::fwrite(glb.data(), 1, glb.size(), f);
    std::fclose(f);

    void *fs_asset = rt_gltf_load_asset(rt_const_cstr(glb_path));
    EXPECT_TRUE(fs_asset != nullptr, "GLTF.LoadAsset loads GLB through filesystem fallback");
    if (fs_asset) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(fs_asset, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                    "Filesystem GLB imports embedded BIN geometry");
        if (mesh) {
            EXPECT_NEAR(mesh->vertices[1].pos[0], 8.0, 0.001, "Filesystem GLB vertex X loads");
            EXPECT_NEAR(mesh->vertices[2].pos[1], 9.0, 0.001, "Filesystem GLB vertex Y loads");
        }
    }

    zanna::asset::ZpakWriter writer;
    writer.addEntry("assets/models/tri.glb", glb.data(), glb.size(), false);
    std::string err;
    bool wrote_pack = writer.writeToFile(pack_path, err);
    EXPECT_TRUE(wrote_pack, "GLB asset pack can be written");
    if (!wrote_pack)
        return;
    bool mounted = rt_asset_mount(rt_const_cstr(pack_path)) == 1;
    EXPECT_TRUE(mounted, "GLB asset pack can mount");
    if (!mounted)
        return;

    void *pack_asset = rt_gltf_load_asset(rt_const_cstr("asset://assets/models/tri.glb"));
    EXPECT_TRUE(pack_asset != nullptr, "GLTF.LoadAsset loads GLB from a mounted package");
    if (pack_asset) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(pack_asset, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                    "Mounted GLB imports embedded BIN geometry");
    }

    rt_asset_unmount(rt_const_cstr(pack_path));
    std::remove(pack_path);
    std::remove(glb_path);
}

static void test_gltf_rejects_out_of_range_indices() {
    const char *gltf_path = "/tmp/zanna_gltf_bad_indices.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 99};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]"
        "}";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Invalid-index glTF fixture can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load tolerates malformed primitive indices");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_mesh_count(asset) == 0,
                "GLTF.Load skips primitives whose indices reference missing vertices");
}

static void test_gltf_skips_non_triangle_primitives() {
    const char *gltf_path = "/tmp/zanna_gltf_skip_lines.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1},"
        "{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":1}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Line-primitive glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load keeps triangle primitives when lines are present");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_mesh_count(asset) == 1,
                "GLTF.Load skips non-triangle primitive modes without failing the asset");
    EXPECT_TRUE(load_warnings_contain("unsupported"),
                "Skipped non-triangle primitives record a load warning");
    EXPECT_TRUE(import_report_contains("\"skippedPrimitives\":1"),
                "Import report counts skipped non-triangle primitives");
}

static void test_gltf_drops_invalid_optional_attributes() {
    const char *gltf_path = "/tmp/zanna_gltf_bad_optional_normal.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float bad_normals[6] = {9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : bad_normals)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
        "\"indices\":2}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Invalid optional-attribute glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load drops malformed optional attributes");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->index_count == 3,
                "GLTF.Load keeps geometry after dropping malformed normals");
    if (mesh)
        EXPECT_NEAR(mesh->vertices[0].normal[2],
                    1.0,
                    0.001,
                    "GLTF.Load recalculates normals after dropping malformed normals");
}

static void test_gltf_rejects_unsorted_sparse_indices() {
    const char *gltf_path = "/tmp/zanna_gltf_bad_sparse_order.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float base_positions[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const uint16_t sparse_indices[2] = {2, 1};
    const float sparse_values[6] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : base_positions)
        append_bytes(gltf_buffer, v);
    size_t sparse_idx_off = gltf_buffer.size();
    for (uint16_t v : sparse_indices)
        append_bytes(gltf_buffer, v);
    size_t sparse_value_off = gltf_buffer.size();
    for (float v : sparse_values)
        append_bytes(gltf_buffer, v);
    size_t index_off = gltf_buffer.size();
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(sparse_idx_off) +
        ",\"byteLength\":4},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(sparse_value_off) +
        ",\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(index_off) +
        ",\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"sparse\":{\"count\":2,\"indices\":{\"bufferView\":1,\"componentType\":5123},"
        "\"values\":{\"bufferView\":2}}},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Unsorted sparse glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset == nullptr, "GLTF.Load rejects unsorted sparse accessor indices");
}

static void test_gltf_rejects_invalid_skin_reference() {
    const char *gltf_path = "/tmp/zanna_gltf_invalid_skin_ref.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"nodes\":[{\"mesh\":0,\"skin\":4}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Invalid-skin-reference glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset == nullptr, "GLTF.Load rejects nodes that reference missing skins");
}

static void test_gltf_rejects_invalid_skin_joint_tables() {
    const char *bad_index_path = "/tmp/zanna_gltf_invalid_skin_joint_index.gltf";
    std::string bad_index_json = "{"
                                 "\"asset\":{\"version\":\"2.0\"},"
                                 "\"nodes\":[{\"name\":\"Joint\"}],"
                                 "\"skins\":[{\"joints\":[1]}],"
                                 "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
                                 "}";
    EXPECT_TRUE(write_text_file(bad_index_path, bad_index_json),
                "Invalid skin-joint index glTF fixture can be written");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(bad_index_path)) == nullptr,
                "GLTF.Load rejects skin joints outside the node table");

    const char *duplicate_path = "/tmp/zanna_gltf_duplicate_skin_joints.gltf";
    std::string duplicate_json = "{"
                                 "\"asset\":{\"version\":\"2.0\"},"
                                 "\"nodes\":[{\"name\":\"Joint\"}],"
                                 "\"skins\":[{\"joints\":[0,0]}],"
                                 "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
                                 "}";
    EXPECT_TRUE(write_text_file(duplicate_path, duplicate_json),
                "Duplicate skin-joint glTF fixture can be written");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(duplicate_path)) == nullptr,
                "GLTF.Load rejects duplicate skin joint entries");
}

static void test_gltf_builds_scene_hierarchy_for_active_scene() {
    const char *gltf_path = "/tmp/zanna_gltf_scene_graph.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float normals[9] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const float uvs[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : normals)
        append_bytes(gltf_buffer, v);
    for (float v : uvs)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"buffers\": [{\"uri\": \"data:application/octet-stream;base64," +
        buffer_b64 + "\", \"byteLength\": " + std::to_string(gltf_buffer.size()) +
        "}],\n"
        "  \"bufferViews\": [\n"
        "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
        "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36},\n"
        "    {\"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 24},\n"
        "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 6}\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC2\"},\n"
        "    {\"bufferView\": 3, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\"}\n"
        "  ],\n"
        "  \"materials\": [{\n"
        "    \"pbrMetallicRoughness\": {\n"
        "      \"baseColorFactor\": [0.2, 0.6, 0.8, 1.0]\n"
        "    }\n"
        "  }],\n"
        "  \"meshes\": [{\"primitives\": [{\n"
        "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2},\n"
        "    \"indices\": 3,\n"
        "    \"material\": 0\n"
        "  }]}],\n"
        "  \"nodes\": [\n"
        "    {\"name\": \"RootNode\", \"translation\": [1.0, 2.0, 3.0], \"mesh\": 0, "
        "\"children\": [1]},\n"
        "    {\"name\": \"ChildNode\", \"scale\": [2.0, 3.0, 4.0]}\n"
        "  ],\n"
        "  \"scenes\": [{\"nodes\": [0]}],\n"
        "  \"scene\": 0\n"
        "}\n";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Scene graph glTF file can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses scene graph assets");
    if (!asset)
        return;

    EXPECT_TRUE(rt_gltf_node_count(asset) == 2, "GLTF.Load counts logical scene nodes");
    void *scene_root = rt_gltf_get_scene_root(asset);
    EXPECT_TRUE(scene_root != nullptr, "GLTF.Load exposes a reusable scene root");
    if (!scene_root)
        return;

    EXPECT_TRUE(rt_scene_node3d_child_count(scene_root) == 1,
                "GLTF scene root contains one active-scene child");

    void *root_node = rt_scene_node3d_find(scene_root, rt_const_cstr("RootNode"));
    void *child_node = rt_scene_node3d_find(scene_root, rt_const_cstr("ChildNode"));
    EXPECT_TRUE(root_node != nullptr, "GLTF scene graph preserves named root nodes");
    EXPECT_TRUE(child_node != nullptr, "GLTF scene graph preserves named child nodes");
    if (!root_node || !child_node)
        return;

    EXPECT_TRUE(rt_scene_node3d_get_parent(root_node) == scene_root,
                "Top-level glTF nodes attach below the synthetic scene root");
    EXPECT_TRUE(rt_scene_node3d_get_parent(child_node) == root_node,
                "glTF child nodes preserve hierarchy");
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(root_node)),
                1.0,
                0.001,
                "GLTF root node preserves translation.x");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(root_node)),
                2.0,
                0.001,
                "GLTF root node preserves translation.y");
    EXPECT_NEAR(rt_vec3_z(rt_scene_node3d_get_position(root_node)),
                3.0,
                0.001,
                "GLTF root node preserves translation.z");
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_scale(child_node)),
                2.0,
                0.001,
                "GLTF child node preserves scale.x");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_scale(child_node)),
                3.0,
                0.001,
                "GLTF child node preserves scale.y");
    EXPECT_NEAR(rt_vec3_z(rt_scene_node3d_get_scale(child_node)),
                4.0,
                0.001,
                "GLTF child node preserves scale.z");
    EXPECT_TRUE(rt_scene_node3d_get_mesh(root_node) == rt_gltf_get_mesh(asset, 0),
                "GLTF root node reuses extracted mesh objects");
    EXPECT_TRUE(rt_scene_node3d_get_material(root_node) == rt_gltf_get_material(asset, 0),
                "GLTF root node reuses extracted material objects");
}

static void test_gltf_imports_extended_vertex_attributes_and_triangle_strips() {
    const char *gltf_path = "/tmp/zanna_gltf_extended_attrs.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[12] = {
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
    };
    const float normals[12] = {
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    const float uvs[8] = {
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        1.0f,
    };
    const uint8_t colors[16] = {
        255,
        0,
        0,
        255,
        0,
        255,
        0,
        255,
        0,
        0,
        255,
        255,
        255,
        255,
        255,
        128,
    };
    const float tangents[16] = {
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        0.0f,
        -1.0f,
    };
    const uint8_t joints[16] = {
        1,
        2,
        3,
        4,
        4,
        3,
        2,
        1,
        5,
        6,
        7,
        8,
        8,
        7,
        6,
        5,
    };
    const uint8_t weights[16] = {
        255,
        0,
        0,
        0,
        128,
        127,
        0,
        0,
        64,
        64,
        64,
        63,
        0,
        0,
        255,
        0,
    };
    const uint16_t indices[4] = {0, 1, 2, 3};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : normals)
        append_bytes(gltf_buffer, v);
    for (float v : uvs)
        append_bytes(gltf_buffer, v);
    for (uint8_t v : colors)
        append_bytes(gltf_buffer, v);
    for (float v : tangents)
        append_bytes(gltf_buffer, v);
    for (uint8_t v : joints)
        append_bytes(gltf_buffer, v);
    for (uint8_t v : weights)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"buffers\": [{\"uri\": \"data:application/octet-stream;base64," +
        buffer_b64 + "\", \"byteLength\": " + std::to_string(gltf_buffer.size()) +
        "}],\n"
        "  \"bufferViews\": [\n"
        "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 48},\n"
        "    {\"buffer\": 0, \"byteOffset\": 48, \"byteLength\": 48},\n"
        "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 32},\n"
        "    {\"buffer\": 0, \"byteOffset\": 128, \"byteLength\": 16},\n"
        "    {\"buffer\": 0, \"byteOffset\": 144, \"byteLength\": 64},\n"
        "    {\"buffer\": 0, \"byteOffset\": 208, \"byteLength\": 16},\n"
        "    {\"buffer\": 0, \"byteOffset\": 224, \"byteLength\": 16},\n"
        "    {\"buffer\": 0, \"byteOffset\": 240, \"byteLength\": 8}\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\"},\n"
        "    {\"bufferView\": 3, \"componentType\": 5121, \"normalized\": true, \"count\": 4, "
        "\"type\": \"VEC4\"},\n"
        "    {\"bufferView\": 4, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC4\"},\n"
        "    {\"bufferView\": 5, \"componentType\": 5121, \"count\": 4, \"type\": \"VEC4\"},\n"
        "    {\"bufferView\": 6, \"componentType\": 5121, \"normalized\": true, \"count\": 4, "
        "\"type\": \"VEC4\"},\n"
        "    {\"bufferView\": 7, \"componentType\": 5123, \"count\": 4, \"type\": \"SCALAR\"}\n"
        "  ],\n"
        "  \"materials\": [{\"pbrMetallicRoughness\": {\"baseColorFactor\": [1.0, 1.0, 1.0, "
        "1.0]}}],\n"
        "  \"meshes\": [{\"primitives\": [{\n"
        "    \"attributes\": {\n"
        "      \"POSITION\": 0,\n"
        "      \"NORMAL\": 1,\n"
        "      \"TEXCOORD_0\": 2,\n"
        "      \"COLOR_0\": 3,\n"
        "      \"TANGENT\": 4,\n"
        "      \"JOINTS_0\": 5,\n"
        "      \"WEIGHTS_0\": 6\n"
        "    },\n"
        "    \"indices\": 7,\n"
        "    \"mode\": 5,\n"
        "    \"material\": 0\n"
        "  }]}]\n"
        "}\n";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Extended-attribute glTF file can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr,
                "GLTF.Load parses triangle-strip meshes with extended attributes");
    if (!asset)
        return;

    rt_mesh3d *mesh = (rt_mesh3d *)rt_gltf_get_mesh(asset, 0);
    EXPECT_TRUE(mesh != nullptr, "GLTF.Load returns the extended-attribute mesh");
    if (!mesh)
        return;

    EXPECT_TRUE(mesh->vertex_count == 4 && mesh->index_count == 6,
                "GLTF.Load triangulates triangle strips into indexed triangles");
    EXPECT_NEAR(mesh->vertices[0].color[0], 1.0, 0.001, "GLTF.Load normalizes COLOR_0 red");
    EXPECT_NEAR(
        mesh->vertices[3].color[3], 128.0 / 255.0, 0.001, "GLTF.Load normalizes COLOR_0 alpha");
    EXPECT_NEAR(
        mesh->vertices[3].tangent[3], -1.0, 0.001, "GLTF.Load preserves tangent handedness");
    EXPECT_TRUE(mesh->vertices[0].bone_indices[0] == 1 && mesh->vertices[0].bone_indices[3] == 4,
                "GLTF.Load preserves JOINTS_0 indices");
    EXPECT_NEAR(
        mesh->vertices[1].bone_weights[0], 128.0 / 255.0, 0.001, "GLTF.Load normalizes WEIGHTS_0");
    EXPECT_TRUE(mesh->bone_count == 9, "GLTF.Load grows the mesh bone palette from JOINTS_0 data");
    EXPECT_TRUE(mesh->indices[0] == 0 && mesh->indices[1] == 1 && mesh->indices[2] == 2 &&
                    mesh->indices[3] == 2 && mesh->indices[4] == 1 && mesh->indices[5] == 3,
                "GLTF.Load preserves triangle-strip winding during triangulation");
    EXPECT_TRUE(!load_warnings_contain("bone influences") &&
                    !load_warnings_contain("joints beyond the"),
                "Clean 4-influence skins load without skin-diagnostic warnings");
    EXPECT_TRUE(import_report_contains("\"truncatedInfluenceVertices\":0") &&
                    import_report_contains("\"skippedPrimitives\":0"),
                "Clean assets produce an all-zero import report");
}

static void test_gltf_clips_and_renormalizes_primary_joint_influences() {
    const char *gltf_path = "/tmp/zanna_gltf_primary_joint_clip.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t joints[12] = {256, 1, 2, 3, 1, 2, 3, 4, 1, 2, 3, 4};
    const float weights[12] = {
        0.5f, 0.25f, 0.25f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : joints)
        append_bytes(gltf_buffer, v);
    for (float v : weights)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"buffers\": [{\"uri\": \"data:application/octet-stream;base64," +
        buffer_b64 + "\", \"byteLength\": " + std::to_string(gltf_buffer.size()) +
        "}],\n"
        "  \"bufferViews\": [\n"
        "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
        "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 24},\n"
        "    {\"buffer\": 0, \"byteOffset\": 60, \"byteLength\": 48},\n"
        "    {\"buffer\": 0, \"byteOffset\": 108, \"byteLength\": 6}\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 1, \"componentType\": 5123, \"count\": 3, \"type\": \"VEC4\"},\n"
        "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC4\"},\n"
        "    {\"bufferView\": 3, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\"}\n"
        "  ],\n"
        "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
        "\"JOINTS_0\": 1, \"WEIGHTS_0\": 2}, \"indices\": 3}]}]\n"
        "}\n";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Primary-joint clipping glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts primary skin attributes");
    if (!asset)
        return;

    rt_mesh3d *mesh = (rt_mesh3d *)rt_gltf_get_mesh(asset, 0);
    EXPECT_TRUE(mesh != nullptr, "Primary-joint clipping fixture imports a mesh");
    if (!mesh)
        return;
    EXPECT_TRUE(mesh->vertices[0].bone_indices[0] == 0,
                "GLTF.Load clears clipped invalid primary joint indices");
    EXPECT_NEAR(mesh->vertices[0].bone_weights[0],
                0.0,
                0.001,
                "GLTF.Load clears clipped invalid primary joint weights");
    EXPECT_NEAR(mesh->vertices[0].bone_weights[1],
                0.5,
                0.001,
                "GLTF.Load renormalizes remaining primary joint weight A");
    EXPECT_NEAR(mesh->vertices[0].bone_weights[2],
                0.5,
                0.001,
                "GLTF.Load renormalizes remaining primary joint weight B");
    EXPECT_TRUE(load_warnings_contain("joints beyond the"),
                "GLTF.Load warns when out-of-range joint influences are dropped");
    EXPECT_TRUE(load_warnings_contain("1 vertices"),
                "Out-of-range joint warning reports the affected vertex count");
}

static void test_gltf_reduces_secondary_joint_sets_to_top_four_influences() {
    const char *gltf_path = "/tmp/zanna_gltf_joints1.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };
    auto append_u8_array = [&](const uint8_t *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };
    auto append_u16_array = [&](const uint16_t *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    static const uint8_t joints0[12] = {1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4};
    static const float weights0[12] = {
        0.10f, 0.20f, 0.05f, 0.05f, 0.10f, 0.20f, 0.05f, 0.05f, 0.10f, 0.20f, 0.05f, 0.05f};
    static const uint8_t joints1[12] = {5, 6, 7, 8, 5, 6, 7, 8, 5, 6, 7, 8};
    static const float weights1[12] = {
        0.30f, 0.15f, 0.10f, 0.05f, 0.30f, 0.15f, 0.10f, 0.05f, 0.30f, 0.15f, 0.10f, 0.05f};
    static const uint16_t indices[3] = {0, 1, 2};

    size_t pos_off = append_float_array(positions, 9);
    size_t joints0_off = append_u8_array(joints0, 12);
    size_t weights0_off = append_float_array(weights0, 12);
    size_t joints1_off = append_u8_array(joints1, 12);
    size_t weights1_off = append_float_array(weights1, 12);
    size_t idx_off = append_u16_array(indices, 3);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(pos_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(joints0_off) +
        ",\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(weights0_off) +
        ",\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(joints1_off) +
        ",\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(weights1_off) +
        ",\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(idx_off) +
        ",\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":5,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,"
        "\"WEIGHTS_0\":2,\"JOINTS_1\":3,\"WEIGHTS_1\":4},\"indices\":5}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "JOINTS_1 glTF fixture can be created");
    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "JOINTS_1 preload root bytes can be read");
    if (!root.empty()) {
        uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
        EXPECT_TRUE(owned_root != nullptr, "JOINTS_1 preload root copy can be allocated");
        if (owned_root) {
            std::memcpy(owned_root, root.data(), root.size());
            char error[128] = {0};
            rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
                rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
            EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage JOINTS_1 mesh payloads");
            EXPECT_TRUE(error[0] == '\0', "JOINTS_1 preload bundle build has no terminal error");
            EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                        "Preload bundle worker-decodes JOINTS_1 attributes to mesh POD");
            if (bundle) {
                void *preloaded =
                    rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
                EXPECT_TRUE(preloaded != nullptr, "Preloaded JOINTS_1 bundle builds an asset");
                auto *preloaded_mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(preloaded, 0));
                EXPECT_TRUE(preloaded_mesh != nullptr,
                            "Preloaded JOINTS_1 bundle exposes the mesh");
                if (preloaded_mesh) {
                    EXPECT_TRUE(preloaded_mesh->vertices[0].bone_indices[0] == 1 &&
                                    preloaded_mesh->vertices[0].bone_indices[1] == 2 &&
                                    preloaded_mesh->vertices[0].bone_indices[2] == 5 &&
                                    preloaded_mesh->vertices[0].bone_indices[3] == 6,
                                "Preloaded JOINTS_1 mesh keeps the strongest four influences");
                    EXPECT_NEAR(preloaded_mesh->vertices[0].bone_weights[2],
                                0.30 / 0.75,
                                0.001,
                                "Preloaded JOINTS_1 mesh keeps secondary weights");
                    EXPECT_TRUE(preloaded_mesh->bone_count == 7,
                                "Preloaded JOINTS_1 mesh keeps its bone palette size");
                    EXPECT_TRUE(load_warnings_contain("more than 4 bone influences"),
                                "Preloaded JOINTS_1 load warns when influences are truncated");
                    EXPECT_TRUE(load_warnings_contain("3 vertices"),
                                "Preloaded truncation warning reports the affected vertex count");
                }
            }
        }
    }

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses secondary joint sets");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr, "GLTF.Load exposes the JOINTS_1 mesh");
    if (!mesh)
        return;
    EXPECT_TRUE(mesh->vertices[0].bone_indices[0] == 1 && mesh->vertices[0].bone_indices[1] == 2 &&
                    mesh->vertices[0].bone_indices[2] == 5 &&
                    mesh->vertices[0].bone_indices[3] == 6,
                "GLTF.Load keeps the four strongest influences across JOINTS_0 and JOINTS_1");
    EXPECT_NEAR(mesh->vertices[0].bone_weights[0],
                0.10 / 0.75,
                0.001,
                "GLTF.Load renormalizes reduced JOINTS_1 influence weights");
    EXPECT_NEAR(mesh->vertices[0].bone_weights[2],
                0.30 / 0.75,
                0.001,
                "GLTF.Load keeps the strongest secondary influence");
    EXPECT_TRUE(mesh->bone_count == 7, "GLTF.Load includes retained JOINTS_1 palette entries");
    EXPECT_TRUE(load_warnings_contain("more than 4 bone influences"),
                "GLTF.Load warns when influences are truncated to the top four");
    EXPECT_TRUE(load_warnings_contain("3 vertices"),
                "Truncation warning reports the affected vertex count");
    EXPECT_TRUE(import_report_contains("\"truncatedInfluenceVertices\":3"),
                "Import report counts truncated-influence vertices");
}

static void test_gltf_applies_matrix_nodes_in_column_major_order() {
    const char *gltf_path = "/tmp/zanna_gltf_matrix_node.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"buffers\": [{\"uri\": \"data:application/octet-stream;base64," +
        buffer_b64 + "\", \"byteLength\": " + std::to_string(gltf_buffer.size()) +
        "}],\n"
        "  \"bufferViews\": [\n"
        "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
        "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 6}\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\"},\n"
        "    {\"bufferView\": 1, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\"}\n"
        "  ],\n"
        "  \"materials\": [{\"pbrMetallicRoughness\": {\"baseColorFactor\": [1.0, 1.0, 1.0, "
        "1.0]}}],\n"
        "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0}, \"indices\": 1, "
        "\"material\": 0}]}],\n"
        "  \"nodes\": [{\n"
        "    \"name\": \"MatrixNode\",\n"
        "    \"mesh\": 0,\n"
        "    \"matrix\": [2.0, 0.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 0.0, 4.0, 0.0, 5.0, 6.0, "
        "7.0, 1.0]\n"
        "  }],\n"
        "  \"scenes\": [{\"nodes\": [0]}],\n"
        "  \"scene\": 0\n"
        "}\n";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Matrix-node glTF file can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses matrix-node assets");
    if (!asset)
        return;

    void *scene_root = rt_gltf_get_scene_root(asset);
    void *node = rt_scene_node3d_find(scene_root, rt_const_cstr("MatrixNode"));
    EXPECT_TRUE(node != nullptr, "GLTF.Load exposes nodes authored with matrix transforms");
    if (!node)
        return;

    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(node)),
                5.0,
                0.001,
                "GLTF.Load decodes column-major matrix translation.x correctly");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(node)),
                6.0,
                0.001,
                "GLTF.Load decodes column-major matrix translation.y correctly");
    EXPECT_NEAR(rt_vec3_z(rt_scene_node3d_get_position(node)),
                7.0,
                0.001,
                "GLTF.Load decodes column-major matrix translation.z correctly");
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_scale(node)),
                2.0,
                0.001,
                "GLTF.Load decodes matrix scale.x correctly");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_scale(node)),
                3.0,
                0.001,
                "GLTF.Load decodes matrix scale.y correctly");
    EXPECT_NEAR(rt_vec3_z(rt_scene_node3d_get_scale(node)),
                4.0,
                0.001,
                "GLTF.Load decodes matrix scale.z correctly");
}

static void test_gltf_eight_influence_import() {
    /* 3-vertex triangle where every vertex carries six meaningful influences
     * split across JOINTS_0 (0..3) and JOINTS_1 (4,5). Loading with the
     * eightInfluences option must keep all six: the strongest four in the vertex
     * record and joints 4/5 in the extra-influence side stream. */
    std::vector<uint8_t> buffer;
    auto push_f32 = [&buffer](float v) {
        uint8_t bytes[4];
        std::memcpy(bytes, &v, 4);
        buffer.insert(buffer.end(), bytes, bytes + 4);
    };
    auto push_u8 = [&buffer](uint8_t v) { buffer.push_back(v); };
    size_t pos_off = 0;
    const float tri[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (int v = 0; v < 3; v++) {
        push_f32(tri[v][0]);
        push_f32(tri[v][1]);
        push_f32(tri[v][2]);
    }
    size_t j0_off = buffer.size();
    for (int v = 0; v < 3; v++) {
        push_u8(0);
        push_u8(1);
        push_u8(2);
        push_u8(3);
    }
    size_t w0_off = buffer.size();
    for (int v = 0; v < 3; v++) {
        push_f32(0.4f);
        push_f32(0.25f);
        push_f32(0.15f);
        push_f32(0.1f);
    }
    size_t j1_off = buffer.size();
    for (int v = 0; v < 3; v++) {
        push_u8(4);
        push_u8(5);
        push_u8(0);
        push_u8(0);
    }
    size_t w1_off = buffer.size();
    for (int v = 0; v < 3; v++) {
        push_f32(0.06f);
        push_f32(0.04f);
        push_f32(0.0f);
        push_f32(0.0f);
    }
    std::string b64 = base64_encode(buffer.data(), buffer.size());
    std::string gltf_json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        b64 + "\",\"byteLength\":" + std::to_string(buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(pos_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(j0_off) +
        ",\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(w0_off) +
        ",\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(j1_off) +
        ",\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(w1_off) +
        ",\"byteLength\":48}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,"
        "\"WEIGHTS_0\":2,\"JOINTS_1\":3,\"WEIGHTS_1\":4}}]}]"
        "}";
    const char *gltf_path = "/tmp/zanna_gltf_eight_influences.gltf";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "eight-influence fixture writes");

    rt_gltf_load_options opts = rt_gltf_load_options_default();
    opts.eight_bone_influences = 1;
    rt_gltf_load_options saved = rt_gltf_set_thread_load_options(&opts);
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    (void)rt_gltf_set_thread_load_options(&saved);
    EXPECT_TRUE(asset != nullptr, "eightInfluences load succeeds");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3, "eight-influence mesh imports");
    if (!mesh)
        return;
    EXPECT_TRUE(mesh->extra_influences != nullptr,
                "eightInfluences keeps an extra-influence side stream");
    EXPECT_TRUE(!load_warnings_contain("more than 4 bone influences"),
                "eightInfluences does not warn about influence truncation");
    if (mesh->extra_influences) {
        const vgfx3d_extra_influences_t *extra = &mesh->extra_influences[0];
        EXPECT_TRUE(
            mesh->vertices[0].bone_indices[0] == 0 && mesh->vertices[0].bone_indices[1] == 1 &&
                mesh->vertices[0].bone_indices[2] == 2 && mesh->vertices[0].bone_indices[3] == 3,
            "strongest four influences stay in the vertex record");
        EXPECT_TRUE(extra->indices[0] == 4 && extra->indices[1] == 5,
                    "influences five and six land in the side stream");
        /* Vertex weights renormalize to the primary 4-set (sum 0.9); side-stream
         * weights carry the same scale so the total blend keeps the authored
         * 0.4/0.25/0.15/0.1/0.06/0.04 proportions. */
        EXPECT_NEAR(extra->weights[0], 0.06 / 0.9, 0.001, "fifth weight survives in scale");
        EXPECT_NEAR(extra->weights[1], 0.04 / 0.9, 0.001, "sixth weight survives in scale");
        EXPECT_NEAR(mesh->vertices[0].bone_weights[0],
                    0.4 / 0.9,
                    0.001,
                    "primary weights renormalize to the vertex record");
        EXPECT_TRUE(mesh->bone_count == 6, "bone palette covers the side-stream influences");
        /* Functional: bone 4 translated by (8,0,0); all other bones identity.
         * Skinned x = src.x + w4 * 8. */
        {
            float palette[6 * 16];
            for (int b = 0; b < 6; b++) {
                float *m = &palette[b * 16];
                std::memset(m, 0, 16 * sizeof(float));
                m[0] = m[5] = m[10] = m[15] = 1.0f;
            }
            palette[4 * 16 + 3] = 8.0f;
            vgfx3d_vertex_t skinned[3];
            vgfx3d_skin_vertices_extra(mesh->vertices,
                                       skinned,
                                       mesh->vertex_count,
                                       palette,
                                       6,
                                       mesh->extra_influences,
                                       nullptr);
            EXPECT_NEAR(skinned[0].pos[0],
                        0.0 + 0.06 * 8.0,
                        0.005,
                        "CPU skinning applies side-stream influences");
        }
    }

    /* Same asset without the option: classic top-4 merge with truncation warning. */
    void *asset4 = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset4 != nullptr, "default load of the 6-influence fixture succeeds");
    if (asset4) {
        auto *mesh4 = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset4, 0));
        EXPECT_TRUE(mesh4 && mesh4->extra_influences == nullptr,
                    "default load keeps the 4-influence layout");
        EXPECT_TRUE(load_warnings_contain("more than 4 bone influences"),
                    "default load still warns about dropped influences");
    }
}

static void test_gltf_compress_animations_option() {
    /* 11 collinear LINEAR keys (x = t over [0,1] on a 2-joint skin's root): with
     * compressAnimations the interior nine reconstruct exactly by lerp and drop,
     * leaving the two endpoint keys and identical playback. */
    std::vector<uint8_t> buffer;
    auto push_f32 = [&buffer](float v) {
        uint8_t bytes[4];
        std::memcpy(bytes, &v, 4);
        buffer.insert(buffer.end(), bytes, bytes + 4);
    };
    const int kKeys = 11;
    size_t times_off = 0;
    for (int i = 0; i < kKeys; i++)
        push_f32((float)i / (kKeys - 1));
    size_t values_off = buffer.size();
    for (int i = 0; i < kKeys; i++) {
        push_f32((float)i / (kKeys - 1));
        push_f32(0.0f);
        push_f32(0.0f);
    }
    std::string b64 = base64_encode(buffer.data(), buffer.size());
    std::string gltf_json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"nodes\":[{\"children\":[1]},{}],"
        "\"skins\":[{\"joints\":[0,1]}],"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        b64 + "\",\"byteLength\":" + std::to_string(buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) + ",\"byteLength\":" + std::to_string(values_off - times_off) +
        "},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(values_off) +
        ",\"byteLength\":" + std::to_string(buffer.size() - values_off) +
        "}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
        std::to_string(kKeys) +
        ",\"type\":\"SCALAR\",\"min\":[0.0],\"max\":[1.0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
        std::to_string(kKeys) +
        ",\"type\":\"VEC3\"}"
        "],"
        "\"animations\":[{\"channels\":[{\"sampler\":0,"
        "\"target\":{\"node\":0,\"path\":\"translation\"}}],"
        "\"samplers\":[{\"input\":0,\"interpolation\":\"LINEAR\",\"output\":1}]}]"
        "}";
    const char *gltf_path = "/tmp/zanna_gltf_compress_anim.gltf";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "compressAnimations fixture writes");

    void *asset_raw = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset_raw != nullptr, "uncompressed clip loads");
    if (asset_raw) {
        auto *anim = static_cast<rt_animation3d *>(rt_gltf_get_animation(asset_raw, 0));
        EXPECT_TRUE(anim && anim->channel_count == 1 && anim->channels[0].keyframe_count == kKeys,
                    "default load keeps every authored key");
    }

    rt_gltf_load_options opts = rt_gltf_load_options_default();
    opts.compress_animations = 1;
    rt_gltf_load_options saved = rt_gltf_set_thread_load_options(&opts);
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    (void)rt_gltf_set_thread_load_options(&saved);
    EXPECT_TRUE(asset != nullptr, "compressAnimations load succeeds");
    if (!asset)
        return;
    auto *anim = static_cast<rt_animation3d *>(rt_gltf_get_animation(asset, 0));
    EXPECT_TRUE(anim != nullptr && anim->channel_count == 1, "compressed clip imports");
    if (!anim)
        return;
    EXPECT_TRUE(anim->channels[0].keyframe_count == 2,
                "collinear keys reduce to the two endpoints");
    EXPECT_NEAR(
        anim->channels[0].keyframes[0].position[0], 0.0, 1e-5, "first key survives compression");
    EXPECT_NEAR(
        anim->channels[0].keyframes[1].position[0], 1.0, 1e-5, "last key survives compression");
    EXPECT_TRUE(import_report_contains("\"compressedAnimationKeysDropped\":9"),
                "import report counts dropped animation keys");
}

static void test_gltf_ior_and_volume_extensions() {
    /* KHR_materials_ior + KHR_materials_volume ride the PBR custom params:
     * [4] = ior, [5] = thickness, [6] = folded Beer-Lambert absorption
     * (-ln(avg(attenuationColor)) / attenuationDistance). */
    const char *gltf_path = "/tmp/zanna_gltf_ior_volume.gltf";
    std::string gltf_json = "{\"asset\":{\"version\":\"2.0\"},"
                            "\"materials\":[{"
                            "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
                            "\"extensions\":{"
                            "\"KHR_materials_ior\":{\"ior\":1.33},"
                            "\"KHR_materials_transmission\":{\"transmissionFactor\":0.9},"
                            "\"KHR_materials_volume\":{\"thicknessFactor\":0.5,"
                            "\"attenuationDistance\":2.0,\"attenuationColor\":[0.5,0.5,0.5]}"
                            "}}]}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "ior/volume fixture writes");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "ior/volume material loads");
    if (!asset)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr, "material imports");
    if (!mat)
        return;
    EXPECT_NEAR(mat->custom_params[4], 1.33, 0.001, "KHR_materials_ior lands in param 4");
    EXPECT_NEAR(mat->custom_params[3], 0.9, 0.001, "transmission factor lands in param 3");
    EXPECT_NEAR(mat->custom_params[5], 0.5, 0.001, "volume thickness lands in param 5");
    /* sigma = -ln(0.5) / 2 = 0.3466 */
    EXPECT_NEAR(mat->custom_params[6],
                0.34657,
                0.001,
                "volume attenuation folds to the Beer-Lambert coefficient");

    /* KHR_materials_sheen: luminance-folded intensity + roughness. */
    const char *sheen_path = "/tmp/zanna_gltf_sheen.gltf";
    std::string sheen_json = "{\"asset\":{\"version\":\"2.0\"},"
                             "\"materials\":[{"
                             "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
                             "\"extensions\":{\"KHR_materials_sheen\":{"
                             "\"sheenColorFactor\":[1.0,1.0,1.0],\"sheenRoughnessFactor\":0.4}}}]}";
    EXPECT_TRUE(write_text_file(sheen_path, sheen_json), "sheen fixture writes");
    void *sheen_asset = rt_gltf_load(rt_const_cstr(sheen_path));
    EXPECT_TRUE(sheen_asset != nullptr, "sheen material loads");
    if (sheen_asset) {
        auto *sm = static_cast<rt_material3d *>(rt_gltf_get_material(sheen_asset, 0));
        EXPECT_TRUE(sm != nullptr, "sheen material imports");
        if (sm) {
            EXPECT_NEAR(sm->custom_params[0],
                        1.0,
                        0.001,
                        "sheen color folds to a luminance intensity in param 0");
            EXPECT_NEAR(sm->custom_params[7], 0.4, 0.001, "sheen roughness lands in param 7");
        }
    }

    /* KHR_materials_anisotropy: strength + rotation params. */
    const char *aniso_path = "/tmp/zanna_gltf_aniso.gltf";
    std::string aniso_json = "{\"asset\":{\"version\":\"2.0\"},"
                             "\"materials\":[{"
                             "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
                             "\"extensions\":{\"KHR_materials_anisotropy\":{"
                             "\"anisotropyStrength\":0.8,\"anisotropyRotation\":0.5}}}]}";
    EXPECT_TRUE(write_text_file(aniso_path, aniso_json), "anisotropy fixture writes");
    void *aniso_asset = rt_gltf_load(rt_const_cstr(aniso_path));
    EXPECT_TRUE(aniso_asset != nullptr, "anisotropy material loads");
    if (aniso_asset) {
        auto *am = static_cast<rt_material3d *>(rt_gltf_get_material(aniso_asset, 0));
        EXPECT_TRUE(am != nullptr, "anisotropy material imports");
        if (am) {
            EXPECT_NEAR(am->custom_params[8], 0.8, 0.001, "anisotropy strength lands in param 8");
            EXPECT_NEAR(am->custom_params[9], 0.5, 0.001, "anisotropy rotation lands in param 9");
        }
    }
}

static void test_gltf_partitions_oversized_skins() {
    /* 300-joint skin: 300 unit quads, quad i rigidly bound to joint i. The joint
     * set exceeds the 256-slot draw palette, so the importer must partition the
     * primitive into sub-meshes with bucket-local bone indices + bone_map. */
    const int kJoints = 300;
    const int kVertsPerQuad = 4;
    const int vert_count = kJoints * kVertsPerQuad;
    std::vector<uint8_t> buffer;
    auto push_f32 = [&buffer](float v) {
        uint8_t bytes[4];
        std::memcpy(bytes, &v, 4);
        buffer.insert(buffer.end(), bytes, bytes + 4);
    };
    auto push_u16 = [&buffer](uint16_t v) {
        buffer.push_back((uint8_t)(v & 0xFF));
        buffer.push_back((uint8_t)(v >> 8));
    };
    size_t pos_off = buffer.size();
    for (int q = 0; q < kJoints; q++) {
        const float x = (float)q * 2.0f;
        const float quad[4][3] = {
            {x, 0.0f, 0.0f}, {x + 1.0f, 0.0f, 0.0f}, {x + 1.0f, 1.0f, 0.0f}, {x, 1.0f, 0.0f}};
        for (int v = 0; v < 4; v++) {
            push_f32(quad[v][0]);
            push_f32(quad[v][1]);
            push_f32(quad[v][2]);
        }
    }
    size_t joints_off = buffer.size();
    for (int q = 0; q < kJoints; q++) {
        for (int v = 0; v < 4; v++) {
            push_u16((uint16_t)q);
            push_u16(0);
            push_u16(0);
            push_u16(0);
        }
    }
    size_t weights_off = buffer.size();
    for (int q = 0; q < kJoints; q++) {
        for (int v = 0; v < 4; v++) {
            push_f32(1.0f);
            push_f32(0.0f);
            push_f32(0.0f);
            push_f32(0.0f);
        }
    }
    size_t idx_off = buffer.size();
    for (int q = 0; q < kJoints; q++) {
        uint16_t base = (uint16_t)(q * 4);
        const uint16_t tri[6] = {base,
                                 (uint16_t)(base + 1),
                                 (uint16_t)(base + 2),
                                 base,
                                 (uint16_t)(base + 2),
                                 (uint16_t)(base + 3)};
        for (int i = 0; i < 6; i++)
            push_u16(tri[i]);
    }
    std::string b64 = base64_encode(buffer.data(), buffer.size());

    std::string nodes = "{\"skin\":0,\"mesh\":0}";
    std::string joints;
    for (int j = 0; j < kJoints; j++) {
        nodes += ",{\"translation\":[" + std::to_string((double)j * 2.0) + ",0,0]}";
        if (j)
            joints += ",";
        joints += std::to_string(j + 1);
    }
    std::string gltf_json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"nodes\":[" +
        nodes +
        "],"
        "\"skins\":[{\"joints\":[" +
        joints +
        "]}],"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        b64 + "\",\"byteLength\":" + std::to_string(buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(pos_off) + ",\"byteLength\":" + std::to_string(joints_off - pos_off) +
        "},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(joints_off) + ",\"byteLength\":" + std::to_string(weights_off - joints_off) +
        "},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(weights_off) + ",\"byteLength\":" + std::to_string(idx_off - weights_off) +
        "},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(idx_off) + ",\"byteLength\":" + std::to_string(buffer.size() - idx_off) +
        "}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
        std::to_string(vert_count) +
        ",\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":" +
        std::to_string(vert_count) +
        ",\"type\":\"VEC4\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
        std::to_string(vert_count) +
        ",\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":" +
        std::to_string(kJoints * 6) +
        ",\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,"
        "\"WEIGHTS_0\":2},\"indices\":3}]}]"
        "}";
    const char *gltf_path = "/tmp/zanna_gltf_oversized_skin.gltf";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "oversized-skin glTF fixture writes");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load imports a 300-joint skin");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_skeleton_count(asset) == 1, "300-joint skin builds one skeleton");
    void *skeleton = rt_gltf_get_skeleton(asset, 0);
    EXPECT_TRUE(skeleton != nullptr && rt_skeleton3d_get_bone_count(skeleton) == kJoints,
                "Skeleton3D holds all 300 bones past the draw-palette cap");
    int64_t mesh_count = rt_gltf_mesh_count(asset);
    EXPECT_TRUE(mesh_count == 2, "oversized skin partitions into two sub-meshes");
    int total_vertices = 0;
    int total_indices = 0;
    for (int64_t i = 0; i < mesh_count; i++) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, i));
        EXPECT_TRUE(mesh != nullptr, "partitioned sub-mesh exists");
        if (!mesh)
            continue;
        EXPECT_TRUE(mesh->bone_count > 0 && mesh->bone_count <= 256,
                    "sub-mesh bone palette fits the draw limit");
        EXPECT_TRUE(mesh->bone_map != nullptr, "sub-mesh carries a bone map");
        if (mesh->bone_map) {
            for (int32_t b = 0; b < mesh->bone_count; b++) {
                EXPECT_TRUE(mesh->bone_map[b] >= 0 && mesh->bone_map[b] < kJoints,
                            "bone map entries reference skeleton bones");
            }
        }
        total_vertices += (int)mesh->vertex_count;
        total_indices += (int)mesh->index_count;
        /* Every vertex is rigidly bound: local index 0..bone_count-1, weight 1. */
        for (uint32_t v = 0; v < mesh->vertex_count && v < 8; v++) {
            EXPECT_TRUE(mesh->vertices[v].bone_weights[0] > 0.999f,
                        "partitioned vertices keep full weights");
            EXPECT_TRUE(mesh->vertices[v].bone_indices[0] < mesh->bone_count,
                        "partitioned vertex indices stay palette-local");
        }
    }
    EXPECT_TRUE(total_vertices == vert_count, "partitioning preserves every vertex");
    EXPECT_TRUE(total_indices == kJoints * 6, "partitioning preserves every triangle");
    {
        auto *m0 = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
        auto *m1 = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 1));
        if (m0 && m1) {
            EXPECT_TRUE(m0->bone_count == 256 && m1->bone_count == kJoints - 256,
                        "greedy bucketing fills the first palette before splitting");
            if (m0->bone_map && m1->bone_map) {
                EXPECT_TRUE(m0->bone_map[0] == 0 && m0->bone_map[255] == 255,
                            "first bucket maps palette slots to joints 0..255");
                EXPECT_TRUE(m1->bone_map[0] == 256, "second bucket starts at joint 256");
            }
        }
    }
    EXPECT_TRUE(load_warnings_contain("partitioned into 2 sub-meshes"),
                "partitioning reports a load warning");
}

static void test_gltf_imports_skins_and_animation_clips() {
    const char *gltf_path = "/tmp/zanna_gltf_skinned_anim.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };
    auto append_u16_array = [&](const uint16_t *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    static const uint16_t joints[12] = {0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
    static const float weights[12] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.75f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    static const uint16_t indices[3] = {0, 1, 2};
    static const float inverse_binds[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0,  0, 1,
                                            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
    static const float anim_times[2] = {0.0f, 1.0f};
    static const float anim_translations[18] = {0.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f,
                                                2.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f,
                                                2.0f,
                                                3.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f,
                                                0.0f};
    static const float anim_mid_times[1] = {0.5f};
    static const float anim_scales[3] = {1.0f, 1.0f, 1.0f};

    size_t pos_off = append_float_array(positions, 9);
    size_t joints_off = append_u16_array(joints, 12);
    size_t weights_off = append_float_array(weights, 12);
    size_t indices_off = append_u16_array(indices, 3);
    size_t inverse_off = append_float_array(inverse_binds, 32);
    size_t times_off = append_float_array(anim_times, 2);
    size_t trans_off = append_float_array(anim_translations, 18);
    size_t mid_times_off = append_float_array(anim_mid_times, 1);
    size_t scale_off = append_float_array(anim_scales, 3);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(pos_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(joints_off) +
        ",\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(weights_off) +
        ",\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(indices_off) +
        ",\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(inverse_off) +
        ",\"byteLength\":128},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(trans_off) +
        ",\"byteLength\":72},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(mid_times_off) +
        ",\"byteLength\":4},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(scale_off) +
        ",\"byteLength\":12}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
        "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"},"
        "{\"bufferView\":7,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"},"
        "{\"bufferView\":8,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,"
        "\"WEIGHTS_0\":2},\"indices\":3}]}],"
        "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":4}],"
        "\"animations\":[{\"name\":\"MoveRoot\",\"samplers\":[{\"input\":5,\"output\":6,"
        "\"interpolation\":\"CUBICSPLINE\"},{\"input\":7,\"output\":8}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"scale\"}}]}],"
        "\"nodes\":[{\"name\":\"MeshNode\",\"mesh\":0,\"skin\":0},{\"name\":\"RootJoint\","
        "\"children\":[2]},{\"name\":\"ChildJoint\",\"translation\":[0,1,0]}],"
        "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0"
        "}";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Skinned glTF fixture can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Skinned preload root bytes can be read");
    if (!root.empty()) {
        uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
        EXPECT_TRUE(owned_root != nullptr, "Skinned preload root copy can be allocated");
        if (owned_root) {
            std::memcpy(owned_root, root.data(), root.size());
            char error[128] = {0};
            rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
                rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
            EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage skinned mesh payloads");
            EXPECT_TRUE(error[0] == '\0', "Skinned preload bundle build has no terminal error");
            EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                        "Preload bundle worker-decodes skinned mesh attributes to POD");
            if (bundle) {
                void *preloaded =
                    rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
                EXPECT_TRUE(preloaded != nullptr, "Preloaded skinned bundle builds an asset");
                auto *preloaded_mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(preloaded, 0));
                EXPECT_TRUE(preloaded_mesh != nullptr,
                            "Preloaded skinned bundle exposes the skinned mesh");
                if (preloaded_mesh) {
                    EXPECT_TRUE(preloaded_mesh->bone_count == 2,
                                "Preloaded skinned mesh remaps joint attributes to runtime bones");
                    EXPECT_TRUE(preloaded_mesh->vertices[1].bone_indices[0] == 0 &&
                                    preloaded_mesh->vertices[1].bone_indices[1] == 1,
                                "Preloaded skinned mesh preserves mixed joint influences");
                    EXPECT_NEAR(preloaded_mesh->vertices[1].bone_weights[0],
                                0.25,
                                0.001,
                                "Preloaded skinned mesh keeps root weight");
                    EXPECT_NEAR(preloaded_mesh->vertices[1].bone_weights[1],
                                0.75,
                                0.001,
                                "Preloaded skinned mesh keeps child weight");
                }
            }
        }
    }

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses skinned animation assets");
    if (!asset)
        return;

    EXPECT_TRUE(rt_gltf_skeleton_count(asset) == 1, "GLTF.Load extracts one skeleton");
    EXPECT_TRUE(rt_gltf_animation_count(asset) == 1, "GLTF.Load extracts one animation");
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr, "GLTF.Load exposes the skinned mesh");
    if (mesh) {
        EXPECT_TRUE(mesh->bone_count == 2, "GLTF.Load remaps joint attributes to runtime bones");
        EXPECT_TRUE(mesh->vertices[1].bone_indices[0] == 0 &&
                        mesh->vertices[1].bone_indices[1] == 1,
                    "GLTF.Load preserves mixed joint influences");
        EXPECT_NEAR(mesh->vertices[1].bone_weights[0], 0.25, 0.001, "GLTF.Load keeps root weight");
        EXPECT_NEAR(mesh->vertices[1].bone_weights[1], 0.75, 0.001, "GLTF.Load keeps child weight");
    }
    auto *anim = static_cast<rt_animation3d *>(rt_gltf_get_animation(asset, 0));
    EXPECT_TRUE(anim != nullptr, "GLTF.Load exposes imported Animation3D clips");
    if (anim) {
        EXPECT_NEAR(anim->duration, 1.0, 0.001, "GLTF animation duration follows sampler input");
        EXPECT_TRUE(anim->channel_count == 1, "GLTF animation targets the imported root bone");
        EXPECT_TRUE(anim->channels[0].keyframe_count == 3,
                    "GLTF animation merges CUBICSPLINE and scale sample times");
        EXPECT_TRUE(import_report_contains("\"bakedCubicSplineChannels\":0"),
                    "Skeletal CUBICSPLINE channels keep Hermite tangents (no bake reported)");
        EXPECT_TRUE((anim->channels[0].keyframes[0].cubic_mask & 1u) != 0,
                    "Imported CUBICSPLINE keys carry position tangents");
        EXPECT_NEAR(anim->channels[0].keyframes[1].position[0],
                    1.25,
                    0.001,
                    "GLTF CUBICSPLINE translation samples Hermite tangents");
        EXPECT_NEAR(anim->channels[0].keyframes[2].position[0],
                    2.0,
                    0.001,
                    "GLTF animation imports translation X");
        EXPECT_NEAR(anim->channels[0].keyframes[2].position[1],
                    3.0,
                    0.001,
                    "GLTF animation imports translation Y");
        EXPECT_NEAR(anim->channels[0].keyframes[1].scale_xyz[0],
                    1.0,
                    0.001,
                    "GLTF animation preserves default scale");
    }
}

static void test_gltf_skips_duplicate_skeletal_animation_channels() {
    const char *gltf_path = "/tmp/zanna_gltf_duplicate_skeletal_channels.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float primary_times[2] = {0.0f, 1.0f};
    static const float primary_translation[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    static const float duplicate_times[3] = {0.0f, 0.5f, 1.0f};
    static const float duplicate_translation[9] = {
        0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 20.0f, 0.0f, 0.0f};
    size_t primary_times_off = append_float_array(primary_times, 2);
    size_t primary_translation_off = append_float_array(primary_translation, 6);
    size_t duplicate_times_off = append_float_array(duplicate_times, 3);
    size_t duplicate_translation_off = append_float_array(duplicate_translation, 9);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(primary_times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(primary_translation_off) +
        ",\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(duplicate_times_off) +
        ",\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(duplicate_translation_off) +
        ",\"byteLength\":36}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"skins\":[{\"joints\":[0]}],"
        "\"animations\":[{\"name\":\"DuplicateBoneChannels\",\"samplers\":["
        "{\"input\":0,\"output\":1},{\"input\":2,\"output\":3}],\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"translation\"}}]}],"
        "\"nodes\":[{\"name\":\"Joint\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Duplicate skeletal-channel glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses duplicate skeletal-channel assets");
    if (!asset)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_gltf_get_animation(asset, 0));
    EXPECT_TRUE(anim != nullptr, "Duplicate skeletal-channel fixture exposes Animation3D");
    if (!anim)
        return;
    EXPECT_TRUE(anim->channel_count == 1, "Duplicate skeletal channels keep one bone channel");
    EXPECT_TRUE(anim->channels[0].keyframe_count == 2,
                "Ignored duplicate skeletal channels do not inject extra sample times");
    EXPECT_NEAR(anim->channels[0].keyframes[1].position[0],
                1.0,
                0.001,
                "Duplicate skeletal channels preserve the first valid curve");
}

static void test_gltf_imports_step_skeletal_animation_as_hold_keys() {
    const char *gltf_path = "/tmp/zanna_gltf_step_skeletal_hold_keys.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float times[2] = {0.0f, 1.0f};
    static const float translations[6] = {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f};
    size_t times_off = append_float_array(times, 2);
    size_t translations_off = append_float_array(translations, 6);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(translations_off) +
        ",\"byteLength\":24}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}"
        "],"
        "\"skins\":[{\"joints\":[0]}],"
        "\"animations\":[{\"name\":\"StepBone\",\"samplers\":[{\"input\":0,\"output\":1,"
        "\"interpolation\":\"STEP\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"translation\"}}]}],"
        "\"nodes\":[{\"name\":\"Joint\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "STEP skeletal glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses STEP skeletal animation assets");
    if (!asset)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_gltf_get_animation(asset, 0));
    EXPECT_TRUE(anim != nullptr, "STEP skeletal fixture exposes Animation3D");
    if (!anim)
        return;
    EXPECT_TRUE(anim->channel_count == 1, "STEP skeletal animation targets one bone channel");
    EXPECT_TRUE(anim->channels[0].keyframe_count == 3,
                "STEP skeletal animation inserts a pre-next-key hold sample");
    if (anim->channels[0].keyframe_count >= 3) {
        EXPECT_TRUE(anim->channels[0].keyframes[1].time > 0.0f &&
                        anim->channels[0].keyframes[1].time < 1.0f,
                    "STEP skeletal hold sample is placed inside the source interval");
        EXPECT_NEAR(anim->channels[0].keyframes[1].position[0],
                    0.0,
                    0.001,
                    "STEP skeletal hold sample preserves the previous translation value");
        EXPECT_NEAR(anim->channels[0].keyframes[2].position[0],
                    10.0,
                    0.001,
                    "STEP skeletal authored key still applies at the next time");
    }
}

static void test_gltf_rejects_skeletal_trs_animation_output_count_mismatch() {
    const char *gltf_path = "/tmp/zanna_gltf_bad_skeletal_trs_output_count.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float times[2] = {0.0f, 1.0f};
    static const float translations[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    size_t times_off = append_float_array(times, 2);
    size_t translations_off = append_float_array(translations, 9);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(translations_off) +
        ",\"byteLength\":36}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"skins\":[{\"joints\":[0]}],"
        "\"animations\":[{\"name\":\"BadSkeletalCount\",\"samplers\":[{\"input\":0,"
        "\"output\":1}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"translation\"}}]}],"
        "\"nodes\":[{\"name\":\"Joint\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Mismatched skeletal TRS glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses skeletal TRS count-mismatch assets");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_animation_count(asset) == 0,
                "glTF rejects skeletal TRS animation outputs whose accessor count is not exact");
    std::remove(gltf_path);
}

static void test_gltf_rejects_node_trs_animation_output_count_mismatch() {
    const char *gltf_path = "/tmp/zanna_gltf_bad_node_trs_output_count.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float times[2] = {0.0f, 1.0f};
    static const float translations[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    size_t times_off = append_float_array(times, 2);
    size_t translations_off = append_float_array(translations, 9);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(translations_off) +
        ",\"byteLength\":36}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"animations\":[{\"name\":\"BadNodeCount\",\"samplers\":[{\"input\":0,"
        "\"output\":1}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"translation\"}}]}],"
        "\"nodes\":[{\"name\":\"Animated\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Mismatched node TRS glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses node TRS count-mismatch assets");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_node_animation_count(asset) == 0,
                "glTF rejects node TRS animation outputs whose accessor count is not exact");
    std::remove(gltf_path);
}

static void test_gltf_ignores_inverse_bind_matrices_with_count_mismatch() {
    const char *gltf_path = "/tmp/zanna_gltf_bad_inverse_bind_count.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float inverse_binds[32] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 1.0f, 0.0f, 9.0f, 0.0f, 0.0f, 1.0f,
                                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 1.0f, 0.0f, 9.0f, 0.0f, 0.0f, 1.0f};
    size_t inverse_off = append_float_array(inverse_binds, 32);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json = "{"
                            "\"asset\":{\"version\":\"2.0\"},"
                            "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
                            buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
                            "}],"
                            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":" +
                            std::to_string(inverse_off) +
                            ",\"byteLength\":128}],"
                            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
                            "\"type\":\"MAT4\"}],"
                            "\"skins\":[{\"joints\":[0],\"inverseBindMatrices\":0}],"
                            "\"nodes\":[{\"name\":\"Joint\",\"translation\":[4.0,0.0,0.0]}],"
                            "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
                            "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Mismatched inverse-bind glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses inverse-bind count-mismatch assets");
    if (!asset)
        return;
    auto *skeleton = static_cast<rt_skeleton3d *>(rt_gltf_get_skeleton(asset, 0));
    EXPECT_TRUE(skeleton != nullptr && skeleton->bone_count == 1,
                "glTF still builds the skin skeleton when inverse-bind count is malformed");
    if (skeleton && skeleton->bone_count == 1) {
        EXPECT_NEAR(skeleton->bones[0].inverse_bind[3],
                    -4.0,
                    0.001,
                    "glTF ignores inverse-bind accessors whose count does not match joint count");
    }
    std::remove(gltf_path);
}

static void test_gltf_skips_duplicate_node_animation_channels() {
    const char *gltf_path = "/tmp/zanna_gltf_duplicate_node_channels.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float times[2] = {0.0f, 1.0f};
    static const float primary_translation[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    static const float duplicate_translation[6] = {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f};
    size_t times_off = append_float_array(times, 2);
    size_t primary_translation_off = append_float_array(primary_translation, 6);
    size_t duplicate_translation_off = append_float_array(duplicate_translation, 6);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(primary_translation_off) +
        ",\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(duplicate_translation_off) +
        ",\"byteLength\":24}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}"
        "],"
        "\"animations\":[{\"name\":\"DuplicateNodeChannels\",\"samplers\":["
        "{\"input\":0,\"output\":1},{\"input\":0,\"output\":2}],\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"translation\"}}]}],"
        "\"nodes\":[{\"name\":\"Animated\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Duplicate node-channel glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses duplicate node-channel assets");
    if (!asset)
        return;

    auto *node_anim = static_cast<rt_node_animation3d *>(rt_gltf_get_node_animation(asset, 0));
    EXPECT_TRUE(node_anim != nullptr, "Duplicate node-channel fixture exposes NodeAnimation3D");
    if (!node_anim)
        return;
    EXPECT_TRUE(node_anim->channel_count == 1,
                "Duplicate node animation channels keep one target/path channel");
    EXPECT_TRUE(node_anim->channels[0].target_node_index == 0,
                "Duplicate node animation channel keeps its import-node binding");
    EXPECT_NEAR(node_anim->channels[0].values[3],
                1.0,
                0.001,
                "Duplicate node animation channels preserve the first valid curve");
}

static void test_gltf_imports_step_node_animation_duplicate_times() {
    const char *gltf_path = "/tmp/zanna_gltf_step_duplicate_node_times.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float times[3] = {0.0f, 0.0f, 1.0f};
    static const float translations[9] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f};
    size_t times_off = append_float_array(times, 3);
    size_t translations_off = append_float_array(translations, 9);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(translations_off) +
        ",\"byteLength\":36}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"animations\":[{\"name\":\"StepDuplicateTimes\",\"samplers\":["
        "{\"input\":0,\"output\":1,\"interpolation\":\"STEP\"}],\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}],"
        "\"nodes\":[{\"name\":\"Animated\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "STEP duplicate-time glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses STEP duplicate-time node animations");
    if (!asset)
        return;

    auto *node_anim = static_cast<rt_node_animation3d *>(rt_gltf_get_node_animation(asset, 0));
    EXPECT_TRUE(node_anim != nullptr, "STEP duplicate-time fixture exposes NodeAnimation3D");
    if (!node_anim)
        return;
    EXPECT_TRUE(node_anim->channel_count == 1,
                "STEP duplicate-time glTF emits one node animation channel");
    EXPECT_TRUE(node_anim->channels[0].interpolation == RT_NODE_ANIM_INTERP_STEP,
                "STEP duplicate-time glTF preserves step interpolation");
    EXPECT_TRUE(node_anim->channels[0].key_count == 3,
                "STEP duplicate-time glTF preserves duplicate input keys");
    EXPECT_NEAR(node_anim->channels[0].values[3],
                2.0,
                0.001,
                "STEP duplicate-time glTF keeps the later duplicate key value");
}

static void test_gltf_rejects_mismatched_morph_weight_animation_width() {
    const char *gltf_path = "/tmp/zanna_gltf_mismatched_weight_animation_width.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    static const float morph_a[9] = {0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    static const float morph_b[9] = {0.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    static const float times[1] = {0.0f};
    static const float weights[1] = {1.0f};
    size_t positions_off = append_float_array(positions, 9);
    size_t morph_a_off = append_float_array(morph_a, 9);
    size_t morph_b_off = append_float_array(morph_b, 9);
    size_t times_off = append_float_array(times, 1);
    size_t weights_off = append_float_array(weights, 1);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(positions_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_a_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_b_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":4},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(weights_off) +
        ",\"byteLength\":4}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"weights\":[0.0,0.0],\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"targets\":[{\"POSITION\":1},{\"POSITION\":2}]}]}],"
        "\"nodes\":[{\"name\":\"BadWeights\",\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"animations\":[{\"name\":\"BadWeightWidth\",\"samplers\":[{\"input\":3,\"output\":4}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]"
        "}";

    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Mismatched morph-weight animation fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses malformed morph-weight animation assets");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_node_animation_count(asset) == 0,
                "glTF rejects morph-weight animation widths that do not match target morph count");
    std::remove(gltf_path);
}

static void test_gltf_splits_animation_clips_per_skin() {
    const char *gltf_path = "/tmp/zanna_gltf_multi_skin_animation.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float times[2] = {0.0f, 1.0f};
    static const float skin_a_trans[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    static const float skin_b_trans[6] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    size_t times_off = append_float_array(times, 2);
    size_t skin_a_off = append_float_array(skin_a_trans, 6);
    size_t skin_b_off = append_float_array(skin_b_trans, 6);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(skin_a_off) +
        ",\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(skin_b_off) +
        ",\"byteLength\":24}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}"
        "],"
        "\"skins\":[{\"joints\":[0]},{\"joints\":[1]}],"
        "\"animations\":[{\"name\":\"Split\",\"samplers\":[{\"input\":0,\"output\":1},"
        "{\"input\":0,\"output\":2}],\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"translation\"}}]}],"
        "\"nodes\":[{\"name\":\"JointA\"},{\"name\":\"JointB\"}],"
        "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0"
        "}";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Multi-skin glTF fixture can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses multi-skin animation assets");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_skeleton_count(asset) == 2, "GLTF.Load extracts both skeletons");
    EXPECT_TRUE(rt_gltf_animation_count(asset) == 2,
                "GLTF.Load creates one runtime clip per animated skin");

    auto *anim_a = static_cast<rt_animation3d *>(rt_gltf_get_animation(asset, 0));
    auto *anim_b = static_cast<rt_animation3d *>(rt_gltf_get_animation(asset, 1));
    EXPECT_TRUE(anim_a != nullptr && anim_b != nullptr, "GLTF.Load exposes both split clips");
    if (!anim_a || !anim_b)
        return;
    EXPECT_TRUE(anim_a->channel_count == 1 && anim_b->channel_count == 1,
                "Split skin clips each target one local bone");
    EXPECT_NEAR(anim_a->channels[0].keyframes[1].position[0],
                1.0,
                0.001,
                "First split skin clip keeps its own node curve");
    EXPECT_NEAR(anim_b->channels[0].keyframes[1].position[0],
                2.0,
                0.001,
                "Second split skin clip keeps its own node curve");
}

static void test_gltf_applies_sparse_accessors() {
    const char *gltf_path = "/tmp/zanna_gltf_sparse_accessor.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_u16_array = [&](const uint16_t *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const uint16_t sparse_indices[2] = {1, 2};
    static const float sparse_values[6] = {1.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f};
    static const uint16_t triangle_indices[3] = {0, 1, 2};
    size_t sparse_idx_off = append_u16_array(sparse_indices, 2);
    size_t sparse_value_off = append_float_array(sparse_values, 6);
    size_t index_off = append_u16_array(triangle_indices, 3);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(sparse_idx_off) +
        ",\"byteLength\":4},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(sparse_value_off) +
        ",\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(index_off) +
        ",\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"sparse\":{\"count\":2,\"indices\":{\"bufferView\":0,\"componentType\":5123},"
        "\"values\":{\"bufferView\":1}}},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]"
        "}";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Sparse glTF fixture can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Sparse preload root bytes can be read");
    if (!root.empty()) {
        uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
        EXPECT_TRUE(owned_root != nullptr, "Sparse preload root copy can be allocated");
        if (owned_root) {
            std::memcpy(owned_root, root.data(), root.size());
            char error[128] = {0};
            rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
                rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
            EXPECT_TRUE(bundle != nullptr,
                        "Preload bundle can stage sparse accessor mesh payloads");
            EXPECT_TRUE(error[0] == '\0', "Sparse preload bundle build has no terminal error");
            EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                        "Preload bundle worker-decodes sparse accessor mesh to POD");
            if (bundle) {
                void *preloaded =
                    rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
                EXPECT_TRUE(preloaded != nullptr, "Preloaded sparse bundle builds an asset");
                auto *preloaded_mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(preloaded, 0));
                EXPECT_TRUE(preloaded_mesh != nullptr,
                            "Preloaded sparse bundle exposes sparse accessor mesh");
                if (preloaded_mesh) {
                    EXPECT_NEAR(preloaded_mesh->vertices[1].pos[0],
                                1.0,
                                0.001,
                                "Sparse preload overrides vertex 1 X");
                    EXPECT_NEAR(preloaded_mesh->vertices[1].pos[1],
                                2.0,
                                0.001,
                                "Sparse preload overrides vertex 1 Y");
                    EXPECT_NEAR(preloaded_mesh->vertices[2].pos[1],
                                1.0,
                                0.001,
                                "Sparse preload can author a valid triangle");
                }
            }
        }
    }

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses sparse accessor assets");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr, "GLTF.Load exposes sparse accessor mesh");
    if (!mesh)
        return;
    EXPECT_NEAR(mesh->vertices[0].pos[0], 0.0, 0.001, "Sparse accessor defaults missing values");
    EXPECT_NEAR(mesh->vertices[1].pos[0], 1.0, 0.001, "Sparse accessor overrides X");
    EXPECT_NEAR(mesh->vertices[1].pos[1], 2.0, 0.001, "Sparse accessor overrides Y");
    EXPECT_NEAR(mesh->vertices[1].pos[2], 3.0, 0.001, "Sparse accessor overrides Z");
    EXPECT_NEAR(
        mesh->vertices[2].pos[1], 1.0, 0.001, "Sparse accessor can author a valid triangle");
}

static void test_gltf_imports_morph_targets() {
    const char *gltf_path = "/tmp/zanna_gltf_morph_targets.gltf";
    std::vector<uint8_t> gltf_buffer;
    auto align4 = [&]() {
        while ((gltf_buffer.size() & 3u) != 0)
            gltf_buffer.push_back(0);
    };
    auto append_float_array = [&](const float *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };
    auto append_u16_array = [&](const uint16_t *values, size_t count) -> size_t {
        align4();
        size_t offset = gltf_buffer.size();
        for (size_t i = 0; i < count; i++)
            append_bytes(gltf_buffer, values[i]);
        return offset;
    };

    static const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    static const float normals[9] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    static const float morph_positions[9] = {
        0.0f, 0.0f, 0.0f, 0.25f, 0.5f, 0.0f, 0.0f, -0.25f, 0.0f};
    static const float morph_normals[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.0f, 0.0f, 0.0f, -0.1f};
    static const float morph_tangents[9] = {0.0f, 0.0f, 0.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, -0.2f};
    static const uint16_t indices[3] = {0, 1, 2};

    size_t pos_off = append_float_array(positions, 9);
    size_t norm_off = append_float_array(normals, 9);
    size_t morph_pos_off = append_float_array(morph_positions, 9);
    size_t morph_norm_off = append_float_array(morph_normals, 9);
    size_t morph_tangent_off = append_float_array(morph_tangents, 9);
    size_t idx_off = append_u16_array(indices, 3);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());

    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(pos_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(norm_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_pos_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_norm_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_tangent_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(idx_off) +
        ",\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":5,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"weights\":[0.25],\"extras\":{\"targetNames\":[\"Smile\"]},"
        "\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":5,"
        "\"targets\":[{\"POSITION\":2,\"NORMAL\":3,\"TANGENT\":4}]}]}],"
        "\"nodes\":[{\"name\":\"MorphNodeA\",\"mesh\":0,\"weights\":[0.75]},"
        "{\"name\":\"MorphNodeB\",\"mesh\":0,\"weights\":[0.10]}],"
        "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0"
        "}";

    FILE *gltf = std::fopen(gltf_path, "wb");
    EXPECT_TRUE(gltf != nullptr, "Morph-target glTF fixture can be created");
    if (!gltf)
        return;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);

    std::vector<uint8_t> root;
    EXPECT_TRUE(read_file_bytes(gltf_path, root), "Morph preload root bytes can be read");
    if (!root.empty()) {
        uint8_t *owned_root = static_cast<uint8_t *>(std::malloc(root.size()));
        EXPECT_TRUE(owned_root != nullptr, "Morph preload root copy can be allocated");
        if (owned_root) {
            std::memcpy(owned_root, root.data(), root.size());
            char error[128] = {0};
            rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create(
                rt_const_cstr(gltf_path), owned_root, root.size(), 0, error, sizeof(error));
            EXPECT_TRUE(bundle != nullptr, "Preload bundle can stage morph-target mesh payloads");
            EXPECT_TRUE(error[0] == '\0', "Morph preload bundle build has no terminal error");
            EXPECT_TRUE(rt_gltf_preload_bundle_decoded_mesh_count(bundle) == 1,
                        "Preload bundle worker-decodes morph-target meshes to POD");
            if (bundle) {
                void *preloaded =
                    rt_gltf_load_preloaded_bundle(rt_const_cstr(gltf_path), bundle, 0);
                EXPECT_TRUE(preloaded != nullptr, "Preloaded morph bundle builds an asset");
                auto *preloaded_mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(preloaded, 0));
                EXPECT_TRUE(preloaded_mesh != nullptr &&
                                preloaded_mesh->morph_targets_ref != nullptr,
                            "Preloaded morph bundle exposes attached morph targets");
                if (preloaded_mesh && preloaded_mesh->morph_targets_ref) {
                    EXPECT_TRUE(
                        rt_morphtarget3d_get_shape_count(preloaded_mesh->morph_targets_ref) == 1,
                        "Preloaded morph bundle imports one blend shape");
                    EXPECT_TRUE(
                        rt_morphtarget3d_has_tangent_deltas(preloaded_mesh->morph_targets_ref) == 1,
                        "Preloaded morph bundle imports tangent deltas");
                    EXPECT_NEAR(rt_morphtarget3d_get_weight(preloaded_mesh->morph_targets_ref, 0),
                                0.25,
                                0.001,
                                "Preloaded morph bundle preserves default weight");
                    const float *preloaded_pos =
                        rt_morphtarget3d_get_packed_deltas(preloaded_mesh->morph_targets_ref);
                    const float *preloaded_norm = rt_morphtarget3d_get_packed_normal_deltas(
                        preloaded_mesh->morph_targets_ref);
                    EXPECT_TRUE(preloaded_pos != nullptr,
                                "Preloaded morph bundle packs position deltas");
                    EXPECT_TRUE(preloaded_norm != nullptr,
                                "Preloaded morph bundle packs normal deltas");
                    if (preloaded_pos)
                        EXPECT_NEAR(preloaded_pos[4],
                                    0.5,
                                    0.001,
                                    "Preloaded morph bundle keeps vertex 1 position delta Y");
                    if (preloaded_norm)
                        EXPECT_NEAR(preloaded_norm[8],
                                    -0.1,
                                    0.001,
                                    "Preloaded morph bundle keeps vertex 2 normal delta Z");
                }
                void *preloaded_root = rt_gltf_get_scene_root(preloaded);
                void *preloaded_node =
                    preloaded_root ? rt_scene_node3d_get_child(preloaded_root, 0) : nullptr;
                auto *preloaded_scene_mesh =
                    preloaded_node
                        ? static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(preloaded_node))
                        : nullptr;
                if (preloaded_scene_mesh && preloaded_scene_mesh->morph_targets_ref) {
                    EXPECT_NEAR(
                        rt_morphtarget3d_get_weight(preloaded_scene_mesh->morph_targets_ref, 0),
                        0.75,
                        0.001,
                        "Preloaded morph bundle applies node morph weights");
                }
            }
        }
    }

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses morph-target assets");
    if (!asset)
        return;

    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr, "GLTF.Load exposes the morphed mesh");
    if (!mesh)
        return;
    EXPECT_TRUE(mesh->morph_targets_ref != nullptr, "GLTF.Load binds morph targets to the mesh");
    if (!mesh->morph_targets_ref)
        return;

    EXPECT_TRUE(rt_morphtarget3d_get_shape_count(mesh->morph_targets_ref) == 1,
                "GLTF.Load imports one blend shape");
    EXPECT_TRUE(rt_morphtarget3d_has_tangent_deltas(mesh->morph_targets_ref) == 1,
                "GLTF.Load imports morph tangent deltas");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(mesh->morph_targets_ref, 0),
                0.25,
                0.001,
                "GLTF.Load preserves mesh default morph weights on the shared asset mesh");
    void *scene_root = rt_gltf_get_scene_root(asset);
    void *scene_node = scene_root ? rt_scene_node3d_get_child(scene_root, 0) : nullptr;
    void *scene_node_b = scene_root ? rt_scene_node3d_get_child(scene_root, 1) : nullptr;
    auto *scene_mesh =
        scene_node ? static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(scene_node)) : nullptr;
    auto *scene_mesh_b =
        scene_node_b ? static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(scene_node_b)) : nullptr;
    EXPECT_TRUE(scene_mesh != nullptr && scene_mesh != mesh,
                "GLTF.Load creates a per-node morph variant when node weights override defaults");
    if (scene_mesh && scene_mesh->morph_targets_ref) {
        EXPECT_NEAR(rt_morphtarget3d_get_weight(scene_mesh->morph_targets_ref, 0),
                    0.75,
                    0.001,
                    "GLTF.Load applies node morph weights to the scene-node mesh variant");
    }
    EXPECT_TRUE(scene_mesh_b != nullptr && scene_mesh_b != mesh && scene_mesh_b != scene_mesh,
                "GLTF.Load creates independent variants for repeated mesh nodes");
    if (scene_mesh_b && scene_mesh_b->morph_targets_ref) {
        EXPECT_NEAR(rt_morphtarget3d_get_weight(scene_mesh_b->morph_targets_ref, 0),
                    0.10,
                    0.001,
                    "GLTF.Load keeps repeated-node morph weights independent");
    }
    const float *pos_deltas = rt_morphtarget3d_get_packed_deltas(mesh->morph_targets_ref);
    const float *normal_deltas = rt_morphtarget3d_get_packed_normal_deltas(mesh->morph_targets_ref);
    EXPECT_TRUE(pos_deltas != nullptr, "GLTF.Load packs morph position deltas");
    EXPECT_TRUE(normal_deltas != nullptr, "GLTF.Load packs morph normal deltas");
    if (pos_deltas) {
        EXPECT_NEAR(pos_deltas[3], 0.25, 0.001, "GLTF morph keeps vertex 1 position delta X");
        EXPECT_NEAR(pos_deltas[4], 0.5, 0.001, "GLTF morph keeps vertex 1 position delta Y");
        EXPECT_NEAR(pos_deltas[7], -0.25, 0.001, "GLTF morph keeps vertex 2 position delta Y");
    }
    if (normal_deltas) {
        EXPECT_NEAR(normal_deltas[4], 0.1, 0.001, "GLTF morph keeps vertex 1 normal delta Y");
        EXPECT_NEAR(normal_deltas[8], -0.1, 0.001, "GLTF morph keeps vertex 2 normal delta Z");
    }
}

static void test_gltf_rejects_malformed_glb_headers() {
    const char *glb_path = "/tmp/zanna_gltf_bad_header.glb";
    std::vector<uint8_t> glb;
    std::string json = "{\"asset\":{\"version\":\"2.0\"}}";
    while ((json.size() & 3u) != 0)
        json.push_back(' ');

    glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
    append_u32_le(glb, 1); /* invalid: only GLB 2 is supported */
    append_u32_le(glb, (uint32_t)(12 + 8 + json.size()));
    append_u32_le(glb, (uint32_t)json.size());
    append_u32_le(glb, 0x4E4F534Au);
    glb.insert(glb.end(), json.begin(), json.end());

    FILE *f = std::fopen(glb_path, "wb");
    EXPECT_TRUE(f != nullptr, "Malformed GLB fixture can be created");
    if (!f)
        return;
    std::fwrite(glb.data(), 1, glb.size(), f);
    std::fclose(f);

    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(glb_path)) == nullptr,
                "GLTF.Load rejects unsupported GLB versions");
}

static void test_gltf_rejects_corrupt_required_image_payload() {
    const char *gltf_path = "/tmp/zanna_gltf_corrupt_required_image.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"images\":[{\"uri\":\"data:image/png;base64,AAAA\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Corrupt required-image glTF fixture can be written");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(gltf_path)) == nullptr,
                "GLTF.Load rejects corrupt texture image payloads instead of dropping the map");
}

static void test_gltf_rejects_corrupt_extension_texture_payloads() {
    const char *gltf_path = "/tmp/zanna_gltf_corrupt_extension_texture.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"KHR_materials_clearcoat\",\"KHR_materials_transmission\"],"
        "\"images\":[{\"uri\":\"data:image/png;base64,AAAA\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_clearcoat\":{\"clearcoatFactor\":0.5,"
        "\"clearcoatTexture\":{\"index\":0}},"
        "\"KHR_materials_transmission\":{\"transmissionFactor\":0.4,"
        "\"transmissionTexture\":{\"index\":0}}}}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Corrupt extension-texture glTF fixture can be written");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(gltf_path)) == nullptr,
                "GLTF.Load rejects corrupt clearcoat/transmission texture payloads");
}

static void test_gltf_rejects_unsafe_external_buffer_paths() {
    const char *gltf_path = "/tmp/zanna_gltf_unsafe_uri.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"../outside.bin\",\"byteLength\":4}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Unsafe-URI glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(gltf_path)) == nullptr,
                "GLTF.Load rejects external resource paths outside the asset directory");
}

static void test_gltf_accepts_dot_relative_external_buffer_paths() {
    const char *gltf_path = "/tmp/zanna_gltf_dot_relative_uri.gltf";
    const char *bin_path = "/tmp/zanna_gltf_dot_relative_uri.bin";
    const float scalar = 1.0f;
    FILE *bin = std::fopen(bin_path, "wb");
    EXPECT_TRUE(bin != nullptr, "Dot-relative glTF buffer fixture can be created");
    if (!bin)
        return;
    std::fwrite(&scalar, 1, sizeof(scalar), bin);
    std::fclose(bin);
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"./zanna_gltf_dot_relative_uri.bin\",\"byteLength\":4}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Dot-relative URI glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(gltf_path)) != nullptr,
                "GLTF.Load accepts safe ./ external resource paths");
}

static void test_gltf_rejects_percent_decoded_nul_external_paths() {
    const char *gltf_path = "/tmp/zanna_gltf_nul_uri.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"textures/%00.bin\",\"byteLength\":4}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "NUL-URI glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(gltf_path)) == nullptr,
                "GLTF.Load rejects URI paths containing percent-decoded NUL bytes");
}

static void test_gltf_rejects_control_chars_in_external_paths() {
    const char *raw_path = "/tmp/zanna_gltf_raw_control_uri.gltf";
    std::string raw_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"textures/line\\nbreak.bin\",\"byteLength\":4}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}]"
        "}";
    EXPECT_TRUE(write_text_file(raw_path, raw_json), "Raw-control URI glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(raw_path)) == nullptr,
                "GLTF.Load rejects raw control characters in external paths");

    const char *encoded_path = "/tmp/zanna_gltf_encoded_control_uri.gltf";
    std::string encoded_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"textures/%0A.bin\",\"byteLength\":4}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}]"
        "}";
    EXPECT_TRUE(write_text_file(encoded_path, encoded_json),
                "Encoded-control URI glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(encoded_path)) == nullptr,
                "GLTF.Load rejects percent-decoded control characters in external paths");
}

static void test_gltf_rejects_external_uri_schemes_and_malformed_escapes() {
    const char *scheme_path = "/tmp/zanna_gltf_scheme_uri.gltf";
    std::string scheme_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"file:secret.bin\",\"byteLength\":4}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}]"
        "}";
    EXPECT_TRUE(write_text_file(scheme_path, scheme_json),
                "Scheme-URI glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(scheme_path)) == nullptr,
                "GLTF.Load rejects external URI schemes without relying on ://");

    const char *escape_path = "/tmp/zanna_gltf_bad_escape_uri.gltf";
    std::string escape_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"buffers/%GG.bin\",\"byteLength\":4}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}]"
        "}";
    EXPECT_TRUE(write_text_file(escape_path, escape_json),
                "Bad-escape glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(escape_path)) == nullptr,
                "GLTF.Load rejects malformed percent escapes in external paths");
}

static void test_gltf_rejects_malformed_data_uri_percent_escapes() {
    const char *gltf_path = "/tmp/zanna_gltf_bad_data_uri_escape.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream,%GG\",\"byteLength\":1}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Bad data-URI escape glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(gltf_path)) == nullptr,
                "GLTF.Load rejects malformed percent escapes in data URI payloads");
}

static void test_gltf_rejects_invalid_node_resource_references() {
    const char *mesh_path = "/tmp/zanna_gltf_invalid_node_mesh.gltf";
    std::string mesh_json = "{"
                            "\"asset\":{\"version\":\"2.0\"},"
                            "\"meshes\":[{\"primitives\":[]}],"
                            "\"nodes\":[{\"name\":\"BadMesh\",\"mesh\":1}],"
                            "\"scenes\":[{\"nodes\":[0]}],"
                            "\"scene\":0"
                            "}";
    EXPECT_TRUE(write_text_file(mesh_path, mesh_json),
                "Invalid node mesh glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(mesh_path)) == nullptr,
                "GLTF.Load rejects nodes that reference missing meshes");

    const char *light_path = "/tmp/zanna_gltf_invalid_node_light.gltf";
    std::string light_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"KHR_lights_punctual\"],"
        "\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":\"point\"}]}},"
        "\"nodes\":[{\"name\":\"BadLight\","
        "\"extensions\":{\"KHR_lights_punctual\":{\"light\":1}}}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(light_path, light_json),
                "Invalid node light glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(light_path)) == nullptr,
                "GLTF.Load rejects nodes that reference missing punctual lights");
}

static void test_gltf_rejects_invalid_declared_scene_roots() {
    const char *gltf_path = "/tmp/zanna_gltf_invalid_declared_scene_root.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"nodes\":[{\"name\":\"Parent\",\"children\":[1]},{\"name\":\"Child\"}],"
        "\"scenes\":[{\"nodes\":[1]}],"
        "\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Invalid declared-scene-root glTF fixture can be created");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(gltf_path)) == nullptr,
                "GLTF.Load rejects declared scene roots that are children of other nodes");
}

static void test_gltf_material_without_pbr_uses_pbr_defaults() {
    const char *gltf_path = "/tmp/zanna_gltf_material_no_pbr.gltf";
    std::string gltf_json = "{\"asset\":{\"version\":\"2.0\"},\"materials\":[{}]}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "No-PBR material glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses material objects without PBR blocks");
    if (!asset)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr, "No-PBR glTF material is exposed");
    if (!mat)
        return;
    EXPECT_TRUE(mat->workflow == 1, "No-PBR glTF materials use Material3D's PBR workflow");
    EXPECT_NEAR(mat->metallic, 1.0, 0.001, "No-PBR glTF material default metallic is 1");
    EXPECT_NEAR(mat->roughness, 1.0, 0.001, "No-PBR glTF material default roughness is 1");
}

static void test_gltf_ignores_wrong_typed_optional_string_fields() {
    const char *gltf_path = "/tmp/zanna_gltf_wrong_typed_optional_strings.gltf";
    std::string gltf_json = "{\"asset\":{\"version\":\"2.0\"},"
                            "\"images\":[{\"uri\":17,\"mimeType\":18}],"
                            "\"textures\":[{\"source\":0}],"
                            "\"materials\":[{\"alphaMode\":19,"
                            "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Wrong-typed optional string glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr,
                "GLTF.Load ignores wrong-typed optional string fields without trapping");
    if (!asset)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr, "Wrong-typed optional string fixture exposes a material");
    if (!mat)
        return;
    EXPECT_TRUE(mat->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE,
                "Wrong-typed alphaMode falls back to opaque");
}

static void test_gltf_assigns_default_material_to_materialless_primitives() {
    const char *gltf_path = "/tmp/zanna_gltf_default_material.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"nodes\":[{\"name\":\"Matless\",\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Materialless glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses materialless primitives");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_material_count(asset) == 1,
                "GLTF.Load creates a shared default material for materialless primitives");
    void *node = rt_scene_node3d_find(rt_gltf_get_scene_root(asset), rt_const_cstr("Matless"));
    EXPECT_TRUE(node != nullptr && rt_scene_node3d_get_material(node) != nullptr,
                "GLTF scene nodes bind the default material so geometry renders");
}

static void test_gltf_uses_texture_texcoord_and_transform() {
    const char *gltf_path = "/tmp/zanna_gltf_texcoord_transform.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float uv0[6] = {0.0f, 0.0f, 0.1f, 0.1f, 0.2f, 0.2f};
    const float uv1[6] = {0.25f, 0.5f, 0.5f, 0.25f, 1.0f, 0.75f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uv0)
        append_bytes(gltf_buffer, v);
    for (float v : uv1)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"textures\":[{}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"texCoord\":1,\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.1,0.2],"
        "\"scale\":[2.0,3.0]}}}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1,"
        "\"TEXCOORD_1\":2},\"indices\":3,\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "UV-transform glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses texture-info UV transforms");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mesh != nullptr, "GLTF.Load exposes UV-transform mesh");
    EXPECT_TRUE(mat != nullptr, "GLTF.Load exposes UV-transform material");
    if (!mesh || !mat)
        return;
    EXPECT_NEAR(mesh->vertices[0].uv[0], 0.0, 0.001, "GLTF.Load preserves TEXCOORD_0 on vertex");
    EXPECT_NEAR(mesh->vertices[0].uv1[0], 0.25, 0.001, "GLTF.Load preserves TEXCOORD_1 on vertex");
    EXPECT_TRUE(mat->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] == 1,
                "GLTF.Load records baseColorTexture texCoord=1 on the material slot");
    EXPECT_NEAR(mat->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][0],
                2.0,
                0.001,
                "GLTF.Load records texture transform scale.x");
    EXPECT_NEAR(mat->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][3],
                3.0,
                0.001,
                "GLTF.Load records texture transform scale.y");
    EXPECT_NEAR(mat->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][4],
                0.1,
                0.001,
                "GLTF.Load records texture transform offset.x");
    EXPECT_NEAR(mat->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][5],
                0.2,
                0.001,
                "GLTF.Load records texture transform offset.y");
}

/**
 * @brief Build a one-triangle glTF fixture with selectable authored UV streams/material demands.
 * @details `INT_MIN` means the corresponding material has no texture. Any other texcoord is
 *          serialized verbatim, including deliberately invalid negative/UV2 values. When a
 *          variant texcoord is supplied the primitive maps one KHR_materials_variants entry to
 *          that second material.
 * @param base_texcoord Base material texture coordinate, or `INT_MIN` for an untextured base.
 * @param variant_texcoord Variant material texture coordinate, or `INT_MIN` for no variant.
 * @param include_uv0 Whether the primitive carries a valid TEXCOORD_0 accessor.
 * @param include_uv1 Whether the primitive carries a valid TEXCOORD_1 accessor.
 * @return Complete self-contained JSON glTF document with a data-URI buffer.
 */
static std::string build_texture_uv_requirement_fixture(int base_texcoord,
                                                        int variant_texcoord,
                                                        bool include_uv0,
                                                        bool include_uv1) {
    std::vector<uint8_t> buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float uv[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t indices[3] = {0, 1, 2};
    std::string views;
    std::string accessors;
    std::string attributes = "\"POSITION\":0";
    int view_index = 0;
    int accessor_index = 0;
    auto add_view_accessor =
        [&](size_t offset, size_t length, int component_type, const char *type) {
            if (!views.empty()) {
                views += ',';
                accessors += ',';
            }
            views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                     ",\"byteLength\":" + std::to_string(length) + "}";
            accessors += "{\"bufferView\":" + std::to_string(view_index++) +
                         ",\"componentType\":" + std::to_string(component_type) +
                         ",\"count\":3,\"type\":\"" + type + "\"}";
            return accessor_index++;
        };
    for (float value : positions)
        append_bytes(buffer, value);
    add_view_accessor(0u, sizeof(positions), 5126, "VEC3");
    if (include_uv0) {
        size_t offset = buffer.size();
        for (float value : uv)
            append_bytes(buffer, value);
        attributes += ",\"TEXCOORD_0\":" +
                      std::to_string(add_view_accessor(offset, sizeof(uv), 5126, "VEC2"));
    }
    if (include_uv1) {
        size_t offset = buffer.size();
        for (float value : uv)
            append_bytes(buffer, value);
        attributes += ",\"TEXCOORD_1\":" +
                      std::to_string(add_view_accessor(offset, sizeof(uv), 5126, "VEC2"));
    }
    size_t index_offset = buffer.size();
    for (uint16_t value : indices)
        append_bytes(buffer, value);
    int index_accessor = add_view_accessor(index_offset, sizeof(indices), 5123, "SCALAR");
    auto material_json = [](int texcoord) {
        if (texcoord == INT_MIN)
            return std::string("{}");
        return std::string("{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,") +
               "\"texCoord\":" + std::to_string(texcoord) + "}}}";
    };
    std::string extension_prefix;
    std::string primitive_extension;
    std::string materials = material_json(base_texcoord);
    if (variant_texcoord != INT_MIN) {
        extension_prefix =
            "\"extensionsUsed\":[\"KHR_materials_variants\"],"
            "\"extensions\":{\"KHR_materials_variants\":{\"variants\":[{\"name\":\"alt\"}]}},";
        primitive_extension =
            ",\"extensions\":{\"KHR_materials_variants\":{\"mappings\":[{\"material\":1,"
            "\"variants\":[0]}]}}";
        materials += ',' + material_json(variant_texcoord);
    }
    std::string encoded = base64_encode(buffer.data(), buffer.size());
    return "{\"asset\":{\"version\":\"2.0\"}," + extension_prefix +
           "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," + encoded +
           "\",\"byteLength\":" + std::to_string(buffer.size()) + "}],\"bufferViews\":[" + views +
           "],\"accessors\":[" + accessors + "],\"textures\":[{}],\"materials\":[" + materials +
           "],\"meshes\":[{\"primitives\":[{\"attributes\":{" + attributes +
           "},\"indices\":" + std::to_string(index_accessor) + ",\"material\":0" +
           primitive_extension + "}]}]}";
}

/// @brief Exercise UV0/UV1 success plus missing, unsupported, variant, and preload failures.
static void test_gltf_validates_authored_texture_uv_streams() {
    const char *path = "/tmp/zanna_gltf_texture_uv_validation.gltf";
    EXPECT_TRUE(
        write_text_file(path, build_texture_uv_requirement_fixture(0, INT_MIN, true, false)),
        "UV0 requirement success fixture can be written");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) != nullptr,
                "GLTF.Load accepts an authored UV0 texture when TEXCOORD_0 decoded");

    EXPECT_TRUE(
        write_text_file(path, build_texture_uv_requirement_fixture(1, INT_MIN, false, true)),
        "UV1 requirement success fixture can be written");
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) != nullptr,
                "GLTF.Load accepts an authored UV1 texture when TEXCOORD_1 decoded");

    EXPECT_TRUE(
        write_text_file(path, build_texture_uv_requirement_fixture(0, INT_MIN, false, false)),
        "Missing UV0 fixture can be written");
    rt_asset_error_clear();
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) == nullptr,
                "GLTF.Load rejects a texture whose TEXCOORD_0 stream is missing");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT &&
                    std::strstr(rt_asset_error_get_message(), "missing TEXCOORD_0") != nullptr,
                "Missing UV0 rejection records a precise corrupt-data diagnostic");

    EXPECT_TRUE(
        write_text_file(path, build_texture_uv_requirement_fixture(1, INT_MIN, true, false)),
        "Missing UV1 fixture can be written");
    rt_asset_error_clear();
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) == nullptr,
                "GLTF.Load rejects a texture whose TEXCOORD_1 stream is missing");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT &&
                    std::strstr(rt_asset_error_get_message(), "missing TEXCOORD_1") != nullptr,
                "Missing UV1 rejection records a precise corrupt-data diagnostic");

    for (int unsupported : {-1, 2}) {
        EXPECT_TRUE(
            write_text_file(path,
                            build_texture_uv_requirement_fixture(unsupported, INT_MIN, true, true)),
            "Unsupported texture coordinate fixture can be written");
        rt_asset_error_clear();
        EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) == nullptr,
                    "GLTF.Load rejects an unrepresentable texture coordinate index");
        EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_UNSUPPORTED &&
                        std::strstr(rt_asset_error_get_message(), "unsupported TEXCOORD_") !=
                            nullptr,
                    "Unsupported UV rejection records a precise capability diagnostic");
    }

    EXPECT_TRUE(
        write_text_file(path, build_texture_uv_requirement_fixture(INT_MIN, 1, true, false)),
        "Variant UV mismatch fixture can be written");
    rt_asset_error_clear();
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) == nullptr,
                "GLTF.Load validates variant materials before committing a primitive");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT &&
                    std::strstr(rt_asset_error_get_message(), "material 1") != nullptr,
                "Variant UV mismatch identifies the mapped material");

    std::string missing_uv_json = build_texture_uv_requirement_fixture(0, INT_MIN, false, false);
    uint8_t *owned = static_cast<uint8_t *>(std::malloc(missing_uv_json.size()));
    EXPECT_TRUE(owned != nullptr, "Missing-UV preload root copy can be allocated");
    if (owned) {
        std::memcpy(owned, missing_uv_json.data(), missing_uv_json.size());
        char error[160] = {0};
        rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create_cstr(
            path, owned, missing_uv_json.size(), 0, error, sizeof(error));
        EXPECT_TRUE(bundle != nullptr,
                    "Preload worker can stage a primitive before material commit");
        if (bundle) {
            rt_asset_error_clear();
            EXPECT_TRUE(rt_gltf_load_preloaded_bundle(rt_const_cstr(path), bundle, 0) == nullptr,
                        "Preloaded commit rejects the same missing UV0 stream");
            EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT &&
                            std::strstr(rt_asset_error_get_message(), "missing TEXCOORD_0") !=
                                nullptr,
                        "Preloaded UV rejection preserves the precise diagnostic");
        }
    }
    std::remove(path);
}

static std::string build_variant_fixture_json() {
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    return "{\"asset\":{\"version\":\"2.0\"},"
           "\"extensionsUsed\":[\"KHR_materials_variants\"],"
           "\"extensionsRequired\":[\"KHR_materials_variants\"],"
           "\"extensions\":{\"KHR_materials_variants\":{\"variants\":"
           "[{\"name\":\"day\"},{\"name\":\"night\"}]}},"
           "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
           buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
           "}],"
           "\"bufferViews\":["
           "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
           "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
           "],"
           "\"accessors\":["
           "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
           "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
           "],"
           "\"materials\":["
           "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,0.0,0.0,1.0]}},"
           "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.0,0.0,1.0,1.0]}}"
           "],"
           "\"meshes\":[{\"primitives\":[{"
           "\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0,"
           "\"extensions\":{\"KHR_materials_variants\":{\"mappings\":"
           "[{\"material\":1,\"variants\":[1]}]}}"
           "}]}],"
           "\"nodes\":[{\"mesh\":0,\"name\":\"variant_node\"}],"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"scene\":0"
           "}";
}

static void test_gltf_imports_material_variants() {
    const char *gltf_path = "/tmp/zanna_gltf_material_variants.gltf";
    EXPECT_TRUE(write_text_file(gltf_path, build_variant_fixture_json()),
                "Material-variants glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts required KHR_materials_variants");
    if (!asset)
        return;
    EXPECT_TRUE(rt_gltf_variant_count(asset) == 2, "GLTF.Load imports both variant names");
    rt_string day = rt_gltf_get_variant_name(asset, 0);
    rt_string night = rt_gltf_get_variant_name(asset, 1);
    EXPECT_TRUE(day && std::strcmp(rt_string_cstr(day), "day") == 0,
                "GLTF.Load imports variant name 0");
    EXPECT_TRUE(night && std::strcmp(rt_string_cstr(night), "night") == 0,
                "GLTF.Load imports variant name 1");
    rt_string out_of_range = rt_gltf_get_variant_name(asset, 7);
    EXPECT_TRUE(out_of_range && rt_string_cstr(out_of_range)[0] == '\0',
                "Out-of-range variant name is empty");
}

// --- EXT_meshopt_compression test-side encoders (emit spec-conformant streams) ---

static void meshopt_test_push_leb(std::vector<uint8_t> &out, uint32_t v) {
    while (v >= 0x80u) {
        out.push_back((uint8_t)((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    out.push_back((uint8_t)v);
}

static uint32_t meshopt_test_zigzag32(int32_t delta) {
    return delta < 0 ? (((~(uint32_t)delta) << 1) | 1u) : ((uint32_t)delta << 1);
}

/// Mode 0 stream using raw byte groups (header bits = 3) and a zero baseline tail.
static std::vector<uint8_t> meshopt_test_encode_attributes(const uint8_t *elements,
                                                           size_t count,
                                                           size_t stride) {
    std::vector<uint8_t> out;
    out.push_back(0xA0);
    size_t max_block = std::min<size_t>((8192u / stride) & ~(size_t)15u, 256u);
    std::vector<uint8_t> baseline(stride, 0);
    size_t done = 0;
    while (done < count) {
        size_t block = std::min(count - done, max_block);
        size_t groups = (block + 15u) / 16u;
        for (size_t k = 0; k < stride; k++) {
            for (size_t h = 0; h < (groups + 3u) / 4u; h++)
                out.push_back(0xFF); /* every group: raw bytes (bits = 3) */
            uint8_t prev = baseline[k];
            for (size_t g = 0; g < groups; g++) {
                for (int j = 0; j < 16; j++) {
                    size_t e = g * 16u + (size_t)j;
                    uint8_t cur = e < block ? elements[(done + e) * stride + k] : prev;
                    uint8_t delta = (uint8_t)(cur - prev);
                    uint8_t zig =
                        (delta & 0x80u) ? (uint8_t)~(uint8_t)(delta << 1) : (uint8_t)(delta << 1);
                    out.push_back(zig);
                    if (e < block)
                        prev = cur;
                }
            }
            baseline[k] = prev;
        }
        done += block;
    }
    size_t tail = stride < 32u ? 32u : stride;
    for (size_t i = 0; i < tail; i++)
        out.push_back(0); /* zero baseline element (padded) */
    return out;
}

/// Mode 1 stream where every triangle uses code 0xff with fully explicit indices.
static std::vector<uint8_t> meshopt_test_encode_triangles(const uint32_t *indices, size_t count) {
    std::vector<uint8_t> out;
    out.push_back(0xE1);
    size_t tris = count / 3u;
    for (size_t t = 0; t < tris; t++)
        out.push_back(0xFF);
    uint32_t last = 0;
    for (size_t t = 0; t < tris; t++) {
        out.push_back(0xFF); /* aux: Z = W = 0xf -> a, b, c all explicit */
        for (int j = 0; j < 3; j++) {
            uint32_t v = indices[t * 3u + (size_t)j];
            meshopt_test_push_leb(out, meshopt_test_zigzag32((int32_t)(v - last)));
            last = v;
        }
    }
    for (int i = 0; i < 16; i++)
        out.push_back(0); /* codeaux tail (all-zero satisfies the table constraints) */
    return out;
}

/// Mode 2 stream using baseline 0 for every delta.
static std::vector<uint8_t> meshopt_test_encode_indices(const uint32_t *indices, size_t count) {
    std::vector<uint8_t> out;
    out.push_back(0xD1);
    uint32_t last = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t delta = (int32_t)(indices[i] - last);
        uint32_t v = delta < 0 ? (((~(uint32_t)delta) << 2) | 2u) : ((uint32_t)delta << 2);
        meshopt_test_push_leb(out, v);
        last = indices[i];
    }
    for (int i = 0; i < 4; i++)
        out.push_back(0); /* reserved tail */
    return out;
}

/// Build a triangle .gltf whose POSITION and index views are meshopt-compressed.
/// The parent views reference a data-less placeholder fallback buffer per the spec.
static std::string meshopt_test_build_gltf(const std::vector<uint8_t> &pos_stream,
                                           const std::vector<uint8_t> &idx_stream,
                                           const char *index_mode) {
    std::vector<uint8_t> compressed = pos_stream;
    compressed.insert(compressed.end(), idx_stream.begin(), idx_stream.end());
    std::string b64 = base64_encode(compressed.data(), compressed.size());
    return "{\"asset\":{\"version\":\"2.0\"},"
           "\"extensionsUsed\":[\"EXT_meshopt_compression\"],"
           "\"extensionsRequired\":[\"EXT_meshopt_compression\"],"
           "\"buffers\":["
           "{\"uri\":\"data:application/octet-stream;base64," +
           b64 + "\",\"byteLength\":" + std::to_string(compressed.size()) +
           "},"
           "{\"byteLength\":44,\"extensions\":{\"EXT_meshopt_compression\":{\"fallback\":true}}}"
           "],"
           "\"bufferViews\":["
           "{\"buffer\":1,\"byteOffset\":0,\"byteLength\":36,\"byteStride\":12,"
           "\"extensions\":{\"EXT_meshopt_compression\":{"
           "\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
           std::to_string(pos_stream.size()) +
           ",\"byteStride\":12,\"count\":3,\"mode\":\"ATTRIBUTES\"}}},"
           "{\"buffer\":1,\"byteOffset\":36,\"byteLength\":6,"
           "\"extensions\":{\"EXT_meshopt_compression\":{"
           "\"buffer\":0,\"byteOffset\":" +
           std::to_string(pos_stream.size()) +
           ",\"byteLength\":" + std::to_string(idx_stream.size()) +
           ",\"byteStride\":2,\"count\":3,\"mode\":\"" + index_mode +
           "\"}}}"
           "],"
           "\"accessors\":["
           "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
           "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
           "],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"scene\":0"
           "}";
}

static void meshopt_test_check_triangle_asset(void *asset, const char *label) {
    char message[160];
    snprintf(message, sizeof(message), "%s: asset loads", label);
    EXPECT_TRUE(asset != nullptr, message);
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    snprintf(message, sizeof(message), "%s: mesh decodes", label);
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3, message);
    if (!mesh || mesh->vertex_count != 3)
        return;
    snprintf(message, sizeof(message), "%s: decompressed positions match", label);
    EXPECT_TRUE(mesh->vertices[0].pos[0] == 0.0f && mesh->vertices[1].pos[0] == 1.0f &&
                    mesh->vertices[2].pos[1] == 1.0f,
                message);
    snprintf(message, sizeof(message), "%s: decompressed indices match", label);
    EXPECT_TRUE(mesh->indices[0] == 0 && mesh->indices[1] == 1 && mesh->indices[2] == 2, message);
}

static void test_gltf_meshopt_compressed_views_roundtrip() {
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint32_t indices[3] = {0, 1, 2};
    std::vector<uint8_t> pos_bytes(sizeof(positions));
    std::memcpy(pos_bytes.data(), positions, sizeof(positions));
    std::vector<uint8_t> pos_stream = meshopt_test_encode_attributes(pos_bytes.data(), 3, 12);
    std::vector<uint8_t> tri_stream = meshopt_test_encode_triangles(indices, 3);
    std::vector<uint8_t> seq_stream = meshopt_test_encode_indices(indices, 3);

    const char *gltf_path = "/tmp/zanna_gltf_meshopt_triangles.gltf";
    EXPECT_TRUE(
        write_text_file(gltf_path, meshopt_test_build_gltf(pos_stream, tri_stream, "TRIANGLES")),
        "meshopt TRIANGLES fixture can be created");
    meshopt_test_check_triangle_asset(rt_gltf_load(rt_const_cstr(gltf_path)),
                                      "meshopt ATTRIBUTES+TRIANGLES");

    const char *seq_path = "/tmp/zanna_gltf_meshopt_indices.gltf";
    EXPECT_TRUE(
        write_text_file(seq_path, meshopt_test_build_gltf(pos_stream, seq_stream, "INDICES")),
        "meshopt INDICES fixture can be created");
    meshopt_test_check_triangle_asset(rt_gltf_load(rt_const_cstr(seq_path)),
                                      "meshopt ATTRIBUTES+INDICES");

    /* Corrupt stream: flip the attribute header byte; the load must fail cleanly. */
    std::vector<uint8_t> corrupt_stream = pos_stream;
    corrupt_stream[0] = 0x00;
    const char *corrupt_path = "/tmp/zanna_gltf_meshopt_corrupt.gltf";
    EXPECT_TRUE(write_text_file(corrupt_path,
                                meshopt_test_build_gltf(corrupt_stream, tri_stream, "TRIANGLES")),
                "meshopt corrupt fixture can be created");
    void *corrupt_asset = rt_gltf_load(rt_const_cstr(corrupt_path));
    EXPECT_TRUE(corrupt_asset == nullptr, "Corrupt meshopt stream fails the load");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "Corrupt meshopt stream reports Corrupt");
    EXPECT_TRUE(std::strstr(rt_asset_error_get_message(), "EXT_meshopt_compression") != nullptr,
                "Corrupt meshopt diagnostic names the extension");
}

static void test_gltf_meshopt_octahedral_filter() {
    /* Octahedral texels (x, y, one=127, passthrough): (127,0) -> +X, (0,0) -> +Z,
     * (-127,0) after hemisphere fold -> -X... use simple axis cases. Normals bind as
     * normalized BYTE VEC3 over a 4-byte-stride view (KHR_mesh_quantization). */
    const int8_t oct[12] = {127,
                            0,
                            127,
                            0, /* +X */
                            0,
                            127,
                            127,
                            0, /* +Y */
                            0,
                            0,
                            127,
                            0}; /* +Z */
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint32_t indices[3] = {0, 1, 2};
    std::vector<uint8_t> pos_bytes(sizeof(positions));
    std::memcpy(pos_bytes.data(), positions, sizeof(positions));
    std::vector<uint8_t> pos_stream = meshopt_test_encode_attributes(pos_bytes.data(), 3, 12);
    std::vector<uint8_t> norm_stream =
        meshopt_test_encode_attributes(reinterpret_cast<const uint8_t *>(oct), 3, 4);
    std::vector<uint8_t> tri_stream = meshopt_test_encode_triangles(indices, 3);

    std::vector<uint8_t> compressed = pos_stream;
    size_t norm_at = compressed.size();
    compressed.insert(compressed.end(), norm_stream.begin(), norm_stream.end());
    size_t idx_at = compressed.size();
    compressed.insert(compressed.end(), tri_stream.begin(), tri_stream.end());
    std::string b64 = base64_encode(compressed.data(), compressed.size());
    std::string gltf_json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"EXT_meshopt_compression\",\"KHR_mesh_quantization\"],"
        "\"extensionsRequired\":[\"EXT_meshopt_compression\",\"KHR_mesh_quantization\"],"
        "\"buffers\":["
        "{\"uri\":\"data:application/octet-stream;base64," +
        b64 + "\",\"byteLength\":" + std::to_string(compressed.size()) +
        "},"
        "{\"byteLength\":56}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":1,\"byteOffset\":0,\"byteLength\":36,\"byteStride\":12,"
        "\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":0,"
        "\"byteLength\":" +
        std::to_string(pos_stream.size()) +
        ",\"byteStride\":12,\"count\":3,\"mode\":\"ATTRIBUTES\"}}},"
        "{\"buffer\":1,\"byteOffset\":36,\"byteLength\":12,\"byteStride\":4,"
        "\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":" +
        std::to_string(norm_at) + ",\"byteLength\":" + std::to_string(norm_stream.size()) +
        ",\"byteStride\":4,\"count\":3,\"mode\":\"ATTRIBUTES\",\"filter\":\"OCTAHEDRAL\"}}},"
        "{\"buffer\":1,\"byteOffset\":48,\"byteLength\":6,"
        "\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":" +
        std::to_string(idx_at) + ",\"byteLength\":" + std::to_string(tri_stream.size()) +
        ",\"byteStride\":2,\"count\":3,\"mode\":\"TRIANGLES\"}}}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5120,\"normalized\":true,\"count\":3,"
        "\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
        "\"indices\":2}]}]"
        "}";
    const char *gltf_path = "/tmp/zanna_gltf_meshopt_octahedral.gltf";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "meshopt OCTAHEDRAL fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "meshopt OCTAHEDRAL asset loads");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3, "meshopt OCTAHEDRAL mesh decodes");
    if (!mesh || mesh->vertex_count != 3)
        return;
    EXPECT_NEAR(mesh->vertices[0].normal[0], 1.0, 0.02, "Octahedral filter decodes +X normal");
    EXPECT_NEAR(mesh->vertices[1].normal[1], 1.0, 0.02, "Octahedral filter decodes +Y normal");
    EXPECT_NEAR(mesh->vertices[2].normal[2], 1.0, 0.02, "Octahedral filter decodes +Z normal");
}

static void test_gltf_quantized_attributes_roundtrip() {
    /* KHR_mesh_quantization: SHORT (non-normalized) positions and normalized BYTE
     * normals decode to floats through the standard accessor rules. */
    const char *gltf_path = "/tmp/zanna_gltf_quantized.gltf";
    std::vector<uint8_t> gltf_buffer;
    const int16_t positions[9] = {0, 0, 0, 2, 0, 0, 0, 3, 0};
    const int8_t normals[9] = {0, 0, 127, 0, 0, 127, 0, 0, 127};
    const uint16_t indices[3] = {0, 1, 2};
    for (int16_t v : positions)
        append_bytes(gltf_buffer, v);
    while (gltf_buffer.size() % 4 != 0)
        gltf_buffer.push_back(0);
    size_t norm_off = gltf_buffer.size();
    for (int8_t v : normals)
        append_bytes(gltf_buffer, v);
    while (gltf_buffer.size() % 4 != 0)
        gltf_buffer.push_back(0);
    size_t idx_off = gltf_buffer.size();
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"KHR_mesh_quantization\"],"
        "\"extensionsRequired\":[\"KHR_mesh_quantization\"],"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":18},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(norm_off) +
        ",\"byteLength\":9},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(idx_off) +
        ",\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5122,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5120,\"normalized\":true,\"count\":3,"
        "\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
        "\"indices\":2}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Quantized glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts required KHR_mesh_quantization");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3, "Quantized mesh decodes");
    if (!mesh || mesh->vertex_count != 3)
        return;
    EXPECT_NEAR(mesh->vertices[1].pos[0], 2.0, 0.0001, "SHORT positions convert to plain floats");
    EXPECT_NEAR(mesh->vertices[2].pos[1], 3.0, 0.0001, "SHORT positions keep integer values");
    EXPECT_NEAR(
        mesh->vertices[0].normal[2], 1.0, 0.01, "Normalized BYTE normals decode to unit floats");
}

static void test_gltf_meshopt_exponential_filter() {
    /* Exact e/m pairs: 5.0, 0.5, -2.0, 1.0, 2.0, 4.0, 0.0, 3.0, -0.5 */
    const uint32_t words[9] = {
        0x00000005u, /* 2^0 * 5 = 5.0 */
        0xFF000001u, /* 2^-1 * 1 = 0.5 */
        0x01FFFFFFu, /* 2^1 * -1 = -2.0 */
        0x00000001u, /* 1.0 */
        0x01000001u, /* 2.0 */
        0x02000001u, /* 4.0 */
        0x00000000u, /* 0.0 */
        0x00000003u, /* 3.0 */
        0xFEFFFFFEu, /* 2^-2 * -2 = -0.5 */
    };
    uint8_t elements[36];
    for (int i = 0; i < 9; i++) {
        elements[i * 4 + 0] = (uint8_t)(words[i] & 0xFFu);
        elements[i * 4 + 1] = (uint8_t)((words[i] >> 8) & 0xFFu);
        elements[i * 4 + 2] = (uint8_t)((words[i] >> 16) & 0xFFu);
        elements[i * 4 + 3] = (uint8_t)((words[i] >> 24) & 0xFFu);
    }
    std::vector<uint8_t> pos_stream = meshopt_test_encode_attributes(elements, 3, 12);
    const uint32_t indices[3] = {0, 1, 2};
    std::vector<uint8_t> tri_stream = meshopt_test_encode_triangles(indices, 3);
    std::string gltf_json = meshopt_test_build_gltf(pos_stream, tri_stream, "TRIANGLES");
    const std::string needle = "\"mode\":\"ATTRIBUTES\"";
    size_t at = gltf_json.find(needle);
    EXPECT_TRUE(at != std::string::npos, "Exponential fixture finds the ATTRIBUTES view");
    gltf_json.replace(at, needle.size(), "\"mode\":\"ATTRIBUTES\",\"filter\":\"EXPONENTIAL\"");
    const char *gltf_path = "/tmp/zanna_gltf_meshopt_exponential.gltf";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "meshopt EXPONENTIAL fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "meshopt EXPONENTIAL asset loads");
    if (!asset)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_gltf_get_mesh(asset, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3, "meshopt EXPONENTIAL mesh decodes");
    if (!mesh || mesh->vertex_count != 3)
        return;
    EXPECT_NEAR(mesh->vertices[0].pos[0], 5.0, 0.0001, "Exponential filter decodes 2^0*5");
    EXPECT_NEAR(mesh->vertices[0].pos[1], 0.5, 0.0001, "Exponential filter decodes 2^-1*1");
    EXPECT_NEAR(mesh->vertices[0].pos[2], -2.0, 0.0001, "Exponential filter decodes 2^1*-1");
    EXPECT_NEAR(mesh->vertices[1].pos[0], 1.0, 0.0001, "Exponential filter decodes 1.0");
    EXPECT_NEAR(mesh->vertices[2].pos[2], -0.5, 0.0001, "Exponential filter decodes 2^-2*-2");
}

static void test_gltf_imports_material_extensions_supported_by_material3d() {
    const char *gltf_path = "/tmp/zanna_gltf_material_extensions.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"textures\":[{}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]},"
        "\"extensions\":{\"KHR_materials_unlit\":{},"
        "\"KHR_materials_specular\":{\"specularFactor\":0.25,"
        "\"specularColorFactor\":[0.2,0.3,0.4],\"specularTexture\":{\"index\":0}},"
        "\"KHR_materials_clearcoat\":{\"clearcoatFactor\":0.6,"
        "\"clearcoatRoughnessFactor\":0.35},"
        "\"KHR_materials_transmission\":{\"transmissionFactor\":0.5}}}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Material-extension glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses material extension assets");
    if (!asset)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr, "GLTF.Load exposes extension material");
    if (!mat)
        return;
    EXPECT_TRUE(mat->unlit == 1 && mat->shading_model == 3,
                "GLTF.Load maps KHR_materials_unlit to Material3D unlit shading");
    EXPECT_NEAR(mat->specular[0], 0.2, 0.001, "GLTF.Load imports specularColorFactor R");
    EXPECT_NEAR(mat->specular[1], 0.3, 0.001, "GLTF.Load imports specularColorFactor G");
    EXPECT_NEAR(mat->specular[2], 0.4, 0.001, "GLTF.Load imports specularColorFactor B");
    EXPECT_NEAR(mat->reflectivity, 0.6, 0.001, "GLTF.Load approximates clearcoat reflectivity");
    EXPECT_NEAR(mat->alpha, 1.0, 0.001, "GLTF.Load keeps transmission materials opaque");
    EXPECT_NEAR(mat->custom_params[2],
                0.35,
                0.001,
                "GLTF.Load records clearcoat roughness in material custom params");
    EXPECT_NEAR(mat->custom_params[3],
                0.5,
                0.001,
                "GLTF.Load records transmission factor in material custom params");
}

extern "C" {
void *rt_model3d_load(rt_string path);
void *rt_model3d_load_with_options(rt_string path, int8_t force_tangents);
void *rt_model3d_get_mesh(void *obj, int64_t index);
}

static void test_gltf_converts_spec_glossiness_materials() {
    const char *gltf_path = "/tmp/zanna_gltf_spec_gloss.gltf";
    std::string gltf_json = "{"
                            "\"asset\":{\"version\":\"2.0\"},"
                            "\"extensionsRequired\":[\"KHR_materials_pbrSpecularGlossiness\"],"
                            "\"extensionsUsed\":[\"KHR_materials_pbrSpecularGlossiness\"],"
                            "\"materials\":["
                            "{\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{"
                            "\"diffuseFactor\":[0.8,0.4,0.2,1.0],"
                            "\"specularFactor\":[0.04,0.04,0.04],"
                            "\"glossinessFactor\":0.7}}},"
                            "{\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{"
                            "\"diffuseFactor\":[0.1,0.1,0.1,1.0],"
                            "\"specularFactor\":[1.0,1.0,1.0],"
                            "\"glossinessFactor\":1.0}}}"
                            "]"
                            "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Spec-gloss glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr,
                "GLTF.Load accepts assets requiring KHR_materials_pbrSpecularGlossiness");
    if (!asset)
        return;
    auto *dielectric = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    auto *metal = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 1));
    EXPECT_TRUE(dielectric != nullptr && metal != nullptr,
                "GLTF.Load exposes converted spec-gloss materials");
    if (!dielectric || !metal)
        return;
    EXPECT_NEAR(
        dielectric->diffuse[0], 0.8, 0.001, "Spec-gloss diffuseFactor becomes the base color");
    EXPECT_NEAR(dielectric->roughness, 0.3, 0.001, "roughness = 1 - glossinessFactor");
    EXPECT_NEAR(
        dielectric->metallic, 0.0, 0.001, "Dielectric specular (0.04) converts to metallic 0");
    EXPECT_NEAR(metal->metallic, 1.0, 0.001, "Full specular converts to metallic 1");
    EXPECT_NEAR(metal->roughness, 0.0, 0.001, "Full glossiness converts to roughness 0");
}

static std::vector<uint8_t> build_test_bmp_1x1_bgra(uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    std::vector<uint8_t> bmp;
    bmp.push_back('B');
    bmp.push_back('M');
    append_u32_le(bmp, 58u); /* file size: 14 + 40 + 4 */
    append_u32_le(bmp, 0u);
    append_u32_le(bmp, 54u); /* pixel offset */
    append_u32_le(bmp, 40u); /* BITMAPINFOHEADER size */
    append_u32_le(bmp, 1u);  /* width */
    append_u32_le(bmp, 1u);  /* height (bottom-up) */
    bmp.push_back(1);        /* planes lo */
    bmp.push_back(0);        /* planes hi */
    bmp.push_back(32);       /* bpp lo */
    bmp.push_back(0);        /* bpp hi */
    append_u32_le(bmp, 0u);  /* compression BI_RGB */
    append_u32_le(bmp, 4u);  /* image size */
    append_u32_le(bmp, 0u);  /* x ppm */
    append_u32_le(bmp, 0u);  /* y ppm */
    append_u32_le(bmp, 0u);  /* colors */
    append_u32_le(bmp, 0u);  /* important colors */
    bmp.push_back(b);
    bmp.push_back(g);
    bmp.push_back(r);
    bmp.push_back(a);
    return bmp;
}

static void test_gltf_converts_spec_glossiness_texture_per_texel() {
    const char *gltf_path = "/tmp/zanna_gltf_spec_gloss_texture.gltf";
    /* 1x1 spec-gloss texel: sRGB specular 128 (linear ~0.2159), gloss alpha 128. */
    std::vector<uint8_t> bmp = build_test_bmp_1x1_bgra(128, 128, 128, 128);
    std::string bmp_b64 = base64_encode(bmp.data(), bmp.size());
    std::string gltf_json = "{"
                            "\"asset\":{\"version\":\"2.0\"},"
                            "\"extensionsUsed\":[\"KHR_materials_pbrSpecularGlossiness\"],"
                            "\"images\":[{\"uri\":\"data:image/bmp;base64," +
                            bmp_b64 +
                            "\"}],"
                            "\"textures\":[{\"source\":0}],"
                            "\"materials\":["
                            "{\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{"
                            "\"diffuseFactor\":[1.0,1.0,1.0,1.0],"
                            "\"specularFactor\":[1.0,1.0,1.0],"
                            "\"glossinessFactor\":0.8,"
                            "\"specularGlossinessTexture\":{\"index\":0}}}}"
                            "]"
                            "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Spec-gloss texture glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts spec-gloss materials with textures");
    if (!asset)
        return;
    auto *material = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(material != nullptr, "Spec-gloss texture material is exposed");
    if (!material)
        return;
    EXPECT_TRUE(material->metallic_roughness_map != nullptr,
                "Spec-gloss texture synthesizes a per-texel metallic-roughness map");
    if (!material->metallic_roughness_map)
        return;
    int64_t texel = rt_pixels_get_rgba(material->metallic_roughness_map, 0, 0);
    double roughness_texel = (double)((texel >> 16) & 0xFF);
    double metallic_texel = (double)((texel >> 8) & 0xFF);
    /* roughness = 1 - glossFactor * glossTexel = 1 - 0.8 * (128/255) ~= 0.5984 -> 153 */
    EXPECT_NEAR(roughness_texel, 153.0, 1.5, "MR map bakes gloss factor and texel into roughness");
    /* metallic from linearized sRGB 128 (~0.2159): (0.2159 - 0.04)/0.96 ~= 0.1832 -> 47 */
    EXPECT_NEAR(metallic_texel, 47.0, 1.5, "MR map bakes per-texel specular into metallic");
    EXPECT_NEAR(material->roughness,
                1.0,
                0.001,
                "Scalar roughness resets to 1 when factors bake into the MR map");
    EXPECT_NEAR(material->metallic,
                1.0,
                0.001,
                "Scalar metallic resets to 1 when factors bake into the MR map");
    EXPECT_TRUE(material->specular_map != nullptr,
                "Spec-gloss texture still binds to the specular slot");
}

static void test_gltf_forced_tangents_load_option() {
    const char *gltf_path = "/tmp/zanna_gltf_forced_tangents.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    const float uvs[6] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (float v : uvs)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]}}],"
        "\"meshes\":[{\"primitives\":[{"
        "\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"indices\":2,\"material\":0}]}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Forced-tangents glTF fixture can be created");

    /* Default load: no normal map bound, so no tangents (current behavior pinned). */
    void *asset = rt_model3d_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "SceneAsset.Load parses the tangent fixture");
    if (asset) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(asset, 0));
        EXPECT_TRUE(mesh != nullptr && std::fabs(mesh->vertices[0].tangent[0]) < 0.5f,
                    "Default load leaves UV0 meshes without tangents when no normal map");
    }

    /* Forced load: tangents generated regardless of the material's maps. */
    void *forced = rt_model3d_load_with_options(rt_const_cstr(gltf_path), 1);
    EXPECT_TRUE(forced != nullptr, "SceneAsset.LoadWithOptions parses the tangent fixture");
    if (forced) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(forced, 0));
        EXPECT_TRUE(mesh != nullptr && std::fabs(mesh->vertices[0].tangent[0]) > 0.9f &&
                        std::fabs(mesh->vertices[0].tangent[3]) > 0.9f,
                    "LoadWithOptions(forceTangents) generates tangents without a normal map");
    }
}

static void test_gltf_imports_ktx2_basisu_textures() {
    const char *ktx_path = "/tmp/zanna_gltf_basisu_albedo.ktx2";
    const char *gltf_path = "/tmp/zanna_gltf_basisu_texture.gltf";
    const uint8_t rgba[16] = {0x10,
                              0x20,
                              0x30,
                              0xFF,
                              0x40,
                              0x50,
                              0x60,
                              0xFF,
                              0x70,
                              0x80,
                              0x90,
                              0xFF,
                              0xA0,
                              0xB0,
                              0xC0,
                              0xFF};
    EXPECT_TRUE(write_test_ktx2_rgba8(ktx_path, 2u, 2u, rgba, sizeof(rgba)),
                "KTX2 fixture can be written");
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"KHR_texture_basisu\"],"
        "\"images\":[{\"uri\":\"zanna_gltf_basisu_albedo.ktx2\",\"mimeType\":\"image/ktx2\"}],"
        "\"textures\":[{\"extensions\":{\"KHR_texture_basisu\":{\"source\":0}}}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "KTX2 glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts optional KHR_texture_basisu textures");
    if (!asset)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr && mat->texture != nullptr,
                "GLTF.Load binds KTX2 image as a material texture");
    if (mat && mat->texture) {
        EXPECT_TRUE(rt_textureasset3d_get_width(mat->texture) == 2,
                    "GLTF.Load stores KTX2 material textures as TextureAsset3D");
        EXPECT_TRUE(rt_material3d_get_has_texture(mat) == 1,
                    "Material3D sees imported KTX2 TextureAsset3D as drawable");
    }
}

static void test_gltf_preserves_negative_matrix_scale_sign() {
    const char *gltf_path = "/tmp/zanna_gltf_negative_scale.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"nodes\":[{\"name\":\"Mirror\",\"matrix\":[-2,0,0,0,0,3,0,0,0,0,4,0,0,0,0,1]}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Negative-scale glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses negative-scale matrix nodes");
    if (!asset)
        return;
    void *node = rt_scene_node3d_find(rt_gltf_get_scene_root(asset), rt_const_cstr("Mirror"));
    EXPECT_TRUE(node != nullptr, "GLTF.Load exposes negative-scale node");
    if (!node)
        return;
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_scale(node)),
                -2.0,
                0.001,
                "GLTF.Load preserves negative X scale from matrix transforms");
}

static void test_gltf_matrix_shear_does_not_leak_into_rotation() {
    const char *gltf_path = "/tmp/zanna_gltf_sheared_matrix.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"nodes\":[{\"name\":\"Sheared\",\"matrix\":[1,0,0,0,0.5,1,0,0,0,0,1,0,0,0,0,1]}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Sheared glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses sheared matrix nodes");
    if (!asset)
        return;
    void *node = rt_scene_node3d_find(rt_gltf_get_scene_root(asset), rt_const_cstr("Sheared"));
    EXPECT_TRUE(node != nullptr, "GLTF.Load exposes sheared matrix node");
    if (!node)
        return;
    void *rot = rt_scene_node3d_get_rotation(node);
    EXPECT_NEAR(rt_quat_x(rot), 0.0, 0.001, "GLTF.Load drops unsupported matrix shear rotation X");
    EXPECT_NEAR(rt_quat_y(rot), 0.0, 0.001, "GLTF.Load drops unsupported matrix shear rotation Y");
    EXPECT_NEAR(rt_quat_z(rot), 0.0, 0.001, "GLTF.Load drops unsupported matrix shear rotation Z");
    EXPECT_NEAR(rt_quat_w(rot), 1.0, 0.001, "GLTF.Load keeps sheared matrix rotation normalized");
}

static void test_gltf_rejects_skins_over_runtime_bone_limit() {
    /* Skeletons accept up to VGFX3D_MAX_SKELETON_BONES (1024); rejection begins
     * past that. Draw palettes stay at 256 via mesh partitioning. */
    const char *gltf_path = "/tmp/zanna_gltf_rejected_skin.gltf";
    std::string nodes;
    std::string joints;
    for (int i = 0; i < 1025; i++) {
        if (i > 0) {
            nodes += ",";
            joints += ",";
        }
        nodes += "{\"name\":\"J" + std::to_string(i) + "\"}";
        joints += std::to_string(i);
    }
    std::string gltf_json = "{\"asset\":{\"version\":\"2.0\"},\"skins\":[{\"joints\":[" + joints +
                            "]}],\"nodes\":[" + nodes +
                            "],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Oversized-skin glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset == nullptr, "GLTF.Load rejects skins above the 1024-bone skeleton limit");
}

static void test_gltf_rejects_unsupported_required_extensions() {
    /* KHR_draco_mesh_compression graduated to the supported gate; EXT_texture_webp
     * remains genuinely unsupported. */
    const char *gltf_path = "/tmp/zanna_gltf_required_extension.gltf";
    std::string gltf_json = "{\"asset\":{\"version\":\"2.0\"},"
                            "\"extensionsRequired\":[\"EXT_texture_webp\"],"
                            "\"extensionsUsed\":[\"EXT_texture_webp\"]}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Required-extension glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(
        asset == nullptr,
        "GLTF.Load rejects unsupported required extensions instead of rendering fallback data");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_UNSUPPORTED,
                "GLTF.Load records unsupported required extensions as Unsupported");
    EXPECT_TRUE(std::strstr(rt_asset_error_get_message(),
                            "requires EXT_texture_webp (unsupported)") != nullptr,
                "GLTF.Load names unsupported required extensions in LastLoadError");
    std::remove(gltf_path);
}

static void test_gltf_warns_unsupported_optional_extensions() {
    const char *gltf_path = "/tmp/zanna_gltf_optional_extension_warning.gltf";
    std::string gltf_json = "{\"asset\":{\"version\":\"2.0\"},"
                            "\"extensionsUsed\":[\"EXT_missing_material_model\"]}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Optional-extension glTF fixture can be created");

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts unsupported optional extensions");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_NONE,
                "Optional unsupported glTF extensions do not set a load error");
    EXPECT_TRUE(rt_asset_error_get_warning_count() == 1,
                "Unsupported optional glTF extensions record one load warning");
    EXPECT_TRUE(std::strstr(rt_asset_error_get_warning(0), "EXT_missing_material_model") != nullptr,
                "Unsupported optional glTF warning names the extension");
    EXPECT_TRUE(std::strstr(rt_asset_error_get_warning(0), "visual") != nullptr,
                "Unsupported optional glTF warning describes the visual consequence");
    EXPECT_TRUE(import_report_contains("\"ignoredExtensions\":1"),
                "Import report counts ignored optional extensions");
    EXPECT_TRUE(import_report_contains("EXT_missing_material_model"),
                "Import report carries the load warnings verbatim");
    std::remove(gltf_path);
}

static void test_gltf_rejects_required_extensions_with_partial_runtime_support() {
    const char *gltf_path = "/tmp/zanna_gltf_partial_required_extensions.gltf";
    /* KHR_texture_basisu moved to the fully-supported list once the runtime gained
     * ETC1S/BasisLZ and UASTC KTX2 decoding; clearcoat/transmission remain
     * factor-level approximations. */
    const char *extensions[] = {
        "KHR_materials_clearcoat",
        "KHR_materials_transmission",
    };
    for (const char *extension : extensions) {
        std::string gltf_json = std::string("{\"asset\":{\"version\":\"2.0\"},") +
                                "\"extensionsRequired\":[\"" + extension + "\"]," +
                                "\"extensionsUsed\":[\"" + extension + "\"]}";
        EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                    "Partially-supported required-extension glTF fixture can be created");
        void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
        EXPECT_TRUE(asset == nullptr,
                    "GLTF.Load rejects required extensions that only have degraded support");
    }
}

static void test_gltf_rejects_non_string_required_extensions() {
    const char *gltf_path = "/tmp/zanna_gltf_required_extension_non_string.gltf";
    std::string gltf_json = "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[17]}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Non-string required-extension glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset == nullptr,
                "GLTF.Load rejects non-string required extension names without trapping");
}

static void test_gltf_accepts_supported_required_extensions_with_parser_coverage() {
    const char *gltf_path = "/tmp/zanna_gltf_supported_required_extensions.gltf";
    std::string gltf_json = "{\n"
                            "  \"asset\": {\"version\": \"2.0\"},\n"
                            "  \"extensionsUsed\": [\"KHR_texture_transform\", "
                            "\"KHR_materials_emissive_strength\", \"KHR_materials_unlit\", "
                            "\"KHR_lights_punctual\"],\n"
                            "  \"extensionsRequired\": [\"KHR_texture_transform\", "
                            "\"KHR_materials_emissive_strength\", \"KHR_materials_unlit\", "
                            "\"KHR_lights_punctual\"],\n"
                            "  \"textures\": [{}],\n"
                            "  \"materials\": [{\n"
                            "    \"pbrMetallicRoughness\": {\"baseColorTexture\": {\"index\": 0, "
                            "\"extensions\": {\"KHR_texture_transform\": {\"offset\": [0.3, "
                            "0.4]}}}},\n"
                            "    \"emissiveFactor\": [0.1, 0.2, 0.3],\n"
                            "    \"extensions\": {\"KHR_materials_emissive_strength\": "
                            "{\"emissiveStrength\": 2.5}, \"KHR_materials_unlit\": {}}\n"
                            "  }],\n"
                            "  \"extensions\": {\"KHR_lights_punctual\": {\"lights\": "
                            "[{\"type\": \"point\", \"range\": 4.0}]}},\n"
                            "  \"nodes\": [{\"extensions\": {\"KHR_lights_punctual\": "
                            "{\"light\": 0}}}],\n"
                            "  \"scenes\": [{\"nodes\": [0]}],\n"
                            "  \"scene\": 0\n"
                            "}\n";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Supported-required-extension glTF fixture can be created");

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr,
                "GLTF.Load accepts every extension listed as supported for extensionsRequired");
    if (!asset)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr, "Supported-required-extension fixture imports a material");
    if (!mat)
        return;
    EXPECT_TRUE(mat->unlit == 1, "Required KHR_materials_unlit is interpreted");
    EXPECT_NEAR(mat->emissive_intensity,
                2.5,
                0.001,
                "Required KHR_materials_emissive_strength is interpreted");
    EXPECT_NEAR(mat->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR][4],
                0.3,
                0.001,
                "Required KHR_texture_transform offset.x is interpreted");
}

static void test_gltf_imports_required_punctual_lights() {
    const char *gltf_path = "/tmp/zanna_gltf_punctual_light.gltf";
    std::string gltf_json = "{\n"
                            "  \"asset\": {\"version\": \"2.0\"},\n"
                            "  \"extensionsUsed\": [\"KHR_lights_punctual\"],\n"
                            "  \"extensionsRequired\": [\"KHR_lights_punctual\"],\n"
                            "  \"extensions\": {\"KHR_lights_punctual\": {\"lights\": [{\n"
                            "    \"type\": \"spot\",\n"
                            "    \"color\": [0.2, 0.4, 0.6],\n"
                            "    \"intensity\": 7.5,\n"
                            "    \"range\": 5.0,\n"
                            "    \"spot\": {\"innerConeAngle\": 0.1, \"outerConeAngle\": 0.5}\n"
                            "  }, {\"type\": \"point\"}]}},\n"
                            "  \"nodes\": [{\"name\": \"lamp\", \"translation\": [1.0, 2.0, 3.0], "
                            "\"extensions\": {\"KHR_lights_punctual\": {\"light\": 0}}},\n"
                            "  {\"name\": \"point\", \"extensions\": {\"KHR_lights_punctual\": "
                            "{\"light\": 1}}}],\n"
                            "  \"scenes\": [{\"nodes\": [0, 1]}],\n"
                            "  \"scene\": 0\n"
                            "}\n";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Punctual-light glTF fixture can be created");

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts required KHR_lights_punctual");
    if (!asset)
        return;
    void *root = rt_gltf_get_scene_root(asset);
    EXPECT_TRUE(root != nullptr && rt_scene_node3d_child_count(root) == 2,
                "GLTF.Load builds scene nodes for punctual-light fixtures");
    if (!root || rt_scene_node3d_child_count(root) != 2)
        return;
    void *node = rt_scene_node3d_get_child(root, 0);
    rt_light3d *light = (rt_light3d *)rt_scene_node3d_get_light(node);
    EXPECT_TRUE(light != nullptr, "GLTF.Load attaches punctual lights to their nodes");
    if (!light)
        return;
    EXPECT_TRUE(light->type == 3, "GLTF.Load preserves spot-light type");
    EXPECT_NEAR(light->color[1], 0.4, 0.001, "GLTF.Load preserves light color");
    EXPECT_NEAR(light->intensity, 7.5, 0.001, "GLTF.Load preserves light intensity");
    EXPECT_NEAR(light->attenuation, 0.04, 0.001, "GLTF.Load maps range to attenuation");
    EXPECT_TRUE(light->casts_shadows == 0, "GLTF.Load imports spot lights with shadows disabled");
    EXPECT_TRUE(light->inner_cos > light->outer_cos,
                "GLTF.Load stores valid spot inner/outer cone cosines");
    void *point_node = rt_scene_node3d_get_child(root, 1);
    rt_light3d *point = (rt_light3d *)rt_scene_node3d_get_light(point_node);
    EXPECT_TRUE(point != nullptr, "GLTF.Load attaches point lights to their nodes");
    if (!point)
        return;
    EXPECT_TRUE(point->type == 1, "GLTF.Load preserves point-light type");
    EXPECT_NEAR(point->attenuation, 0.001, 0.0001, "GLTF.Load floors omitted point-light range");
    EXPECT_TRUE(point->casts_shadows == 0, "GLTF.Load imports point lights with shadows disabled");
}

static void test_gltf_preserves_primary_texture_sampler_state() {
    const char *gltf_path = "/tmp/zanna_gltf_sampler_state.gltf";
    std::string gltf_json =
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"samplers\": [{\"magFilter\": 9728, \"minFilter\": 9728, \"wrapS\": 33071, "
        "\"wrapT\": 33648}],\n"
        "  \"textures\": [{\"sampler\": 0, \"source\": 0}],\n"
        "  \"materials\": [{\"pbrMetallicRoughness\": {\"baseColorTexture\": {\"index\": 0}}}]\n"
        "}\n";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json), "Sampler-state glTF fixture can be created");

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts material sampler fixtures");
    if (!asset)
        return;

    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr, "Sampler-state fixture imports a material");
    if (!mat)
        return;
    EXPECT_TRUE(mat->texture_wrap_s == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE,
                "GLTF.Load preserves sampler wrapS=CLAMP_TO_EDGE");
    EXPECT_TRUE(mat->texture_wrap_t == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT,
                "GLTF.Load preserves sampler wrapT=MIRRORED_REPEAT");
    EXPECT_TRUE(mat->texture_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST,
                "GLTF.Load preserves nearest sampler filtering");
    EXPECT_TRUE(mat->texture_min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST &&
                    mat->texture_mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST &&
                    mat->texture_mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE,
                "GLTF.Load preserves non-mipmapped nearest min/mag axes independently");
    EXPECT_TRUE(mat->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE,
                "GLTF.Load mirrors base slot sampler wrapS");
    EXPECT_TRUE(mat->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT,
                "GLTF.Load mirrors base slot sampler wrapT");
    EXPECT_TRUE(mat->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_FILTER_NEAREST,
                "GLTF.Load mirrors base slot sampler filtering");
}

/// @brief Verify every glTF minification enum and both legal magnification enums independently.
/// @details The fixture creates one material per 6-by-2 sampler tuple plus one sampler with
///          omitted filters. It proves that the importer retains texel minification,
///          magnification, and mip selection as three distinct axes rather than collapsing the
///          glTF `minFilter` enum into one lossy nearest/linear value.
static void test_gltf_maps_every_sampler_filter_enum_to_independent_axes() {
    struct MinCase {
        int gltf_value;
        int32_t expected_min;
        int32_t expected_mip;
    };

    static const MinCase min_cases[] = {
        {9728, RT_MATERIAL3D_TEXTURE_FILTER_NEAREST, RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE},
        {9729, RT_MATERIAL3D_TEXTURE_FILTER_LINEAR, RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE},
        {9984, RT_MATERIAL3D_TEXTURE_FILTER_NEAREST, RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST},
        {9985, RT_MATERIAL3D_TEXTURE_FILTER_LINEAR, RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST},
        {9986, RT_MATERIAL3D_TEXTURE_FILTER_NEAREST, RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR},
        {9987, RT_MATERIAL3D_TEXTURE_FILTER_LINEAR, RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR},
    };
    static const int mag_values[] = {9728, 9729};
    const char *path = "/tmp/zanna_gltf_sampler_axis_table.gltf";
    std::string samplers;
    std::string textures;
    std::string materials;
    size_t case_count = 0;
    for (const MinCase &min_case : min_cases) {
        for (int mag_value : mag_values) {
            if (!samplers.empty()) {
                samplers += ',';
                textures += ',';
                materials += ',';
            }
            samplers += "{\"minFilter\":" + std::to_string(min_case.gltf_value) +
                        ",\"magFilter\":" + std::to_string(mag_value) + "}";
            textures += "{\"sampler\":" + std::to_string(case_count) + "}";
            materials += "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" +
                         std::to_string(case_count) + "}}}";
            case_count++;
        }
    }
    samplers += ",{}";
    textures += ",{\"sampler\":" + std::to_string(case_count) + "}";
    materials += ",{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" +
                 std::to_string(case_count) + "}}}";
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"samplers\":[" + samplers +
                       "],\"textures\":[" + textures + "],\"materials\":[" + materials + "]}";
    EXPECT_TRUE(write_text_file(path, json), "Sampler-axis table fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load accepts every standard sampler filter enum");
    if (!asset)
        return;

    size_t material_index = 0;
    for (const MinCase &min_case : min_cases) {
        for (int mag_value : mag_values) {
            auto *mat = static_cast<rt_material3d *>(
                rt_gltf_get_material(asset, static_cast<int64_t>(material_index)));
            std::string prefix = "sampler tuple " + std::to_string(material_index);
            EXPECT_TRUE(mat != nullptr, (prefix + " imports its material").c_str());
            if (mat) {
                EXPECT_TRUE(mat->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                                min_case.expected_min,
                            (prefix + " preserves minification").c_str());
                EXPECT_TRUE(mat->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                                (mag_value == 9728 ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                                                   : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR),
                            (prefix + " preserves magnification").c_str());
                EXPECT_TRUE(mat->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                                min_case.expected_mip,
                            (prefix + " preserves mip selection").c_str());
            }
            material_index++;
        }
    }
    auto *defaults = static_cast<rt_material3d *>(
        rt_gltf_get_material(asset, static_cast<int64_t>(material_index)));
    EXPECT_TRUE(defaults != nullptr, "Sampler with omitted filters imports its material");
    if (defaults) {
        EXPECT_TRUE(defaults->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                            RT_MATERIAL3D_TEXTURE_FILTER_LINEAR &&
                        defaults->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                            RT_MATERIAL3D_TEXTURE_FILTER_LINEAR &&
                        defaults->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                            RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR,
                    "Omitted glTF filters retain the specified linear trilinear default");
    }
    std::remove(path);
}

static void test_gltf_preserves_independent_texture_slot_metadata() {
    const char *gltf_path = "/tmp/zanna_gltf_texture_slot_metadata.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33071,\"minFilter\":9728,\"magFilter\":9728},"
        "{\"wrapS\":33648,\"wrapT\":10497,\"minFilter\":9729,\"magFilter\":9729}],"
        "\"textures\":[{\"sampler\":0},{\"sampler\":1}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"texCoord\":0},\"metallicRoughnessTexture\":{\"index\":1,\"texCoord\":1,"
        "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.25,0.5],"
        "\"scale\":[4,5]}}}},\"normalTexture\":{\"index\":1,\"texCoord\":1}}]"
        "}";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Independent texture-slot glTF fixture can be created");
    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset != nullptr, "GLTF.Load parses independent texture-slot metadata");
    if (!asset)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_gltf_get_material(asset, 0));
    EXPECT_TRUE(mat != nullptr, "Independent slot fixture imports a material");
    if (!mat)
        return;
    EXPECT_TRUE(mat->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] ==
                    RT_MATERIAL3D_TEXTURE_FILTER_NEAREST,
                "GLTF.Load keeps base-color nearest sampling independent");
    EXPECT_TRUE(mat->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] ==
                    RT_MATERIAL3D_TEXTURE_FILTER_LINEAR,
                "GLTF.Load keeps metallic-roughness linear sampling independent");
    EXPECT_TRUE(mat->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS] == 1,
                "GLTF.Load keeps metallic-roughness texCoord independent");
    EXPECT_NEAR(mat->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS][0],
                4.0,
                0.001,
                "GLTF.Load keeps metallic-roughness transform scale.x independent");
    EXPECT_NEAR(mat->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS][5],
                0.5,
                0.001,
                "GLTF.Load keeps metallic-roughness transform offset.y independent");
    EXPECT_TRUE(mat->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL] == 1,
                "GLTF.Load keeps normal-map texCoord independent");
}

static void test_gltf_rejects_invalid_scene_graph_links() {
    const char *gltf_path = "/tmp/zanna_gltf_invalid_scene_graph.gltf";
    std::string gltf_json = "{\n"
                            "  \"asset\": {\"version\": \"2.0\"},\n"
                            "  \"nodes\": [\n"
                            "    {\"name\": \"A\", \"children\": [1]},\n"
                            "    {\"name\": \"B\", \"children\": [0]}\n"
                            "  ],\n"
                            "  \"scenes\": [{\"nodes\": [0]}],\n"
                            "  \"scene\": 0\n"
                            "}\n";
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Invalid scene-graph glTF fixture can be created");

    void *asset = rt_gltf_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(asset == nullptr,
                "GLTF.Load rejects cyclic node graphs instead of returning an empty scene");
}

/// @brief Patch one little-endian 32-bit field already present in a byte vector.
/// @param bytes Destination byte vector.
/// @param offset Byte offset of the four-byte field.
/// @param value Value to encode.
static void patch_u32_le(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4u)
        return;
    bytes[offset + 0u] = static_cast<uint8_t>(value & 0xFFu);
    bytes[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    bytes[offset + 2u] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
    bytes[offset + 3u] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}

/// @brief Begin a GLB 2.0 fixture whose declared length is patched after chunks are appended.
/// @return Twelve-byte GLB header with the standard magic/version and a provisional length.
static std::vector<uint8_t> begin_glb_fixture() {
    std::vector<uint8_t> bytes{'g', 'l', 'T', 'F'};
    append_u32_le(bytes, 2u);
    append_u32_le(bytes, 12u);
    return bytes;
}

/// @brief Append one already-aligned GLB chunk to a test fixture.
/// @param bytes GLB fixture under construction.
/// @param type Raw GLB chunk type.
/// @param payload Exact chunk payload; callers deliberately control malformed alignment.
static void append_glb_test_chunk(std::vector<uint8_t> &bytes,
                                  uint32_t type,
                                  const std::vector<uint8_t> &payload) {
    append_u32_le(bytes, static_cast<uint32_t>(payload.size()));
    append_u32_le(bytes, type);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

/// @brief Set a fixture's GLB declared length to its current physical byte length.
/// @param bytes GLB fixture containing at least the standard 12-byte header.
static void finalize_glb_fixture(std::vector<uint8_t> &bytes) {
    patch_u32_le(bytes, 8u, static_cast<uint32_t>(bytes.size()));
}

/// @brief Require synchronous and preload GLB paths to reject the same malformed root bytes.
/// @details The preload call takes ownership of a malloc copy, so this also exercises its complete
/// failure cleanup. Both paths must record `RT_ASSET_ERROR_CORRUPT` and publish no bundle/asset.
/// @param bytes Malformed GLB bytes.
/// @param case_index Stable index used to produce a unique temporary filename.
static void expect_gltf_root_rejected_by_both_paths(const std::vector<uint8_t> &bytes,
                                                    int case_index) {
    std::string path = "/tmp/zanna_gltf_malformed_shared_" + std::to_string(case_index) + ".glb";
    uint8_t *owned = static_cast<uint8_t *>(std::malloc(bytes.size()));
    EXPECT_TRUE(owned != nullptr, "Malformed GLB preload copy can be allocated");
    if (!owned)
        return;
    std::memcpy(owned, bytes.data(), bytes.size());
    char error[160] = {0};
    rt_asset_error_clear();
    rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create_cstr(
        path.c_str(), owned, bytes.size(), 0, error, sizeof(error));
    EXPECT_TRUE(bundle == nullptr, "Malformed GLB preload publishes no partial bundle");
    EXPECT_TRUE(error[0] != '\0', "Malformed GLB preload records a stable staging error");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "Malformed GLB preload records invalid-data status");
    if (bundle)
        rt_gltf_preload_bundle_free(bundle);

    EXPECT_TRUE(write_binary_file(path.c_str(), bytes),
                "Malformed GLB sync fixture can be written");
    rt_asset_error_clear();
    void *asset = rt_gltf_load(rt_const_cstr(path.c_str()));
    EXPECT_TRUE(asset == nullptr, "Malformed GLB synchronous load publishes no partial asset");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "Malformed GLB synchronous load records the same invalid-data status");
    std::remove(path.c_str());
}

/// @brief Exercise the shared bounded GLB iterator against every rejected layout class.
/// @details Covers a missing/late JSON chunk, duplicate JSON/BIN chunks, an unknown chunk,
/// truncated payload bounds, and malformed trailing bytes through both public load paths.
static void test_gltf_shared_glb_chunk_validation_is_atomic() {
    std::string json_text = "{\"asset\":{\"version\":\"2.0\"}}";
    while ((json_text.size() & 3u) != 0u)
        json_text.push_back(' ');
    std::vector<uint8_t> json(json_text.begin(), json_text.end());
    std::vector<uint8_t> empty;
    std::vector<std::vector<uint8_t>> malformed;

    malformed.push_back(begin_glb_fixture());
    append_glb_test_chunk(malformed.back(), UINT32_C(0x004E4942), empty);
    finalize_glb_fixture(malformed.back());

    malformed.push_back(begin_glb_fixture());
    append_glb_test_chunk(malformed.back(), UINT32_C(0x4E4F534A), json);
    append_glb_test_chunk(malformed.back(), UINT32_C(0x4E4F534A), json);
    finalize_glb_fixture(malformed.back());

    malformed.push_back(begin_glb_fixture());
    append_glb_test_chunk(malformed.back(), UINT32_C(0x4E4F534A), json);
    append_glb_test_chunk(malformed.back(), UINT32_C(0x12345678), empty);
    finalize_glb_fixture(malformed.back());

    malformed.push_back(begin_glb_fixture());
    append_glb_test_chunk(malformed.back(), UINT32_C(0x4E4F534A), json);
    append_glb_test_chunk(malformed.back(), UINT32_C(0x004E4942), empty);
    append_glb_test_chunk(malformed.back(), UINT32_C(0x004E4942), empty);
    finalize_glb_fixture(malformed.back());

    malformed.push_back(begin_glb_fixture());
    append_u32_le(malformed.back(), static_cast<uint32_t>(json.size() + 4u));
    append_u32_le(malformed.back(), UINT32_C(0x4E4F534A));
    malformed.back().insert(malformed.back().end(), json.begin(), json.end());
    finalize_glb_fixture(malformed.back());

    malformed.push_back(begin_glb_fixture());
    append_glb_test_chunk(malformed.back(), UINT32_C(0x4E4F534A), json);
    malformed.back().push_back(0xA5u);
    finalize_glb_fixture(malformed.back());

    for (size_t i = 0; i < malformed.size(); ++i)
        expect_gltf_root_rejected_by_both_paths(malformed[i], static_cast<int>(i));
}

/// @brief Verify exact-token parsing for every integral and boolean-like JSON helper.
/// @details Fractional, exponent, sign-only, suffix, leading-zero, and overflow spellings all use
/// the caller's fallback instead of being accepted as an integer prefix.
static void test_gltf_json_integral_tokens_are_exact() {
    struct IntegerCase {
        const char *token;
        int expected;
    };

    const IntegerCase cases[] = {{"0", 0},
                                 {"-1", -1},
                                 {"2147483647", INT_MAX},
                                 {"-2147483648", INT_MIN},
                                 {"1.0", 73},
                                 {"1e2", 73},
                                 {"-", 73},
                                 {"1x", 73},
                                 {"01", 73},
                                 {"2147483648", 73},
                                 {"-2147483649", 73}};
    for (const IntegerCase &test_case : cases) {
        std::string json = std::string("{\"v\":") + test_case.token + "}";
        EXPECT_TRUE(gltf_json_object_get_int(json.data(), json.size(), 0, json.size(), "v", 73) ==
                        test_case.expected,
                    "glTF integer fields consume exactly one bounded JSON token");
    }

    const char *size_invalid[] = {"-1", "1.0", "1e2", "+1", "1x", "01", "184467440737095516160"};
    for (const char *token : size_invalid) {
        std::string json = std::string("{\"v\":") + token + "}";
        EXPECT_TRUE(gltf_json_object_get_size(
                        json.data(), json.size(), 0, json.size(), "v", SIZE_MAX) == SIZE_MAX,
                    "glTF size fields reject non-integral or overflowing tokens");
    }
    const std::string fractional_bool = "{\"v\":1.5}";
    EXPECT_TRUE(
        gltf_json_object_get_boolish(
            fractional_bool.data(), fractional_bool.size(), 0, fractional_bool.size(), "v", 0) == 0,
        "Boolean-like fields do not interpret an integer prefix from a fractional token");

    const char *invalid_documents[] = {
        "{\"scene\":1e0}",
        "{\"accessors\":[{\"count\":1.0}]}",
        "{\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0e0}}]}]}",
        "{\"scenes\":[{\"nodes\":[0e0]}]}",
        "{\"accessors\":[{\"normalized\":1e0}]}",
        "{\"extensions\":{\"KHR_texture_transform\":{\"texCoord\":1e0}}}"};
    for (const char *json : invalid_documents) {
        EXPECT_TRUE(!gltf_json_validate_gltf_integral_tokens(json, std::strlen(json)),
                    "glTF schema integral positions reject decimal/exponent lexical forms");
    }
    const char *opaque_documents[] = {"{\"extras\":{\"count\":1e2}}",
                                      "{\"extensions\":{\"VENDOR_optional\":{\"count\":1e2}}}"};
    for (const char *json : opaque_documents) {
        EXPECT_TRUE(gltf_json_validate_gltf_integral_tokens(json, std::strlen(json)),
                    "Application extras and unknown optional extensions remain opaque");
    }

    const char *path = "/tmp/zanna_gltf_exponent_integral_field.gltf";
    const std::string exponent_scene =
        "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[],\"scene\":0e0}";
    EXPECT_TRUE(write_text_file(path, exponent_scene),
                "Exponent-form integral root fixture can be written");
    rt_asset_error_clear();
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) == nullptr,
                "Synchronous GLTF.Load rejects exponent-form integral fields before the DOM");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "Synchronous exponent rejection records corrupt input");
    uint8_t *owned = static_cast<uint8_t *>(std::malloc(exponent_scene.size()));
    EXPECT_TRUE(owned != nullptr, "Exponent-form preload root copy can be allocated");
    if (owned) {
        std::memcpy(owned, exponent_scene.data(), exponent_scene.size());
        char error[128] = {0};
        rt_asset_error_clear();
        rt_gltf_preload_bundle *bundle = rt_gltf_preload_bundle_create_cstr(
            path, owned, exponent_scene.size(), 0, error, sizeof(error));
        EXPECT_TRUE(bundle == nullptr,
                    "Preload rejects exponent-form integral fields before staging dependencies");
        EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                    "Preload exponent rejection records corrupt input");
        if (bundle)
            rt_gltf_preload_bundle_free(bundle);
    }
    std::remove(path);
}

/// @brief Verify malformed array separators invalidate the full array before any prefix returns.
/// @details Missing/duplicate commas, trailing commas, trailing junk, and malformed nested values
/// all make item zero unavailable; a valid nested array still returns its requested object span.
static void test_gltf_json_array_iteration_rejects_malformed_suffixes() {
    const char *invalid[] = {"[1 2]", "[1,,2]", "[1,]", "[1,2 junk]", "[1,{\"x\":1,}]"};
    for (const char *json : invalid) {
        size_t start = SIZE_MAX;
        size_t end = SIZE_MAX;
        size_t len = std::strlen(json);
        EXPECT_TRUE(!gltf_json_array_item_range(json, len, 0, len, 0, &start, &end),
                    "Malformed array suffix invalidates every prefix element");
        EXPECT_TRUE(!gltf_json_validate_document(json, len),
                    "Malformed array separators invalidate the complete JSON document");
    }
    const char *valid = "[1,{\"x\":[2,3]},4]";
    size_t start = SIZE_MAX;
    size_t end = SIZE_MAX;
    EXPECT_TRUE(gltf_json_array_item_range(
                    valid, std::strlen(valid), 0, std::strlen(valid), 1, &start, &end),
                "Strict array iteration preserves valid nested values");
    EXPECT_TRUE(start < end && valid[start] == '{' && valid[end - 1u] == '}',
                "Strict array iteration returns the exact requested nested span");
}

/// @brief Reject whitespace bytes that C locales classify as space but JSON does not.
/// @details JSON permits only space, tab, CR, and LF. Form-feed and vertical-tab must not be
/// accepted after primitive tokens or around schema-integral values.
static void test_gltf_json_uses_exact_json_whitespace() {
    const std::string invalid_documents[] = {std::string("1\v", 2),
                                             std::string("1\f", 2),
                                             std::string("{\"v\":1\v}", 8),
                                             std::string("{\"v\":1\f}", 8)};
    for (const std::string &json : invalid_documents)
        EXPECT_TRUE(!gltf_json_validate_document(json.data(), json.size()),
                    "Only the four JSON whitespace bytes may trail a primitive");

    const std::string bad_int = std::string("{\"v\":\v1}", 8);
    const std::string bad_size = std::string("{\"v\":1\f}", 8);
    EXPECT_TRUE(
        gltf_json_object_get_int(bad_int.data(), bad_int.size(), 0, bad_int.size(), "v", 73) == 73,
        "Signed integer fields reject non-JSON leading whitespace");
    EXPECT_TRUE(gltf_json_object_get_size(
                    bad_size.data(), bad_size.size(), 0, bad_size.size(), "v", 91) == 91,
                "Size fields reject non-JSON trailing whitespace");
    EXPECT_TRUE(gltf_json_skip_ws(nullptr, 4, 0) == 4,
                "Whitespace scanning is null-safe for a nonempty claimed range");
    EXPECT_TRUE(gltf_json_skip_ws("x", 1, SIZE_MAX) == 1,
                "Whitespace scanning clamps an out-of-range cursor to the buffer end");
}

/// @brief Require raw JSON strings and Unicode escapes to encode Unicode scalar values.
static void test_gltf_json_rejects_malformed_utf8_and_surrogates() {
    const std::vector<std::vector<uint8_t>> invalid_utf8 = {{0x80u},
                                                            {0xC0u, 0x80u},
                                                            {0xC2u},
                                                            {0xC2u, 0x20u},
                                                            {0xE0u, 0x80u, 0x80u},
                                                            {0xEDu, 0xA0u, 0x80u},
                                                            {0xE2u, 0x82u},
                                                            {0xE2u, 0x28u, 0xA1u},
                                                            {0xF0u, 0x80u, 0x80u, 0x80u},
                                                            {0xF4u, 0x90u, 0x80u, 0x80u},
                                                            {0xF0u, 0x9Fu, 0x92u},
                                                            {0xF0u, 0x28u, 0x8Cu, 0xBCu},
                                                            {0xF5u, 0x80u, 0x80u, 0x80u}};
    for (const std::vector<uint8_t> &payload : invalid_utf8) {
        std::string json = "{\"v\":\"";
        json.append(reinterpret_cast<const char *>(payload.data()), payload.size());
        json += "\"}";
        EXPECT_TRUE(!gltf_json_validate_document(json.data(), json.size()),
                    "Malformed raw UTF-8 cannot enter a glTF JSON string");
        size_t next = 17;
        EXPECT_TRUE(gltf_json_read_string_alloc(json.data(), json.size(), 5, &next) == nullptr &&
                        next == SIZE_MAX,
                    "String extraction rejects the same malformed raw UTF-8");
    }

    const char *invalid_escapes[] = {
        "{\"v\":\"\\uD800\"}", "{\"v\":\"\\uDC00\"}", "{\"v\":\"\\uD800\\u0041\"}"};
    for (const char *json : invalid_escapes)
        EXPECT_TRUE(!gltf_json_validate_document(json, std::strlen(json)),
                    "Unpaired UTF-16 surrogate escapes are rejected consistently");

    const char escaped_nul[] = "{\"v\":\"\\u0000\"}";
    EXPECT_TRUE(gltf_json_validate_document(escaped_nul, sizeof(escaped_nul) - 1u),
                "Escaped U+0000 remains valid JSON syntax");
    EXPECT_TRUE(gltf_json_object_get_string(
                    escaped_nul, sizeof(escaped_nul) - 1u, 0, sizeof(escaped_nul) - 1u, "v") ==
                    nullptr,
                "C-string extraction rejects decoded U+0000 instead of returning truncated data");

    const char valid[] = "{\"v\":\"\xC3\xA9 \\uD834\\uDD1E\"}";
    EXPECT_TRUE(gltf_json_validate_document(valid, sizeof(valid) - 1u),
                "Valid raw UTF-8 and a valid surrogate pair remain accepted");
    char *decoded =
        gltf_json_object_get_string(valid, sizeof(valid) - 1u, 0, sizeof(valid) - 1u, "v");
    EXPECT_TRUE(decoded != nullptr && std::strcmp(decoded, "\xC3\xA9 \xF0\x9D\x84\x9E") == 0,
                "Valid mixed Unicode strings decode to canonical UTF-8");
    std::free(decoded);
}

/// @brief Verify every public object/range helper validates its exact claimed container.
static void test_gltf_json_object_ranges_are_transactional() {
    size_t start = 1;
    size_t end = 2;
    EXPECT_TRUE(!gltf_json_object_find_value(nullptr, 1, 0, 1, "x", &start, &end) &&
                    start == SIZE_MAX && end == SIZE_MAX,
                "Object lookup rejects a null source without touching memory");

    const char valid[] = "{\"x\":1}";
    EXPECT_TRUE(!gltf_json_object_find_value(
                    valid, sizeof(valid) - 1u, 0, sizeof(valid), "x", &start, &end),
                "Object lookup rejects an end beyond the source range");
    EXPECT_TRUE(!gltf_json_object_find_value("[1]", 3, 0, 3, "x", &start, &end),
                "Object lookup requires an object container");

    const char malformed_suffix[] = "{\"x\":1,}";
    EXPECT_TRUE(!gltf_json_object_find_value(malformed_suffix,
                                             sizeof(malformed_suffix) - 1u,
                                             0,
                                             sizeof(malformed_suffix) - 1u,
                                             "x",
                                             &start,
                                             &end),
                "A malformed suffix invalidates an earlier object member");

    const char duplicate[] = "{\"x\":1,\"x\":2}";
    EXPECT_TRUE(
        !gltf_json_object_find_value(
            duplicate, sizeof(duplicate) - 1u, 0, sizeof(duplicate) - 1u, "x", &start, &end),
        "Duplicate queried keys are rejected instead of selecting an ambiguous value");

    const char escaped_range[] = "{\"x\":\"outside\"}";
    EXPECT_TRUE(gltf_json_object_get_string(escaped_range, sizeof(escaped_range) - 1u, 0, 9, "x") ==
                    nullptr,
                "String extraction cannot scan past the claimed object range");

    const char bad_root[] = "{\"items\":[],\"bad\":}";
    EXPECT_TRUE(
        !gltf_json_find_top_level_array(bad_root, sizeof(bad_root) - 1u, "items", &start, &end),
        "A malformed root suffix invalidates a previously found top-level array");
    EXPECT_TRUE(!gltf_json_find_top_level_array(nullptr, 1, "items", &start, &end),
                "Top-level array lookup rejects null input safely");

    const char crossed[] = "{\"x\":[}]";
    EXPECT_TRUE(gltf_json_find_matching(crossed, sizeof(crossed) - 1u, 0, '{', '}') == SIZE_MAX,
                "Matching rejects crossed object and array delimiters");
    EXPECT_TRUE(gltf_json_find_matching("()", 2, 0, '(', ')') == SIZE_MAX,
                "Matching accepts only JSON container delimiter pairs");
}

/// @brief Reject wrong JSON types at glTF positions whose schema requires integral values.
static void test_gltf_json_integral_schema_rejects_wrong_types() {
    const char *invalid[] = {
        "[]",
        "{\"asset\":\"2.0\"}",
        "{\"buffers\":{}}",
        "{\"nodes\":[0]}",
        "{\"scene\":\"0\"}",
        "{\"accessors\":[{\"normalized\":null}]}",
        "{\"scenes\":[{\"nodes\":[\"0\"]}]}",
        "{\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":\"0\"}}]}]}",
        "{\"meshes\":[{\"primitives\":[{\"targets\":[0]}]}]}",
        "{\"extensions\":{\"KHR_texture_transform\":0}}",
        "{\"extensions\":{\"KHR_materials_variants\":{\"variants\":[0]}}}",
        "{\"extensions\":{\"EXT_meshopt_compression\":{\"mode\":0}}}"};
    for (const char *json : invalid)
        EXPECT_TRUE(!gltf_json_validate_gltf_integral_tokens(json, std::strlen(json)),
                    "Recognized glTF integral positions reject the wrong JSON type");

    const char invalid_bool[] = "{\"v\":1.5}";
    EXPECT_TRUE(
        gltf_json_object_get_boolish(
            invalid_bool, sizeof(invalid_bool) - 1u, 0, sizeof(invalid_bool) - 1u, "v", 73) == 73,
        "An invalid present boolish value returns the caller's exact fallback");

    const char valid_polymorphic[] =
        "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"children\":[0]}],"
        "\"animations\":[{\"channels\":[{\"target\":{\"node\":0,"
        "\"path\":\"translation\"}}]}],\"accessors\":[{\"sparse\":{\"indices\":{"
        "\"bufferView\":0,\"componentType\":5123}}}],\"extensions\":{"
        "\"KHR_materials_variants\":{\"variants\":[{\"name\":\"red\"}]},"
        "\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":0,"
        "\"byteLength\":1,\"byteStride\":1,\"count\":1,\"mode\":\"ATTRIBUTES\","
        "\"filter\":\"NONE\"}}}";
    EXPECT_TRUE(
        gltf_json_validate_gltf_integral_tokens(valid_polymorphic, sizeof(valid_polymorphic) - 1u),
        "Schema hardening preserves valid root objects, polymorphic objects, variants, "
        "and meshopt string enums");
}

/// @brief Ensure plain `.gltf` bytes containing a literal NUL cannot parse as a valid prefix.
/// @details Exercises both synchronous and preload entry points with a valid object before the NUL
/// and attacker-controlled trailing bytes after it.
static void test_gltf_embedded_nul_is_not_prefix_truncated() {
    const char *path = "/tmp/zanna_gltf_embedded_nul.gltf";
    const char prefix[] = "{\"asset\":{\"version\":\"2.0\"}}";
    const char suffix[] = "{\"asset\":{\"version\":\"2.0\"}}";
    std::vector<uint8_t> bytes(prefix, prefix + std::strlen(prefix));
    bytes.push_back(0u);
    bytes.insert(bytes.end(), suffix, suffix + std::strlen(suffix));
    EXPECT_TRUE(write_binary_file(path, bytes), "Embedded-NUL glTF fixture can be written");
    rt_asset_error_clear();
    EXPECT_TRUE(rt_gltf_load(rt_const_cstr(path)) == nullptr,
                "Synchronous glTF load rejects a valid JSON prefix before embedded NUL");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "Embedded-NUL synchronous rejection records invalid data");

    uint8_t *owned = static_cast<uint8_t *>(std::malloc(bytes.size()));
    EXPECT_TRUE(owned != nullptr, "Embedded-NUL preload copy can be allocated");
    if (owned) {
        std::memcpy(owned, bytes.data(), bytes.size());
        char error[128] = {0};
        rt_asset_error_clear();
        rt_gltf_preload_bundle *bundle =
            rt_gltf_preload_bundle_create_cstr(path, owned, bytes.size(), 0, error, sizeof(error));
        EXPECT_TRUE(bundle == nullptr,
                    "Preload glTF path rejects a valid JSON prefix before embedded NUL");
        EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                    "Embedded-NUL preload rejection records invalid data");
        if (bundle)
            rt_gltf_preload_bundle_free(bundle);
    }
    std::remove(path);
}

int main() {
    test_gltf_accessors_reject_wrong_handles();
    test_gltf_loads_data_uri_buffers_and_embedded_textures();
    test_gltf_resolves_percent_encoded_external_buffers();
    test_gltf_preload_bundle_supplies_external_buffers();
    test_gltf_preload_bundle_rejects_missing_required_buffers();
    test_gltf_preload_bundle_rejects_short_glb_bin();
    test_gltf_preload_bundle_validates_accessor_ranges();
    test_gltf_preload_bundle_rejects_corrupt_required_image_payload();
    test_gltf_preload_bundle_stages_data_uri_buffers_and_images();
    test_gltf_preload_bundle_decodes_bmp_images_to_rgba_pod();
    test_gltf_preload_bundle_decodes_jpeg_images_to_rgba_pod();
    test_gltf_preload_bundle_decodes_gif_images_to_rgba_pod();
    test_gltf_preload_bundle_decodes_static_mesh_to_pod();
    test_gltf_preload_bundle_decodes_strip_and_fan_without_normals_to_pod();
    test_gltf_preload_bundle_stages_buffer_view_images();
    test_gltf_preload_bundle_slices_decoded_image_commit();
    test_gltf_load_asset_resolves_mounted_external_buffers();
    test_gltf_load_asset_handles_glb_filesystem_and_mounted_package();
    test_gltf_rejects_out_of_range_indices();
    test_gltf_skips_non_triangle_primitives();
    test_gltf_drops_invalid_optional_attributes();
    test_gltf_rejects_unsorted_sparse_indices();
    test_gltf_rejects_invalid_skin_reference();
    test_gltf_rejects_invalid_skin_joint_tables();
    test_gltf_builds_scene_hierarchy_for_active_scene();
    test_gltf_imports_extended_vertex_attributes_and_triangle_strips();
    test_gltf_clips_and_renormalizes_primary_joint_influences();
    test_gltf_reduces_secondary_joint_sets_to_top_four_influences();
    test_gltf_applies_matrix_nodes_in_column_major_order();
    test_gltf_imports_skins_and_animation_clips();
    test_gltf_partitions_oversized_skins();
    test_gltf_eight_influence_import();
    test_gltf_compress_animations_option();
    test_gltf_ior_and_volume_extensions();
    test_gltf_skips_duplicate_skeletal_animation_channels();
    test_gltf_imports_step_skeletal_animation_as_hold_keys();
    test_gltf_rejects_skeletal_trs_animation_output_count_mismatch();
    test_gltf_rejects_node_trs_animation_output_count_mismatch();
    test_gltf_ignores_inverse_bind_matrices_with_count_mismatch();
    test_gltf_skips_duplicate_node_animation_channels();
    test_gltf_imports_step_node_animation_duplicate_times();
    test_gltf_rejects_mismatched_morph_weight_animation_width();
    test_gltf_splits_animation_clips_per_skin();
    test_gltf_applies_sparse_accessors();
    test_gltf_imports_morph_targets();
    test_gltf_rejects_malformed_glb_headers();
    test_gltf_rejects_corrupt_required_image_payload();
    test_gltf_rejects_corrupt_extension_texture_payloads();
    test_gltf_rejects_unsafe_external_buffer_paths();
    test_gltf_accepts_dot_relative_external_buffer_paths();
    test_gltf_rejects_percent_decoded_nul_external_paths();
    test_gltf_rejects_control_chars_in_external_paths();
    test_gltf_rejects_external_uri_schemes_and_malformed_escapes();
    test_gltf_rejects_malformed_data_uri_percent_escapes();
    test_gltf_rejects_invalid_node_resource_references();
    test_gltf_rejects_invalid_declared_scene_roots();
    test_gltf_material_without_pbr_uses_pbr_defaults();
    test_gltf_ignores_wrong_typed_optional_string_fields();
    test_gltf_assigns_default_material_to_materialless_primitives();
    test_gltf_uses_texture_texcoord_and_transform();
    test_gltf_validates_authored_texture_uv_streams();
    test_gltf_imports_material_variants();
    test_gltf_meshopt_compressed_views_roundtrip();
    test_gltf_meshopt_exponential_filter();
    test_gltf_meshopt_octahedral_filter();
    test_gltf_quantized_attributes_roundtrip();
    test_gltf_imports_material_extensions_supported_by_material3d();
    test_gltf_converts_spec_glossiness_materials();
    test_gltf_converts_spec_glossiness_texture_per_texel();
    test_gltf_forced_tangents_load_option();
    test_gltf_imports_ktx2_basisu_textures();
    test_gltf_preserves_negative_matrix_scale_sign();
    test_gltf_matrix_shear_does_not_leak_into_rotation();
    test_gltf_rejects_skins_over_runtime_bone_limit();
    test_gltf_rejects_unsupported_required_extensions();
    test_gltf_warns_unsupported_optional_extensions();
    test_gltf_rejects_required_extensions_with_partial_runtime_support();
    test_gltf_rejects_non_string_required_extensions();
    test_gltf_accepts_supported_required_extensions_with_parser_coverage();
    test_gltf_imports_required_punctual_lights();
    test_gltf_preserves_primary_texture_sampler_state();
    test_gltf_maps_every_sampler_filter_enum_to_independent_axes();
    test_gltf_preserves_independent_texture_slot_metadata();
    test_gltf_rejects_invalid_scene_graph_links();
    test_gltf_shared_glb_chunk_validation_is_atomic();
    test_gltf_json_integral_tokens_are_exact();
    test_gltf_json_array_iteration_rejects_malformed_suffixes();
    test_gltf_json_uses_exact_json_whitespace();
    test_gltf_json_rejects_malformed_utf8_and_surrogates();
    test_gltf_json_object_ranges_are_transactional();
    test_gltf_json_integral_schema_rejects_wrong_types();
    test_gltf_embedded_nul_is_not_prefix_truncated();
    std::printf("GLTF tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
