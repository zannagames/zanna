//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

// File: src/tests/unit/test_rt_model3d.cpp
// Purpose: Unit tests for SceneAsset, FBX, and glTF asset import/runtime accessors.
// Key invariants:
//   - Imported asset handles expose stable meshes/materials/animations/scenes.
//   - Recoverable content failures return NULL with asset diagnostics.
//   - SceneAsset templates and instances preserve independent typed node metadata.
// Ownership/Lifetime:
//   - Tests release GC-managed runtime objects they allocate or load.
//   - Temporary fixture files are removed by their owning tests.
// Links: rt_model3d.h, rt_fbx_loader.h, rt_gltf.h, rt_asset_error.h,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_asset_error.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_fbx_loader.h"
#include "rt_gltf.h"
#include "rt_model3d.h"
#include "rt_morphtarget3d.h"
#include "rt_morphtarget3d_internal.h"
#include "rt_option.h"
#include "rt_pixels.h"
#include "rt_result.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ZpakWriter.hpp"

extern "C" {
extern rt_string rt_const_cstr(const char *s);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern double rt_quat_x(void *q);
extern double rt_quat_y(void *q);
extern double rt_quat_z(void *q);
extern double rt_quat_w(void *q);
extern void *rt_mat4_identity(void);
extern void *rt_mesh3d_new_box(double w, double h, double d);
extern void *rt_material3d_new_color(double r, double g, double b);
extern void *rt_asset_decode_typed(const char *name, const uint8_t *data, size_t size);
extern void *rt_vec3_new(double x, double y, double z);
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_retain_maybe(void *p);
extern int32_t rt_obj_release_check0(void *p);
extern void rt_obj_free(void *p);
extern void bc6h_decode_block(const uint8_t block[16], int is_signed, uint16_t out_halves[16][3]);
}

struct SceneAssetView {
    void *vptr;
    rt_scene_node3d *template_root;
    void **meshes;
    int32_t mesh_count;
    int32_t mesh_capacity;
    void **materials;
    int32_t material_count;
    int32_t material_capacity;
    void **skeletons;
    int32_t skeleton_count;
    int32_t skeleton_capacity;
    void **animations;
    int32_t animation_count;
    int32_t animation_capacity;
    void **node_animations;
    int32_t node_animation_count;
    int32_t node_animation_capacity;
    void **cameras;
    int32_t camera_count;
    int32_t camera_capacity;

    struct {
        rt_scene_node3d *root;
        char *name;
        void **cameras;
        int32_t camera_count;
        int32_t camera_capacity;
    } *scenes;

    int32_t scene_count;
    int32_t scene_capacity;
};

struct FbxAssetView {
    void *vptr;
    void **meshes;
    int32_t mesh_count;
    int32_t mesh_capacity;
    void *skeleton;
    void **animations;
    int32_t animation_count;
    int32_t animation_capacity;
    void **node_animations;
    int32_t node_animation_count;
    int32_t node_animation_capacity;
    void **cameras;
    int32_t camera_count;
    int32_t camera_capacity;
    void **materials;
    int32_t material_count;
    int32_t material_capacity;
    void **morph_targets;
    int32_t morph_count;
    int32_t morph_capacity;
    void *scene_root;
};

struct GltfSceneInfoView {
    void *root;
    char *name;
    int32_t node_count;
    void **cameras;
    int32_t camera_count;
    int32_t camera_capacity;
};

struct GltfAssetView {
    void *vptr;
    void **meshes;
    int32_t mesh_count;
    int32_t mesh_capacity;
    void **materials;
    int32_t material_count;
    int32_t material_capacity;
    void **skeletons;
    int32_t skeleton_count;
    int32_t skeleton_capacity;
    void **animations;
    int32_t animation_count;
    int32_t animation_capacity;
    void **node_animations;
    int32_t node_animation_count;
    int32_t node_animation_capacity;
    void **cameras;
    int32_t camera_count;
    int32_t camera_capacity;
    GltfSceneInfoView *scenes;
    int32_t scene_count;
    int32_t scene_capacity;
    void *scene_root;
    int32_t node_count;
};

static int tests_passed = 0;
static int tests_run = 0;
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::fprintf(stderr, "Unexpected trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

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

static void skip_test(const char *msg) {
    tests_run++;
    tests_passed++;
    std::printf("SKIP: %s\n", msg);
}

template <typename T> static void append_bytes(std::vector<uint8_t> &buf, const T &value) {
    size_t offset = buf.size();
    buf.resize(offset + sizeof(T));
    std::memcpy(buf.data() + offset, &value, sizeof(T));
}

static bool write_text_file(const char *path, const std::string &text) {
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

static bool asset_warning_contains(const char *required_a, const char *required_b) {
    int64_t count = rt_asset_error_get_warning_count();
    for (int64_t i = 0; i < count; ++i) {
        const char *warning = rt_asset_error_get_warning(i);
        if (warning && std::strstr(warning, required_a) &&
            (!required_b || std::strstr(warning, required_b))) {
            return true;
        }
    }
    return false;
}

static bool write_binary_file(const char *path, const std::vector<uint8_t> &bytes) {
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool read_binary_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)len);
    bool ok = len == 0 || std::fread(out.data(), 1, (size_t)len, f) == (size_t)len;
    std::fclose(f);
    return ok;
}

static bool texture_source_equals(void *texture,
                                  const char *expected_kind,
                                  const std::vector<uint8_t> &expected) {
    const uint8_t *data = nullptr;
    uint64_t size = 0;
    const char *kind = nullptr;
    return texture && expected_kind &&
           rt_textureasset3d_get_source_container(texture, &data, &size, &kind) && data && kind &&
           std::strcmp(kind, expected_kind) == 0 && size == expected.size() &&
           std::memcmp(data, expected.data(), expected.size()) == 0;
}

static void release_test_ref(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

static bool replace_first_text(std::string &text,
                               const std::string &needle,
                               const std::string &replacement) {
    size_t offset = text.find(needle);
    if (offset == std::string::npos)
        return false;
    text.replace(offset, needle.size(), replacement);
    return true;
}

static bool replace_first_json_string_value(std::string &text,
                                            const char *field,
                                            const char *replacement) {
    std::string prefix = std::string("\"") + field + "\": \"";
    size_t value_offset = text.find(prefix);
    if (value_offset == std::string::npos)
        return false;
    value_offset += prefix.size();
    size_t value_end = text.find('"', value_offset);
    if (value_end == std::string::npos)
        return false;
    text.replace(value_offset, value_end - value_offset, replacement);
    return true;
}

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

static int base64_fixture_value(char ch) {
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    if (ch == '+')
        return 62;
    if (ch == '/')
        return 63;
    return -1;
}

static bool base64_decode_fixture(const char *text, std::vector<uint8_t> &out) {
    if (!text)
        return false;
    const size_t length = std::strlen(text);
    if ((length & 3u) != 0u)
        return false;
    out.clear();
    out.reserve((length / 4u) * 3u);
    for (size_t offset = 0; offset < length; offset += 4u) {
        const bool third_padding = text[offset + 2u] == '=';
        const bool fourth_padding = text[offset + 3u] == '=';
        const int a = base64_fixture_value(text[offset]);
        const int b = base64_fixture_value(text[offset + 1u]);
        const int c = third_padding ? 0 : base64_fixture_value(text[offset + 2u]);
        const int d = fourth_padding ? 0 : base64_fixture_value(text[offset + 3u]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (third_padding && !fourth_padding) ||
            ((third_padding || fourth_padding) && offset + 4u != length)) {
            out.clear();
            return false;
        }
        const uint32_t packed = (static_cast<uint32_t>(a) << 18u) |
                                (static_cast<uint32_t>(b) << 12u) |
                                (static_cast<uint32_t>(c) << 6u) | static_cast<uint32_t>(d);
        out.push_back(static_cast<uint8_t>(packed >> 16u));
        if (!third_padding)
            out.push_back(static_cast<uint8_t>(packed >> 8u));
        if (!fourth_padding)
            out.push_back(static_cast<uint8_t>(packed));
    }
    return true;
}

struct FbxPropFixture {
    char type;
    std::vector<uint8_t> payload;
};

struct FbxNodeFixture {
    std::string name;
    std::vector<FbxPropFixture> props;
    std::vector<FbxNodeFixture> children;
};

static void patch_u32(std::vector<uint8_t> &buf, size_t offset, uint32_t value) {
    if (offset + sizeof(value) <= buf.size())
        std::memcpy(buf.data() + offset, &value, sizeof(value));
}

static FbxPropFixture fbx_prop_string_fixture(const std::string &value) {
    FbxPropFixture prop;
    uint32_t len = (uint32_t)value.size();
    prop.type = 'S';
    append_bytes(prop.payload, len);
    prop.payload.insert(prop.payload.end(), value.begin(), value.end());
    return prop;
}

static FbxPropFixture fbx_prop_i64_fixture(int64_t value) {
    FbxPropFixture prop;
    prop.type = 'L';
    append_bytes(prop.payload, value);
    return prop;
}

static FbxPropFixture fbx_prop_f64_fixture(double value) {
    FbxPropFixture prop;
    prop.type = 'D';
    append_bytes(prop.payload, value);
    return prop;
}

static FbxPropFixture fbx_prop_raw_fixture(const uint8_t *data, size_t len) {
    FbxPropFixture prop;
    uint32_t byte_length = (uint32_t)len;
    prop.type = 'R';
    append_bytes(prop.payload, byte_length);
    if (data && len > 0)
        prop.payload.insert(prop.payload.end(), data, data + len);
    return prop;
}

template <typename T>
static FbxPropFixture fbx_prop_array_fixture(char type, const T *data, size_t count) {
    FbxPropFixture prop;
    uint32_t element_count = (uint32_t)count;
    uint32_t byte_length = (uint32_t)(count * sizeof(T));
    prop.type = type;
    append_bytes(prop.payload, element_count);
    append_bytes(prop.payload, (uint32_t)0);
    append_bytes(prop.payload, byte_length);
    if (data && count > 0) {
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
        prop.payload.insert(prop.payload.end(), bytes, bytes + byte_length);
    }
    return prop;
}

/// @brief Compute RFC 1950 Adler-32 for a fixture byte span.
/// @param data Source bytes; may be NULL only when @p length is zero.
/// @param length Number of bytes to checksum.
/// @return Adler-32 value in host integer form.
static uint32_t fbx_fixture_adler32(const uint8_t *data, size_t length) {
    uint32_t a = 1u;
    uint32_t b = 0u;
    for (size_t i = 0; i < length; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16u) | a;
}

/// @brief Wrap raw bytes in a standards-compliant zlib stream containing stored DEFLATE blocks.
/// @details Stored blocks keep fixture construction zero-dependency while still exercising FBX's
///          compressed-array path and the shared strict RFC 1950 decoder.
/// @param data Raw source bytes.
/// @param length Raw byte count.
/// @return Complete CMF/FLG + DEFLATE + Adler-32 stream.
static std::vector<uint8_t> fbx_fixture_zlib_store(const uint8_t *data, size_t length) {
    std::vector<uint8_t> out;
    size_t offset = 0;
    out.push_back(0x78u);
    out.push_back(0x01u);
    do {
        size_t remaining = length - offset;
        uint16_t block_length = (uint16_t)(remaining > UINT16_MAX ? UINT16_MAX : remaining);
        uint16_t inverse_length = (uint16_t)~block_length;
        int final_block = offset + block_length == length;
        out.push_back((uint8_t)(final_block ? 1u : 0u));
        out.push_back((uint8_t)(block_length & 0xffu));
        out.push_back((uint8_t)(block_length >> 8u));
        out.push_back((uint8_t)(inverse_length & 0xffu));
        out.push_back((uint8_t)(inverse_length >> 8u));
        if (block_length > 0) {
            out.insert(out.end(), data + offset, data + offset + block_length);
            offset += block_length;
        }
    } while (offset < length);
    uint32_t adler = fbx_fixture_adler32(data, length);
    out.push_back((uint8_t)(adler >> 24u));
    out.push_back((uint8_t)(adler >> 16u));
    out.push_back((uint8_t)(adler >> 8u));
    out.push_back((uint8_t)adler);
    return out;
}

/// @brief Build one encoding-1 FBX numeric array property from stored-block zlib bytes.
/// @tparam T Trivially copyable numeric element type.
/// @param type FBX array type byte (`i`, `l`, `f`, or `d`).
/// @param data Numeric source array.
/// @param count Element count.
/// @return Compressed-array fixture property.
template <typename T>
static FbxPropFixture fbx_prop_compressed_array_fixture(char type, const T *data, size_t count) {
    FbxPropFixture prop;
    size_t byte_length = count * sizeof(T);
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    std::vector<uint8_t> compressed = fbx_fixture_zlib_store(bytes, byte_length);
    prop.type = type;
    append_bytes(prop.payload, (uint32_t)count);
    append_bytes(prop.payload, (uint32_t)1);
    append_bytes(prop.payload, (uint32_t)compressed.size());
    prop.payload.insert(prop.payload.end(), compressed.begin(), compressed.end());
    return prop;
}

static void write_fbx_node_fixture(const FbxNodeFixture &node, std::vector<uint8_t> &out) {
    size_t start = out.size();
    size_t prop_len_offset;
    size_t prop_start;

    append_bytes(out, (uint32_t)0);
    append_bytes(out, (uint32_t)node.props.size());
    prop_len_offset = out.size();
    append_bytes(out, (uint32_t)0);
    append_bytes(out, (uint8_t)node.name.size());
    out.insert(out.end(), node.name.begin(), node.name.end());

    prop_start = out.size();
    for (const FbxPropFixture &prop : node.props) {
        append_bytes(out, (uint8_t)prop.type);
        out.insert(out.end(), prop.payload.begin(), prop.payload.end());
    }
    patch_u32(out, prop_len_offset, (uint32_t)(out.size() - prop_start));

    for (const FbxNodeFixture &child : node.children)
        write_fbx_node_fixture(child, out);
    if (!node.children.empty())
        out.resize(out.size() + 13, 0);

    patch_u32(out, start, (uint32_t)out.size());
}

static std::string make_fbx_object_name(const char *name, const char *kind) {
    std::string out = name ? name : "";
    out.push_back('\0');
    out.push_back('\x01');
    out += kind ? kind : "";
    return out;
}

static FbxNodeFixture make_fbx_property_vec3(const char *name, double x, double y, double z) {
    FbxNodeFixture node;
    node.name = "P";
    node.props.push_back(fbx_prop_string_fixture(name ? name : ""));
    node.props.push_back(fbx_prop_string_fixture("Vector3D"));
    node.props.push_back(fbx_prop_string_fixture(""));
    node.props.push_back(fbx_prop_string_fixture("A"));
    node.props.push_back(fbx_prop_f64_fixture(x));
    node.props.push_back(fbx_prop_f64_fixture(y));
    node.props.push_back(fbx_prop_f64_fixture(z));
    return node;
}

static FbxNodeFixture make_fbx_property_scalar(const char *name, double value) {
    FbxNodeFixture node;
    node.name = "P";
    node.props.push_back(fbx_prop_string_fixture(name ? name : ""));
    node.props.push_back(fbx_prop_string_fixture("Number"));
    node.props.push_back(fbx_prop_string_fixture(""));
    node.props.push_back(fbx_prop_string_fixture("A"));
    node.props.push_back(fbx_prop_f64_fixture(value));
    node.props.push_back(fbx_prop_f64_fixture(0.0));
    node.props.push_back(fbx_prop_f64_fixture(0.0));
    return node;
}

static FbxNodeFixture make_fbx_property_int(const char *name, int64_t value) {
    FbxNodeFixture node;
    node.name = "P";
    node.props.push_back(fbx_prop_string_fixture(name ? name : ""));
    node.props.push_back(fbx_prop_string_fixture("int"));
    node.props.push_back(fbx_prop_string_fixture(""));
    node.props.push_back(fbx_prop_string_fixture("A"));
    node.props.push_back(fbx_prop_i64_fixture(value));
    return node;
}

static FbxNodeFixture make_fbx_property_string(const char *name, const char *value) {
    FbxNodeFixture node;
    node.name = "P";
    node.props.push_back(fbx_prop_string_fixture(name ? name : ""));
    node.props.push_back(fbx_prop_string_fixture("KString"));
    node.props.push_back(fbx_prop_string_fixture(""));
    node.props.push_back(fbx_prop_string_fixture("A"));
    node.props.push_back(fbx_prop_string_fixture(value ? value : ""));
    return node;
}

static FbxNodeFixture make_fbx_model_fixture(
    int64_t id, const char *name, const char *type, double tx, double ty, double tz) {
    FbxNodeFixture node;
    FbxNodeFixture props70;
    node.name = "Model";
    node.props.push_back(fbx_prop_i64_fixture(id));
    node.props.push_back(fbx_prop_string_fixture(make_fbx_object_name(name, "Model")));
    node.props.push_back(fbx_prop_string_fixture(type ? type : "Null"));
    props70.name = "Properties70";
    props70.children.push_back(make_fbx_property_vec3("Lcl Translation", tx, ty, tz));
    props70.children.push_back(make_fbx_property_vec3("Lcl Rotation", 0.0, 0.0, 0.0));
    props70.children.push_back(make_fbx_property_vec3("Lcl Scaling", 1.0, 1.0, 1.0));
    node.children.push_back(props70);
    return node;
}

static FbxNodeFixture make_fbx_matrix_child(const char *name, const double *values16) {
    FbxNodeFixture node;
    node.name = name ? name : "";
    node.props.push_back(fbx_prop_array_fixture('d', values16, 16));
    return node;
}

static FbxNodeFixture make_fbx_connection_fixture(int64_t child,
                                                  int64_t parent,
                                                  const char *prop = nullptr) {
    FbxNodeFixture node;
    node.name = "C";
    node.props.push_back(fbx_prop_string_fixture(prop ? "OP" : "OO"));
    node.props.push_back(fbx_prop_i64_fixture(child));
    node.props.push_back(fbx_prop_i64_fixture(parent));
    if (prop)
        node.props.push_back(fbx_prop_string_fixture(prop));
    return node;
}

static bool write_fbx_document_fixture(const char *path,
                                       const FbxNodeFixture &objects,
                                       const FbxNodeFixture &connections,
                                       bool z_up = false) {
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    if (z_up) {
        FbxNodeFixture settings;
        FbxNodeFixture properties70;
        settings.name = "GlobalSettings";
        properties70.name = "Properties70";
        properties70.children.push_back(make_fbx_property_int("UpAxis", 2));
        settings.children.push_back(std::move(properties70));
        write_fbx_node_fixture(settings, bytes);
    }
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);
    return write_binary_file(path, bytes);
}

static FbxNodeFixture make_fbx_animation_curve_fixture(int64_t id,
                                                       const int64_t *times,
                                                       const double *values,
                                                       size_t count);

/// @brief Construct one triangle Geometry node for binary typed-graph parity fixtures.
/// @param id Numeric object ID.
/// @param name Display name.
/// @param x_offset X offset applied to all control points.
/// @return Complete mesh Geometry node.
static FbxNodeFixture make_fbx_triangle_geometry_fixture(int64_t id,
                                                         const char *name,
                                                         double x_offset) {
    const double positions[] = {x_offset, 0.0, 0.0, x_offset + 1.0, 0.0, 0.0, x_offset, 1.0, 0.0};
    const int32_t indices[] = {0, 1, ~2};
    FbxNodeFixture geometry;
    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(id));
    geometry.props.push_back(fbx_prop_string_fixture(make_fbx_object_name(name, "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(
        FbxNodeFixture{"Vertices", {fbx_prop_array_fixture('d', positions, 9u)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"PolygonVertexIndex", {fbx_prop_array_fixture('i', indices, 3u)}, {}});
    return geometry;
}

/// @brief Construct a small rational NURBS surface using the native FBX field layout.
/// @details The four control points form a unit square. The final control point has weight two,
///          so the evaluated center is `(0.6, 0.6, 0)` rather than `(0.5, 0.5, 0)`.
/// @param id Numeric Geometry object ID.
/// @param step_u Authored subdivisions per non-empty U knot span.
/// @param descending_u_knots Whether to deliberately corrupt the U knot vector.
/// @return Complete `NurbsSurface` Geometry node.
static FbxNodeFixture make_fbx_nurbs_surface_geometry_fixture(int64_t id,
                                                              int64_t step_u,
                                                              bool descending_u_knots = false) {
    static const double kPoints[] = {
        0.0,
        0.0,
        0.0,
        1.0,
        1.0,
        0.0,
        0.0,
        1.0,
        0.0,
        1.0,
        0.0,
        1.0,
        1.0,
        1.0,
        0.0,
        2.0,
    };
    static const double kKnots[] = {0.0, 0.0, 1.0, 1.0};
    static const double kDescendingKnots[] = {0.0, 1.0, 0.5, 1.0};
    FbxNodeFixture geometry;
    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(id));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("RationalSurface", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("NurbsSurface"));
    geometry.children.push_back(FbxNodeFixture{
        "NurbsSurfaceOrder", {fbx_prop_i64_fixture(2), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Dimensions", {fbx_prop_i64_fixture(2), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Step", {fbx_prop_i64_fixture(step_u), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(FbxNodeFixture{
        "Form", {fbx_prop_string_fixture("Open"), fbx_prop_string_fixture("Open")}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Points", {fbx_prop_array_fixture('d', kPoints, 16u)}, {}});
    geometry.children.push_back(FbxNodeFixture{
        "KnotVectorU",
        {fbx_prop_array_fixture('d', descending_u_knots ? kDescendingKnots : kKnots, 4u)},
        {}});
    geometry.children.push_back(
        FbxNodeFixture{"KnotVectorV", {fbx_prop_array_fixture('d', kKnots, 4u)}, {}});
    return geometry;
}

/// @brief Construct a bilinear FBX Patch over the same unit square as the NURBS fixture.
/// @param id Numeric Geometry object ID.
/// @return Complete `Patch` Geometry node.
static FbxNodeFixture make_fbx_linear_patch_geometry_fixture(int64_t id) {
    static const double kPoints[] = {
        0.0,
        0.0,
        0.0,
        1.0,
        1.0,
        0.0,
        0.0,
        1.0,
        0.0,
        1.0,
        0.0,
        1.0,
        1.0,
        1.0,
        0.0,
        1.0,
    };
    FbxNodeFixture geometry;
    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(id));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("LinearPatch", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Patch"));
    geometry.children.push_back(FbxNodeFixture{
        "PatchType", {fbx_prop_string_fixture("Linear"), fbx_prop_string_fixture("Linear")}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Dimensions", {fbx_prop_i64_fixture(2), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Step", {fbx_prop_i64_fixture(2), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Closed", {fbx_prop_i64_fixture(0), fbx_prop_i64_fixture(0)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Points", {fbx_prop_array_fixture('d', kPoints, 16u)}, {}});
    return geometry;
}

/// @brief Construct one open patch whose U axis exercises a selected FBX basis.
static FbxNodeFixture make_fbx_patch_basis_geometry_fixture(
    int64_t id, const char *name, const char *basis_name, int64_t count_u, bool closed_u = false) {
    std::vector<double> points;
    FbxNodeFixture geometry;
    points.reserve((size_t)count_u * 2u * 4u);
    for (int64_t v = 0; v < 2; ++v) {
        for (int64_t u = 0; u < count_u; ++u) {
            points.push_back(count_u > 1 ? (double)u / (double)(count_u - 1) : 0.0);
            points.push_back((double)v);
            points.push_back((u == count_u / 2) ? 0.25 : 0.0);
            points.push_back(1.0);
        }
    }
    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(id));
    geometry.props.push_back(fbx_prop_string_fixture(make_fbx_object_name(name, "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Patch"));
    geometry.children.push_back(FbxNodeFixture{
        "PatchType", {fbx_prop_string_fixture(basis_name), fbx_prop_string_fixture("Linear")}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Dimensions", {fbx_prop_i64_fixture(count_u), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Step", {fbx_prop_i64_fixture(2), fbx_prop_i64_fixture(1)}, {}});
    geometry.children.push_back(FbxNodeFixture{
        "Closed", {fbx_prop_i64_fixture(closed_u ? 1 : 0), fbx_prop_i64_fixture(0)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Points", {fbx_prop_array_fixture('d', points.data(), points.size())}, {}});
    return geometry;
}

/// @brief Construct a closed/periodic linear NURBS strip with an authored duplicate seam CV.
static FbxNodeFixture make_fbx_closed_nurbs_geometry_fixture(int64_t id,
                                                             const char *name,
                                                             const char *form,
                                                             const char *subtype = "NurbsSurface") {
    static const double kPoints[] = {
        1.0,  0.0, 0.0,           1.0, -0.5, 0.0, 0.8660254038, 1.0, -0.5, 0.0, -0.8660254038, 1.0,
        1.0,  0.0, 0.0,           1.0, 1.0,  1.0, 0.0,          1.0, -0.5, 1.0, 0.8660254038,  1.0,
        -0.5, 1.0, -0.8660254038, 1.0, 1.0,  1.0, 0.0,          1.0,
    };
    static const double kKnotsU[] = {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    static const double kKnotsV[] = {0.0, 0.0, 1.0, 1.0};
    FbxNodeFixture geometry;
    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(id));
    geometry.props.push_back(fbx_prop_string_fixture(make_fbx_object_name(name, "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture(subtype));
    geometry.children.push_back(FbxNodeFixture{
        "NurbsSurfaceOrder", {fbx_prop_i64_fixture(2), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Dimensions", {fbx_prop_i64_fixture(4), fbx_prop_i64_fixture(2)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Step", {fbx_prop_i64_fixture(1), fbx_prop_i64_fixture(1)}, {}});
    geometry.children.push_back(FbxNodeFixture{
        "Form", {fbx_prop_string_fixture(form), fbx_prop_string_fixture("Open")}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"Points", {fbx_prop_array_fixture('d', kPoints, 32u)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"KnotVectorU", {fbx_prop_array_fixture('d', kKnotsU, 6u)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"KnotVectorV", {fbx_prop_array_fixture('d', kKnotsV, 4u)}, {}});
    return geometry;
}

static FbxNodeFixture make_fbx_material_fixture(
    int64_t id, const char *name, double red, double green, double blue);

/// @brief Write a typed binary FBX containing one NURBS surface and one linear patch.
/// @param path Destination file path.
/// @param step_u NURBS U step, allowing malformed-limit fixtures.
/// @param descending_u_knots Whether to deliberately corrupt the NURBS U knots.
/// @return True when the file was written.
static bool write_fbx_procedural_surface_fixture(const char *path,
                                                 int64_t step_u = 2,
                                                 int64_t step_v = 2,
                                                 bool descending_u_knots = false) {
    static const int64_t kNurbsGeometryId = 5701;
    static const int64_t kNurbsModelId = 5702;
    static const int64_t kPatchGeometryId = 5703;
    static const int64_t kPatchModelId = 5704;
    static const int64_t kMaterialId = 5705;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    objects.name = "Objects";
    FbxNodeFixture nurbs =
        make_fbx_nurbs_surface_geometry_fixture(kNurbsGeometryId, step_u, descending_u_knots);
    for (FbxNodeFixture &child : nurbs.children) {
        if (child.name == "Step") {
            child.props[1] = fbx_prop_i64_fixture(step_v);
            break;
        }
    }
    objects.children.push_back(nurbs);
    objects.children.push_back(
        make_fbx_model_fixture(kNurbsModelId, "RationalSurface", "NurbsSurface", 0.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_linear_patch_geometry_fixture(kPatchGeometryId));
    objects.children.push_back(
        make_fbx_model_fixture(kPatchModelId, "LinearPatch", "Patch", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_material_fixture(kMaterialId, "SurfaceMaterial", 0.2, 0.4, 0.6));
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kNurbsModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kNurbsGeometryId, kNurbsModelId));
    connections.children.push_back(make_fbx_connection_fixture(kPatchModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kPatchGeometryId, kPatchModelId));
    connections.children.push_back(make_fbx_connection_fixture(kMaterialId, kNurbsModelId));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write the ASCII counterpart of the rational NURBS surface fixture.
/// @param path Destination file path.
/// @return True when the file was written.
static bool write_fbx_ascii_procedural_surface_fixture(const char *path) {
    const std::string text =
        "; FBX 7.4.0 procedural surface fixture\n"
        "Objects: {\n"
        "  Geometry: 5801, \"Geometry::AsciiSurface\", \"NurbsSurface\" {\n"
        "    NurbsSurfaceOrder: 2, 2\n"
        "    Dimensions: 2, 2\n"
        "    Step: 2, 2\n"
        "    Form: \"Open\", \"Open\"\n"
        "    Points: *16 { a: 0,0,0,1, 1,0,0,1, 0,1,0,1, 1,1,0,2 }\n"
        "    KnotVectorU: *4 { a: 0,0,1,1 }\n"
        "    KnotVectorV: *4 { a: 0,0,1,1 }\n"
        "  }\n"
        "  Model: 5802, \"Model::AsciiSurface\", \"NurbsSurface\" {\n"
        "    Properties70: {\n"
        "      P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 0,0,0\n"
        "    }\n"
        "  }\n"
        "}\n"
        "Connections: {\n"
        "  C: \"OO\", 5802, 0\n"
        "  C: \"OO\", 5801, 5802\n"
        "}\n";
    return write_text_file(path, text);
}

/// @brief Write closed/periodic NURBS and every supported patch basis in one typed graph.
static bool write_fbx_procedural_surface_variants_fixture(const char *path) {
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    int64_t next_id = 5900;
    objects.name = "Objects";
    connections.name = "Connections";
    auto append_geometry = [&](FbxNodeFixture geometry, const char *model_name, const char *type) {
        int64_t geometry_id = 0;
        if (!geometry.props.empty() && geometry.props[0].payload.size() >= sizeof(geometry_id))
            std::memcpy(&geometry_id, geometry.props[0].payload.data(), sizeof(geometry_id));
        int64_t model_id = ++next_id;
        objects.children.push_back(geometry);
        objects.children.push_back(
            make_fbx_model_fixture(model_id, model_name, type, 0.0, 0.0, 0.0));
        connections.children.push_back(make_fbx_connection_fixture(model_id, 0));
        connections.children.push_back(make_fbx_connection_fixture(geometry_id, model_id));
    };

    append_geometry(
        make_fbx_closed_nurbs_geometry_fixture(++next_id, "ClosedSurface", "Closed", "Nurb"),
        "ClosedSurface",
        "Nurb");
    append_geometry(make_fbx_closed_nurbs_geometry_fixture(
                        ++next_id, "PeriodicSurface", "Periodic", "NurbsSurface"),
                    "PeriodicSurface",
                    "NurbsSurface");
    {
        FbxNodeFixture missing_knots = make_fbx_nurbs_surface_geometry_fixture(++next_id, 1, false);
        missing_knots.props[1] =
            fbx_prop_string_fixture(make_fbx_object_name("GeneratedKnots", "Geometry"));
        missing_knots.props[2] = fbx_prop_string_fixture("Nurbs");
        for (auto it = missing_knots.children.begin(); it != missing_knots.children.end();) {
            if (it->name == "KnotVectorU" || it->name == "KnotVectorV")
                it = missing_knots.children.erase(it);
            else
                ++it;
        }
        append_geometry(missing_knots, "GeneratedKnots", "Nurbs");
    }
    append_geometry(make_fbx_patch_basis_geometry_fixture(++next_id, "BezierPatch", "Bezier", 4),
                    "BezierPatch",
                    "Patch");
    append_geometry(
        make_fbx_patch_basis_geometry_fixture(++next_id, "QuadraticPatch", "BezierQuadric", 3),
        "QuadraticPatch",
        "Patch");
    append_geometry(
        make_fbx_patch_basis_geometry_fixture(++next_id, "CardinalPatch", "Cardinal", 4),
        "CardinalPatch",
        "Patch");
    append_geometry(make_fbx_patch_basis_geometry_fixture(++next_id, "BSplinePatch", "BSpline", 4),
                    "BSplinePatch",
                    "Patch");
    append_geometry(
        make_fbx_patch_basis_geometry_fixture(++next_id, "LinearBasisPatch", "Linear", 2),
        "LinearBasisPatch",
        "Patch");
    append_geometry(
        make_fbx_patch_basis_geometry_fixture(++next_id, "ClosedLinearPatch", "Linear", 4, true),
        "ClosedLinearPatch",
        "Patch");
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a NURBS surface with a connected trim boundary, which must fail closed.
static bool write_fbx_trimmed_surface_fixture(const char *path) {
    static const int64_t kGeometryId = 6101;
    static const int64_t kBoundaryId = 6102;
    static const int64_t kModelId = 6103;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture boundary;
    boundary.name = "Geometry";
    boundary.props.push_back(fbx_prop_i64_fixture(kBoundaryId));
    boundary.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("TrimBoundary", "Geometry")));
    boundary.props.push_back(fbx_prop_string_fixture("Boundary"));
    objects.name = "Objects";
    objects.children.push_back(make_fbx_nurbs_surface_geometry_fixture(kGeometryId, 2));
    objects.children.push_back(boundary);
    objects.children.push_back(
        make_fbx_model_fixture(kModelId, "TrimmedSurface", "NurbsSurface", 0.0, 0.0, 0.0));
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kBoundaryId, kGeometryId));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a NURBS surface whose declared control counts do not match its point array.
static bool write_fbx_bad_surface_control_count_fixture(const char *path) {
    static const int64_t kGeometryId = 6201;
    static const int64_t kModelId = 6202;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture geometry = make_fbx_nurbs_surface_geometry_fixture(kGeometryId, 2);
    for (FbxNodeFixture &child : geometry.children) {
        if (child.name == "Dimensions") {
            child.props[0] = fbx_prop_i64_fixture(3);
            break;
        }
    }
    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(
        make_fbx_model_fixture(kModelId, "BadControlNet", "NurbsSurface", 0.0, 0.0, 0.0));
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Construct one material object with an authored DiffuseColor.
/// @param id Numeric object ID.
/// @param name Display name.
/// @param red Diffuse red lane.
/// @param green Diffuse green lane.
/// @param blue Diffuse blue lane.
/// @return Complete Material node.
static FbxNodeFixture make_fbx_material_fixture(
    int64_t id, const char *name, double red, double green, double blue) {
    FbxNodeFixture material;
    FbxNodeFixture properties;
    material.name = "Material";
    material.props.push_back(fbx_prop_i64_fixture(id));
    material.props.push_back(fbx_prop_string_fixture(make_fbx_object_name(name, "Material")));
    material.props.push_back(fbx_prop_string_fixture(""));
    properties.name = "Properties70";
    properties.children.push_back(make_fbx_property_vec3("DiffuseColor", red, green, blue));
    material.children.push_back(properties);
    return material;
}

/// @brief Build the binary counterpart of the multi-object typed ASCII regression graph.
/// @details The graph contains two meshes/models/materials, a two-bone hierarchy, and one keyed
///          animation stack so parity covers every extraction family enabled by ASCII FBX.
/// @param objects Receives an `Objects` node.
/// @param connections Receives a matching `Connections` node.
static void make_fbx_multi_object_graph_fixture(FbxNodeFixture *objects,
                                                FbxNodeFixture *connections) {
    static const int64_t times[] = {0, 46186158000LL};
    static const double values[] = {0.0, 2.0};
    FbxNodeFixture stack;
    FbxNodeFixture layer;
    FbxNodeFixture curve_node;
    if (!objects || !connections)
        return;
    objects->name = "Objects";
    objects->children.push_back(make_fbx_triangle_geometry_fixture(100, "MeshA", 0.0));
    objects->children.push_back(make_fbx_triangle_geometry_fixture(101, "MeshB", 2.0));
    objects->children.push_back(make_fbx_model_fixture(200, "ModelA", "Mesh", 0.0, 0.0, 0.0));
    objects->children.push_back(make_fbx_model_fixture(201, "ModelB", "Mesh", 0.0, 0.0, 0.0));
    objects->children.push_back(make_fbx_material_fixture(300, "Red", 1.0, 0.0, 0.0));
    objects->children.push_back(make_fbx_material_fixture(301, "Green", 0.0, 1.0, 0.0));
    objects->children.push_back(make_fbx_model_fixture(400, "RigRoot", "Root", 0.0, 0.0, 0.0));
    objects->children.push_back(make_fbx_model_fixture(401, "RigChild", "LimbNode", 0.0, 1.0, 0.0));
    stack.name = "AnimationStack";
    stack.props = {fbx_prop_i64_fixture(500),
                   fbx_prop_string_fixture(make_fbx_object_name("Move", "AnimStack")),
                   fbx_prop_string_fixture("")};
    layer.name = "AnimationLayer";
    layer.props = {fbx_prop_i64_fixture(501),
                   fbx_prop_string_fixture(make_fbx_object_name("Base", "AnimLayer")),
                   fbx_prop_string_fixture("")};
    curve_node.name = "AnimationCurveNode";
    curve_node.props = {fbx_prop_i64_fixture(502),
                        fbx_prop_string_fixture(make_fbx_object_name("MoveX", "AnimCurveNode")),
                        fbx_prop_string_fixture("")};
    objects->children.push_back(stack);
    objects->children.push_back(layer);
    objects->children.push_back(curve_node);
    objects->children.push_back(make_fbx_animation_curve_fixture(503, times, values, 2u));

    connections->name = "Connections";
    connections->children.push_back(make_fbx_connection_fixture(200, 0));
    connections->children.push_back(make_fbx_connection_fixture(201, 0));
    connections->children.push_back(make_fbx_connection_fixture(400, 0));
    connections->children.push_back(make_fbx_connection_fixture(401, 400));
    connections->children.push_back(make_fbx_connection_fixture(100, 200));
    connections->children.push_back(make_fbx_connection_fixture(101, 201));
    connections->children.push_back(make_fbx_connection_fixture(300, 200));
    connections->children.push_back(make_fbx_connection_fixture(301, 201));
    connections->children.push_back(make_fbx_connection_fixture(501, 500));
    connections->children.push_back(make_fbx_connection_fixture(502, 501));
    connections->children.push_back(make_fbx_connection_fixture(502, 401, "Lcl Translation"));
    connections->children.push_back(make_fbx_connection_fixture(503, 502, "d|X"));
}

/// @brief Write the binary multi-object parity graph.
/// @param path Destination fixture path.
/// @return True when the document was written completely.
static bool write_fbx_multi_object_binary_fixture(const char *path) {
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    make_fbx_multi_object_graph_fixture(&objects, &connections);
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write an ASCII FBX graph equivalent to @ref write_fbx_multi_object_binary_fixture.
/// @param path Destination fixture path.
/// @return True when the document was written completely.
static bool write_fbx_multi_object_ascii_fixture(const char *path) {
    static const char text[] =
        "; FBX 7.4.0 typed parity fixture\n"
        "Objects: {\n"
        " Geometry: 100, \"Geometry::MeshA\", \"Mesh\" {\n"
        "  Vertices: *9 { a: 0,0,0, 1,0,0, 0,1,0 }\n"
        "  PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
        " }\n"
        " Geometry: 101, \"Geometry::MeshB\", \"Mesh\" {\n"
        "  Vertices: *9 { a: 2,0,0, 3,0,0, 2,1,0 }\n"
        "  PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
        " }\n"
        " Model: 200, \"Model::ModelA\", \"Mesh\" {\n"
        "  Properties70: { P: \"Lcl Translation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "   P: \"Lcl Rotation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "   P: \"Lcl Scaling\",\"Vector3D\",\"\",\"A\",1,1,1 }\n"
        " }\n"
        " Model: 201, \"Model::ModelB\", \"Mesh\" {\n"
        "  Properties70: { P: \"Lcl Translation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "   P: \"Lcl Rotation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "   P: \"Lcl Scaling\",\"Vector3D\",\"\",\"A\",1,1,1 }\n"
        " }\n"
        " Material: 300, \"Material::Red\", \"\" { Properties70: {\n"
        "  P: \"DiffuseColor\",\"Color\",\"\",\"A\",1,0,0 } }\n"
        " Material: 301, \"Material::Green\", \"\" { Properties70: {\n"
        "  P: \"DiffuseColor\",\"Color\",\"\",\"A\",0,1,0 } }\n"
        " Model: 400, \"Model::RigRoot\", \"Root\" { Properties70: {\n"
        "  P: \"Lcl Translation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "  P: \"Lcl Rotation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "  P: \"Lcl Scaling\",\"Vector3D\",\"\",\"A\",1,1,1 } }\n"
        " Model: 401, \"Model::RigChild\", \"LimbNode\" { Properties70: {\n"
        "  P: \"Lcl Translation\",\"Vector3D\",\"\",\"A\",0,1,0\n"
        "  P: \"Lcl Rotation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "  P: \"Lcl Scaling\",\"Vector3D\",\"\",\"A\",1,1,1 } }\n"
        " AnimationStack: 500, \"AnimationStack::Move\", \"\"\n"
        " AnimationLayer: 501, \"AnimationLayer::Base\", \"\"\n"
        " AnimationCurveNode: 502, \"AnimationCurveNode::MoveX\", \"\"\n"
        " AnimationCurve: 503, \"AnimationCurve::X\", \"\" {\n"
        "  KeyTime: *2 { a: 0,46186158000 }\n"
        "  KeyValueFloat: *2 { a: 0,2 }\n"
        " }\n"
        "}\n"
        "Connections: {\n"
        " C: \"OO\",200,0\n C: \"OO\",201,0\n C: \"OO\",400,0\n"
        " C: \"OO\",401,400\n C: \"OO\",100,200\n C: \"OO\",101,201\n"
        " C: \"OO\",300,200\n C: \"OO\",301,201\n C: \"OO\",501,500\n"
        " C: \"OO\",502,501\n C: \"OP\",502,401,\"Lcl Translation\"\n"
        " C: \"OP\",503,502,\"d|X\"\n"
        "}\n";
    return write_text_file(path, text);
}

/// @brief Write a skeleton-only FBX whose bone count crosses the historical 256-bone cap.
static bool write_fbx_large_skeleton_fixture(const char *path, int32_t bone_count) {
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    objects.name = "Objects";
    connections.name = "Connections";
    for (int32_t i = 0; i < bone_count; i++) {
        std::string name = "Bone_" + std::to_string(i);
        objects.children.push_back(make_fbx_model_fixture(
            1000 + i, name.c_str(), i == 0 ? "Root" : "LimbNode", 0.0, i == 0 ? 0.0 : 1.0, 0.0));
        if (i > 0)
            connections.children.push_back(make_fbx_connection_fixture(1000 + i, 1000 + i - 1));
    }
    return write_fbx_document_fixture(path, objects, connections);
}

static FbxNodeFixture make_fbx_deformer_fixture(int64_t id, const char *name, const char *type) {
    FbxNodeFixture node;
    node.name = "Deformer";
    node.props.push_back(fbx_prop_i64_fixture(id));
    node.props.push_back(fbx_prop_string_fixture(make_fbx_object_name(name, "Deformer")));
    node.props.push_back(fbx_prop_string_fixture(type ? type : ""));
    return node;
}

static FbxNodeFixture make_fbx_cluster_fixture(int64_t id,
                                               const char *name,
                                               const int32_t *indices,
                                               size_t index_count,
                                               const double *weights,
                                               size_t weight_count) {
    FbxNodeFixture node = make_fbx_deformer_fixture(id, name, "Cluster");
    node.children.push_back(
        FbxNodeFixture{"Indexes", {fbx_prop_array_fixture('i', indices, index_count)}, {}});
    node.children.push_back(
        FbxNodeFixture{"Weights", {fbx_prop_array_fixture('d', weights, weight_count)}, {}});
    return node;
}

static FbxNodeFixture make_fbx_animation_curve_fixture(int64_t id,
                                                       const int64_t *times,
                                                       const double *values,
                                                       size_t count) {
    FbxNodeFixture node;
    node.name = "AnimationCurve";
    node.props.push_back(fbx_prop_i64_fixture(id));
    node.props.push_back(fbx_prop_string_fixture(make_fbx_object_name("Curve", "AnimCurve")));
    node.props.push_back(fbx_prop_string_fixture(""));
    node.children.push_back(
        FbxNodeFixture{"KeyTime", {fbx_prop_array_fixture('l', times, count)}, {}});
    node.children.push_back(
        FbxNodeFixture{"KeyValueFloat", {fbx_prop_array_fixture('d', values, count)}, {}});
    return node;
}

static FbxNodeFixture make_fbx_animation_curve_fixture_with_attrs(int64_t id,
                                                                  const int64_t *times,
                                                                  const double *values,
                                                                  const int32_t *attr_flags,
                                                                  size_t count) {
    FbxNodeFixture node = make_fbx_animation_curve_fixture(id, times, values, count);
    node.children.push_back(
        FbxNodeFixture{"KeyAttrFlags", {fbx_prop_array_fixture('i', attr_flags, count)}, {}});
    return node;
}

static FbxNodeFixture make_fbx_animation_curve_mismatched_fixture(
    int64_t id, const int64_t *times, size_t time_count, const double *values, size_t value_count) {
    FbxNodeFixture node;
    node.name = "AnimationCurve";
    node.props.push_back(fbx_prop_i64_fixture(id));
    node.props.push_back(fbx_prop_string_fixture(make_fbx_object_name("Curve", "AnimCurve")));
    node.props.push_back(fbx_prop_string_fixture(""));
    node.children.push_back(
        FbxNodeFixture{"KeyTime", {fbx_prop_array_fixture('l', times, time_count)}, {}});
    node.children.push_back(
        FbxNodeFixture{"KeyValueFloat", {fbx_prop_array_fixture('d', values, value_count)}, {}});
    return node;
}

static bool write_fbx_fixture(const char *path) {
    static const int64_t kGeometryId = 100;
    static const int64_t kMaterialId = 200;
    static const int64_t kParentModelId = 300;
    static const int64_t kChildModelId = 301;
    static const double kPositions[9] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[3] = {0, 1, -3};

    FbxNodeFixture geometry;
    FbxNodeFixture material;
    FbxNodeFixture material_props70;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    std::vector<uint8_t> bytes;

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("FixtureMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    material.name = "Material";
    material.props.push_back(fbx_prop_i64_fixture(kMaterialId));
    material.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("FixtureMaterial", "Material")));
    material.props.push_back(fbx_prop_string_fixture(""));
    material_props70.name = "Properties70";
    material_props70.children.push_back(make_fbx_property_vec3("DiffuseColor", 0.2, 0.4, 0.8));
    material_props70.children.push_back(make_fbx_property_scalar("Shininess", 16.0));
    material.children.push_back(material_props70);

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(material);
    objects.children.push_back(
        make_fbx_model_fixture(kParentModelId, "Parent", "Null", 1.0, 2.0, 3.0));
    objects.children.push_back(
        make_fbx_model_fixture(kChildModelId, "Child", "Mesh", 0.0, 5.0, 0.0));

    connections.name = "Connections";
    connections.children.push_back(FbxNodeFixture{"C",
                                                  {fbx_prop_string_fixture("OO"),
                                                   fbx_prop_i64_fixture(kParentModelId),
                                                   fbx_prop_i64_fixture(0)},
                                                  {}});
    connections.children.push_back(FbxNodeFixture{"C",
                                                  {fbx_prop_string_fixture("OO"),
                                                   fbx_prop_i64_fixture(kChildModelId),
                                                   fbx_prop_i64_fixture(kParentModelId)},
                                                  {}});
    connections.children.push_back(FbxNodeFixture{"C",
                                                  {fbx_prop_string_fixture("OO"),
                                                   fbx_prop_i64_fixture(kGeometryId),
                                                   fbx_prop_i64_fixture(kChildModelId)},
                                                  {}});
    connections.children.push_back(FbxNodeFixture{"C",
                                                  {fbx_prop_string_fixture("OO"),
                                                   fbx_prop_i64_fixture(kMaterialId),
                                                   fbx_prop_i64_fixture(kChildModelId)},
                                                  {}});

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool write_fbx_multimaterial_fixture(const char *path) {
    static const int64_t kGeometryId = 2100;
    static const int64_t kRedMaterialId = 2200;
    static const int64_t kBlueMaterialId = 2201;
    static const int64_t kModelId = 2300;
    static const double kPositions[12] = {
        0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    static const int32_t kIndices[6] = {0, 1, -3, 0, 2, -4};
    static const int32_t kMaterialSlots[2] = {0, 1};

    FbxNodeFixture geometry;
    FbxNodeFixture layer_material;
    FbxNodeFixture red_material;
    FbxNodeFixture blue_material;
    FbxNodeFixture red_props70;
    FbxNodeFixture blue_props70;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture model = make_fbx_model_fixture(kModelId, "Multi", "Mesh", 1.0, 2.0, 3.0);
    std::vector<uint8_t> bytes;

    model.children[0].children.push_back(
        make_fbx_property_vec3("GeometricTranslation", 4.0, 5.0, 6.0));
    model.children[0].children.push_back(make_fbx_property_vec3("PreRotation", 0.0, 0.0, 90.0));

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("MultiMaterialMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});
    layer_material.name = "LayerElementMaterial";
    layer_material.children.push_back(
        FbxNodeFixture{"MappingInformationType", {fbx_prop_string_fixture("ByPolygon")}, {}});
    layer_material.children.push_back(
        FbxNodeFixture{"ReferenceInformationType", {fbx_prop_string_fixture("IndexToDirect")}, {}});
    layer_material.children.push_back(FbxNodeFixture{
        "Materials",
        {fbx_prop_array_fixture(
            'i', kMaterialSlots, sizeof(kMaterialSlots) / sizeof(kMaterialSlots[0]))},
        {}});
    geometry.children.push_back(layer_material);

    red_material.name = "Material";
    red_material.props.push_back(fbx_prop_i64_fixture(kRedMaterialId));
    red_material.props.push_back(fbx_prop_string_fixture(make_fbx_object_name("Red", "Material")));
    red_material.props.push_back(fbx_prop_string_fixture(""));
    red_props70.name = "Properties70";
    red_props70.children.push_back(make_fbx_property_vec3("DiffuseColor", 0.9, 0.1, 0.1));
    red_material.children.push_back(red_props70);

    blue_material.name = "Material";
    blue_material.props.push_back(fbx_prop_i64_fixture(kBlueMaterialId));
    blue_material.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("Blue", "Material")));
    blue_material.props.push_back(fbx_prop_string_fixture(""));
    blue_props70.name = "Properties70";
    blue_props70.children.push_back(make_fbx_property_vec3("DiffuseColor", 0.1, 0.2, 0.9));
    blue_material.children.push_back(blue_props70);

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(red_material);
    objects.children.push_back(blue_material);
    objects.children.push_back(model);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kRedMaterialId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kBlueMaterialId, kModelId));

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

/// @brief Write an FBX scene containing typed camera/light attributes and object animation.
static bool write_fbx_camera_light_animation_fixture(const char *path, double area_width = 4.0) {
    static const int64_t kSecond = 46186158000LL;
    static const int64_t kTimes[2] = {0, kSecond};
    static const double kValues[2] = {1.0, 5.0};
    constexpr int64_t kCameraModel = 2400;
    constexpr int64_t kTargetModel = 2401;
    constexpr int64_t kLightModel = 2402;
    constexpr int64_t kPointModel = 2403;
    constexpr int64_t kDirectionalModel = 2404;
    constexpr int64_t kAreaModel = 2405;
    constexpr int64_t kSphereAreaModel = 2406;
    constexpr int64_t kVolumeModel = 2407;
    constexpr int64_t kCameraAttribute = 2410;
    constexpr int64_t kLightAttribute = 2411;
    constexpr int64_t kPointAttribute = 2412;
    constexpr int64_t kDirectionalAttribute = 2413;
    constexpr int64_t kAreaAttribute = 2414;
    constexpr int64_t kSphereAreaAttribute = 2415;
    constexpr int64_t kVolumeAttribute = 2416;
    constexpr int64_t kStack = 2420;
    constexpr int64_t kLayer = 2421;
    constexpr int64_t kCurveNode = 2422;
    constexpr int64_t kCurve = 2423;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture camera_model =
        make_fbx_model_fixture(kCameraModel, "Camera01", "Camera", 2.0, 3.0, 4.0);
    FbxNodeFixture target_model =
        make_fbx_model_fixture(kTargetModel, "CameraTarget", "Marker", 2.0, 3.0, 0.0);
    FbxNodeFixture light_model =
        make_fbx_model_fixture(kLightModel, "KeyLight", "Light", 1.0, 2.0, 3.0);
    FbxNodeFixture point_model =
        make_fbx_model_fixture(kPointModel, "FillLight", "Light", 4.0, 5.0, 6.0);
    FbxNodeFixture directional_model =
        make_fbx_model_fixture(kDirectionalModel, "SunLight", "Light", 0.0, 0.0, 0.0);
    FbxNodeFixture area_model =
        make_fbx_model_fixture(kAreaModel, "PanelLight", "Light", 7.0, 8.0, 9.0);
    FbxNodeFixture sphere_area_model =
        make_fbx_model_fixture(kSphereAreaModel, "OrbLight", "Light", -2.0, 3.0, 1.0);
    FbxNodeFixture volume_model =
        make_fbx_model_fixture(kVolumeModel, "FogLight", "Light", 0.0, 4.0, -2.0);
    FbxNodeFixture camera_attribute;
    FbxNodeFixture camera_properties;
    FbxNodeFixture light_attribute;
    FbxNodeFixture light_properties;
    FbxNodeFixture point_attribute;
    FbxNodeFixture point_properties;
    FbxNodeFixture directional_attribute;
    FbxNodeFixture directional_properties;
    FbxNodeFixture area_attribute;
    FbxNodeFixture area_properties;
    FbxNodeFixture sphere_area_attribute;
    FbxNodeFixture sphere_area_properties;
    FbxNodeFixture volume_attribute;
    FbxNodeFixture volume_properties;
    FbxNodeFixture stack;
    FbxNodeFixture layer;
    FbxNodeFixture curve_node;

    camera_attribute.name = "NodeAttribute";
    camera_attribute.props = {
        fbx_prop_i64_fixture(kCameraAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("Camera01", "NodeAttribute")),
        fbx_prop_string_fixture("Camera")};
    camera_properties.name = "Properties70";
    camera_properties.children.push_back(make_fbx_property_int("CameraProjectionType", 0));
    camera_properties.children.push_back(make_fbx_property_scalar("FieldOfViewY", 40.0));
    camera_properties.children.push_back(make_fbx_property_scalar("AspectWidth", 16.0));
    camera_properties.children.push_back(make_fbx_property_scalar("AspectHeight", 9.0));
    camera_properties.children.push_back(make_fbx_property_scalar("NearPlane", 0.25));
    camera_properties.children.push_back(make_fbx_property_scalar("FarPlane", 500.0));
    camera_attribute.children.push_back(camera_properties);

    light_attribute.name = "NodeAttribute";
    light_attribute.props = {
        fbx_prop_i64_fixture(kLightAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("KeyLight", "NodeAttribute")),
        fbx_prop_string_fixture("Light")};
    light_properties.name = "Properties70";
    light_properties.children.push_back(make_fbx_property_vec3("Color", 0.25, 0.5, 0.75));
    light_properties.children.push_back(make_fbx_property_scalar("Intensity", 250.0));
    light_properties.children.push_back(make_fbx_property_int("LightType", 2));
    light_properties.children.push_back(make_fbx_property_scalar("InnerAngle", 20.0));
    light_properties.children.push_back(make_fbx_property_scalar("OuterAngle", 60.0));
    light_properties.children.push_back(make_fbx_property_scalar("FarAttenuationEnd", 10.0));
    light_properties.children.push_back(make_fbx_property_int("CastShadows", 1));
    light_attribute.children.push_back(light_properties);

    point_attribute.name = "NodeAttribute";
    point_attribute.props = {
        fbx_prop_i64_fixture(kPointAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("FillLight", "NodeAttribute")),
        fbx_prop_string_fixture("Light")};
    point_properties.name = "Properties70";
    point_properties.children.push_back(make_fbx_property_int("LightType", 0));
    point_attribute.children.push_back(point_properties);

    directional_attribute.name = "NodeAttribute";
    directional_attribute.props = {
        fbx_prop_i64_fixture(kDirectionalAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("SunLight", "NodeAttribute")),
        fbx_prop_string_fixture("Light")};
    directional_properties.name = "Properties70";
    directional_properties.children.push_back(make_fbx_property_int("LightType", 1));
    directional_attribute.children.push_back(directional_properties);

    area_attribute.name = "NodeAttribute";
    area_attribute.props = {
        fbx_prop_i64_fixture(kAreaAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("PanelLight", "NodeAttribute")),
        fbx_prop_string_fixture("Light")};
    area_properties.name = "Properties70";
    area_properties.children.push_back(make_fbx_property_int("LightType", 3));
    area_properties.children.push_back(make_fbx_property_int("AreaLightShape", 0));
    area_properties.children.push_back(make_fbx_property_scalar("Width", area_width));
    area_properties.children.push_back(make_fbx_property_scalar("Height", 2.0));
    area_properties.children.push_back(make_fbx_property_int("DecayType", 2));
    area_properties.children.push_back(make_fbx_property_scalar("DecayStart", 15.0));
    area_attribute.children.push_back(area_properties);

    sphere_area_attribute.name = "NodeAttribute";
    sphere_area_attribute.props = {
        fbx_prop_i64_fixture(kSphereAreaAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("OrbLight", "NodeAttribute")),
        fbx_prop_string_fixture("Light")};
    sphere_area_properties.name = "Properties70";
    sphere_area_properties.children.push_back(make_fbx_property_int("LightType", 3));
    sphere_area_properties.children.push_back(make_fbx_property_int("AreaLightShape", 1));
    sphere_area_properties.children.push_back(make_fbx_property_scalar("AreaLightRadius", 2.5));
    sphere_area_properties.children.push_back(make_fbx_property_int("DecayType", 1));
    sphere_area_properties.children.push_back(make_fbx_property_scalar("FarAttenuationEnd", 20.0));
    sphere_area_attribute.children.push_back(sphere_area_properties);

    volume_attribute.name = "NodeAttribute";
    volume_attribute.props = {
        fbx_prop_i64_fixture(kVolumeAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("FogLight", "NodeAttribute")),
        fbx_prop_string_fixture("Light")};
    volume_properties.name = "Properties70";
    volume_properties.children.push_back(make_fbx_property_int("LightType", 4));
    volume_properties.children.push_back(make_fbx_property_scalar("VolumeRadius", 6.0));
    volume_properties.children.push_back(make_fbx_property_int("DecayType", 0));
    volume_properties.children.push_back(make_fbx_property_scalar("DecayStart", 9.0));
    volume_properties.children.push_back(make_fbx_property_int("CastShadows", 1));
    volume_attribute.children.push_back(volume_properties);

    stack.name = "AnimationStack";
    stack.props = {fbx_prop_i64_fixture(kStack),
                   fbx_prop_string_fixture(make_fbx_object_name("MoveLight", "AnimStack")),
                   fbx_prop_string_fixture("")};
    layer.name = "AnimationLayer";
    layer.props = {fbx_prop_i64_fixture(kLayer),
                   fbx_prop_string_fixture(make_fbx_object_name("Base", "AnimLayer")),
                   fbx_prop_string_fixture("")};
    curve_node.name = "AnimationCurveNode";
    curve_node.props = {
        fbx_prop_i64_fixture(kCurveNode),
        fbx_prop_string_fixture(make_fbx_object_name("MoveLightX", "AnimCurveNode")),
        fbx_prop_string_fixture("")};

    objects.name = "Objects";
    objects.children = {camera_model,
                        target_model,
                        light_model,
                        point_model,
                        directional_model,
                        area_model,
                        sphere_area_model,
                        volume_model,
                        camera_attribute,
                        light_attribute,
                        point_attribute,
                        directional_attribute,
                        area_attribute,
                        sphere_area_attribute,
                        volume_attribute,
                        stack,
                        layer,
                        curve_node,
                        make_fbx_animation_curve_fixture(kCurve, kTimes, kValues, 2u)};
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kCameraModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kTargetModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kLightModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kPointModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kDirectionalModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kAreaModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kSphereAreaModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kVolumeModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kCameraAttribute, kCameraModel));
    connections.children.push_back(make_fbx_connection_fixture(kLightAttribute, kLightModel));
    connections.children.push_back(make_fbx_connection_fixture(kPointAttribute, kPointModel));
    connections.children.push_back(
        make_fbx_connection_fixture(kDirectionalAttribute, kDirectionalModel));
    connections.children.push_back(make_fbx_connection_fixture(kAreaAttribute, kAreaModel));
    connections.children.push_back(
        make_fbx_connection_fixture(kSphereAreaAttribute, kSphereAreaModel));
    connections.children.push_back(make_fbx_connection_fixture(kVolumeAttribute, kVolumeModel));
    connections.children.push_back(
        make_fbx_connection_fixture(kTargetModel, kCameraAttribute, "LookAtProperty"));
    connections.children.push_back(make_fbx_connection_fixture(kLayer, kStack));
    connections.children.push_back(make_fbx_connection_fixture(kCurveNode, kLayer));
    connections.children.push_back(
        make_fbx_connection_fixture(kCurveNode, kLightModel, "Lcl Translation"));
    connections.children.push_back(make_fbx_connection_fixture(kCurve, kCurveNode, "d|X"));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write fully animated FBX camera projection properties, including focal/aperture input.
static bool write_fbx_camera_projection_animation_fixture(const char *path,
                                                          bool invalid_projection = false) {
    constexpr int64_t kCameraModel = 2450;
    constexpr int64_t kCameraAttribute = 2451;
    constexpr int64_t kStack = 2452;
    constexpr int64_t kLayer = 2453;
    static const int64_t kTimes[2] = {0, 46186158000LL};
    static const int64_t kProjectionTimes[3] = {0, 23093079000LL, 46186158000LL};
    static const double kFocal[2] = {50.0, 25.0};
    static const double kFilmWidth[2] = {0.8, 1.0};
    static const double kAspectWidth[2] = {2.0, 4.0};
    static const double kAspectHeight[2] = {1.0, 1.0};
    static const double kNear[2] = {0.25, 0.5};
    static const double kFar[2] = {500.0, 1000.0};
    static const double kOrtho[2] = {10.0, 20.0};
    static const double kIgnoredFovY[2] = {40.0, 80.0};
    static const double kProjection[3] = {0.0, 1.0, 1.0};
    static const double kInvalidProjection[3] = {0.0, 1.0, 2.0};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture attribute;
    FbxNodeFixture properties70;
    int64_t next_id = 2460;
    objects.name = "Objects";
    connections.name = "Connections";
    objects.children.push_back(
        make_fbx_model_fixture(kCameraModel, "AnimatedCamera", "Camera", 0.0, 1.0, 5.0));
    attribute.name = "NodeAttribute";
    attribute.props = {
        fbx_prop_i64_fixture(kCameraAttribute),
        fbx_prop_string_fixture(make_fbx_object_name("AnimatedCamera", "NodeAttribute")),
        fbx_prop_string_fixture("Camera")};
    properties70.name = "Properties70";
    properties70.children = {make_fbx_property_int("CameraProjectionType", 0),
                             make_fbx_property_int("ApertureMode", 3),
                             make_fbx_property_scalar("FocalLength", 50.0),
                             make_fbx_property_scalar("FilmWidth", 0.8),
                             make_fbx_property_scalar("FilmHeight", 0.6),
                             make_fbx_property_scalar("AspectWidth", 2.0),
                             make_fbx_property_scalar("AspectHeight", 1.0),
                             make_fbx_property_scalar("NearPlane", 0.25),
                             make_fbx_property_scalar("FarPlane", 500.0),
                             make_fbx_property_scalar("CameraOrthoZoom", 10.0),
                             make_fbx_property_scalar("FieldOfViewY", 40.0)};
    attribute.children.push_back(std::move(properties70));
    objects.children.push_back(std::move(attribute));
    objects.children.push_back(FbxNodeFixture{
        "AnimationStack",
        {fbx_prop_i64_fixture(kStack),
         fbx_prop_string_fixture(make_fbx_object_name("CameraProjection", "AnimStack")),
         fbx_prop_string_fixture("")},
        {}});
    objects.children.push_back(
        FbxNodeFixture{"AnimationLayer",
                       {fbx_prop_i64_fixture(kLayer),
                        fbx_prop_string_fixture(make_fbx_object_name("Base", "AnimLayer")),
                        fbx_prop_string_fixture("")},
                       {}});
    connections.children.push_back(make_fbx_connection_fixture(kCameraModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kCameraAttribute, kCameraModel));
    connections.children.push_back(make_fbx_connection_fixture(kLayer, kStack));
    auto add_curve = [&](int64_t target_id,
                         const char *property,
                         const int64_t *times,
                         const double *values,
                         size_t count) {
        int64_t curve_node_id = next_id++;
        int64_t curve_id = next_id++;
        objects.children.push_back(FbxNodeFixture{
            "AnimationCurveNode",
            {fbx_prop_i64_fixture(curve_node_id),
             fbx_prop_string_fixture(make_fbx_object_name(property, "AnimCurveNode")),
             fbx_prop_string_fixture("")},
            {}});
        objects.children.push_back(
            make_fbx_animation_curve_fixture(curve_id, times, values, count));
        connections.children.push_back(make_fbx_connection_fixture(curve_node_id, kLayer));
        connections.children.push_back(
            make_fbx_connection_fixture(curve_node_id, target_id, property));
        connections.children.push_back(make_fbx_connection_fixture(curve_id, curve_node_id, "d|X"));
    };
    add_curve(kCameraAttribute, "FocalLength", kTimes, kFocal, 2u);
    add_curve(kCameraAttribute, "FilmWidth", kTimes, kFilmWidth, 2u);
    add_curve(kCameraAttribute, "AspectWidth", kTimes, kAspectWidth, 2u);
    add_curve(kCameraAttribute, "AspectHeight", kTimes, kAspectHeight, 2u);
    /* Model-targeted camera properties are accepted for exporter compatibility. */
    add_curve(kCameraModel, "NearPlane", kTimes, kNear, 2u);
    add_curve(kCameraAttribute, "FarPlane", kTimes, kFar, 2u);
    add_curve(kCameraAttribute, "CameraOrthoZoom", kTimes, kOrtho, 2u);
    add_curve(kCameraAttribute, "FieldOfViewY", kTimes, kIgnoredFovY, 2u);
    add_curve(kCameraAttribute,
              "CameraProjectionType",
              kProjectionTimes,
              invalid_projection ? kInvalidProjection : kProjection,
              3u);
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief ASCII counterpart of the typed camera/light/object-animation fixture.
static bool write_fbx_camera_light_animation_ascii_fixture(const char *path) {
    static const char text[] =
        "; FBX 7.4.0 camera/light/object animation fixture\n"
        "Objects: {\n"
        " Model: 2400, \"Model::Camera01\", \"Camera\" { Properties70: {\n"
        "  P: \"Lcl Translation\",\"Vector3D\",\"\",\"A\",2,3,4\n"
        "  P: \"Lcl Rotation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "  P: \"Lcl Scaling\",\"Vector3D\",\"\",\"A\",1,1,1 } }\n"
        " Model: 2401, \"Model::CameraTarget\", \"Marker\" { Properties70: {\n"
        "  P: \"Lcl Translation\",\"Vector3D\",\"\",\"A\",2,3,0\n"
        "  P: \"Lcl Rotation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "  P: \"Lcl Scaling\",\"Vector3D\",\"\",\"A\",1,1,1 } }\n"
        " Model: 2402, \"Model::KeyLight\", \"Light\" { Properties70: {\n"
        "  P: \"Lcl Translation\",\"Vector3D\",\"\",\"A\",1,2,3\n"
        "  P: \"Lcl Rotation\",\"Vector3D\",\"\",\"A\",0,0,0\n"
        "  P: \"Lcl Scaling\",\"Vector3D\",\"\",\"A\",1,1,1 } }\n"
        " NodeAttribute: 2410, \"NodeAttribute::Camera01\", \"Camera\" { Properties70: {\n"
        "  P: \"CameraProjectionType\",\"enum\",\"\",\"A\",1\n"
        "  P: \"OrthoZoom\",\"double\",\"\",\"A\",8\n"
        "  P: \"AspectWidth\",\"double\",\"\",\"A\",4\n"
        "  P: \"AspectHeight\",\"double\",\"\",\"A\",3\n"
        "  P: \"NearPlane\",\"double\",\"\",\"A\",0.5\n"
        "  P: \"FarPlane\",\"double\",\"\",\"A\",250 } }\n"
        " NodeAttribute: 2411, \"NodeAttribute::KeyLight\", \"Light\" { Properties70: {\n"
        "  P: \"Color\",\"Color\",\"\",\"A\",0.25,0.5,0.75\n"
        "  P: \"Intensity\",\"double\",\"\",\"A\",250\n"
        "  P: \"LightType\",\"enum\",\"\",\"A\",2\n"
        "  P: \"InnerAngle\",\"double\",\"\",\"A\",20\n"
        "  P: \"OuterAngle\",\"double\",\"\",\"A\",60 } }\n"
        " AnimationStack: 2420, \"AnimationStack::MoveLight\", \"\"\n"
        " AnimationLayer: 2421, \"AnimationLayer::Base\", \"\"\n"
        " AnimationCurveNode: 2422, \"AnimationCurveNode::MoveLightX\", \"\"\n"
        " AnimationCurve: 2423, \"AnimationCurve::X\", \"\" {\n"
        "  KeyTime: *2 { a: 0,46186158000 }\n"
        "  KeyValueFloat: *2 { a: 1,5 } }\n"
        "}\n"
        "Connections: {\n"
        " C: \"OO\",2400,0\n C: \"OO\",2401,0\n C: \"OO\",2402,0\n"
        " C: \"OO\",2410,2400\n C: \"OO\",2411,2402\n"
        " C: \"OP\",2401,2410,\"LookAtProperty\"\n"
        " C: \"OO\",2421,2420\n C: \"OO\",2422,2421\n"
        " C: \"OP\",2422,2402,\"Lcl Translation\"\n"
        " C: \"OP\",2423,2422,\"d|X\"\n"
        "}\n";
    return write_text_file(path, text);
}

/// @brief Construct one typed FBX Constraint object with a Properties70 payload.
static FbxNodeFixture make_fbx_constraint_fixture(int64_t id,
                                                  const char *name,
                                                  const char *type,
                                                  std::vector<FbxNodeFixture> properties) {
    FbxNodeFixture constraint;
    FbxNodeFixture type_node;
    FbxNodeFixture properties70;
    constraint.name = "Constraint";
    constraint.props.push_back(fbx_prop_i64_fixture(id));
    constraint.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name(name ? name : "Constraint", "Constraint")));
    constraint.props.push_back(fbx_prop_string_fixture(type ? type : "Custom"));
    type_node.name = "Type";
    type_node.props.push_back(fbx_prop_string_fixture(type ? type : "Custom"));
    constraint.children.push_back(type_node);
    properties70.name = "Properties70";
    properties70.children = std::move(properties);
    constraint.children.push_back(std::move(properties70));
    return constraint;
}

/// @brief Write a compact zoo covering every built-in FBX constraint and animated dependency.
static bool write_fbx_constraint_zoo_fixture(const char *path,
                                             bool unreachable_ik = false,
                                             double ik_weight = 100.0,
                                             bool z_up = false) {
    constexpr int64_t kPositionTarget = 3000;
    constexpr int64_t kPositionSourceA = 3001;
    constexpr int64_t kPositionSourceB = 3002;
    constexpr int64_t kPositionConstraint = 3100;
    constexpr int64_t kRotationTarget = 3010;
    constexpr int64_t kRotationSource = 3011;
    constexpr int64_t kRotationConstraint = 3110;
    constexpr int64_t kScaleTarget = 3020;
    constexpr int64_t kScaleSource = 3021;
    constexpr int64_t kScaleConstraint = 3120;
    constexpr int64_t kParentTarget = 3030;
    constexpr int64_t kParentSource = 3031;
    constexpr int64_t kParentConstraint = 3130;
    constexpr int64_t kAimTarget = 3040;
    constexpr int64_t kAimSource = 3041;
    constexpr int64_t kAimUp = 3042;
    constexpr int64_t kAimConstraint = 3140;
    constexpr int64_t kInactiveTarget = 3050;
    constexpr int64_t kInactiveSource = 3051;
    constexpr int64_t kInactiveConstraint = 3150;
    constexpr int64_t kIkFirst = 3060;
    constexpr int64_t kIkMiddle = 3061;
    constexpr int64_t kIkEnd = 3062;
    constexpr int64_t kIkEffector = 3063;
    constexpr int64_t kIkPole = 3064;
    constexpr int64_t kIkConstraint = 3160;
    constexpr int64_t kCustomConstraint = 3170;
    constexpr int64_t kStack = 3200;
    constexpr int64_t kLayer = 3201;
    constexpr int64_t kCurveNode = 3202;
    constexpr int64_t kCurve = 3203;
    static const int64_t kTimes[2] = {0, 46186158000LL};
    static const double kValues[2] = {2.0, 10.0};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    objects.name = "Objects";
    connections.name = "Connections";

    objects.children.push_back(
        make_fbx_model_fixture(kPositionTarget, "PositionTarget", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kPositionSourceA, "SourceA", "Null", 2.0, 4.0, 6.0));
    objects.children.push_back(
        make_fbx_model_fixture(kPositionSourceB, "SourceB", "Null", 6.0, 8.0, 10.0));
    objects.children.push_back(
        make_fbx_constraint_fixture(kPositionConstraint,
                                    "Point",
                                    "Position From Positions",
                                    {make_fbx_property_vec3("TranslationOffset", 1.0, 0.0, -1.0),
                                     make_fbx_property_scalar("Weight", 50.0),
                                     make_fbx_property_int("AffectY", 0),
                                     make_fbx_property_scalar("SourceA.Weight", 100.0),
                                     make_fbx_property_scalar("SourceB.Weight", 300.0)}));

    FbxNodeFixture rotation_target =
        make_fbx_model_fixture(kRotationTarget, "RotationTarget", "Null", 0.0, 0.0, 0.0);
    FbxNodeFixture rotation_source =
        make_fbx_model_fixture(kRotationSource, "RotationSource", "Null", 0.0, 0.0, 0.0);
    rotation_source.children[0].children[1] =
        make_fbx_property_vec3("Lcl Rotation", 0.0, 0.0, 90.0);
    objects.children.push_back(std::move(rotation_target));
    objects.children.push_back(std::move(rotation_source));
    objects.children.push_back(
        make_fbx_constraint_fixture(kRotationConstraint,
                                    "Orient",
                                    "Rotation From Rotations",
                                    {make_fbx_property_vec3("RotationOffset", 0.0, 0.0, 30.0),
                                     make_fbx_property_scalar("RotationSource.Weight", 100.0)}));

    FbxNodeFixture scale_source =
        make_fbx_model_fixture(kScaleSource, "ScaleSource", "Null", 0.0, 0.0, 0.0);
    scale_source.children[0].children[2] = make_fbx_property_vec3("Lcl Scaling", 2.0, 3.0, 4.0);
    objects.children.push_back(
        make_fbx_model_fixture(kScaleTarget, "ScaleTarget", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(std::move(scale_source));
    objects.children.push_back(
        make_fbx_constraint_fixture(kScaleConstraint,
                                    "Scale",
                                    "Scale From Scales",
                                    {make_fbx_property_vec3("ScalingOffset", 0.5, 2.0, 1.0),
                                     make_fbx_property_scalar("Weight", 50.0),
                                     make_fbx_property_scalar("ScaleSource.Weight", 100.0)}));

    FbxNodeFixture parent_source =
        make_fbx_model_fixture(kParentSource, "ParentSource", "Null", 10.0, 0.0, 0.0);
    parent_source.children[0].children[1] = make_fbx_property_vec3("Lcl Rotation", 0.0, 0.0, 60.0);
    objects.children.push_back(
        make_fbx_model_fixture(kParentTarget, "ParentTarget", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(std::move(parent_source));
    objects.children.push_back(make_fbx_constraint_fixture(
        kParentConstraint,
        "Parent",
        "Parent-Child",
        {make_fbx_property_scalar("ParentSource.Weight", 100.0),
         make_fbx_property_vec3("ParentSource.Offset T", 2.0, 0.0, 0.0),
         make_fbx_property_vec3("ParentSource.Offset R", 0.0, 0.0, 30.0),
         make_fbx_property_vec3("ParentSource.Offset S", 1.0, 1.0, 1.0),
         make_fbx_property_int("AffectScaleX", 1),
         make_fbx_property_int("AffectScaleY", 1),
         make_fbx_property_int("AffectScaleZ", 1)}));

    objects.children.push_back(
        make_fbx_model_fixture(kAimTarget, "AimTarget", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kAimSource, "AimSource", "Null", 0.0, 0.0, -5.0));
    objects.children.push_back(make_fbx_model_fixture(kAimUp, "AimUp", "Null", 0.0, 5.0, 0.0));
    objects.children.push_back(
        make_fbx_constraint_fixture(kAimConstraint,
                                    "Aim",
                                    "Aim",
                                    {make_fbx_property_int("WorldUpType", 1),
                                     make_fbx_property_vec3("AimVector", 1.0, 0.0, 0.0),
                                     make_fbx_property_vec3("UpVector", 0.0, 1.0, 0.0),
                                     make_fbx_property_scalar("AimSource.Weight", 100.0)}));

    objects.children.push_back(
        make_fbx_model_fixture(kInactiveTarget, "InactiveTarget", "Null", 1.0, 2.0, 3.0));
    objects.children.push_back(
        make_fbx_model_fixture(kInactiveSource, "InactiveSource", "Null", 9.0, 9.0, 9.0));
    objects.children.push_back(
        make_fbx_constraint_fixture(kInactiveConstraint,
                                    "Inactive",
                                    "Position From Positions",
                                    {make_fbx_property_int("Active", 0),
                                     make_fbx_property_int("Lock", 1),
                                     make_fbx_property_scalar("InactiveSource.Weight", 100.0)}));

    objects.children.push_back(make_fbx_model_fixture(kIkFirst, "IkFirst", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kIkMiddle, "IkMiddle", "Null", 1.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_model_fixture(kIkEnd, "IkEnd", "Null", 1.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_model_fixture(kIkEffector,
                                                      "IkEffector",
                                                      "Null",
                                                      unreachable_ik ? 5.0 : 1.0,
                                                      unreachable_ik ? 0.0 : 1.0,
                                                      0.0));
    objects.children.push_back(make_fbx_model_fixture(kIkPole, "IkPole", "Null", 0.0, 0.0, 2.0));
    objects.children.push_back(
        make_fbx_constraint_fixture(kIkConstraint,
                                    "IK",
                                    "Single Chain IK",
                                    {make_fbx_property_int("PoleVectorType", 1),
                                     make_fbx_property_vec3("PoleVector", 0.0, 0.0, 2.0),
                                     make_fbx_property_scalar("Twist", 30.0),
                                     make_fbx_property_scalar("Weight", ik_weight),
                                     make_fbx_property_scalar("IkPole.Weight", 1.0)}));
    objects.children.push_back(
        make_fbx_constraint_fixture(kCustomConstraint, "VendorTwist", "Vendor Twist", {}));

    objects.children.push_back(FbxNodeFixture{
        "AnimationStack",
        {fbx_prop_i64_fixture(kStack),
         fbx_prop_string_fixture(make_fbx_object_name("ConstraintMotion", "AnimStack")),
         fbx_prop_string_fixture("")},
        {}});
    objects.children.push_back(
        FbxNodeFixture{"AnimationLayer",
                       {fbx_prop_i64_fixture(kLayer),
                        fbx_prop_string_fixture(make_fbx_object_name("Base", "AnimLayer")),
                        fbx_prop_string_fixture("")},
                       {}});
    objects.children.push_back(
        FbxNodeFixture{"AnimationCurveNode",
                       {fbx_prop_i64_fixture(kCurveNode),
                        fbx_prop_string_fixture(make_fbx_object_name("SourceX", "AnimCurveNode")),
                        fbx_prop_string_fixture("")},
                       {}});
    objects.children.push_back(make_fbx_animation_curve_fixture(kCurve, kTimes, kValues, 2u));

    const int64_t roots[] = {kPositionTarget,
                             kPositionSourceA,
                             kPositionSourceB,
                             kRotationTarget,
                             kRotationSource,
                             kScaleTarget,
                             kScaleSource,
                             kParentTarget,
                             kParentSource,
                             kAimTarget,
                             kAimSource,
                             kAimUp,
                             kInactiveTarget,
                             kInactiveSource,
                             kIkFirst,
                             kIkEffector,
                             kIkPole};
    for (int64_t id : roots)
        connections.children.push_back(make_fbx_connection_fixture(id, 0));
    connections.children.push_back(make_fbx_connection_fixture(kIkMiddle, kIkFirst));
    connections.children.push_back(make_fbx_connection_fixture(kIkEnd, kIkMiddle));

    connections.children.push_back(
        make_fbx_connection_fixture(kPositionTarget, kPositionConstraint, "Constrained Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kPositionSourceA, kPositionConstraint, "Source"));
    connections.children.push_back(
        make_fbx_connection_fixture(kPositionSourceB, kPositionConstraint, "Source"));
    connections.children.push_back(
        make_fbx_connection_fixture(kRotationTarget, kRotationConstraint, "Constrained Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kRotationSource, kRotationConstraint, "Source"));
    connections.children.push_back(
        make_fbx_connection_fixture(kScaleTarget, kScaleConstraint, "Constrained Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kScaleSource, kScaleConstraint, "Source"));
    connections.children.push_back(make_fbx_connection_fixture(
        kParentTarget, kParentConstraint, "Constrained object (Child)"));
    connections.children.push_back(
        make_fbx_connection_fixture(kParentSource, kParentConstraint, "Source (Parent)"));
    connections.children.push_back(
        make_fbx_connection_fixture(kAimTarget, kAimConstraint, "Constrained Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kAimSource, kAimConstraint, "Aim At Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kAimUp, kAimConstraint, "World Up Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kInactiveTarget, kInactiveConstraint, "Constrained Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kInactiveSource, kInactiveConstraint, "Source"));
    connections.children.push_back(
        make_fbx_connection_fixture(kIkFirst, kIkConstraint, "First Joint"));
    connections.children.push_back(make_fbx_connection_fixture(kIkEnd, kIkConstraint, "End Joint"));
    connections.children.push_back(
        make_fbx_connection_fixture(kIkEffector, kIkConstraint, "Effector"));
    connections.children.push_back(
        make_fbx_connection_fixture(kIkPole, kIkConstraint, "Pole Vector Object"));
    connections.children.push_back(
        make_fbx_connection_fixture(kAimTarget, kCustomConstraint, "Constrained Object"));
    connections.children.push_back(make_fbx_connection_fixture(kLayer, kStack));
    connections.children.push_back(make_fbx_connection_fixture(kCurveNode, kLayer));
    connections.children.push_back(
        make_fbx_connection_fixture(kCurveNode, kPositionSourceA, "Lcl Translation"));
    connections.children.push_back(make_fbx_connection_fixture(kCurve, kCurveNode, "d|X"));
    return write_fbx_document_fixture(path, objects, connections, z_up);
}

enum class FbxBadConstraintFixture {
    MissingTarget,
    Cycle,
    WeightMismatch,
    NonFiniteOffset,
    MismatchedIkChain,
    DescendantSource,
};

/// @brief Write one malformed active constraint graph for transactional-rejection tests.
static bool write_fbx_bad_constraint_fixture(const char *path, FbxBadConstraintFixture kind) {
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    objects.name = "Objects";
    connections.name = "Connections";
    objects.children.push_back(make_fbx_model_fixture(3300, "A", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_model_fixture(3301, "B", "Null", 1.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_model_fixture(3302, "C", "Null", 2.0, 0.0, 0.0));
    connections.children.push_back(make_fbx_connection_fixture(3300, 0));
    connections.children.push_back(make_fbx_connection_fixture(
        3301, kind == FbxBadConstraintFixture::DescendantSource ? 3300 : 0));
    connections.children.push_back(make_fbx_connection_fixture(3302, 0));
    if (kind == FbxBadConstraintFixture::MismatchedIkChain) {
        objects.children.push_back(make_fbx_constraint_fixture(
            3400, "BrokenIK", "Single Chain IK", {make_fbx_property_scalar("C.Weight", 1.0)}));
        connections.children.push_back(make_fbx_connection_fixture(3300, 3400, "First Joint"));
        connections.children.push_back(make_fbx_connection_fixture(3301, 3400, "End Joint"));
        connections.children.push_back(make_fbx_connection_fixture(3302, 3400, "Effector"));
    } else {
        std::vector<FbxNodeFixture> properties;
        if (kind == FbxBadConstraintFixture::WeightMismatch)
            properties.push_back(make_fbx_property_scalar("B.Weight", 100.0));
        else if (kind == FbxBadConstraintFixture::NonFiniteOffset)
            properties.push_back(make_fbx_property_vec3("Translation", NAN, 0.0, 0.0));
        objects.children.push_back(make_fbx_constraint_fixture(
            3400, "BrokenPoint", "Position From Positions", std::move(properties)));
        if (kind != FbxBadConstraintFixture::MissingTarget)
            connections.children.push_back(
                make_fbx_connection_fixture(3300, 3400, "Constrained Object"));
        connections.children.push_back(make_fbx_connection_fixture(3301, 3400, "Source"));
        if (kind == FbxBadConstraintFixture::WeightMismatch)
            connections.children.push_back(make_fbx_connection_fixture(3302, 3400, "Source"));
        if (kind == FbxBadConstraintFixture::Cycle) {
            objects.children.push_back(
                make_fbx_constraint_fixture(3401, "CyclePoint", "Position From Positions", {}));
            connections.children.push_back(
                make_fbx_connection_fixture(3301, 3401, "Constrained Object"));
            connections.children.push_back(make_fbx_connection_fixture(3300, 3401, "Source"));
        }
    }
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write constraints whose consumer reads through a producer-driven ancestor.
static bool write_fbx_constraint_ancestor_order_fixture(const char *path) {
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    objects.name = "Objects";
    connections.name = "Connections";
    objects.children.push_back(make_fbx_model_fixture(3450, "Ancestor", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_model_fixture(3451, "NestedSource", "Null", 2.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_model_fixture(3452, "Target", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(make_fbx_model_fixture(3453, "Driver", "Null", 10.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_constraint_fixture(3460, "ConsumerFirst", "Position From Positions", {}));
    objects.children.push_back(
        make_fbx_constraint_fixture(3461, "AncestorProducer", "Position From Positions", {}));
    connections.children.push_back(make_fbx_connection_fixture(3450, 0));
    connections.children.push_back(make_fbx_connection_fixture(3451, 3450));
    connections.children.push_back(make_fbx_connection_fixture(3452, 0));
    connections.children.push_back(make_fbx_connection_fixture(3453, 0));
    connections.children.push_back(make_fbx_connection_fixture(3452, 3460, "Constrained Object"));
    connections.children.push_back(make_fbx_connection_fixture(3451, 3460, "Source"));
    connections.children.push_back(make_fbx_connection_fixture(3450, 3461, "Constrained Object"));
    connections.children.push_back(make_fbx_connection_fixture(3453, 3461, "Source"));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a bone constrained by an animated non-skeleton driver.
static bool write_fbx_animated_skeleton_constraint_fixture(const char *path) {
    constexpr int64_t kBone = 3500;
    constexpr int64_t kDriver = 3501;
    constexpr int64_t kConstraint = 3510;
    constexpr int64_t kStack = 3520;
    constexpr int64_t kLayer = 3521;
    constexpr int64_t kCurveNode = 3522;
    constexpr int64_t kCurve = 3523;
    static const int64_t kTimes[2] = {0, 46186158000LL};
    static const double kValues[2] = {2.0, 10.0};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    objects.name = "Objects";
    connections.name = "Connections";
    objects.children.push_back(make_fbx_model_fixture(kBone, "DrivenBone", "LimbNode", 0, 0, 0));
    objects.children.push_back(make_fbx_model_fixture(kDriver, "BoneDriver", "Null", 2, 0, 0));
    objects.children.push_back(
        make_fbx_constraint_fixture(kConstraint,
                                    "DriveBone",
                                    "Position From Positions",
                                    {make_fbx_property_vec3("Translation", 1.0, 0.0, 0.0),
                                     make_fbx_property_scalar("BoneDriver.Weight", 100.0)}));
    objects.children.push_back(FbxNodeFixture{
        "AnimationStack",
        {fbx_prop_i64_fixture(kStack),
         fbx_prop_string_fixture(make_fbx_object_name("DrivenBoneClip", "AnimStack")),
         fbx_prop_string_fixture("")},
        {}});
    objects.children.push_back(
        FbxNodeFixture{"AnimationLayer",
                       {fbx_prop_i64_fixture(kLayer),
                        fbx_prop_string_fixture(make_fbx_object_name("Base", "AnimLayer")),
                        fbx_prop_string_fixture("")},
                       {}});
    objects.children.push_back(
        FbxNodeFixture{"AnimationCurveNode",
                       {fbx_prop_i64_fixture(kCurveNode),
                        fbx_prop_string_fixture(make_fbx_object_name("DriverX", "AnimCurveNode")),
                        fbx_prop_string_fixture("")},
                       {}});
    objects.children.push_back(make_fbx_animation_curve_fixture(kCurve, kTimes, kValues, 2u));
    connections.children.push_back(make_fbx_connection_fixture(kBone, 0));
    connections.children.push_back(make_fbx_connection_fixture(kDriver, 0));
    connections.children.push_back(
        make_fbx_connection_fixture(kBone, kConstraint, "Constrained Object"));
    connections.children.push_back(make_fbx_connection_fixture(kDriver, kConstraint, "Source"));
    connections.children.push_back(make_fbx_connection_fixture(kLayer, kStack));
    connections.children.push_back(make_fbx_connection_fixture(kCurveNode, kLayer));
    connections.children.push_back(
        make_fbx_connection_fixture(kCurveNode, kDriver, "Lcl Translation"));
    connections.children.push_back(make_fbx_connection_fixture(kCurve, kCurveNode, "d|X"));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write layered transform animation shared by one scene node and one skeleton bone.
static bool write_fbx_animation_layer_composition_fixture(const char *path) {
    constexpr int64_t kNode = 3600;
    constexpr int64_t kBone = 3601;
    constexpr int64_t kStack = 3610;
    constexpr int64_t kBaseLayer = 3620;
    constexpr int64_t kAddLayer = 3621;
    constexpr int64_t kScaleAddLayer = 3622;
    constexpr int64_t kOverrideLayer = 3623;
    constexpr int64_t kPassthroughLayer = 3624;
    constexpr int64_t kMutedLayer = 3625;
    static const int64_t kTimes[2] = {-46186158000LL, 46186158000LL};
    static const int64_t kWeightTimes[3] = {-46186158000LL, 0, 46186158000LL};
    static const double kBaseTranslation[2] = {1.0, 2.0};
    static const double kBaseRotation[2] = {10.0, 20.0};
    static const double kBaseScale[2] = {2.0, 2.0};
    static const double kAddTranslation[2] = {4.0, 4.0};
    static const double kAddRotation[2] = {40.0, 40.0};
    static const double kMultiplyScale[2] = {4.0, 4.0};
    static const double kAdditiveScale[2] = {3.0, 3.0};
    static const double kOverrideTranslation[2] = {8.0, 8.0};
    static const double kPassthroughTranslation[2] = {10.0, 10.0};
    static const double kPassthroughRotation[2] = {100.0, 100.0};
    static const double kPassthroughScale[2] = {8.0, 8.0};
    static const double kMutedTranslation[2] = {100.0, 100.0};
    static const double kWeights[3] = {0.0, 50.0, 100.0};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    int64_t next_id = 3700;
    objects.name = "Objects";
    connections.name = "Connections";
    objects.children.push_back(make_fbx_model_fixture(kNode, "LayerNode", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kBone, "LayerBone", "LimbNode", 0.0, 0.0, 0.0));
    objects.children.push_back(
        FbxNodeFixture{"AnimationStack",
                       {fbx_prop_i64_fixture(kStack),
                        fbx_prop_string_fixture(make_fbx_object_name("Layered", "AnimStack")),
                        fbx_prop_string_fixture("")},
                       {}});
    auto add_layer = [&](int64_t id,
                         const char *name,
                         int blend_mode,
                         double weight,
                         int rotation_mode,
                         int scale_mode,
                         bool mute) {
        FbxNodeFixture layer;
        FbxNodeFixture properties70;
        layer.name = "AnimationLayer";
        layer.props = {fbx_prop_i64_fixture(id),
                       fbx_prop_string_fixture(make_fbx_object_name(name, "AnimLayer")),
                       fbx_prop_string_fixture("")};
        properties70.name = "Properties70";
        properties70.children = {make_fbx_property_scalar("Weight", weight),
                                 make_fbx_property_int("Mute", mute ? 1 : 0),
                                 make_fbx_property_int("BlendMode", blend_mode),
                                 make_fbx_property_int("RotationAccumulationMode", rotation_mode),
                                 make_fbx_property_int("ScaleAccumulationMode", scale_mode)};
        layer.children.push_back(std::move(properties70));
        objects.children.push_back(std::move(layer));
        connections.children.push_back(make_fbx_connection_fixture(id, kStack));
    };
    auto add_curve = [&](int64_t layer_id,
                         int64_t target_id,
                         const char *property,
                         const char *component,
                         const int64_t *times,
                         const double *values,
                         size_t count) {
        const int64_t curve_node_id = next_id++;
        const int64_t curve_id = next_id++;
        objects.children.push_back(FbxNodeFixture{
            "AnimationCurveNode",
            {fbx_prop_i64_fixture(curve_node_id),
             fbx_prop_string_fixture(make_fbx_object_name("LayerCurve", "AnimCurveNode")),
             fbx_prop_string_fixture("")},
            {}});
        objects.children.push_back(
            make_fbx_animation_curve_fixture(curve_id, times, values, count));
        connections.children.push_back(make_fbx_connection_fixture(curve_node_id, layer_id));
        connections.children.push_back(
            make_fbx_connection_fixture(curve_node_id, target_id, property));
        connections.children.push_back(
            make_fbx_connection_fixture(curve_id, curve_node_id, component));
    };
    add_layer(kBaseLayer, "Base", 0, 100.0, 0, 0, false);
    add_layer(kAddLayer, "Add", 0, 50.0, 1, 0, false);
    add_layer(kScaleAddLayer, "ScaleAdd", 0, 50.0, 0, 1, false);
    add_layer(kOverrideLayer, "Override", 1, 50.0, 0, 0, false);
    add_layer(kPassthroughLayer, "Passthrough", 2, 100.0, 0, 0, false);
    add_layer(kMutedLayer, "Muted", 0, 100.0, 0, 0, true);
    for (int64_t target : {kNode, kBone}) {
        add_curve(kBaseLayer, target, "Lcl Translation", "d|X", kTimes, kBaseTranslation, 2u);
        add_curve(kBaseLayer, target, "Lcl Rotation", "d|Z", kTimes, kBaseRotation, 2u);
        add_curve(kBaseLayer, target, "Lcl Scaling", "d|X", kTimes, kBaseScale, 2u);
        add_curve(kAddLayer, target, "Lcl Translation", "d|X", kTimes, kAddTranslation, 2u);
        add_curve(kAddLayer, target, "Lcl Rotation", "d|Z", kTimes, kAddRotation, 2u);
        add_curve(kAddLayer, target, "Lcl Scaling", "d|X", kTimes, kMultiplyScale, 2u);
        add_curve(kScaleAddLayer, target, "Lcl Scaling", "d|Y", kTimes, kAdditiveScale, 2u);
        add_curve(
            kOverrideLayer, target, "Lcl Translation", "d|X", kTimes, kOverrideTranslation, 2u);
        add_curve(kPassthroughLayer,
                  target,
                  "Lcl Translation",
                  "d|X",
                  kTimes,
                  kPassthroughTranslation,
                  2u);
        add_curve(
            kPassthroughLayer, target, "Lcl Rotation", "d|Z", kTimes, kPassthroughRotation, 2u);
        add_curve(kPassthroughLayer, target, "Lcl Scaling", "d|X", kTimes, kPassthroughScale, 2u);
        add_curve(kMutedLayer, target, "Lcl Translation", "d|X", kTimes, kMutedTranslation, 2u);
    }
    add_curve(kPassthroughLayer, kPassthroughLayer, "Weight", "d|X", kWeightTimes, kWeights, 3u);
    connections.children.push_back(make_fbx_connection_fixture(kNode, 0));
    connections.children.push_back(make_fbx_connection_fixture(kBone, 0));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a solo layer with a simultaneously muted solo sibling.
static bool write_fbx_animation_layer_solo_fixture(const char *path, bool invalid_weight = false) {
    static const int64_t kTimes[2] = {0, 46186158000LL};
    static const double kBaseValues[2] = {5.0, 5.0};
    static const double kSoloValues[2] = {9.0, 9.0};
    static const double kMutedValues[2] = {100.0, 100.0};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    int64_t next_id = 3920;
    objects.name = "Objects";
    connections.name = "Connections";
    objects.children.push_back(make_fbx_model_fixture(3900, "SoloNode", "Null", 0.0, 0.0, 0.0));
    objects.children.push_back(
        FbxNodeFixture{"AnimationStack",
                       {fbx_prop_i64_fixture(3901),
                        fbx_prop_string_fixture(make_fbx_object_name("Solo", "AnimStack")),
                        fbx_prop_string_fixture("")},
                       {}});
    auto add_layer_curve = [&](int64_t layer_id,
                               const char *name,
                               bool solo,
                               bool mute,
                               const double *values) {
        FbxNodeFixture layer;
        FbxNodeFixture properties70;
        const int64_t curve_node_id = next_id++;
        const int64_t curve_id = next_id++;
        layer.name = "AnimationLayer";
        layer.props = {fbx_prop_i64_fixture(layer_id),
                       fbx_prop_string_fixture(make_fbx_object_name(name, "AnimLayer")),
                       fbx_prop_string_fixture("")};
        properties70.name = "Properties70";
        properties70.children = {make_fbx_property_int("Solo", solo ? 1 : 0),
                                 make_fbx_property_int("Mute", mute ? 1 : 0)};
        if (invalid_weight && layer_id == 3910)
            properties70.children.push_back(make_fbx_property_scalar("Weight", NAN));
        layer.children.push_back(std::move(properties70));
        objects.children.push_back(std::move(layer));
        objects.children.push_back(FbxNodeFixture{
            "AnimationCurveNode",
            {fbx_prop_i64_fixture(curve_node_id),
             fbx_prop_string_fixture(make_fbx_object_name("SoloCurve", "AnimCurveNode")),
             fbx_prop_string_fixture("")},
            {}});
        objects.children.push_back(make_fbx_animation_curve_fixture(curve_id, kTimes, values, 2u));
        connections.children.push_back(make_fbx_connection_fixture(layer_id, 3901));
        connections.children.push_back(make_fbx_connection_fixture(curve_node_id, layer_id));
        connections.children.push_back(
            make_fbx_connection_fixture(curve_node_id, 3900, "Lcl Translation"));
        connections.children.push_back(make_fbx_connection_fixture(curve_id, curve_node_id, "d|X"));
    };
    add_layer_curve(3910, "Base", false, false, kBaseValues);
    add_layer_curve(3911, "Solo", true, false, kSoloValues);
    add_layer_curve(3912, "MutedSolo", true, true, kMutedValues);
    connections.children.push_back(make_fbx_connection_fixture(3900, 0));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a progressive, animated two-shape FBX morph on a split-material mesh.
static bool write_fbx_progressive_morph_fixture(const char *path, bool layered = false) {
    static const double kPositions[12] = {
        0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    static const int32_t kPolygons[6] = {0, 1, -3, 0, 2, -4};
    static const int32_t kMaterialSlots[2] = {0, 1};
    static const int32_t kShapeIndex0[1] = {0};
    static const int64_t kShapeIndex1[1] = {1};
    static const float kShape0Positions[3] = {1.0f, 0.0f, 0.0f};
    static const float kShape0Normals[3] = {0.0f, 1.0f, 0.0f};
    static const float kShape0Tangents[3] = {0.0f, 0.0f, 1.0f};
    static const double kShape1Positions[3] = {2.0, 0.0, 0.0};
    static const float kFullWeights[2] = {50.0f, 100.0f};
    static const int64_t kTimes[3] = {0, 46186158000LL, 2LL * 46186158000LL};
    static const double kPercentValues[3] = {25.0, 75.0, 125.0};
    constexpr int64_t kGeometry = 2500;
    constexpr int64_t kMaterial0 = 2501;
    constexpr int64_t kMaterial1 = 2502;
    constexpr int64_t kModel = 2503;
    constexpr int64_t kBlendShape = 2510;
    constexpr int64_t kChannel = 2511;
    constexpr int64_t kShape0 = 2512;
    constexpr int64_t kShape1 = 2513;
    constexpr int64_t kStack = 2520;
    constexpr int64_t kLayer = 2521;
    constexpr int64_t kCurveNode = 2522;
    constexpr int64_t kCurve = 2523;
    constexpr int64_t kAddLayer = 2524;
    constexpr int64_t kAddCurveNode = 2525;
    constexpr int64_t kAddCurve = 2526;
    static const double kAddPercentValues[3] = {25.0, 25.0, 25.0};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture geometry;
    FbxNodeFixture material_layer;
    FbxNodeFixture blend_shape;
    FbxNodeFixture channel;
    FbxNodeFixture channel_properties;
    FbxNodeFixture shape0;
    FbxNodeFixture shape1;
    FbxNodeFixture stack;
    FbxNodeFixture layer;
    FbxNodeFixture curve_node;

    geometry.name = "Geometry";
    geometry.props = {fbx_prop_i64_fixture(kGeometry),
                      fbx_prop_string_fixture(make_fbx_object_name("ProgressiveMesh", "Geometry")),
                      fbx_prop_string_fixture("Mesh")};
    geometry.children.push_back(
        FbxNodeFixture{"Vertices", {fbx_prop_array_fixture('d', kPositions, 12u)}, {}});
    geometry.children.push_back(
        FbxNodeFixture{"PolygonVertexIndex", {fbx_prop_array_fixture('i', kPolygons, 6u)}, {}});
    material_layer.name = "LayerElementMaterial";
    material_layer.children.push_back(
        FbxNodeFixture{"MappingInformationType", {fbx_prop_string_fixture("ByPolygon")}, {}});
    material_layer.children.push_back(
        FbxNodeFixture{"ReferenceInformationType", {fbx_prop_string_fixture("IndexToDirect")}, {}});
    material_layer.children.push_back(
        FbxNodeFixture{"Materials", {fbx_prop_array_fixture('i', kMaterialSlots, 2u)}, {}});
    geometry.children.push_back(material_layer);

    blend_shape.name = "Deformer";
    blend_shape.props = {fbx_prop_i64_fixture(kBlendShape),
                         fbx_prop_string_fixture(make_fbx_object_name("Face", "Deformer")),
                         fbx_prop_string_fixture("BlendShape")};
    channel.name = "Deformer";
    channel.props = {fbx_prop_i64_fixture(kChannel),
                     fbx_prop_string_fixture(make_fbx_object_name("Expression", "SubDeformer")),
                     fbx_prop_string_fixture("BlendShapeChannel")};
    channel.children.push_back(
        FbxNodeFixture{"FullWeights", {fbx_prop_array_fixture('f', kFullWeights, 2u)}, {}});
    channel_properties.name = "Properties70";
    channel_properties.children.push_back(make_fbx_property_scalar("DeformPercent", 25.0));
    channel.children.push_back(channel_properties);

    shape0.name = "Geometry";
    shape0.props = {fbx_prop_i64_fixture(kShape0),
                    fbx_prop_string_fixture(make_fbx_object_name("Smile", "Geometry")),
                    fbx_prop_string_fixture("Shape")};
    shape0.children.push_back(
        FbxNodeFixture{"Indexes", {fbx_prop_array_fixture('i', kShapeIndex0, 1u)}, {}});
    shape0.children.push_back(
        FbxNodeFixture{"Vertices", {fbx_prop_array_fixture('f', kShape0Positions, 3u)}, {}});
    shape0.children.push_back(
        FbxNodeFixture{"Normals", {fbx_prop_array_fixture('f', kShape0Normals, 3u)}, {}});
    shape0.children.push_back(
        FbxNodeFixture{"Tangents", {fbx_prop_array_fixture('f', kShape0Tangents, 3u)}, {}});
    shape1.name = "Geometry";
    shape1.props = {fbx_prop_i64_fixture(kShape1),
                    fbx_prop_string_fixture(make_fbx_object_name("WideSmile", "Geometry")),
                    fbx_prop_string_fixture("Shape")};
    shape1.children.push_back(
        FbxNodeFixture{"Indexes", {fbx_prop_array_fixture('l', kShapeIndex1, 1u)}, {}});
    shape1.children.push_back(
        FbxNodeFixture{"Vertices", {fbx_prop_array_fixture('d', kShape1Positions, 3u)}, {}});

    stack.name = "AnimationStack";
    stack.props = {fbx_prop_i64_fixture(kStack),
                   fbx_prop_string_fixture(make_fbx_object_name("Expressions", "AnimStack")),
                   fbx_prop_string_fixture("")};
    layer.name = "AnimationLayer";
    layer.props = {fbx_prop_i64_fixture(kLayer),
                   fbx_prop_string_fixture(make_fbx_object_name("Base", "AnimLayer")),
                   fbx_prop_string_fixture("")};
    curve_node.name = "AnimationCurveNode";
    curve_node.props = {
        fbx_prop_i64_fixture(kCurveNode),
        fbx_prop_string_fixture(make_fbx_object_name("ExpressionWeight", "AnimCurveNode")),
        fbx_prop_string_fixture("")};

    objects.name = "Objects";
    objects.children = {geometry,
                        make_fbx_material_fixture(kMaterial0, "Red", 1.0, 0.0, 0.0),
                        make_fbx_material_fixture(kMaterial1, "Blue", 0.0, 0.0, 1.0),
                        make_fbx_model_fixture(kModel, "MorphModel", "Mesh", 0.0, 0.0, 0.0),
                        blend_shape,
                        channel,
                        shape0,
                        shape1,
                        stack,
                        layer,
                        curve_node,
                        make_fbx_animation_curve_fixture(kCurve, kTimes, kPercentValues, 3u)};
    if (layered) {
        FbxNodeFixture add_layer;
        FbxNodeFixture add_layer_properties;
        add_layer.name = "AnimationLayer";
        add_layer.props = {fbx_prop_i64_fixture(kAddLayer),
                           fbx_prop_string_fixture(make_fbx_object_name("Additive", "AnimLayer")),
                           fbx_prop_string_fixture("")};
        add_layer_properties.name = "Properties70";
        add_layer_properties.children = {make_fbx_property_scalar("Weight", 50.0),
                                         make_fbx_property_int("BlendMode", 0)};
        add_layer.children.push_back(std::move(add_layer_properties));
        objects.children.push_back(std::move(add_layer));
        objects.children.push_back(FbxNodeFixture{
            "AnimationCurveNode",
            {fbx_prop_i64_fixture(kAddCurveNode),
             fbx_prop_string_fixture(make_fbx_object_name("AddWeight", "AnimCurveNode")),
             fbx_prop_string_fixture("")},
            {}});
        objects.children.push_back(
            make_fbx_animation_curve_fixture(kAddCurve, kTimes, kAddPercentValues, 3u));
    }
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModel, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometry, kModel));
    connections.children.push_back(make_fbx_connection_fixture(kMaterial0, kModel));
    connections.children.push_back(make_fbx_connection_fixture(kMaterial1, kModel));
    connections.children.push_back(make_fbx_connection_fixture(kBlendShape, kGeometry));
    connections.children.push_back(make_fbx_connection_fixture(kChannel, kBlendShape));
    connections.children.push_back(make_fbx_connection_fixture(kShape0, kChannel));
    connections.children.push_back(make_fbx_connection_fixture(kShape1, kChannel));
    connections.children.push_back(make_fbx_connection_fixture(kLayer, kStack));
    connections.children.push_back(make_fbx_connection_fixture(kCurveNode, kLayer));
    connections.children.push_back(
        make_fbx_connection_fixture(kCurveNode, kChannel, "DeformPercent"));
    connections.children.push_back(
        make_fbx_connection_fixture(kCurve, kCurveNode, "d|DeformPercent"));
    if (layered) {
        connections.children.push_back(make_fbx_connection_fixture(kAddLayer, kStack));
        connections.children.push_back(make_fbx_connection_fixture(kAddCurveNode, kAddLayer));
        connections.children.push_back(
            make_fbx_connection_fixture(kAddCurveNode, kChannel, "DeformPercent"));
        connections.children.push_back(
            make_fbx_connection_fixture(kAddCurve, kAddCurveNode, "d|DeformPercent"));
    }
    return write_fbx_document_fixture(path, objects, connections);
}

static bool write_fbx_embedded_texture_fixture(const char *path,
                                               const uint8_t *png_data,
                                               size_t png_len) {
    static const int64_t kGeometryId = 3101;
    static const int64_t kModelId = 3102;
    static const int64_t kMaterialId = 3103;
    static const int64_t kTextureId = 3104;
    static const int64_t kVideoId = 3105;
    static const double kPositions[] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[] = {0, 1, ~2};

    FbxNodeFixture geometry;
    FbxNodeFixture material;
    FbxNodeFixture material_props70;
    FbxNodeFixture texture;
    FbxNodeFixture video;
    FbxNodeFixture video_props70;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture model =
        make_fbx_model_fixture(kModelId, "EmbeddedTexture", "Mesh", 0.0, 0.0, 0.0);
    std::vector<uint8_t> bytes;

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("EmbeddedTextureMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    material.name = "Material";
    material.props.push_back(fbx_prop_i64_fixture(kMaterialId));
    material.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("EmbeddedMaterial", "Material")));
    material.props.push_back(fbx_prop_string_fixture(""));
    material_props70.name = "Properties70";
    material_props70.children.push_back(make_fbx_property_vec3("DiffuseColor", 1.0, 1.0, 1.0));
    material.children.push_back(material_props70);

    texture.name = "Texture";
    texture.props.push_back(fbx_prop_i64_fixture(kTextureId));
    texture.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("EmbeddedAlbedo", "Texture")));
    texture.props.push_back(fbx_prop_string_fixture(""));

    video.name = "Video";
    video.props.push_back(fbx_prop_i64_fixture(kVideoId));
    video.props.push_back(fbx_prop_string_fixture(make_fbx_object_name("EmbeddedAlbedo", "Video")));
    video.props.push_back(fbx_prop_string_fixture("Clip"));
    video_props70.name = "Properties70";
    video_props70.children.push_back(
        make_fbx_property_string("OriginalFilename", "embedded_albedo.png"));
    video.children.push_back(video_props70);
    video.children.push_back(
        FbxNodeFixture{"Content", {fbx_prop_raw_fixture(png_data, png_len)}, {}});

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(material);
    objects.children.push_back(texture);
    objects.children.push_back(video);
    objects.children.push_back(model);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kMaterialId, kModelId));
    connections.children.push_back(
        make_fbx_connection_fixture(kTextureId, kMaterialId, "DiffuseColor"));
    connections.children.push_back(make_fbx_connection_fixture(kVideoId, kTextureId));

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool write_fbx_many_property_node_fixture(const char *path) {
    static const int64_t kGeometryId = 4101;
    static const int64_t kModelId = 4102;
    static const int64_t kMaterialId = 4103;
    static const double kPositions[] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[] = {0, 1, ~2};

    FbxNodeFixture geometry;
    FbxNodeFixture material;
    FbxNodeFixture material_props70;
    FbxNodeFixture metadata;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture model =
        make_fbx_model_fixture(kModelId, "ManyProperties", "Mesh", 0.0, 0.0, 0.0);
    std::vector<uint8_t> bytes;

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("ManyPropertyMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    material.name = "Material";
    material.props.push_back(fbx_prop_i64_fixture(kMaterialId));
    material.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("ManyPropertyMaterial", "Material")));
    material.props.push_back(fbx_prop_string_fixture(""));
    material_props70.name = "Properties70";
    material_props70.children.push_back(make_fbx_property_vec3("DiffuseColor", 0.8, 0.7, 0.6));
    material.children.push_back(material_props70);

    metadata.name = "UserData";
    for (int i = 0; i < 48; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "extra_%02d", i);
        metadata.props.push_back(fbx_prop_string_fixture(name));
    }

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(material);
    objects.children.push_back(metadata);
    objects.children.push_back(model);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kMaterialId, kModelId));

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool write_fbx_large_ngon_fixture(const char *path) {
    static const int64_t kGeometryId = 4201;
    static const int64_t kModelId = 4202;
    constexpr int kVertexCount = 80;
    constexpr double kPi = 3.14159265358979323846;

    std::vector<double> positions;
    std::vector<int32_t> indices;
    FbxNodeFixture geometry;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture model = make_fbx_model_fixture(kModelId, "LargeNgon", "Mesh", 0.0, 0.0, 0.0);
    std::vector<uint8_t> bytes;

    positions.reserve(kVertexCount * 3);
    indices.reserve(kVertexCount);
    for (int i = 0; i < kVertexCount; ++i) {
        double angle = (2.0 * kPi * (double)i) / (double)kVertexCount;
        positions.push_back(std::cos(angle));
        positions.push_back(std::sin(angle));
        positions.push_back(0.0);
        indices.push_back(i);
    }
    indices.back() = ~indices.back();

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("LargeNgon", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices", {fbx_prop_array_fixture('d', positions.data(), positions.size())}, {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex", {fbx_prop_array_fixture('i', indices.data(), indices.size())}, {}});

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(model);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool write_fbx_texture_alias_fixture(const char *path, const char *texture_ref) {
    static const int64_t kGeometryId = 4301;
    static const int64_t kModelId = 4302;
    static const int64_t kMaterialId = 4303;
    static const int64_t kBaseTextureId = 4304;
    static const int64_t kNormalTextureId = 4305;
    static const double kPositions[] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[] = {0, 1, ~2};

    FbxNodeFixture geometry;
    FbxNodeFixture material;
    FbxNodeFixture material_props70;
    FbxNodeFixture base_texture;
    FbxNodeFixture normal_texture;
    FbxNodeFixture base_texture_props70;
    FbxNodeFixture normal_texture_props70;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture model =
        make_fbx_model_fixture(kModelId, "TextureAliases", "Mesh", 0.0, 0.0, 0.0);
    std::vector<uint8_t> bytes;

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("TextureAliasMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    material.name = "Material";
    material.props.push_back(fbx_prop_i64_fixture(kMaterialId));
    material.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("TextureAliasMaterial", "Material")));
    material.props.push_back(fbx_prop_string_fixture(""));
    material_props70.name = "Properties70";
    material_props70.children.push_back(make_fbx_property_vec3("DiffuseColor", 1.0, 1.0, 1.0));
    material.children.push_back(material_props70);

    base_texture.name = "Texture";
    base_texture.props.push_back(fbx_prop_i64_fixture(kBaseTextureId));
    base_texture.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("AliasBaseColor", "Texture")));
    base_texture.props.push_back(fbx_prop_string_fixture(""));
    base_texture_props70.name = "Properties70";
    base_texture_props70.children.push_back(
        make_fbx_property_string("RelativeFilename", texture_ref));
    base_texture.children.push_back(base_texture_props70);

    normal_texture.name = "Texture";
    normal_texture.props.push_back(fbx_prop_i64_fixture(kNormalTextureId));
    normal_texture.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("AliasNormal", "Texture")));
    normal_texture.props.push_back(fbx_prop_string_fixture(""));
    normal_texture_props70.name = "Properties70";
    normal_texture_props70.children.push_back(
        make_fbx_property_string("RelativeFilename", texture_ref));
    normal_texture.children.push_back(normal_texture_props70);

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(material);
    objects.children.push_back(base_texture);
    objects.children.push_back(normal_texture);
    objects.children.push_back(model);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kMaterialId, kModelId));
    connections.children.push_back(
        make_fbx_connection_fixture(kBaseTextureId, kMaterialId, "Maya|baseColor"));
    connections.children.push_back(
        make_fbx_connection_fixture(kNormalTextureId, kMaterialId, "Maya|normalCamera"));

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool write_fbx_material_alias_fixture(const char *path) {
    static const int64_t kGeometryId = 4401;
    static const int64_t kModelId = 4402;
    static const int64_t kMaterialId = 4403;
    static const double kPositions[] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[] = {0, 1, ~2};
    static const double kUv[] = {0.25, 0.75};

    FbxNodeFixture geometry;
    FbxNodeFixture uv_layer;
    FbxNodeFixture material;
    FbxNodeFixture material_props70;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture model =
        make_fbx_model_fixture(kModelId, "MaterialAliases", "Mesh", 0.0, 0.0, 0.0);
    std::vector<uint8_t> bytes;

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("MaterialAliasMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});
    uv_layer.name = "LayerElementUV";
    uv_layer.children.push_back(
        FbxNodeFixture{"MappingInformationType", {fbx_prop_string_fixture("AllSame")}, {}});
    uv_layer.children.push_back(
        FbxNodeFixture{"ReferenceInformationType", {fbx_prop_string_fixture("Direct")}, {}});
    uv_layer.children.push_back(
        FbxNodeFixture{"UV", {fbx_prop_array_fixture('d', kUv, sizeof(kUv) / sizeof(kUv[0]))}, {}});
    geometry.children.push_back(uv_layer);

    material.name = "Material";
    material.props.push_back(fbx_prop_i64_fixture(kMaterialId));
    material.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("MaterialAliasMaterial", "Material")));
    material.props.push_back(fbx_prop_string_fixture(""));
    material_props70.name = "Properties70";
    material_props70.children.push_back(make_fbx_property_vec3("BaseColor", 0.3, 0.4, 0.5));
    material_props70.children.push_back(make_fbx_property_scalar("Metalness", 0.7));
    material_props70.children.push_back(make_fbx_property_scalar("Roughness", 0.25));
    material_props70.children.push_back(make_fbx_property_scalar("AmbientOcclusion", 0.6));
    material_props70.children.push_back(make_fbx_property_scalar("NormalMapScale", 0.8));
    material_props70.children.push_back(make_fbx_property_vec3("EmissionColor", 0.1, 0.2, 0.3));
    material_props70.children.push_back(make_fbx_property_scalar("EmissionFactor", 2.0));
    material_props70.children.push_back(make_fbx_property_scalar("TwoSided", 1.0));
    material.children.push_back(material_props70);

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(material);
    objects.children.push_back(model);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kMaterialId, kModelId));

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool write_fbx_multilayer_attribute_fixture(const char *path) {
    static const int64_t kGeometryId = 4501;
    static const int64_t kModelId = 4502;
    static const int64_t kMaterialId = 4503;
    static const double kPositions[] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[] = {0, 1, ~2};
    static const double kUv0[] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
    static const double kUv1[] = {0.9, 0.1, 0.8, 0.2, 0.7, 0.3};
    static const double kColors[] = {1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.5};

    FbxNodeFixture geometry;
    FbxNodeFixture uv0_layer;
    FbxNodeFixture uv1_layer;
    FbxNodeFixture color_layer;
    FbxNodeFixture material;
    FbxNodeFixture material_props70;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture model =
        make_fbx_model_fixture(kModelId, "LayeredAttributes", "Mesh", 0.0, 0.0, 0.0);

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("LayeredAttributeMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    uv0_layer.name = "LayerElementUV";
    uv0_layer.props.push_back(fbx_prop_i64_fixture(0));
    uv0_layer.children.push_back(
        FbxNodeFixture{"MappingInformationType", {fbx_prop_string_fixture("ByPolygonVertex")}, {}});
    uv0_layer.children.push_back(
        FbxNodeFixture{"ReferenceInformationType", {fbx_prop_string_fixture("Direct")}, {}});
    uv0_layer.children.push_back(FbxNodeFixture{
        "UV", {fbx_prop_array_fixture('d', kUv0, sizeof(kUv0) / sizeof(kUv0[0]))}, {}});
    geometry.children.push_back(uv0_layer);

    uv1_layer.name = "LayerElementUV";
    uv1_layer.props.push_back(fbx_prop_i64_fixture(1));
    uv1_layer.children.push_back(
        FbxNodeFixture{"MappingInformationType", {fbx_prop_string_fixture("ByPolygonVertex")}, {}});
    uv1_layer.children.push_back(
        FbxNodeFixture{"ReferenceInformationType", {fbx_prop_string_fixture("Direct")}, {}});
    uv1_layer.children.push_back(FbxNodeFixture{
        "UV", {fbx_prop_array_fixture('d', kUv1, sizeof(kUv1) / sizeof(kUv1[0]))}, {}});
    geometry.children.push_back(uv1_layer);

    color_layer.name = "LayerElementColor";
    color_layer.props.push_back(fbx_prop_i64_fixture(0));
    color_layer.children.push_back(
        FbxNodeFixture{"MappingInformationType", {fbx_prop_string_fixture("ByPolygonVertex")}, {}});
    color_layer.children.push_back(
        FbxNodeFixture{"ReferenceInformationType", {fbx_prop_string_fixture("Direct")}, {}});
    color_layer.children.push_back(
        FbxNodeFixture{"Colors",
                       {fbx_prop_array_fixture('d', kColors, sizeof(kColors) / sizeof(kColors[0]))},
                       {}});
    geometry.children.push_back(color_layer);

    material.name = "Material";
    material.props.push_back(fbx_prop_i64_fixture(kMaterialId));
    material.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("LayeredAttributeMaterial", "Material")));
    material.props.push_back(fbx_prop_string_fixture(""));
    material_props70.name = "Properties70";
    material_props70.children.push_back(make_fbx_property_vec3("DiffuseColor", 1.0, 1.0, 1.0));
    material.children.push_back(material_props70);

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(material);
    objects.children.push_back(model);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kMaterialId, kModelId));
    return write_fbx_document_fixture(path, objects, connections);
}

static bool write_fbx_cluster_transform_link_fixture(const char *path) {
    static const int64_t kGeometryId = 4601;
    static const int64_t kMeshModelId = 4602;
    static const int64_t kRootBoneId = 4603;
    static const int64_t kChildBoneId = 4604;
    static const int64_t kSkinId = 4605;
    static const int64_t kRootClusterId = 4606;
    static const int64_t kChildClusterId = 4607;
    static const double kPositions[] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[] = {0, 1, ~2};
    static const int32_t kRootIndices[] = {0};
    static const int32_t kChildIndices[] = {1, 2};
    static const double kRootWeights[] = {1.0};
    static const double kChildWeights[] = {1.0, 1.0};
    static const double kRootLink[16] = {
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    static const double kChildLink[16] = {
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    FbxNodeFixture geometry;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture skin = make_fbx_deformer_fixture(kSkinId, "Skin", "Skin");
    FbxNodeFixture root_cluster =
        make_fbx_cluster_fixture(kRootClusterId,
                                 "RootCluster",
                                 kRootIndices,
                                 sizeof(kRootIndices) / sizeof(kRootIndices[0]),
                                 kRootWeights,
                                 sizeof(kRootWeights) / sizeof(kRootWeights[0]));
    FbxNodeFixture child_cluster =
        make_fbx_cluster_fixture(kChildClusterId,
                                 "ChildCluster",
                                 kChildIndices,
                                 sizeof(kChildIndices) / sizeof(kChildIndices[0]),
                                 kChildWeights,
                                 sizeof(kChildWeights) / sizeof(kChildWeights[0]));

    root_cluster.children.push_back(make_fbx_matrix_child("TransformLink", kRootLink));
    child_cluster.children.push_back(make_fbx_matrix_child("TransformLink", kChildLink));

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("BindPoseMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(
        make_fbx_model_fixture(kMeshModelId, "MeshNode", "Mesh", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kRootBoneId, "RootBone", "Root", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kChildBoneId, "ChildBone", "LimbNode", 0.0, 1.0, 0.0));
    objects.children.push_back(skin);
    objects.children.push_back(root_cluster);
    objects.children.push_back(child_cluster);

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kMeshModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kRootBoneId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kChildBoneId, kRootBoneId));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kMeshModelId));
    connections.children.push_back(make_fbx_connection_fixture(kSkinId, kGeometryId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kSkinId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kRootBoneId));
    connections.children.push_back(make_fbx_connection_fixture(kChildClusterId, kSkinId));
    connections.children.push_back(make_fbx_connection_fixture(kChildClusterId, kChildBoneId));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a rational NURBS whose four controls are skinned across two bones.
static bool write_fbx_skinned_nurbs_fixture(const char *path) {
    static const int64_t kGeometryId = 4651;
    static const int64_t kMeshModelId = 4652;
    static const int64_t kRootBoneId = 4653;
    static const int64_t kChildBoneId = 4654;
    static const int64_t kSkinId = 4655;
    static const int64_t kRootClusterId = 4656;
    static const int64_t kChildClusterId = 4657;
    static const int32_t kRootIndices[] = {0, 2};
    static const int32_t kChildIndices[] = {1, 3};
    static const double kWeights[] = {1.0, 1.0};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture skin = make_fbx_deformer_fixture(kSkinId, "SurfaceSkin", "Skin");
    FbxNodeFixture root_cluster = make_fbx_cluster_fixture(
        kRootClusterId, "SurfaceRootCluster", kRootIndices, 2u, kWeights, 2u);
    FbxNodeFixture child_cluster = make_fbx_cluster_fixture(
        kChildClusterId, "SurfaceChildCluster", kChildIndices, 2u, kWeights, 2u);
    objects.name = "Objects";
    objects.children.push_back(make_fbx_nurbs_surface_geometry_fixture(kGeometryId, 2));
    objects.children.push_back(
        make_fbx_model_fixture(kMeshModelId, "SkinnedSurface", "NurbsSurface", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kRootBoneId, "SurfaceRoot", "Root", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kChildBoneId, "SurfaceChild", "LimbNode", 0.0, 1.0, 0.0));
    objects.children.push_back(skin);
    objects.children.push_back(root_cluster);
    objects.children.push_back(child_cluster);
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kMeshModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kRootBoneId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kChildBoneId, kRootBoneId));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kMeshModelId));
    connections.children.push_back(make_fbx_connection_fixture(kSkinId, kGeometryId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kSkinId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kRootBoneId));
    connections.children.push_back(make_fbx_connection_fixture(kChildClusterId, kSkinId));
    connections.children.push_back(make_fbx_connection_fixture(kChildClusterId, kChildBoneId));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a NURBS BlendShape that moves only the rational upper-right control.
static bool write_fbx_morphed_nurbs_fixture(const char *path) {
    static const int64_t kGeometryId = 4661;
    static const int64_t kModelId = 4662;
    static const int64_t kBlendShapeId = 4663;
    static const int64_t kChannelId = 4664;
    static const int64_t kShapeId = 4665;
    static const int32_t kShapeIndices[] = {3};
    static const double kShapeDelta[] = {1.0, 0.0, 0.0};
    static const float kFullWeights[] = {100.0f};
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture blend_shape =
        make_fbx_deformer_fixture(kBlendShapeId, "SurfaceBlendShape", "BlendShape");
    FbxNodeFixture channel =
        make_fbx_deformer_fixture(kChannelId, "SurfaceBlendChannel", "BlendShapeChannel");
    FbxNodeFixture channel_properties;
    FbxNodeFixture shape;
    channel_properties.name = "Properties70";
    channel_properties.children.push_back(make_fbx_property_scalar("DeformPercent", 0.0));
    channel.children.push_back(channel_properties);
    channel.children.push_back(
        FbxNodeFixture{"FullWeights", {fbx_prop_array_fixture('f', kFullWeights, 1u)}, {}});
    shape.name = "Geometry";
    shape.props.push_back(fbx_prop_i64_fixture(kShapeId));
    shape.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("SurfaceControlMove", "Geometry")));
    shape.props.push_back(fbx_prop_string_fixture("Shape"));
    shape.children.push_back(
        FbxNodeFixture{"Indexes", {fbx_prop_array_fixture('i', kShapeIndices, 1u)}, {}});
    shape.children.push_back(
        FbxNodeFixture{"Vertices", {fbx_prop_array_fixture('d', kShapeDelta, 3u)}, {}});
    objects.name = "Objects";
    objects.children.push_back(make_fbx_nurbs_surface_geometry_fixture(kGeometryId, 2));
    objects.children.push_back(
        make_fbx_model_fixture(kModelId, "MorphedSurface", "NurbsSurface", 0.0, 0.0, 0.0));
    objects.children.push_back(blend_shape);
    objects.children.push_back(channel);
    objects.children.push_back(shape);
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    connections.children.push_back(make_fbx_connection_fixture(kBlendShapeId, kGeometryId));
    connections.children.push_back(make_fbx_connection_fixture(kChannelId, kBlendShapeId));
    connections.children.push_back(make_fbx_connection_fixture(kShapeId, kChannelId));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write a compact skinned animation fixture for constant, cubic, or transform-stack tests.
/// @details Mode zero emits a constant 0→10 step; mode one emits a 0→0 cubic Hermite arch with
///          ±4 value/second tangents; mode two emits a linear 0→1 translation on a bone carrying
///          pre-rotation, pivot, geometric translation, and scale-compensating inheritance fields.
/// @param path Destination path.
/// @param mode Fixture mode: 0 constant, 1 cubic, 2 transform stack.
/// @return True when the binary FBX was written completely.
static bool write_fbx_interpolation_animation_fixture(const char *path, int mode) {
    static const int64_t kGeometryId = 4701;
    static const int64_t kMeshModelId = 4702;
    static const int64_t kRootBoneId = 4703;
    static const int64_t kSkinId = 4704;
    static const int64_t kRootClusterId = 4705;
    static const int64_t kStackId = 4706;
    static const int64_t kLayerId = 4707;
    static const int64_t kTranslateNodeId = 4708;
    static const int64_t kCurveXId = 4709;
    static const int64_t kFbxSecond = 46186158000LL;
    static const double kPositions[] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[] = {0, 1, ~2};
    static const int32_t kRootIndices[] = {0, 1, 2};
    static const double kRootWeights[] = {1.0, 1.0, 1.0};
    static const int64_t kTimes[] = {0, kFbxSecond};
    const double values[] = {0.0, mode == 0 ? 10.0 : mode == 2 ? 1.0 : 0.0};
    const int32_t flags[] = {mode == 0   ? 0x00000002
                             : mode == 1 ? 0x00000008
                                         : 0x00000004,
                             mode == 0   ? 0x00000002
                             : mode == 1 ? 0x00000008
                                         : 0x00000004};
    const double tangent_data[] = {4.0, 0.0, 0.0, 0.0, 0.0, -4.0, 0.0, 0.0};
    FbxNodeFixture geometry;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture skin = make_fbx_deformer_fixture(kSkinId, "Skin", "Skin");
    FbxNodeFixture anim_stack;
    FbxNodeFixture anim_layer;
    FbxNodeFixture translate_node;
    FbxNodeFixture root_model =
        make_fbx_model_fixture(kRootBoneId, "RootBone", "Root", 0.0, 0.0, 0.0);
    if (mode == 2) {
        root_model.children[0].children.push_back(
            make_fbx_property_vec3("PreRotation", 0.0, 0.0, 90.0));
        root_model.children[0].children.push_back(
            make_fbx_property_vec3("RotationPivot", 1.0, 0.0, 0.0));
        root_model.children[0].children.push_back(
            make_fbx_property_vec3("GeometricTranslation", 2.0, 0.0, 0.0));
        root_model.children[0].children.push_back(make_fbx_property_int("InheritType", 2));
    }

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("ConstantAnimMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    anim_stack.name = "AnimationStack";
    anim_stack.props.push_back(fbx_prop_i64_fixture(kStackId));
    anim_stack.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("Constant", "AnimStack")));
    anim_stack.props.push_back(fbx_prop_string_fixture(""));
    anim_layer.name = "AnimationLayer";
    anim_layer.props.push_back(fbx_prop_i64_fixture(kLayerId));
    anim_layer.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("BaseLayer", "AnimLayer")));
    anim_layer.props.push_back(fbx_prop_string_fixture(""));
    translate_node.name = "AnimationCurveNode";
    translate_node.props.push_back(fbx_prop_i64_fixture(kTranslateNodeId));
    translate_node.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("RootTranslate", "AnimCurveNode")));
    translate_node.props.push_back(fbx_prop_string_fixture(""));

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(
        make_fbx_model_fixture(kMeshModelId, "MeshNode", "Mesh", 0.0, 0.0, 0.0));
    objects.children.push_back(root_model);
    objects.children.push_back(skin);
    objects.children.push_back(
        make_fbx_cluster_fixture(kRootClusterId,
                                 "RootCluster",
                                 kRootIndices,
                                 sizeof(kRootIndices) / sizeof(kRootIndices[0]),
                                 kRootWeights,
                                 sizeof(kRootWeights) / sizeof(kRootWeights[0])));
    objects.children.push_back(anim_stack);
    objects.children.push_back(anim_layer);
    objects.children.push_back(translate_node);
    {
        FbxNodeFixture curve = make_fbx_animation_curve_fixture_with_attrs(
            kCurveXId, kTimes, values, flags, sizeof(kTimes) / sizeof(kTimes[0]));
        if (mode == 1) {
            curve.children.push_back(FbxNodeFixture{
                "KeyAttrDataFloat",
                {fbx_prop_array_fixture(
                    'd', tangent_data, sizeof(tangent_data) / sizeof(tangent_data[0]))},
                {}});
        }
        objects.children.push_back(curve);
    }

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kMeshModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kRootBoneId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kMeshModelId));
    connections.children.push_back(make_fbx_connection_fixture(kSkinId, kGeometryId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kSkinId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kRootBoneId));
    connections.children.push_back(make_fbx_connection_fixture(kLayerId, kStackId));
    connections.children.push_back(make_fbx_connection_fixture(kTranslateNodeId, kLayerId));
    connections.children.push_back(
        make_fbx_connection_fixture(kTranslateNodeId, kRootBoneId, "Lcl Translation"));
    connections.children.push_back(make_fbx_connection_fixture(kCurveXId, kTranslateNodeId, "d|X"));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Write the exact-step constant interpolation regression fixture.
static bool write_fbx_constant_animation_fixture(const char *path) {
    return write_fbx_interpolation_animation_fixture(path, 0);
}

/// @brief Write the adaptive cubic-extrema/tangent regression fixture.
static bool write_fbx_cubic_animation_fixture(const char *path) {
    return write_fbx_interpolation_animation_fixture(path, 1);
}

/// @brief Write the shared static/animated transform-stack regression fixture.
static bool write_fbx_transform_stack_animation_fixture(const char *path) {
    return write_fbx_interpolation_animation_fixture(path, 2);
}

static bool write_fbx_rotation_order_fixture(const char *path) {
    static const int64_t kDefaultModelId = 4801;
    static const int64_t kZyxModelId = 4802;
    FbxNodeFixture default_model =
        make_fbx_model_fixture(kDefaultModelId, "DefaultOrder", "Null", 0.0, 0.0, 0.0);
    FbxNodeFixture zyx_model =
        make_fbx_model_fixture(kZyxModelId, "ZyxOrder", "Null", 1.0, 0.0, 0.0);
    FbxNodeFixture objects;
    FbxNodeFixture connections;

    default_model.children[0].children.push_back(
        make_fbx_property_vec3("Lcl Rotation", 10.0, 20.0, 30.0));
    zyx_model.children[0].children.push_back(
        make_fbx_property_vec3("Lcl Rotation", 10.0, 20.0, 30.0));
    zyx_model.children[0].children.push_back(make_fbx_property_int("RotationOrder", 5));

    objects.name = "Objects";
    objects.children.push_back(default_model);
    objects.children.push_back(zyx_model);
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kDefaultModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kZyxModelId, 0));
    return write_fbx_document_fixture(path, objects, connections);
}

static bool write_fbx_skinned_animation_fixture_ex(const char *path,
                                                   bool duplicate_x_curve,
                                                   int dummy_layers,
                                                   int dummy_curve_nodes,
                                                   bool duplicate_bone_names,
                                                   bool animate_child_bone,
                                                   bool mismatched_x_curve,
                                                   bool bare_curve_component_names = false,
                                                   bool lowercase_curve_component_names = false,
                                                   int64_t key_time_offset = 0,
                                                   bool unknown_constraint = false,
                                                   bool second_stack = false,
                                                   bool constraint_bone_links = false) {
    static const int64_t kGeometryId = 1100;
    static const int64_t kMeshModelId = 1200;
    static const int64_t kRootBoneId = 1300;
    static const int64_t kChildBoneId = 1301;
    static const int64_t kSkinId = 1400;
    static const int64_t kRootClusterId = 1401;
    static const int64_t kChildClusterId = 1402;
    static const int64_t kStackId = 1500;
    static const int64_t kLayerId = 1501;
    static const int64_t kTranslateNodeId = 1510;
    static const int64_t kCurveXId = 1511;
    static const int64_t kCurveYId = 1512;
    static const int64_t kCurveXDuplicateId = 1513;
    static const int64_t kDummyLayerBaseId = 1600;
    static const int64_t kDummyCurveNodeBaseId = 2000;
    static const int64_t kFbxSecond = 46186158000LL;
    static const double kPositions[9] = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    static const int32_t kIndices[3] = {0, 1, -3};
    static const int32_t kRootIndices[2] = {0, 1};
    static const double kRootWeights[2] = {1.0, 0.25};
    static const int32_t kChildIndices[2] = {1, 2};
    static const double kChildWeights[2] = {0.75, 1.0};
    static const double kCurveXValues[2] = {0.0, 10.0};
    static const double kCurveYValues[2] = {0.0, 20.0};
    static const double kCurveXDuplicateValues[2] = {0.0, 99.0};
    static const double kCurveXShortValues[1] = {123.0};
    const int64_t key_times[2] = {key_time_offset, key_time_offset + kFbxSecond};

    FbxNodeFixture geometry;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture skin = make_fbx_deformer_fixture(kSkinId, "Skin", "Skin");
    FbxNodeFixture anim_stack;
    FbxNodeFixture anim_layer;
    FbxNodeFixture translate_node;
    std::vector<uint8_t> bytes;
    const char *child_bone_name = duplicate_bone_names ? "RootBone" : "ChildBone";
    int64_t animated_bone_id = animate_child_bone ? kChildBoneId : kRootBoneId;
    const char *curve_x_prop = bare_curve_component_names
                                   ? (lowercase_curve_component_names ? "x" : "X")
                                   : (lowercase_curve_component_names ? "d|x" : "d|X");
    const char *curve_y_prop = bare_curve_component_names
                                   ? (lowercase_curve_component_names ? "y" : "Y")
                                   : (lowercase_curve_component_names ? "d|y" : "d|Y");

    geometry.name = "Geometry";
    geometry.props.push_back(fbx_prop_i64_fixture(kGeometryId));
    geometry.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("SkinnedMesh", "Geometry")));
    geometry.props.push_back(fbx_prop_string_fixture("Mesh"));
    geometry.children.push_back(FbxNodeFixture{
        "Vertices",
        {fbx_prop_array_fixture('d', kPositions, sizeof(kPositions) / sizeof(kPositions[0]))},
        {}});
    geometry.children.push_back(FbxNodeFixture{
        "PolygonVertexIndex",
        {fbx_prop_array_fixture('i', kIndices, sizeof(kIndices) / sizeof(kIndices[0]))},
        {}});

    anim_stack.name = "AnimationStack";
    anim_stack.props.push_back(fbx_prop_i64_fixture(kStackId));
    anim_stack.props.push_back(fbx_prop_string_fixture(make_fbx_object_name("Walk", "AnimStack")));
    anim_stack.props.push_back(fbx_prop_string_fixture(""));

    anim_layer.name = "AnimationLayer";
    anim_layer.props.push_back(fbx_prop_i64_fixture(kLayerId));
    anim_layer.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("BaseLayer", "AnimLayer")));
    anim_layer.props.push_back(fbx_prop_string_fixture(""));

    translate_node.name = "AnimationCurveNode";
    translate_node.props.push_back(fbx_prop_i64_fixture(kTranslateNodeId));
    translate_node.props.push_back(
        fbx_prop_string_fixture(make_fbx_object_name("RootTranslate", "AnimCurveNode")));
    translate_node.props.push_back(fbx_prop_string_fixture(""));

    objects.name = "Objects";
    objects.children.push_back(geometry);
    objects.children.push_back(
        make_fbx_model_fixture(kMeshModelId, "MeshNode", "Mesh", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kRootBoneId, "RootBone", "Root", 0.0, 0.0, 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kChildBoneId, child_bone_name, "LimbNode", 0.0, 1.0, 0.0));
    objects.children.push_back(skin);
    objects.children.push_back(
        make_fbx_cluster_fixture(kRootClusterId,
                                 "RootCluster",
                                 kRootIndices,
                                 sizeof(kRootIndices) / sizeof(kRootIndices[0]),
                                 kRootWeights,
                                 sizeof(kRootWeights) / sizeof(kRootWeights[0])));
    objects.children.push_back(
        make_fbx_cluster_fixture(kChildClusterId,
                                 "ChildCluster",
                                 kChildIndices,
                                 sizeof(kChildIndices) / sizeof(kChildIndices[0]),
                                 kChildWeights,
                                 sizeof(kChildWeights) / sizeof(kChildWeights[0])));
    objects.children.push_back(anim_stack);
    for (int i = 0; i < dummy_layers; i++) {
        char name[64];
        FbxNodeFixture dummy_layer;
        std::snprintf(name, sizeof(name), "DummyLayer%d", i);
        dummy_layer.name = "AnimationLayer";
        dummy_layer.props.push_back(fbx_prop_i64_fixture(kDummyLayerBaseId + i));
        dummy_layer.props.push_back(
            fbx_prop_string_fixture(make_fbx_object_name(name, "AnimLayer")));
        dummy_layer.props.push_back(fbx_prop_string_fixture(""));
        objects.children.push_back(dummy_layer);
    }
    objects.children.push_back(anim_layer);
    for (int i = 0; i < dummy_curve_nodes; i++) {
        char name[64];
        FbxNodeFixture dummy_curve_node;
        std::snprintf(name, sizeof(name), "DummyCurveNode%d", i);
        dummy_curve_node.name = "AnimationCurveNode";
        dummy_curve_node.props.push_back(fbx_prop_i64_fixture(kDummyCurveNodeBaseId + i));
        dummy_curve_node.props.push_back(
            fbx_prop_string_fixture(make_fbx_object_name(name, "AnimCurveNode")));
        dummy_curve_node.props.push_back(fbx_prop_string_fixture(""));
        objects.children.push_back(dummy_curve_node);
    }
    objects.children.push_back(translate_node);
    if (mismatched_x_curve) {
        objects.children.push_back(make_fbx_animation_curve_mismatched_fixture(
            kCurveXId,
            key_times,
            sizeof(key_times) / sizeof(key_times[0]),
            kCurveXShortValues,
            sizeof(kCurveXShortValues) / sizeof(kCurveXShortValues[0])));
    } else {
        objects.children.push_back(make_fbx_animation_curve_fixture(
            kCurveXId, key_times, kCurveXValues, sizeof(key_times) / sizeof(key_times[0])));
    }
    objects.children.push_back(make_fbx_animation_curve_fixture(
        kCurveYId, key_times, kCurveYValues, sizeof(key_times) / sizeof(key_times[0])));
    if (duplicate_x_curve) {
        objects.children.push_back(
            make_fbx_animation_curve_fixture(kCurveXDuplicateId,
                                             key_times,
                                             kCurveXDuplicateValues,
                                             sizeof(key_times) / sizeof(key_times[0])));
    }

    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kMeshModelId, 0));
    if (constraint_bone_links) {
        /* Character-style OP rig links listed BEFORE the hierarchy links, exactly as Biped
         * exports order them; parent resolution must skip these. */
        connections.children.push_back(make_fbx_connection_fixture(kRootBoneId, 9500, "RootLink"));
        connections.children.push_back(
            make_fbx_connection_fixture(kChildBoneId, 9500, "SpineLink"));
    }
    connections.children.push_back(make_fbx_connection_fixture(kRootBoneId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kChildBoneId, kRootBoneId));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kMeshModelId));
    connections.children.push_back(make_fbx_connection_fixture(kSkinId, kGeometryId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kSkinId));
    connections.children.push_back(make_fbx_connection_fixture(kRootClusterId, kRootBoneId));
    connections.children.push_back(make_fbx_connection_fixture(kChildClusterId, kSkinId));
    connections.children.push_back(make_fbx_connection_fixture(kChildClusterId, kChildBoneId));
    for (int i = 0; i < dummy_layers; i++) {
        connections.children.push_back(
            make_fbx_connection_fixture(kDummyLayerBaseId + i, kStackId));
    }
    connections.children.push_back(make_fbx_connection_fixture(kLayerId, kStackId));
    for (int i = 0; i < dummy_curve_nodes; i++) {
        connections.children.push_back(
            make_fbx_connection_fixture(kDummyCurveNodeBaseId + i, kLayerId));
    }
    connections.children.push_back(make_fbx_connection_fixture(kTranslateNodeId, kLayerId));
    connections.children.push_back(
        make_fbx_connection_fixture(kTranslateNodeId, animated_bone_id, "Lcl Translation"));
    connections.children.push_back(
        make_fbx_connection_fixture(kCurveXId, kTranslateNodeId, curve_x_prop));
    if (duplicate_x_curve) {
        connections.children.push_back(
            make_fbx_connection_fixture(kCurveXDuplicateId, kTranslateNodeId, curve_x_prop));
    }
    connections.children.push_back(
        make_fbx_connection_fixture(kCurveYId, kTranslateNodeId, curve_y_prop));
    if (second_stack) {
        /* A second independent stack named "Run" targeting the same bone, so multi-clip
         * models (the mocap-package shape) can be built for clip-subset tests. */
        static const int64_t kStack2Id = 1550;
        static const int64_t kLayer2Id = 1551;
        static const int64_t kTranslate2NodeId = 1560;
        static const int64_t kCurve2XId = 1561;
        static const int64_t kCurve2YId = 1562;
        static const double kCurve2XValues[2] = {0.0, 20.0};
        static const double kCurve2YValues[2] = {0.0, 4.0};
        FbxNodeFixture run_stack;
        FbxNodeFixture run_layer;
        FbxNodeFixture run_translate;
        run_stack.name = "AnimationStack";
        run_stack.props.push_back(fbx_prop_i64_fixture(kStack2Id));
        run_stack.props.push_back(
            fbx_prop_string_fixture(make_fbx_object_name("Run", "AnimStack")));
        run_stack.props.push_back(fbx_prop_string_fixture(""));
        run_layer.name = "AnimationLayer";
        run_layer.props.push_back(fbx_prop_i64_fixture(kLayer2Id));
        run_layer.props.push_back(
            fbx_prop_string_fixture(make_fbx_object_name("RunLayer", "AnimLayer")));
        run_layer.props.push_back(fbx_prop_string_fixture(""));
        run_translate.name = "AnimationCurveNode";
        run_translate.props.push_back(fbx_prop_i64_fixture(kTranslate2NodeId));
        run_translate.props.push_back(
            fbx_prop_string_fixture(make_fbx_object_name("RunTranslate", "AnimCurveNode")));
        run_translate.props.push_back(fbx_prop_string_fixture(""));
        objects.children.push_back(run_stack);
        objects.children.push_back(run_layer);
        objects.children.push_back(run_translate);
        objects.children.push_back(make_fbx_animation_curve_fixture(
            kCurve2XId, key_times, kCurve2XValues, sizeof(key_times) / sizeof(key_times[0])));
        objects.children.push_back(make_fbx_animation_curve_fixture(
            kCurve2YId, key_times, kCurve2YValues, sizeof(key_times) / sizeof(key_times[0])));
        connections.children.push_back(make_fbx_connection_fixture(kLayer2Id, kStack2Id));
        connections.children.push_back(make_fbx_connection_fixture(kTranslate2NodeId, kLayer2Id));
        connections.children.push_back(
            make_fbx_connection_fixture(kTranslate2NodeId, animated_bone_id, "Lcl Translation"));
        connections.children.push_back(
            make_fbx_connection_fixture(kCurve2XId, kTranslate2NodeId, curve_x_prop));
        connections.children.push_back(
            make_fbx_connection_fixture(kCurve2YId, kTranslate2NodeId, curve_y_prop));
    }
    if (unknown_constraint) {
        /* An unevaluated rig-definition constraint, as mocap exporters ship: unknown kind,
         * no bindings. It must have zero effect on the imported animation. */
        objects.children.push_back(make_fbx_constraint_fixture(9500, "MotionRig", "Character", {}));
    }

    bytes.insert(bytes.end(), {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F',  'B',    'X', ' ',
                               'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', '\0', '\x1A', '\0'});
    append_bytes(bytes, (uint32_t)7400);
    write_fbx_node_fixture(objects, bytes);
    write_fbx_node_fixture(connections, bytes);
    bytes.resize(bytes.size() + 13, 0);

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

static bool write_fbx_skinned_animation_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(path, false, 0, 0, false, false, false);
}

static bool write_fbx_duplicate_animation_curve_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(path, true, 0, 0, false, false, false);
}

static bool write_fbx_many_layer_animation_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(path, false, 20, 0, false, false, false);
}

static bool write_fbx_many_curve_node_animation_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(path, false, 0, 260, false, false, false);
}

static bool write_fbx_duplicate_bone_name_child_animation_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(path, false, 0, 0, true, true, false);
}

static bool write_fbx_mismatched_animation_curve_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(path, false, 0, 0, false, false, true);
}

static bool write_fbx_bare_component_animation_curve_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(path, false, 0, 0, false, false, false, true);
}

static bool write_fbx_lowercase_component_animation_curve_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(
        path, false, 0, 0, false, false, false, false, true);
}

static bool write_fbx_negative_time_animation_fixture(const char *path) {
    return write_fbx_skinned_animation_fixture_ex(
        path, false, 0, 0, false, false, false, false, false, -46186158000LL);
}

/// @brief Write many individually valid compressed arrays whose aggregate expansion exceeds a
///        lowered per-load budget.
/// @param path Destination fixture path.
/// @return True when the binary fixture was written.
static bool write_fbx_aggregate_budget_fixture(const char *path) {
    std::vector<double> values(4096u);
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture geometry = make_fbx_triangle_geometry_fixture(9000, "BudgetMesh", 0.0);
    for (size_t i = 0; i < values.size(); i++)
        values[i] = (double)i * 0.125;
    for (int array_index = 0; array_index < 80; array_index++) {
        char name[32];
        std::snprintf(name, sizeof(name), "BudgetArray%d", array_index);
        geometry.children.push_back(FbxNodeFixture{
            name, {fbx_prop_compressed_array_fixture('d', values.data(), values.size())}, {}});
    }
    objects.name = "Objects";
    objects.children.push_back(geometry);
    connections.name = "Connections";
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Construct a long display name with a shared 180-byte prefix and unique suffix.
/// @param suffix Unique suffix appended after the colliding prefix.
/// @return Long UTF-8/ASCII fixture name.
static std::string make_fbx_long_collision_name(const char *suffix) {
    std::string name(180u, 'q');
    name += suffix ? suffix : "";
    return name;
}

/// @brief Write long colliding-prefix scene/bone names plus an ID-targeted child animation.
/// @details The two scene names collide beyond the historical 128-byte buffer and the two bone
///          names collide beyond the historical 64-byte buffer. The curve targets the second bone
///          exclusively through its numeric object ID.
/// @param path Destination fixture path.
/// @return True when the binary fixture was written completely.
static bool write_fbx_long_name_identity_fixture(const char *path) {
    static const int64_t times[] = {0, 46186158000LL};
    static const double values[] = {0.0, 3.0};
    std::string scene_a = make_fbx_long_collision_name("_scene_a");
    std::string scene_b = make_fbx_long_collision_name("_scene_b");
    std::string bone_a = make_fbx_long_collision_name("_bone_a");
    std::string bone_b = make_fbx_long_collision_name("_bone_b");
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture stack;
    FbxNodeFixture layer;
    FbxNodeFixture curve_node;
    objects.name = "Objects";
    objects.children.push_back(make_fbx_model_fixture(9100, scene_a.c_str(), "Null", 0, 0, 0));
    objects.children.push_back(make_fbx_model_fixture(9101, scene_b.c_str(), "Null", 1, 0, 0));
    objects.children.push_back(make_fbx_model_fixture(9200, bone_a.c_str(), "Root", 0, 0, 0));
    objects.children.push_back(make_fbx_model_fixture(9201, bone_b.c_str(), "LimbNode", 0, 1, 0));
    stack.name = "AnimationStack";
    stack.props = {fbx_prop_i64_fixture(9300),
                   fbx_prop_string_fixture(make_fbx_object_name("LongNames", "AnimStack")),
                   fbx_prop_string_fixture("")};
    layer.name = "AnimationLayer";
    layer.props = {fbx_prop_i64_fixture(9301),
                   fbx_prop_string_fixture(make_fbx_object_name("Base", "AnimLayer")),
                   fbx_prop_string_fixture("")};
    curve_node.name = "AnimationCurveNode";
    curve_node.props = {fbx_prop_i64_fixture(9302),
                        fbx_prop_string_fixture(make_fbx_object_name("ChildMove", "AnimCurveNode")),
                        fbx_prop_string_fixture("")};
    objects.children.push_back(stack);
    objects.children.push_back(layer);
    objects.children.push_back(curve_node);
    objects.children.push_back(make_fbx_animation_curve_fixture(9303, times, values, 2u));
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(9100, 0));
    connections.children.push_back(make_fbx_connection_fixture(9101, 0));
    connections.children.push_back(make_fbx_connection_fixture(9200, 0));
    connections.children.push_back(make_fbx_connection_fixture(9201, 9200));
    connections.children.push_back(make_fbx_connection_fixture(9301, 9300));
    connections.children.push_back(make_fbx_connection_fixture(9302, 9301));
    connections.children.push_back(make_fbx_connection_fixture(9302, 9201, "Lcl Translation"));
    connections.children.push_back(make_fbx_connection_fixture(9303, 9302, "d|X"));
    return write_fbx_document_fixture(path, objects, connections);
}

static bool write_truncated_fbx_fixture(const char *path) {
    const char *valid_path = "/tmp/zanna_model3d_valid_for_truncate.fbx";
    if (!write_fbx_fixture(valid_path))
        return false;

    FILE *f = std::fopen(valid_path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 40) {
        std::fclose(f);
        return false;
    }
    std::vector<uint8_t> bytes((size_t)size);
    bool read_ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!read_ok)
        return false;

    FILE *out = std::fopen(path, "wb");
    if (!out)
        return false;
    size_t truncated_size = bytes.size() - 7;
    bool write_ok = std::fwrite(bytes.data(), 1, truncated_size, out) == truncated_size;
    std::fclose(out);
    return write_ok;
}

static bool write_scene_fixture(const char *path) {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *mesh = rt_mesh3d_new_box(1.0, 2.0, 3.0);
    void *material = rt_material3d_new_color(0.2, 0.4, 0.8);

    if (!scene || !parent || !child || !mesh || !material)
        return false;

    rt_scene_node3d_set_name(parent, rt_const_cstr("parent"));
    rt_scene_node3d_set_name(child, rt_const_cstr("child"));
    rt_scene_node3d_set_position(parent, 1.0, 2.0, 3.0);
    rt_scene_node3d_set_position(child, 0.0, 5.0, 0.0);
    rt_scene_node3d_metadata_set_string(child, rt_const_cstr("game.role"), rt_const_cstr("enemy"));
    rt_scene_node3d_metadata_set_int(child, rt_const_cstr("health"), 125);
    rt_scene_node3d_metadata_set_float(child, rt_const_cstr("radius"), 4.0);
    rt_scene_node3d_metadata_set_bool(child, rt_const_cstr("active"), 1);
    rt_scene_node3d_metadata_set_null(child, rt_const_cstr("target"));
    rt_scene_node3d_set_mesh(parent, mesh);
    rt_scene_node3d_set_material(parent, material);
    rt_scene_node3d_set_mesh(child, mesh);
    rt_scene_node3d_set_material(child, material);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    return rt_scene3d_save(scene, rt_const_cstr(path)) == 1;
}

static bool write_gltf_fixture(const char *path) {
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
        "      \"baseColorFactor\": [0.7, 0.3, 0.2, 1.0]\n"
        "    }\n"
        "  }],\n"
        "  \"meshes\": [{\"primitives\": [{\n"
        "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2},\n"
        "    \"indices\": 3,\n"
        "    \"material\": 0\n"
        "  }]}],\n"
        "  \"nodes\": [\n"
        "    {\"name\": \"GltfParent\", \"translation\": [1.0, 0.0, 0.0], \"mesh\": 0, "
        "\"children\": [1]},\n"
        "    {\"name\": \"GltfChild\", \"scale\": [2.0, 2.0, 2.0], \"camera\": 0},\n"
        "    {\"name\": \"GltfOrthoCamera\", \"translation\": [0.0, 2.0, 0.0], "
        "\"camera\": 1},\n"
        "    {\"name\": \"GltfSecondary\", \"translation\": [-3.0, 0.0, 0.0], \"mesh\": 0}\n"
        "  ],\n"
        "  \"cameras\": [\n"
        "    {\"type\": \"perspective\", \"perspective\": {\"yfov\": 1.0471975512, "
        "\"aspectRatio\": 1.7777777778, \"znear\": 0.2, \"zfar\": 250.0}},\n"
        "    {\"type\": \"orthographic\", \"orthographic\": {\"xmag\": 8.0, \"ymag\": 4.0, "
        "\"znear\": 0.5, \"zfar\": 80.0}}\n"
        "  ],\n"
        "  \"scenes\": [{\"nodes\": [0, 2]}, {\"name\": \"SecondaryScene\", \"nodes\": [3]}],\n"
        "  \"scene\": 0\n"
        "}\n";

    FILE *gltf = std::fopen(path, "wb");
    if (!gltf)
        return false;
    std::fwrite(gltf_json.data(), 1, gltf_json.size(), gltf);
    std::fclose(gltf);
    return true;
}

static const char *find_existing_path(std::initializer_list<const char *> candidates) {
    for (const char *candidate : candidates) {
        FILE *f = std::fopen(candidate, "rb");
        if (f) {
            std::fclose(f);
            return candidate;
        }
    }
    return nullptr;
}

/// @brief ADR 0182: the canonical .scene3d extension dispatches to the VSCN
///        importer exactly like the legacy .vscn alias.
static void test_model3d_accepts_canonical_scene3d_extension() {
    const char *path = "/tmp/zanna_model3d_fixture_canonical.scene3d";
    bool wrote_fixture = write_scene_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Scene fixture can be written to .scene3d");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses canonical .scene3d assets");
    if (model) {
        EXPECT_TRUE(rt_model3d_get_node_count(model) == 2,
                    ".scene3d assets import the same scene nodes as .vscn");
        EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1,
                    ".scene3d assets deduplicate shared meshes like .vscn");
    }
    void *model_result = rt_model3d_load_result(rt_const_cstr(path));
    EXPECT_TRUE(rt_result_is_ok(model_result) == 1,
                "SceneAsset.LoadResult accepts canonical .scene3d paths");
    std::remove(path);
}

/// @brief ADR 0190: VSCN text loads as a SceneAsset with no document on disk.
static void test_model3d_load_text_result_loads_without_disk_document() {
    void *scene = rt_scene3d_new();
    void *node = rt_scene_node3d_new();
    rt_scene_node3d_set_name(node, rt_const_cstr("text-doc-node"));
    rt_scene_node3d_set_mesh(node, rt_mesh3d_new_box(1.0, 1.0, 1.0));
    rt_scene3d_add(scene, node);
    rt_string text = rt_scene3d_save_text(scene);
    EXPECT_TRUE(text && rt_str_len(text) > 0, "SaveToText produces the text fixture document");

    /* Nothing exists at the virtual path — it routes extension detection only. */
    void *ok = rt_model3d_load_text_result(
        rt_const_cstr("/tmp/zanna_load_text_absent_dir/absent.scene3d"), text);
    EXPECT_TRUE(ok && rt_result_is_ok(ok) == 1,
                "SceneAsset.LoadTextResult returns Ok for canonical VSCN text");
    if (ok && rt_result_is_ok(ok) == 1) {
        void *model = rt_result_unwrap(ok);
        EXPECT_TRUE(rt_model3d_get_node_count(model) == 1,
                    "Text-loaded assets expose the authored nodes");
        void *instance = rt_model3d_instantiate_scene(model);
        EXPECT_TRUE(instance != nullptr, "Text-loaded assets instantiate scenes");
        if (instance) {
            EXPECT_TRUE(rt_scene3d_find(instance, rt_const_cstr("text-doc-node")) != nullptr,
                        "Instantiated text-loaded scenes contain the authored node");
        }
    }

    void *wrong_ext = rt_model3d_load_text_result(rt_const_cstr("/tmp/zanna_text_doc.gltf"), text);
    EXPECT_TRUE(wrong_ext && rt_result_is_err(wrong_ext) == 1,
                "SceneAsset.LoadTextResult rejects non-VSCN extensions with Err, not a trap");

    void *malformed = rt_model3d_load_text_result(rt_const_cstr("/tmp/zanna_text_doc.scene3d"),
                                                  rt_const_cstr("{ not a vscn document"));
    EXPECT_TRUE(malformed && rt_result_is_err(malformed) == 1,
                "SceneAsset.LoadTextResult reports malformed text as Err");

    void *empty = rt_model3d_load_text_result(rt_const_cstr("/tmp/zanna_text_doc.scene3d"),
                                              rt_const_cstr(""));
    EXPECT_TRUE(empty && rt_result_is_err(empty) == 1,
                "SceneAsset.LoadTextResult reports empty text as Err");
}

/// @brief ADR 0190: relative prefab references resolve beside the virtual path.
static void test_model3d_load_text_result_resolves_prefabs_beside_virtual_path() {
    const char *referenced_path = "/tmp/zanna_text_prefab_ref.scene3d";
    void *referenced_scene = rt_scene3d_new();
    void *referenced_child = rt_scene_node3d_new();
    rt_scene_node3d_set_name(referenced_child, rt_const_cstr("grafted-child"));
    rt_scene3d_add(referenced_scene, referenced_child);
    EXPECT_TRUE(rt_scene3d_save(referenced_scene, rt_const_cstr(referenced_path)) == 1,
                "Referenced prefab fixture can be written");

    void *document_scene = rt_scene3d_new();
    void *instance_node = rt_scene_node3d_new();
    rt_scene_node3d_set_name(instance_node, rt_const_cstr("instance-node"));
    rt_scene3d_add(document_scene, instance_node);
    EXPECT_TRUE(rt_scene_node3d_set_prefab_reference(
                    instance_node, rt_const_cstr("zanna_text_prefab_ref.scene3d")) == 1,
                "Relative prefab references author onto document nodes");
    rt_string text = rt_scene3d_save_text(document_scene);
    EXPECT_TRUE(text && rt_str_len(text) > 0, "Prefab documents serialize through SaveToText");

    /* The referencing document never exists on disk; only the virtual path's
     * directory matters for resolving the relative reference (ADR 0190). */
    void *loaded =
        rt_model3d_load_text_result(rt_const_cstr("/tmp/zanna_text_prefab_doc.scene3d"), text);
    EXPECT_TRUE(loaded && rt_result_is_ok(loaded) == 1,
                "Text documents with relative prefab references load");
    if (loaded && rt_result_is_ok(loaded) == 1) {
        void *instance = rt_model3d_instantiate_scene(rt_result_unwrap(loaded));
        EXPECT_TRUE(instance != nullptr, "Prefab text documents instantiate scenes");
        if (instance) {
            EXPECT_TRUE(rt_scene3d_find(instance, rt_const_cstr("grafted-child")) != nullptr,
                        "Prefab content grafts against the virtual path's directory");
        }
    }
    std::remove(referenced_path);
}

static void test_model3d_roundtrips_vscn_assets() {
    const char *path = "/tmp/zanna_model3d_fixture.vscn";
    bool wrote_fixture = write_scene_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Scene fixture can be written to .vscn");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses .vscn assets");
    if (!model)
        return;
    void *model_result = rt_model3d_load_result(rt_const_cstr(path));
    EXPECT_TRUE(rt_result_is_ok(model_result) == 1, "SceneAsset.LoadResult returns Ok");
    EXPECT_TRUE(rt_model3d_get_node_count(rt_result_unwrap(model_result)) == 2,
                "SceneAsset.LoadResult unwraps a loaded asset");
    void *missing_result = rt_model3d_load_result(rt_const_cstr("missing_scene_asset_result.vscn"));
    EXPECT_TRUE(rt_result_is_err(missing_result) == 1, "SceneAsset.LoadResult returns Err");
    EXPECT_TRUE(rt_str_len(rt_result_unwrap_err_str(missing_result)) > 0,
                "SceneAsset.LoadResult Err carries a message");
    void *missing_animation = rt_model3d_load_animation_result(rt_const_cstr(path), 0);
    EXPECT_TRUE(rt_result_is_err(missing_animation) == 1,
                "SceneAsset.LoadAnimationResult returns Err for missing clips");

    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "SceneAsset deduplicates shared meshes");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "SceneAsset deduplicates shared materials");
    EXPECT_TRUE(rt_model3d_get_skeleton_count(model) == 0,
                "SceneAsset .vscn fixtures start without skeletons");
    EXPECT_TRUE(rt_model3d_get_animation_count(model) == 0,
                "SceneAsset .vscn fixtures start without animations");
    EXPECT_TRUE(rt_model3d_get_node_count(model) == 2, "SceneAsset counts imported scene nodes");

    void *template_parent = rt_model3d_find_node(model, rt_const_cstr("parent"));
    void *template_child = rt_model3d_find_node(model, rt_const_cstr("child"));
    EXPECT_TRUE(template_parent != nullptr, "SceneAsset.FindNode finds template parent nodes");
    EXPECT_TRUE(template_child != nullptr, "SceneAsset.FindNode finds template child nodes");
    void *template_child_option = rt_model3d_find_node_option(model, rt_const_cstr("child"));
    EXPECT_TRUE(rt_option_is_some(template_child_option) == 1,
                "SceneAsset.FindNodeOption returns Some for template child nodes");
    EXPECT_TRUE(rt_option_unwrap(template_child_option) == template_child,
                "SceneAsset.FindNodeOption unwraps the template child node");
    EXPECT_TRUE(rt_option_is_none(rt_model3d_find_node_option(model, rt_const_cstr("missing"))) ==
                    1,
                "SceneAsset.FindNodeOption returns None for missing nodes");
    if (!template_parent || !template_child)
        return;

    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(template_parent)),
                1.0,
                0.001,
                "SceneAsset preserves parent node translation");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(template_child)),
                5.0,
                0.001,
                "SceneAsset preserves child node translation");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_get_string(
                        template_child, rt_const_cstr("game.role"), rt_const_cstr(""))),
                    "enemy") == 0 &&
            rt_scene_node3d_metadata_get_int(template_child, rt_const_cstr("health"), -1) == 125 &&
            std::strcmp(rt_string_cstr(
                            rt_scene_node3d_metadata_kind(template_child, rt_const_cstr("radius"))),
                        "float") == 0 &&
            rt_scene_node3d_metadata_get_bool(template_child, rt_const_cstr("active"), 0) != 0 &&
            std::strcmp(rt_string_cstr(
                            rt_scene_node3d_metadata_kind(template_child, rt_const_cstr("target"))),
                        "null") == 0,
        "SceneAsset template loading preserves every typed node metadata kind");

    void *inst_root = rt_model3d_instantiate(model);
    EXPECT_TRUE(inst_root != nullptr, "SceneAsset.Instantiate clones a scene-node subtree");
    if (!inst_root)
        return;

    EXPECT_TRUE(rt_scene_node3d_child_count(inst_root) == 1,
                "SceneAsset.Instantiate returns a synthetic root with top-level children");
    void *inst_parent = rt_scene_node3d_find(inst_root, rt_const_cstr("parent"));
    void *inst_child = rt_scene_node3d_find(inst_root, rt_const_cstr("child"));
    EXPECT_TRUE(inst_parent != nullptr, "SceneAsset.Instantiate preserves named parent nodes");
    EXPECT_TRUE(inst_child != nullptr, "SceneAsset.Instantiate preserves named child nodes");
    if (!inst_parent || !inst_child)
        return;

    void *inst_root_min = rt_scene_node3d_get_aabb_min(inst_root);
    void *inst_root_max = rt_scene_node3d_get_aabb_max(inst_root);
    EXPECT_NEAR(rt_vec3_x(inst_root_min),
                0.5,
                0.001,
                "SceneAsset synthetic instance roots expose subtree AABB min X");
    EXPECT_NEAR(rt_vec3_y(inst_root_min),
                1.0,
                0.001,
                "SceneAsset synthetic instance roots expose subtree AABB min Y");
    EXPECT_NEAR(rt_vec3_z(inst_root_min),
                1.5,
                0.001,
                "SceneAsset synthetic instance roots expose subtree AABB min Z");
    EXPECT_NEAR(rt_vec3_x(inst_root_max),
                1.5,
                0.001,
                "SceneAsset synthetic instance roots expose subtree AABB max X");
    EXPECT_NEAR(rt_vec3_y(inst_root_max),
                8.0,
                0.001,
                "SceneAsset synthetic instance roots expose subtree AABB max Y");
    EXPECT_NEAR(rt_vec3_z(inst_root_max),
                4.5,
                0.001,
                "SceneAsset synthetic instance roots expose subtree AABB max Z");

    rt_scene_node3d_set_position(inst_child, 9.0, 9.0, 9.0);
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(inst_child)),
                9.0,
                0.001,
                "Instantiated nodes can be mutated independently");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(template_child)),
                5.0,
                0.001,
                "Template nodes remain unchanged after instance mutation");
    EXPECT_TRUE(rt_scene_node3d_get_mesh(inst_parent) == rt_model3d_get_mesh(model, 0),
                "Instantiated nodes reuse shared mesh objects");
    EXPECT_TRUE(rt_scene_node3d_get_material(inst_parent) == rt_model3d_get_material(model, 0),
                "Instantiated nodes reuse shared material objects");
    EXPECT_TRUE(rt_scene_node3d_metadata_get_int(inst_child, rt_const_cstr("health"), -1) == 125 &&
                    std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(
                                    inst_child, rt_const_cstr("target"))),
                                "null") == 0,
                "SceneAsset.Instantiate deep-copies typed node metadata");
    rt_scene_node3d_metadata_set_int(inst_child, rt_const_cstr("health"), 80);
    EXPECT_TRUE(rt_scene_node3d_metadata_get_int(inst_child, rt_const_cstr("health"), -1) == 80 &&
                    rt_scene_node3d_metadata_get_int(template_child, rt_const_cstr("health"), -1) ==
                        125,
                "Instantiated metadata mutates independently from the asset template");

    void *inst_scene = rt_model3d_instantiate_scene(model);
    EXPECT_TRUE(inst_scene != nullptr, "SceneAsset.InstantiateScene creates a live SceneGraph");
    if (!inst_scene)
        return;

    EXPECT_TRUE(rt_scene3d_get_node_count(inst_scene) == 3,
                "SceneAsset.InstantiateScene attaches imported nodes below the scene root");
    EXPECT_TRUE(rt_scene_node3d_child_count(rt_scene3d_get_root(inst_scene)) == 1,
                "InstantiateScene preserves top-level node grouping");
    EXPECT_TRUE(rt_scene3d_find(inst_scene, rt_const_cstr("child")) != nullptr,
                "InstantiateScene preserves node searchability");
    void *scene_child = rt_scene3d_find(inst_scene, rt_const_cstr("child"));
    EXPECT_TRUE(scene_child != nullptr &&
                    std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_get_string(
                                    scene_child, rt_const_cstr("game.role"), rt_const_cstr(""))),
                                "enemy") == 0 &&
                    std::strcmp(rt_string_cstr(rt_scene_node3d_metadata_kind(
                                    scene_child, rt_const_cstr("radius"))),
                                "float") == 0,
                "SceneAsset.InstantiateScene preserves typed gameplay metadata");

    auto *name_node = static_cast<rt_scene_node3d *>(rt_scene_node3d_new());
    const char *long_name_text = "SceneNode retained public name result beyond inline storage";
    rt_string long_name = rt_string_from_bytes(long_name_text, std::strlen(long_name_text));
    rt_scene_node3d_set_name(name_node, long_name);
    rt_string_unref(long_name);
    rt_string returned_name = rt_scene_node3d_get_name(name_node);
    EXPECT_TRUE(returned_name == name_node->name,
                "SceneNode.Name returns the stored byte-exact name");
    rt_string_unref(returned_name);
    returned_name = rt_scene_node3d_get_name(name_node);
    EXPECT_TRUE(std::strcmp(rt_string_cstr(returned_name), long_name_text) == 0,
                "Repeated SceneNode.Name reads remain valid");
    rt_string_unref(returned_name);
    if (rt_obj_release_check0(name_node))
        rt_obj_free(name_node);
}

static void test_model3d_find_node_rejects_wrong_string_handles() {
    const char *path = "/tmp/zanna_model3d_find_node_fixture.vscn";
    bool wrote_fixture = write_scene_fixture(path);
    EXPECT_TRUE(wrote_fixture, "FindNode corruption fixture can be written to .vscn");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FindNode corruption fixture");
    if (!model) {
        std::remove(path);
        return;
    }

    void *wrong_name = rt_obj_new_i64(0, 8);
    rt_obj_retain_maybe(wrong_name);
    rt_string fake_name = reinterpret_cast<rt_string>(wrong_name);
    EXPECT_TRUE(rt_model3d_find_node(model, fake_name) == nullptr,
                "SceneAsset.FindNode rejects wrong-class query string handles");

    void *template_parent = rt_model3d_find_node(model, rt_const_cstr("parent"));
    auto *parent_node = static_cast<rt_scene_node3d *>(template_parent);
    EXPECT_TRUE(parent_node != nullptr, "FindNode corruption fixture has a parent node");
    if (parent_node) {
        rt_string saved_name = parent_node->name;
        parent_node->name = fake_name;
        EXPECT_TRUE(rt_model3d_find_node(model, rt_const_cstr("parent")) == nullptr,
                    "SceneAsset.FindNode skips wrong-class stored node names");
        EXPECT_TRUE(rt_model3d_find_node(model, rt_const_cstr("child")) != nullptr,
                    "SceneAsset.FindNode keeps walking past corrupt stored node names");
        parent_node->name = saved_name;
    }

    EXPECT_TRUE(rt_obj_release_check0(wrong_name) == 0,
                "SceneAsset.FindNode string guards do not release wrong-class handles");
    if (rt_obj_release_check0(wrong_name))
        rt_obj_free(wrong_name);
    std::remove(path);
}

static void test_model3d_adapts_gltf_scene_graphs() {
    const char *path = "/tmp/zanna_model3d_fixture.gltf";
    bool wrote_fixture = write_gltf_fixture(path);
    EXPECT_TRUE(wrote_fixture, "glTF fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses glTF assets");
    if (!model)
        return;

    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "SceneAsset exposes glTF meshes");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1, "SceneAsset exposes glTF materials");
    EXPECT_TRUE(rt_model3d_get_node_count(model) == 3,
                "SceneAsset preserves logical glTF scene-node counts");
    EXPECT_TRUE(rt_model3d_get_scene_count(model) == 2,
                "SceneAsset exposes glTF active and secondary immutable scenes");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_model3d_get_scene_name(model, 0)), "default") == 0,
                "SceneAsset.GetSceneName names the default scene");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_model3d_get_scene_name(model, 1)), "SecondaryScene") == 0,
        "SceneAsset.GetSceneName preserves secondary glTF scene names");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_model3d_get_scene_name(model, 2)), "") == 0,
                "SceneAsset.GetSceneName returns empty for invalid scene indices");
    EXPECT_TRUE(rt_model3d_get_camera_count(model, 0) == 2,
                "SceneAsset.GetCameraCount reports active-scene glTF cameras");
    EXPECT_TRUE(rt_model3d_get_camera_count(model, 1) == 0,
                "SceneAsset.GetCameraCount reports no cameras for the secondary scene");
    EXPECT_TRUE(rt_model3d_get_camera_count(model, 2) == 0,
                "SceneAsset.GetCameraCount returns zero for invalid scene indices");
    void *perspective_camera = rt_model3d_get_camera(model, 0, 0);
    void *ortho_camera = rt_model3d_get_camera(model, 0, 1);
    EXPECT_TRUE(perspective_camera != nullptr,
                "SceneAsset.GetCamera returns glTF perspective cameras");
    EXPECT_TRUE(ortho_camera != nullptr, "SceneAsset.GetCamera returns glTF orthographic cameras");
    EXPECT_TRUE(rt_model3d_get_camera(model, 0, 2) == nullptr,
                "SceneAsset.GetCamera rejects out-of-range camera indices");
    EXPECT_TRUE(rt_model3d_get_camera(model, 1, 0) == nullptr,
                "SceneAsset.GetCamera returns null for secondary scenes without cameras");
    EXPECT_TRUE(rt_model3d_get_camera(model, 2, 0) == nullptr,
                "SceneAsset.GetCamera rejects invalid scene indices");
    EXPECT_TRUE(rt_camera3d_is_ortho(perspective_camera) == 0,
                "glTF perspective cameras import as perspective Camera3D handles");
    EXPECT_TRUE(rt_camera3d_is_ortho(ortho_camera) == 1,
                "glTF orthographic cameras import as orthographic Camera3D handles");
    EXPECT_NEAR(rt_camera3d_get_fov(perspective_camera),
                60.0,
                0.001,
                "glTF perspective yfov is converted from radians to degrees");
    void *perspective_pos = rt_camera3d_get_position(perspective_camera);
    void *perspective_forward = rt_camera3d_get_forward(perspective_camera);
    void *ortho_pos = rt_camera3d_get_position(ortho_camera);
    EXPECT_NEAR(
        rt_vec3_x(perspective_pos), 1.0, 0.001, "glTF camera inherits parent world translation");
    EXPECT_NEAR(
        rt_vec3_z(perspective_forward), -1.0, 0.001, "glTF camera uses local -Z as world forward");
    EXPECT_NEAR(
        rt_vec3_y(ortho_pos), 2.0, 0.001, "glTF orthographic camera preserves node translation");

    void *parent = rt_model3d_find_node(model, rt_const_cstr("GltfParent"));
    void *child = rt_model3d_find_node(model, rt_const_cstr("GltfChild"));
    EXPECT_TRUE(parent != nullptr, "SceneAsset.FindNode finds glTF parent nodes");
    EXPECT_TRUE(child != nullptr, "SceneAsset.FindNode finds glTF child nodes");
    if (!parent || !child)
        return;

    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(parent)),
                1.0,
                0.001,
                "SceneAsset preserves glTF node translations");
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_scale(child)),
                2.0,
                0.001,
                "SceneAsset preserves glTF node scales");

    void *inst_scene = rt_model3d_instantiate_scene(model);
    EXPECT_TRUE(inst_scene != nullptr, "SceneAsset.InstantiateScene works for glTF assets");
    if (!inst_scene)
        return;

    EXPECT_TRUE(rt_scene3d_get_node_count(inst_scene) == 4,
                "glTF-backed SceneAsset instances attach below a new scene root");
    EXPECT_TRUE(rt_scene3d_find(inst_scene, rt_const_cstr("GltfChild")) != nullptr,
                "glTF-backed SceneAsset instances preserve child names");

    void *indexed_scene = rt_model3d_instantiate_scene_at(model, 0);
    EXPECT_TRUE(indexed_scene != nullptr, "SceneAsset.InstantiateSceneAt clones the default scene");
    void *secondary_scene = rt_model3d_instantiate_scene_at(model, 1);
    EXPECT_TRUE(secondary_scene != nullptr,
                "SceneAsset.InstantiateSceneAt clones secondary glTF scenes");
    EXPECT_TRUE(rt_model3d_instantiate_scene_at(model, 2) == nullptr,
                "SceneAsset.InstantiateSceneAt rejects invalid scene indices");
    if (!indexed_scene)
        return;
    EXPECT_TRUE(rt_scene3d_get_node_count(indexed_scene) == 4,
                "SceneAsset.InstantiateSceneAt preserves default-scene nodes");
    EXPECT_TRUE(rt_scene3d_find(indexed_scene, rt_const_cstr("GltfParent")) != nullptr,
                "SceneAsset.InstantiateSceneAt preserves indexed-scene searchability");
    EXPECT_TRUE(rt_scene3d_find(indexed_scene, rt_const_cstr("GltfSecondary")) == nullptr,
                "SceneAsset.InstantiateSceneAt keeps secondary roots out of the default scene");
    if (!secondary_scene)
        return;
    EXPECT_TRUE(rt_scene3d_get_node_count(secondary_scene) == 2,
                "SceneAsset.InstantiateSceneAt builds secondary scene roots");
    EXPECT_TRUE(rt_scene3d_find(secondary_scene, rt_const_cstr("GltfSecondary")) != nullptr,
                "SceneAsset.InstantiateSceneAt preserves secondary scene searchability");
    EXPECT_TRUE(rt_scene3d_find(secondary_scene, rt_const_cstr("GltfParent")) == nullptr,
                "SceneAsset.InstantiateSceneAt keeps default roots out of the secondary scene");
}

static void test_model3d_rejects_gltf_accessor_overrun_of_buffer_view() {
    const char *path = "/tmp/zanna_model3d_accessor_overrun.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
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
                            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                            "\"nodes\":[{\"mesh\":0}],"
                            "\"scenes\":[{\"nodes\":[0]}],"
                            "\"scene\":0"
                            "}";

    EXPECT_TRUE(write_text_file(path, gltf_json), "Accessor-overrun glTF fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model == nullptr || rt_model3d_get_mesh_count(model) == 0,
                "glTF accessors cannot read past their declared bufferView byteLength");
}

static void test_gltf_asset_accessors_clamp_corrupt_counts() {
    const char *path = "/tmp/zanna_gltf_asset_corrupt_counts.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    for (float v : positions)
        append_bytes(gltf_buffer, v);

    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
        std::to_string(gltf_buffer.size()) +
        "}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0,0,1]}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
        "\"nodes\":[{\"name\":\"Triangle\",\"mesh\":0}],"
        "\"scenes\":[{\"name\":\"Main\",\"nodes\":[0]}],"
        "\"scene\":0"
        "}";

    EXPECT_TRUE(write_text_file(path, gltf_json),
                "glTF asset corrupt-count fixture can be written");
    void *asset = rt_gltf_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "Direct glTF.Load parses corrupt-count fixture");
    if (!asset) {
        std::remove(path);
        return;
    }

    auto *view = static_cast<GltfAssetView *>(asset);
    EXPECT_TRUE(rt_gltf_mesh_count(asset) == 1, "glTF asset starts with one mesh");
    EXPECT_TRUE(rt_gltf_material_count(asset) == 1, "glTF asset starts with one material");
    EXPECT_TRUE(rt_gltf_scene_count(asset) == 1, "glTF asset starts with one scene");
    EXPECT_TRUE(rt_gltf_node_count(asset) == 1, "glTF asset starts with one scene node");
    EXPECT_TRUE(rt_gltf_get_scene_root(asset) != nullptr, "glTF asset starts with a scene root");

    void *wrong_material = rt_material3d_new_color(0.2, 0.3, 0.4);
    void *wrong_mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
    void *valid_camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    EXPECT_TRUE(wrong_material != nullptr, "Wrong-class material fixture object is created");
    EXPECT_TRUE(wrong_mesh != nullptr, "Wrong-class mesh fixture object is created");
    EXPECT_TRUE(valid_camera != nullptr, "Valid camera fixture object is created");

    void **saved_meshes = view->meshes;
    view->meshes = nullptr;
    view->mesh_count = 1;
    view->mesh_capacity = 1;
    EXPECT_TRUE(rt_gltf_mesh_count(asset) == 0, "glTF mesh count rejects missing storage");
    view->meshes = saved_meshes;
    view->mesh_count = 99;
    view->mesh_capacity = 1;
    EXPECT_TRUE(rt_gltf_mesh_count(asset) == 1, "glTF mesh count clamps corrupt count");
    EXPECT_TRUE(rt_gltf_get_mesh(asset, 1) == nullptr,
                "glTF mesh accessor rejects indexes past repaired count");
    void *saved_mesh = view->meshes[0];
    view->meshes[0] = wrong_material;
    EXPECT_TRUE(rt_gltf_get_mesh(asset, 0) == nullptr,
                "glTF mesh accessor rejects wrong-class mesh slots");
    view->meshes[0] = saved_mesh;

    void **saved_materials = view->materials;
    view->materials = nullptr;
    view->material_count = 1;
    view->material_capacity = 1;
    EXPECT_TRUE(rt_gltf_material_count(asset) == 0, "glTF material count rejects missing storage");
    view->materials = saved_materials;
    view->material_count = 99;
    view->material_capacity = 1;
    EXPECT_TRUE(rt_gltf_material_count(asset) == 1, "glTF material count clamps corrupt count");
    EXPECT_TRUE(rt_gltf_get_material(asset, 1) == nullptr,
                "glTF material accessor rejects indexes past repaired count");
    void *saved_material = view->materials[0];
    view->materials[0] = wrong_mesh;
    EXPECT_TRUE(rt_gltf_get_material(asset, 0) == nullptr,
                "glTF material accessor rejects wrong-class material slots");
    view->materials[0] = saved_material;

    view->skeletons = static_cast<void **>(std::calloc(1, sizeof(void *)));
    if (view->skeletons) {
        view->skeletons[0] = rt_skeleton3d_new();
        view->skeleton_count = 99;
        view->skeleton_capacity = 1;
        EXPECT_TRUE(rt_gltf_skeleton_count(asset) == 1, "glTF skeleton count clamps corrupt count");
        EXPECT_TRUE(rt_gltf_get_skeleton(asset, 1) == nullptr,
                    "glTF skeleton accessor rejects indexes past repaired count");
        void *saved_skeleton = view->skeletons[0];
        view->skeletons[0] = wrong_material;
        EXPECT_TRUE(rt_gltf_get_skeleton(asset, 0) == nullptr,
                    "glTF skeleton accessor rejects wrong-class skeleton slots");
        view->skeletons[0] = saved_skeleton;
    }

    view->animations = static_cast<void **>(std::calloc(1, sizeof(void *)));
    if (view->animations) {
        view->animations[0] = rt_animation3d_new(rt_const_cstr("clip"), 1.0);
        view->animation_count = 99;
        view->animation_capacity = 1;
        EXPECT_TRUE(rt_gltf_animation_count(asset) == 1,
                    "glTF animation count clamps corrupt count");
        EXPECT_TRUE(rt_gltf_get_animation(asset, 1) == nullptr,
                    "glTF animation accessor rejects indexes past repaired count");
        void *saved_animation = view->animations[0];
        view->animations[0] = wrong_material;
        EXPECT_TRUE(rt_gltf_get_animation(asset, 0) == nullptr,
                    "glTF animation accessor rejects wrong-class animation slots");
        view->animations[0] = saved_animation;
    }

    view->node_animations = static_cast<void **>(std::calloc(1, sizeof(void *)));
    if (view->node_animations) {
        view->node_animations[0] = rt_node_animation3d_new(rt_const_cstr("node_clip"), 1.0);
        view->node_animation_count = 99;
        view->node_animation_capacity = 1;
        EXPECT_TRUE(rt_gltf_node_animation_count(asset) == 1,
                    "glTF node-animation count clamps corrupt count");
        EXPECT_TRUE(rt_gltf_get_node_animation(asset, 1) == nullptr,
                    "glTF node-animation accessor rejects indexes past repaired count");
        void *saved_node_animation = view->node_animations[0];
        view->node_animations[0] = wrong_material;
        EXPECT_TRUE(rt_gltf_get_node_animation(asset, 0) == nullptr,
                    "glTF node-animation accessor rejects wrong-class node-animation slots");
        view->node_animations[0] = saved_node_animation;
    }

    void **saved_cameras = view->cameras;
    int32_t saved_camera_count = view->camera_count;
    int32_t saved_camera_capacity = view->camera_capacity;
    void *camera_slots[1] = {valid_camera};
    view->cameras = camera_slots;
    view->camera_count = 99;
    view->camera_capacity = 1;
    EXPECT_TRUE(rt_gltf_camera_count(asset) == 1, "glTF camera count clamps corrupt count");
    EXPECT_TRUE(rt_gltf_get_camera(asset, 0) == valid_camera,
                "glTF camera accessor preserves valid camera slots");
    EXPECT_TRUE(rt_gltf_get_camera(asset, 1) == nullptr,
                "glTF camera accessor rejects indexes past repaired count");
    camera_slots[0] = wrong_material;
    EXPECT_TRUE(rt_gltf_get_camera(asset, 0) == nullptr,
                "glTF camera accessor rejects wrong-class camera slots");
    view->cameras = saved_cameras;
    view->camera_count = saved_camera_count;
    view->camera_capacity = saved_camera_capacity;

    view->scene_count = 99;
    view->scene_capacity = 1;
    EXPECT_TRUE(rt_gltf_scene_count(asset) == 1, "glTF scene count clamps corrupt count");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_gltf_get_scene_name(asset, 1)), "") == 0,
                "glTF scene-name accessor rejects indexes past repaired count");
    EXPECT_TRUE(rt_gltf_get_scene_root_at(asset, 1) == nullptr,
                "glTF scene-root accessor rejects indexes past repaired count");
    EXPECT_TRUE(rt_gltf_scene_camera_count(asset, 1) == 0,
                "glTF scene-camera count rejects invalid scene index");
    void *saved_scene_root_at = view->scenes[0].root;
    view->scenes[0].root = wrong_mesh;
    EXPECT_TRUE(rt_gltf_get_scene_root_at(asset, 0) == nullptr,
                "glTF scene-root accessor rejects wrong-class scene roots");
    view->scenes[0].root = saved_scene_root_at;

    void **saved_scene_cameras = view->scenes[0].cameras;
    int32_t saved_scene_camera_count = view->scenes[0].camera_count;
    int32_t saved_scene_camera_capacity = view->scenes[0].camera_capacity;
    void *scene_camera_slots[1] = {valid_camera};
    view->scenes[0].cameras = scene_camera_slots;
    view->scenes[0].camera_count = 99;
    view->scenes[0].camera_capacity = 1;
    EXPECT_TRUE(rt_gltf_scene_camera_count(asset, 0) == 1,
                "glTF scene-camera count clamps corrupt count");
    EXPECT_TRUE(rt_gltf_get_scene_camera(asset, 0, 0) == valid_camera,
                "glTF scene-camera accessor preserves valid camera slots");
    EXPECT_TRUE(rt_gltf_get_scene_camera(asset, 0, 1) == nullptr,
                "glTF scene-camera accessor rejects indexes past repaired count");
    scene_camera_slots[0] = wrong_material;
    EXPECT_TRUE(rt_gltf_get_scene_camera(asset, 0, 0) == nullptr,
                "glTF scene-camera accessor rejects wrong-class camera slots");
    view->scenes[0].cameras = saved_scene_cameras;
    view->scenes[0].camera_count = saved_scene_camera_count;
    view->scenes[0].camera_capacity = saved_scene_camera_capacity;

    int32_t saved_node_count = view->node_count;
    void *saved_scene_root = view->scene_root;
    view->node_count = -7;
    EXPECT_TRUE(rt_gltf_node_count(asset) == 0, "glTF node count rejects negative counts");
    view->node_count = saved_node_count;
    view->scene_root = wrong_mesh;
    EXPECT_TRUE(rt_gltf_node_count(asset) == 0,
                "glTF node count rejects wrong-class active scene roots");
    EXPECT_TRUE(rt_gltf_get_scene_root(asset) == nullptr,
                "glTF active scene-root accessor rejects wrong-class roots");
    view->scene_root = saved_scene_root;

    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);
    if (valid_camera && rt_obj_release_check0(valid_camera))
        rt_obj_free(valid_camera);
    if (wrong_mesh && rt_obj_release_check0(wrong_mesh))
        rt_obj_free(wrong_mesh);
    if (wrong_material && rt_obj_release_check0(wrong_material))
        rt_obj_free(wrong_material);
    std::remove(path);
}

static void test_model3d_load_asset_resolves_mounted_gltf_dependencies() {
    const char *pack_path = "/tmp/zanna_model3d_asset_pack.zpak";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f, 0.0f, 7.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};

    for (float v : positions)
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
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]"
        "}";

    zanna::asset::ZpakWriter writer;
    writer.addEntry("assets/models/model.gltf",
                    reinterpret_cast<const uint8_t *>(gltf_json.data()),
                    gltf_json.size(),
                    false);
    writer.addEntry("assets/models/buffers/tri.bin", gltf_buffer.data(), gltf_buffer.size(), false);
    std::string err;
    bool wrote_pack = writer.writeToFile(pack_path, err);
    EXPECT_TRUE(wrote_pack, "SceneAsset asset pack can be written");
    if (!wrote_pack)
        return;
    bool mounted = rt_asset_mount(rt_const_cstr(pack_path)) == 1;
    EXPECT_TRUE(mounted, "SceneAsset asset pack can mount");
    if (!mounted)
        return;

    void *model = rt_model3d_load_asset(rt_const_cstr("assets/models/model.gltf"));
    EXPECT_TRUE(model != nullptr, "SceneAsset.LoadAsset loads a mounted glTF model path");
    if (model) {
        EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1,
                    "SceneAsset.LoadAsset exposes meshes loaded from mounted dependencies");
        auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                    "SceneAsset.LoadAsset imports geometry from a package-relative buffer");
        if (mesh) {
            EXPECT_NEAR(mesh->vertices[1].pos[0],
                        6.0,
                        0.001,
                        "SceneAsset.LoadAsset keeps mounted buffer vertex X");
            EXPECT_NEAR(mesh->vertices[2].pos[1],
                        7.0,
                        0.001,
                        "SceneAsset.LoadAsset keeps mounted buffer vertex Y");
        }
    }

    rt_asset_unmount(rt_const_cstr(pack_path));
    std::remove(pack_path);
}

static void test_model3d_load_asset_diagnostics_name_missing_dependency() {
    const char *pack_path = "/tmp/zanna_model3d_missing_dep_pack.zpak";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"missing.bin\",\"byteLength\":12}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]"
        "}";

    zanna::asset::ZpakWriter writer;
    writer.addEntry("assets/models/missing_dep.gltf",
                    reinterpret_cast<const uint8_t *>(gltf_json.data()),
                    gltf_json.size(),
                    false);
    std::string err;
    bool wrote_pack = writer.writeToFile(pack_path, err);
    EXPECT_TRUE(wrote_pack, "Missing-dependency asset pack can be written");
    if (!wrote_pack)
        return;
    bool mounted = rt_asset_mount(rt_const_cstr(pack_path)) == 1;
    EXPECT_TRUE(mounted, "Missing-dependency asset pack can mount");
    if (!mounted)
        return;

    void *model = rt_model3d_load_asset(rt_const_cstr("asset://assets/models/missing_dep.gltf"));
    EXPECT_TRUE(model == nullptr,
                "SceneAsset.LoadAsset returns null for missing glTF dependencies");
    EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                "SceneAsset.LoadAsset records an error for missing glTF dependencies");
    EXPECT_TRUE(
        std::strstr(rt_asset_error_get_message(), "assets/models/missing_dep.gltf") != nullptr &&
            std::strstr(rt_asset_error_get_message(), "assets/models/missing.bin") != nullptr,
        "SceneAsset.LoadAsset diagnostics name the model and missing dependency");

    rt_asset_unmount(rt_const_cstr(pack_path));
    std::remove(pack_path);
}

static void test_model3d_load_asset_resolves_vscn_obj_and_stl_packages() {
    const char *pack_path = "/tmp/zanna_model3d_native_asset_pack.zpak";
    const char *vscn_path = "/tmp/zanna_model3d_native_asset_scene.vscn";
    const char *png_path = "/tmp/zanna_model3d_native_asset_texture.png";
    std::vector<uint8_t> vscn_bytes;
    std::vector<uint8_t> png_bytes;
    const std::string obj = "mtllib materials/packed.mtl\n"
                            "v 0 0 0\n"
                            "v 2 0 0\n"
                            "v 0 3 0\n"
                            "vt 0 0\n"
                            "vt 1 0\n"
                            "vt 0 1\n"
                            "usemtl Packed\n"
                            "f 1/1 2/2 3/3\n";
    const std::string mtl = "newmtl Packed\n"
                            "Kd 0.25 0.5 0.75\n"
                            "map_Kd textures/albedo.png\n";
    const std::string stl = "solid packed\n"
                            "facet normal 0 0 1\n"
                            "outer loop\n"
                            "vertex 0 0 0\n"
                            "vertex 4 0 0\n"
                            "vertex 0 5 0\n"
                            "endloop\n"
                            "endfacet\n"
                            "endsolid packed\n";

    EXPECT_TRUE(write_scene_fixture(vscn_path), "Packaged VSCN fixture can be written");
    EXPECT_TRUE(read_binary_file(vscn_path, vscn_bytes), "Packaged VSCN fixture can be read");
    void *pixels = rt_pixels_new(1, 1);
    EXPECT_TRUE(pixels != nullptr, "Packaged OBJ texture fixture can allocate pixels");
    if (pixels) {
        rt_pixels_set(pixels, 0, 0, 0x3366CCFFll);
        EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                    "Packaged OBJ texture fixture can be written");
        if (rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
    }
    EXPECT_TRUE(read_binary_file(png_path, png_bytes), "Packaged OBJ texture fixture can be read");

    zanna::asset::ZpakWriter writer;
    writer.addEntry("assets/scenes/packed.vscn", vscn_bytes.data(), vscn_bytes.size(), false);
    writer.addEntry("assets/models/packed.obj",
                    reinterpret_cast<const uint8_t *>(obj.data()),
                    obj.size(),
                    false);
    writer.addEntry("assets/models/materials/packed.mtl",
                    reinterpret_cast<const uint8_t *>(mtl.data()),
                    mtl.size(),
                    false);
    writer.addEntry(
        "assets/models/materials/textures/albedo.png", png_bytes.data(), png_bytes.size(), false);
    writer.addEntry("assets/models/packed.stl",
                    reinterpret_cast<const uint8_t *>(stl.data()),
                    stl.size(),
                    false);
    std::string err;
    bool wrote_pack = writer.writeToFile(pack_path, err);
    EXPECT_TRUE(wrote_pack, "Native SceneAsset asset pack can be written");
    std::remove(vscn_path);
    std::remove(png_path);
    if (!wrote_pack)
        return;
    bool mounted = rt_asset_mount(rt_const_cstr(pack_path)) == 1;
    EXPECT_TRUE(mounted, "Native SceneAsset asset pack can mount");
    if (!mounted) {
        std::remove(pack_path);
        return;
    }

    void *vscn = rt_model3d_load_asset(rt_const_cstr("asset://assets/scenes/packed.vscn"));
    EXPECT_TRUE(vscn != nullptr, "SceneAsset.LoadAsset loads mounted VSCN bytes");
    if (vscn) {
        EXPECT_TRUE(rt_model3d_get_mesh_count(vscn) == 1,
                    "Mounted VSCN exposes its deduplicated mesh");
        EXPECT_TRUE(rt_model3d_find_node(vscn, rt_const_cstr("parent")) != nullptr,
                    "Mounted VSCN preserves its scene hierarchy");
        if (rt_obj_release_check0(vscn))
            rt_obj_free(vscn);
    }

    void *obj_model = rt_model3d_load_asset(rt_const_cstr("assets/models/packed.obj"));
    EXPECT_TRUE(obj_model != nullptr, "SceneAsset.LoadAsset loads mounted OBJ bytes");
    if (obj_model) {
        EXPECT_TRUE(rt_model3d_get_mesh_count(obj_model) == 1,
                    "Mounted OBJ exposes geometry from its root asset");
        auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(obj_model, 0));
        auto *material = static_cast<rt_material3d *>(rt_model3d_get_material(obj_model, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                    "Mounted OBJ preserves triangle geometry");
        EXPECT_TRUE(material != nullptr && material->diffuse[1] > 0.49f,
                    "Mounted OBJ resolves its package-relative MTL library");
        EXPECT_TRUE(material != nullptr && rt_material3d_get_has_texture(material) == 1,
                    "Mounted OBJ resolves its package-relative MTL texture");
        if (rt_obj_release_check0(obj_model))
            rt_obj_free(obj_model);
    }

    void *stl_model = rt_model3d_load_asset(rt_const_cstr("asset://assets/models/packed.stl"));
    EXPECT_TRUE(stl_model != nullptr, "SceneAsset.LoadAsset loads mounted STL bytes");
    if (stl_model) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(stl_model, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                    "Mounted STL preserves triangle geometry");
        if (rt_obj_release_check0(stl_model))
            rt_obj_free(stl_model);
    }

    rt_asset_unmount(rt_const_cstr(pack_path));
    std::remove(pack_path);
}

static void test_model3d_adapts_fbx_scene_graphs() {
    const char *path = "/tmp/zanna_model3d_fixture.fbx";
    bool wrote_fixture = write_fbx_fixture(path);
    EXPECT_TRUE(wrote_fixture, "FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses generated FBX assets");
    if (!model)
        return;

    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "SceneAsset exposes FBX meshes");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1, "SceneAsset exposes FBX materials");
    EXPECT_TRUE(rt_model3d_get_node_count(model) == 2,
                "SceneAsset preserves logical FBX scene-node counts");

    void *parent = rt_model3d_find_node(model, rt_const_cstr("Parent"));
    void *child = rt_model3d_find_node(model, rt_const_cstr("Child"));
    EXPECT_TRUE(parent != nullptr, "SceneAsset.FindNode finds FBX parent nodes");
    EXPECT_TRUE(child != nullptr, "SceneAsset.FindNode finds FBX child nodes");
    if (!parent || !child)
        return;

    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(parent)),
                1.0,
                0.001,
                "SceneAsset preserves FBX parent translations");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(parent)),
                2.0,
                0.001,
                "SceneAsset preserves FBX parent Y translations");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(child)),
                5.0,
                0.001,
                "SceneAsset preserves FBX child translations");
    EXPECT_TRUE(rt_scene_node3d_get_mesh(child) == rt_model3d_get_mesh(model, 0),
                "FBX scene nodes reuse the extracted mesh object");
    EXPECT_TRUE(rt_scene_node3d_get_material(child) == rt_model3d_get_material(model, 0),
                "FBX scene nodes reuse the extracted material object");

    void *inst_scene = rt_model3d_instantiate_scene(model);
    EXPECT_TRUE(inst_scene != nullptr, "SceneAsset.InstantiateScene works for FBX assets");
    if (!inst_scene)
        return;

    EXPECT_TRUE(rt_scene3d_get_node_count(inst_scene) == 3,
                "FBX-backed SceneAsset instances attach below a new scene root");
    EXPECT_TRUE(rt_scene_node3d_child_count(rt_scene3d_get_root(inst_scene)) == 1,
                "FBX-backed SceneAsset instances preserve top-level grouping");
    EXPECT_TRUE(rt_scene3d_find(inst_scene, rt_const_cstr("Child")) != nullptr,
                "FBX-backed SceneAsset instances preserve child names");
}

static void test_model3d_imports_fbx_nodes_with_many_properties() {
    const char *path = "/tmp/zanna_model3d_many_properties.fbx";
    bool wrote_fixture = write_fbx_many_property_node_fixture(path);
    EXPECT_TRUE(wrote_fixture, "FBX many-property fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load accepts FBX nodes with many direct properties");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1,
                "Many-property FBX fixture still imports geometry");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "Many-property FBX fixture still imports materials");
}

static void test_model3d_loads_preloaded_fbx_bytes() {
    const char *path = "/tmp/zanna_model3d_preloaded_fixture.fbx";
    std::vector<uint8_t> bytes;
    bool wrote_fixture = write_fbx_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Preloaded FBX fixture can be written");
    if (!wrote_fixture)
        return;
    EXPECT_TRUE(read_binary_file(path, bytes), "Preloaded FBX fixture can be read");
    if (bytes.empty())
        return;
    uint8_t *owned = static_cast<uint8_t *>(std::malloc(bytes.size()));
    EXPECT_TRUE(owned != nullptr, "Preloaded FBX byte buffer can be allocated");
    if (!owned)
        return;
    std::memcpy(owned, bytes.data(), bytes.size());

    void *model = rt_model3d_load_preloaded_fbx(rt_const_cstr(path), owned, bytes.size(), 0);
    EXPECT_TRUE(model != nullptr, "SceneAsset.LoadPreloadedFBX parses staged FBX bytes");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "Preloaded FBX byte path imports meshes");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "Preloaded FBX byte path imports materials");
    EXPECT_TRUE(rt_model3d_find_node(model, rt_const_cstr("Child")) != nullptr,
                "Preloaded FBX byte path preserves scene nodes");
}

static void test_model3d_loads_obj_as_template_asset() {
    const char *path = "/tmp/zanna_model3d_fixture.obj";
    const char *obj = "# simple indexed triangle\n"
                      "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "vt 0 0\n"
                      "vt 1 0\n"
                      "vt 0 1\n"
                      "vn 0 0 1\n"
                      "f 1/1/1 2/2/1 3/3/1\n";
    FILE *f = std::fopen(path, "wb");
    bool wrote_fixture = f && std::fwrite(obj, 1, std::strlen(obj), f) == std::strlen(obj);
    if (f)
        std::fclose(f);
    EXPECT_TRUE(wrote_fixture, "OBJ fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses OBJ assets");
    if (!model)
        return;

    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "OBJ-backed SceneAsset exposes one mesh");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "OBJ-backed SceneAsset creates a default material");
    EXPECT_TRUE(rt_model3d_get_node_count(model) == 1,
                "OBJ-backed SceneAsset synthesizes one template node");

    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                "OBJ-backed SceneAsset preserves imported mesh geometry");

    void *node = rt_model3d_find_node(model, rt_const_cstr("mesh_0"));
    EXPECT_TRUE(node != nullptr, "OBJ-backed SceneAsset names the synthesized mesh node");
    if (node) {
        EXPECT_TRUE(rt_scene_node3d_get_mesh(node) == rt_model3d_get_mesh(model, 0),
                    "OBJ synthesized node reuses the imported mesh");
        EXPECT_TRUE(rt_scene_node3d_get_material(node) == rt_model3d_get_material(model, 0),
                    "OBJ synthesized node uses the generated material");
    }
}

static void test_model3d_preserves_obj_mtl_material_groups() {
    const char *obj_path = "/tmp/zanna_model3d_mtl_groups.obj";
    const char *mtl_path = "/tmp/zanna_model3d_mtl_groups.mtl";
    const char *mtl = "newmtl Red\n"
                      "Kd 0.9 0.1 0.1\n"
                      "newmtl Blue\n"
                      "Kd 0.1 0.2 0.9\n";
    const char *obj = "mtllib zanna_model3d_mtl_groups.mtl\n"
                      "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "v 0 0 1\n"
                      "vn 0 0 1\n"
                      "usemtl Red\n"
                      "f 1//1 2//1 3//1\n"
                      "usemtl Blue\n"
                      "f 1//1 3//1 4//1\n";
    EXPECT_TRUE(write_text_file(mtl_path, mtl), "OBJ MTL fixture can be written");
    EXPECT_TRUE(write_text_file(obj_path, obj), "OBJ usemtl fixture can be written");

    void *model = rt_model3d_load(rt_const_cstr(obj_path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses OBJ assets with mtllib/usemtl");
    if (!model)
        return;

    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 2,
                "OBJ usemtl groups become separate renderable meshes");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 2,
                "OBJ mtllib materials are imported as Material3D handles");
    EXPECT_TRUE(rt_model3d_get_node_count(model) == 2,
                "OBJ material groups become separate template nodes");

    void *red_node = rt_model3d_find_node(model, rt_const_cstr("Red"));
    void *blue_node = rt_model3d_find_node(model, rt_const_cstr("Blue"));
    EXPECT_TRUE(red_node != nullptr && blue_node != nullptr,
                "OBJ material group nodes preserve usemtl names");
    if (!red_node || !blue_node)
        return;
    auto *red_mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(red_node));
    auto *blue_mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(blue_node));
    auto *red_mat = static_cast<rt_material3d *>(rt_scene_node3d_get_material(red_node));
    auto *blue_mat = static_cast<rt_material3d *>(rt_scene_node3d_get_material(blue_node));
    EXPECT_TRUE(red_mesh && red_mesh->index_count == 3,
                "OBJ Red group contains only its own triangle");
    EXPECT_TRUE(blue_mesh && blue_mesh->index_count == 3,
                "OBJ Blue group contains only its own triangle");
    EXPECT_TRUE(red_mat && blue_mat, "OBJ material group nodes have materials");
    if (!red_mat || !blue_mat)
        return;
    EXPECT_NEAR(red_mat->diffuse[0], 0.9, 0.001, "OBJ MTL Kd imports red material color");
    EXPECT_NEAR(blue_mat->diffuse[2], 0.9, 0.001, "OBJ MTL Kd imports blue material color");
}

static void test_model3d_imports_obj_mtl_texture_maps() {
    const char *obj_path = "/tmp/zanna_model3d_mtl_texture.obj";
    const char *mtl_a_path = "/tmp/zanna_model3d_mtl_texture_a.mtl";
    const char *mtl_b_path = "/tmp/zanna_model3d_mtl_texture_b.mtl";
    const char *png_path = "/tmp/zanna_model3d_mtl_texture.png";
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0xFF8844FFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "OBJ texture PNG fixture can be written");

    const char *mtl_a = "newmtl Unused\nKd 0.2 0.2 0.2\n";
    const char *mtl_b = "newmtl Textured\n"
                        "Kd 1.0 1.0 1.0\n"
                        "map_Kd zanna_model3d_mtl_texture.png\n"
                        "map_Bump zanna_model3d_mtl_texture.png\n";
    const char *obj = "mtllib zanna_model3d_mtl_texture_a.mtl "
                      "zanna_model3d_mtl_texture_b.mtl\n"
                      "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "vt 0 0\n"
                      "vt 1 0\n"
                      "vt 0 1\n"
                      "usemtl Textured\n"
                      "f 1/1 2/2 3/3\n";
    EXPECT_TRUE(write_text_file(mtl_a_path, mtl_a), "First OBJ MTL library can be written");
    EXPECT_TRUE(write_text_file(mtl_b_path, mtl_b), "Second OBJ MTL library can be written");
    EXPECT_TRUE(write_text_file(obj_path, obj), "OBJ multi-mtllib texture fixture can be written");

    void *model = rt_model3d_load(rt_const_cstr(obj_path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses OBJ assets with texture maps");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "OBJ MTL texture fixture imports the referenced material");
    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(mat != nullptr && rt_material3d_get_has_texture(mat) == 1,
                "OBJ MTL map_Kd imports a diffuse texture");
    EXPECT_TRUE(mat != nullptr && rt_material3d_get_has_normal_map(mat) == 1,
                "OBJ MTL map_Bump imports a normal texture");
}

static void test_model3d_imports_quoted_obj_mtl_references() {
    const char *obj_path = "/tmp/zanna_model3d quoted refs.obj";
    const char *mtl_path = "/tmp/zanna model3d quoted refs.mtl";
    const char *png_path = "/tmp/zanna model3d quoted texture.png";
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x7799BBFFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "Quoted OBJ texture PNG fixture can be written");

    std::string mtl = "newmtl \"Quoted Red\"\n"
                      "Kd 0.7 0.1 0.2\n"
                      "map_Kd -o 0 0 \"zanna model3d quoted texture.png\"\n";
    std::string obj = "mtllib \"zanna model3d quoted refs.mtl\"\n"
                      "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "vt 0 0\n"
                      "vt 1 0\n"
                      "vt 0 1\n"
                      "usemtl \"Quoted Red\"\n"
                      "f 1/1 2/2 3/3\n";
    EXPECT_TRUE(write_text_file(mtl_path, mtl), "Quoted OBJ MTL file can be written");
    EXPECT_TRUE(write_text_file(obj_path, obj), "Quoted OBJ file can be written");

    void *model = rt_model3d_load(rt_const_cstr(obj_path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load accepts quoted OBJ/MTL references");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1,
                "Quoted OBJ material group produces one mesh");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "Quoted OBJ material name resolves against quoted newmtl");
    void *node = rt_model3d_find_node(model, rt_const_cstr("Quoted Red"));
    EXPECT_TRUE(node != nullptr, "Quoted usemtl name is preserved on the template node");
    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(mat != nullptr && rt_material3d_get_has_texture(mat) == 1,
                "Quoted MTL texture path with map options imports the texture");
}

static void test_model3d_sanitizes_obj_mtl_values_and_rejects_uri_maps() {
    const char *obj_path = "/tmp/zanna_model3d_mtl_sanitize.obj";
    const char *mtl_path = "/tmp/zanna_model3d_mtl_sanitize.mtl";
    const char *unsafe_texture_ref = "file:zanna_model3d_mtl_unsafe.png";
    const char *normal_png_path = "/tmp/zanna_model3d_mtl_norm.png";
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x4488FFFFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(normal_png_path)) == 1,
                "OBJ norm texture PNG fixture can be written");

    const char *mtl = "newmtl Clamp\n"
                      "Kd 2.0 -1.0 0.5\n"
                      "Ks 2.0 -0.5 0.25\n"
                      "d 1.5\n"
                      "map_Kd file:zanna_model3d_mtl_unsafe.png\n"
                      "norm zanna_model3d_mtl_norm.png\n";
    const char *obj = "mtllib zanna_model3d_mtl_sanitize.mtl\n"
                      "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "vn 0 0 1\n"
                      "usemtl Clamp\n"
                      "f 1//1 2//1 3//1\n";
    EXPECT_TRUE(write_text_file(mtl_path, mtl), "Sanitized OBJ MTL fixture can be written");
    EXPECT_TRUE(write_text_file(obj_path, obj), "Sanitized OBJ fixture can be written");

    void *model = rt_model3d_load(rt_const_cstr(obj_path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses OBJ assets with clamped MTL values");
    if (!model)
        return;
    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(mat != nullptr, "OBJ MTL sanitize fixture imports a material");
    if (!mat)
        return;
    EXPECT_NEAR(mat->diffuse[0], 1.0, 0.001, "OBJ MTL Kd clamps high diffuse values");
    EXPECT_NEAR(mat->diffuse[1], 0.0, 0.001, "OBJ MTL Kd clamps low diffuse values");
    EXPECT_NEAR(mat->diffuse[2], 0.5, 0.001, "OBJ MTL Kd preserves finite diffuse values");
    EXPECT_NEAR(mat->specular[0], 1.0, 0.001, "OBJ MTL Ks clamps high specular values");
    EXPECT_NEAR(mat->specular[1], 0.0, 0.001, "OBJ MTL Ks clamps low specular values");
    EXPECT_NEAR(mat->specular[2], 0.25, 0.001, "OBJ MTL Ks preserves finite specular values");
    EXPECT_NEAR(mat->alpha, 1.0, 0.001, "OBJ MTL d clamps alpha values");
    EXPECT_TRUE(rt_material3d_get_has_texture(mat) == 0,
                "OBJ MTL rejects URI-scheme texture references");
    EXPECT_TRUE(asset_warning_contains("unsafe relative path", unsafe_texture_ref),
                "OBJ MTL unsafe texture URI reports an unsafe-path warning");
    EXPECT_TRUE(rt_material3d_get_has_normal_map(mat) == 1,
                "OBJ MTL norm imports a normal texture");
}

static void test_model3d_preserves_empty_gltf_scene_without_synth_nodes() {
    const char *path = "/tmp/zanna_model3d_empty_scene.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(gltf_buffer.size()) +
        "}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
        std::to_string(gltf_buffer.size()) +
        "}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"nodes\":[{\"name\":\"HiddenMesh\",\"mesh\":0}],"
        "\"scenes\":[{\"name\":\"Empty\",\"nodes\":[]}],"
        "\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(path, gltf_json), "Empty-scene glTF fixture can be written");

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load parses glTF assets with an explicit empty scene");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1,
                "SceneAsset retains meshes that are outside an empty glTF scene");
    EXPECT_TRUE(rt_model3d_get_scene_count(model) == 1,
                "SceneAsset exposes the explicit empty glTF scene");
    EXPECT_TRUE(rt_model3d_get_node_count(model) == 0,
                "SceneAsset does not synthesize display nodes for an explicit empty glTF scene");
    void *scene = rt_model3d_instantiate_scene_at(model, 0);
    EXPECT_TRUE(scene != nullptr, "SceneAsset.InstantiateSceneAt handles explicit empty scenes");
    EXPECT_TRUE(rt_scene3d_get_node_count(scene) == 1,
                "SceneAsset empty-scene instantiation contains only the scene root");
}

static void test_model3d_loads_stl_as_template_asset() {
    const char *path = "/tmp/zanna_model3d_fixture.stl";
    const char *stl = "solid tri\n"
                      "facet normal 0 0 1\n"
                      "  outer loop\n"
                      "    vertex 0 0 0\n"
                      "    vertex 1 0 0\n"
                      "    vertex 0 1 0\n"
                      "  endloop\n"
                      "endfacet\n"
                      "endsolid tri\n";
    EXPECT_TRUE(write_text_file(path, stl), "ASCII STL fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses STL assets");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "STL-backed SceneAsset exposes one mesh");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "STL-backed SceneAsset creates a default material");
    EXPECT_TRUE(rt_model3d_get_node_count(model) == 1,
                "STL-backed SceneAsset synthesizes one template node");
    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh && mesh->index_count == 3,
                "STL-backed SceneAsset preserves triangle geometry");
}

static void test_model3d_loads_minimal_ascii_fbx() {
    const char *path = "/tmp/zanna_model3d_ascii_fixture.fbx";
    const char *fbx = "; FBX 7.4.0 project file\n"
                      "Objects:  {\n"
                      "  Geometry: 1, \"Geometry::AsciiMesh\", \"Mesh\" {\n"
                      "    Vertices: *9 { a: 0,0,0, 1,0,0, 0,1,0 }\n"
                      "    PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
                      "  }\n"
                      "}\n";
    EXPECT_TRUE(write_text_file(path, fbx), "ASCII FBX fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses minimal ASCII FBX assets");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "ASCII FBX exposes one mesh");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1, "ASCII FBX creates a default material");
    EXPECT_TRUE(rt_model3d_find_node(model, rt_const_cstr("mesh_0")) != nullptr,
                "ASCII FBX builds a renderable mesh node");
}

static void test_model3d_splits_fbx_layer_element_materials() {
    const char *path = "/tmp/zanna_model3d_multimaterial_fixture.fbx";
    EXPECT_TRUE(write_fbx_multimaterial_fixture(path), "Multi-material FBX fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX LayerElementMaterial fixtures");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 2, "FBX connected materials are imported");
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 3,
                "FBX multi-material geometry keeps source mesh and adds two render submeshes");
    void *multi = rt_model3d_find_node(model, rt_const_cstr("Multi"));
    EXPECT_TRUE(multi != nullptr, "FBX multi-material model node is preserved");
    if (!multi)
        return;
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(multi)),
                5.0,
                0.001,
                "FBX GeometricTranslation contributes to imported model position X");
    EXPECT_NEAR(rt_vec3_y(rt_scene_node3d_get_position(multi)),
                7.0,
                0.001,
                "FBX GeometricTranslation contributes to imported model position Y");
    EXPECT_TRUE(rt_scene_node3d_get_mesh(multi) == nullptr,
                "FBX multi-material model acts as a transform group");
    EXPECT_TRUE(rt_scene_node3d_child_count(multi) == 2,
                "FBX LayerElementMaterial creates one child submesh per material slot");
    void *red_child = rt_scene_node3d_get_child(multi, 0);
    void *blue_child = rt_scene_node3d_get_child(multi, 1);
    auto *red_mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(red_child));
    auto *blue_mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(blue_child));
    auto *red_mat = static_cast<rt_material3d *>(rt_scene_node3d_get_material(red_child));
    auto *blue_mat = static_cast<rt_material3d *>(rt_scene_node3d_get_material(blue_child));
    EXPECT_TRUE(red_mesh && red_mesh->index_count == 3,
                "FBX material slot 0 submesh contains one triangle");
    EXPECT_TRUE(blue_mesh && blue_mesh->index_count == 3,
                "FBX material slot 1 submesh contains one triangle");
    EXPECT_TRUE(red_mat && blue_mat, "FBX material submesh nodes have materials");
    if (!red_mat || !blue_mat)
        return;
    EXPECT_NEAR(red_mat->diffuse[0], 0.9, 0.001, "FBX first material slot uses Red material");
    EXPECT_NEAR(blue_mat->diffuse[2], 0.9, 0.001, "FBX second material slot uses Blue material");
}

static void test_model3d_triangulates_large_fbx_ngons() {
    const char *path = "/tmp/zanna_model3d_large_ngon.fbx";
    bool wrote_fixture = write_fbx_large_ngon_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Large n-gon FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load triangulates FBX n-gons larger than 32 vertices");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "Large n-gon FBX fixture imports one mesh");
    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 80,
                "Large n-gon FBX mesh preserves all polygon vertices");
    EXPECT_TRUE(mesh != nullptr && mesh->index_count == (80 - 2) * 3,
                "Large n-gon FBX mesh triangulates to N-2 triangles");
}

static void test_model3d_imports_fbx_embedded_textures() {
    const char *path = "/tmp/zanna_model3d_embedded_texture_fixture.fbx";
    const char *png_path = "/tmp/zanna_model3d_embedded_texture_source.png";
    std::vector<uint8_t> png_bytes;
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x4A6C8EFFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "Embedded FBX PNG fixture can be written");
    EXPECT_TRUE(read_binary_file(png_path, png_bytes), "Embedded FBX PNG can be read");
    if (png_bytes.empty())
        return;
    EXPECT_TRUE(write_fbx_embedded_texture_fixture(path, png_bytes.data(), png_bytes.size()),
                "Embedded-texture FBX fixture can be written");

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX assets with embedded textures");
    if (!model)
        return;

    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(mat != nullptr && rt_material3d_get_has_texture(mat) == 1,
                "FBX embedded Video content imports as the diffuse material texture");
    EXPECT_TRUE(mat != nullptr && mat->texture != nullptr &&
                    rt_pixels_get(rt_textureasset3d_get_pixels(mat->texture), 0, 0) == 0x4A6C8EFFll,
                "FBX embedded texture pixels preserve decoded PNG contents");
    EXPECT_TRUE(mat != nullptr && texture_source_equals(mat->texture, "png", png_bytes),
                "FBX embedded Video content retains its exact PNG bytes");
}

static void test_model3d_imports_fbx_texture_aliases_and_absolute_basename_fallback() {
    const char *path = "/tmp/zanna_model3d_texture_aliases.fbx";
    const char *png_path = "/tmp/zanna_model3d_texture_aliases.png";
    const char *exporter_path = "/missing/export/machine/zanna_model3d_texture_aliases.png";
    std::vector<uint8_t> png_bytes;
    void *pixels = rt_pixels_new(1, 1);
    rt_pixels_set(pixels, 0, 0, 0x66AAEEFFll);
    EXPECT_TRUE(rt_pixels_save_png(pixels, rt_const_cstr(png_path)) == 1,
                "FBX texture alias PNG fixture can be written");
    EXPECT_TRUE(read_binary_file(png_path, png_bytes), "FBX external PNG fixture can be read");
    EXPECT_TRUE(write_fbx_texture_alias_fixture(path, exporter_path),
                "FBX texture alias fixture can be written");

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load imports FBX texture aliases with absolute basename fallback");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "FBX texture alias fixture imports one material");
    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(mat != nullptr && rt_material3d_get_has_texture(mat) == 1,
                "FBX Maya baseColor texture alias assigns the diffuse texture slot");
    EXPECT_TRUE(mat != nullptr && rt_material3d_get_has_normal_map(mat) == 1,
                "FBX Maya normalCamera texture alias assigns the normal map slot");
    EXPECT_TRUE(mat != nullptr && texture_source_equals(mat->texture, "png", png_bytes) &&
                    texture_source_equals(mat->normal_map, "png", png_bytes),
                "FBX external base-color and normal textures retain exact file bytes");
}

static void test_model3d_imports_fbx_material_scalar_aliases_and_allsame_uvs() {
    const char *path = "/tmp/zanna_model3d_material_aliases.fbx";
    bool wrote_fixture = write_fbx_material_alias_fixture(path);
    EXPECT_TRUE(wrote_fixture, "FBX material alias fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX material aliases");
    if (!model)
        return;

    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3,
                "FBX material alias fixture imports mesh vertices");
    if (mesh && mesh->vertex_count == 3) {
        for (uint32_t i = 0; i < mesh->vertex_count; ++i) {
            EXPECT_NEAR(
                mesh->vertices[i].uv[0], 0.25, 0.001, "FBX AllSame UV maps U to all vertices");
            EXPECT_NEAR(mesh->vertices[i].uv[1],
                        0.25,
                        0.001,
                        "FBX AllSame UV maps flipped V to all vertices");
        }
    }

    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(mat != nullptr, "FBX material alias fixture imports a material");
    if (!mat)
        return;
    EXPECT_NEAR(mat->diffuse[0], 0.3, 0.001, "FBX BaseColor aliases diffuse R");
    EXPECT_NEAR(mat->diffuse[1], 0.4, 0.001, "FBX BaseColor aliases diffuse G");
    EXPECT_NEAR(mat->diffuse[2], 0.5, 0.001, "FBX BaseColor aliases diffuse B");
    EXPECT_NEAR(mat->metallic, 0.7, 0.001, "FBX Metalness aliases metallic factor");
    EXPECT_NEAR(mat->roughness, 0.25, 0.001, "FBX Roughness imports roughness factor");
    EXPECT_NEAR(mat->ao, 0.6, 0.001, "FBX AmbientOcclusion imports AO factor");
    EXPECT_NEAR(mat->normal_scale, 0.8, 0.001, "FBX NormalMapScale imports normal scale");
    EXPECT_NEAR(mat->emissive[0], 0.1, 0.001, "FBX EmissionColor imports emissive R");
    EXPECT_NEAR(mat->emissive_intensity, 2.0, 0.001, "FBX EmissionFactor imports intensity");
    EXPECT_TRUE(mat->double_sided == 1, "FBX TwoSided enables double-sided rendering");
}

static void test_model3d_imports_fbx_secondary_uvs_and_vertex_colors() {
    const char *path = "/tmp/zanna_model3d_multilayer_attributes.fbx";
    EXPECT_TRUE(write_fbx_multilayer_attribute_fixture(path),
                "FBX multi-layer attribute fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX secondary UV/color layers");
    if (!model)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3,
                "FBX secondary attribute fixture imports three vertices");
    if (!mesh || mesh->vertex_count != 3)
        return;
    EXPECT_NEAR(mesh->vertices[0].uv[0], 0.0, 0.001, "FBX UV0 layer imports primary U");
    EXPECT_NEAR(mesh->vertices[0].uv[1], 1.0, 0.001, "FBX UV0 layer flips primary V");
    EXPECT_NEAR(mesh->vertices[0].uv1[0], 0.9, 0.001, "FBX UV1 layer imports secondary U");
    EXPECT_NEAR(mesh->vertices[0].uv1[1], 0.9, 0.001, "FBX UV1 layer flips secondary V");
    EXPECT_NEAR(mesh->vertices[1].color[1], 1.0, 0.001, "FBX vertex colors import green");
    EXPECT_NEAR(mesh->vertices[2].color[2], 1.0, 0.001, "FBX vertex colors import blue");
    EXPECT_NEAR(mesh->vertices[2].color[3], 0.5, 0.001, "FBX vertex colors import alpha");
}

static void test_fbx_cluster_transform_link_drives_bind_pose() {
    const char *path = "/tmp/zanna_model3d_cluster_transform_link.fbx";
    EXPECT_TRUE(write_fbx_cluster_transform_link_fixture(path),
                "FBX TransformLink bind-pose fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX Cluster TransformLink bind poses");
    if (!model)
        return;
    auto *skeleton = static_cast<rt_skeleton3d *>(rt_model3d_get_skeleton(model, 0));
    EXPECT_TRUE(skeleton != nullptr && skeleton->bone_count == 2,
                "FBX TransformLink fixture imports a two-bone skeleton");
    if (!skeleton || skeleton->bone_count < 2)
        return;
    EXPECT_TRUE(skeleton->bones[1].parent_index == 0,
                "FBX TransformLink fixture keeps child parent index");
    EXPECT_NEAR(skeleton->bones[1].bind_pose_local[7],
                4.0,
                0.001,
                "FBX child bind pose uses Cluster TransformLink instead of Model Lcl Translation");
}

static void test_fbx_procedural_surface_interpolates_skin_weights() {
    const char *path = "/tmp/zanna_model3d_skinned_nurbs.fbx";
    EXPECT_TRUE(write_fbx_skinned_nurbs_fixture(path),
                "FBX skinned rational NURBS fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX applies Skin deformers to tessellated NURBS geometry");
    auto *mesh = asset ? static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 0)) : nullptr;
    EXPECT_TRUE(mesh && mesh->vertex_count == 9 && mesh->bone_count == 2,
                "Tessellated NURBS retains its two-bone skin palette");
    if (mesh && mesh->vertex_count == 9) {
        double center_root_weight = 0.0;
        double center_child_weight = 0.0;
        for (int influence = 0; influence < 4; ++influence) {
            if (mesh->vertices[4].bone_indices[influence] == 0)
                center_root_weight += mesh->vertices[4].bone_weights[influence];
            if (mesh->vertices[4].bone_indices[influence] == 1)
                center_child_weight += mesh->vertices[4].bone_weights[influence];
        }
        EXPECT_NEAR(center_root_weight,
                    0.4,
                    0.001,
                    "Surface skinning interpolates the two left control weights");
        EXPECT_NEAR(center_child_weight,
                    0.6,
                    0.001,
                    "Surface skinning includes rational weighting from the upper-right control");
        EXPECT_NEAR(mesh->vertices[0].bone_weights[0],
                    1.0,
                    0.001,
                    "Surface corner samples retain their exact control-point skin weight");
    }
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

static void test_fbx_procedural_surface_interpolates_morph_deltas() {
    const char *path = "/tmp/zanna_model3d_morphed_nurbs.fbx";
    EXPECT_TRUE(write_fbx_morphed_nurbs_fixture(path),
                "FBX morphed rational NURBS fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX applies BlendShapes to tessellated NURBS geometry");
    void *morph = asset ? rt_fbx_get_morph_target(asset, 0) : nullptr;
    rt_morphtarget3d_shape_view_internal view{};
    EXPECT_TRUE(morph && rt_morphtarget3d_get_shape_view_internal(morph, 0, &view),
                "Tessellated NURBS retains its imported BlendShape payload");
    if (view.position_deltas && view.vertex_count == 9) {
        EXPECT_NEAR(view.position_deltas[4 * 3 + 0],
                    0.4,
                    0.001,
                    "Surface morph interpolation applies the rational center basis weight");
        EXPECT_NEAR(view.position_deltas[8 * 3 + 0],
                    1.0,
                    0.001,
                    "Surface morph interpolation preserves the exact authored corner delta");
        EXPECT_NEAR(view.position_deltas[0],
                    0.0,
                    0.001,
                    "Surface morph interpolation leaves unrelated control corners unchanged");
    }
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

static void test_fbx_imports_skeletons_beyond_draw_palette_limit() {
    const char *path = "/tmp/zanna_model3d_257_bone_fixture.fbx";
    EXPECT_TRUE(write_fbx_large_skeleton_fixture(path, 257), "FBX 257-bone fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX loader accepts skeletons larger than one draw palette");
    void *skeleton = asset ? rt_fbx_get_skeleton(asset) : nullptr;
    EXPECT_TRUE(skeleton != nullptr && rt_skeleton3d_get_bone_count(skeleton) == 257,
                "FBX loader preserves bones 257+ up to Skeleton3D's 1024-bone limit");
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

static void test_fbx_constant_animation_curve_preserves_step() {
    const char *path = "/tmp/zanna_model3d_constant_anim_curve.fbx";
    EXPECT_TRUE(write_fbx_constant_animation_fixture(path),
                "FBX constant animation fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX constant animation curves");
    if (!model)
        return;
    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "FBX constant animation imports an Animation3D");
    if (!anim)
        return;
    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "FBX constant animation creates a root channel");
    if (!root_channel)
        return;
    EXPECT_TRUE(root_channel->keyframe_count >= 3,
                "FBX constant curves add a pre-step sample for runtime interpolation");
    if (root_channel->keyframe_count < 3)
        return;
    EXPECT_NEAR(root_channel->keyframes[root_channel->keyframe_count - 2].position[0],
                0.0,
                0.001,
                "FBX constant curve holds the previous value before the next key");
    EXPECT_NEAR(root_channel->keyframes[root_channel->keyframe_count - 1].position[0],
                10.0,
                0.001,
                "FBX constant curve jumps at the authored next key");
    EXPECT_NEAR(root_channel->keyframes[root_channel->keyframe_count - 1].time -
                    root_channel->keyframes[root_channel->keyframe_count - 2].time,
                1.0 / 46186158000.0,
                1e-15,
                "FBX constant curve pre-step is exactly one representable source tick");
}

static void test_model3d_loads_ascii_fbx_attributes_and_materials() {
    const char *path = "/tmp/zanna_model3d_ascii_attributes.fbx";
    const char *fbx = "; FBX 7.4.0 project file\n"
                      "Objects:  {\n"
                      "  Model: 2, \"Model::AsciiNamed\", \"Mesh\" {\n"
                      "  }\n"
                      "  Material: 3, \"Material::AsciiMat\", \"\" {\n"
                      "    Properties70:  {\n"
                      "      P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.2,0.4,0.6\n"
                      "    }\n"
                      "  }\n"
                      "  Geometry: 1, \"Geometry::AsciiMesh\", \"Mesh\" {\n"
                      "    Vertices: *9 { a: 0,0,0, 1,0,0, 0,1,0 }\n"
                      "    PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
                      "    Normals: *9 { a: 0,0,1, 0,0,1, 0,0,1 }\n"
                      "    UV: *6 { a: 0,0, 1,0, 0,1 }\n"
                      "    UVIndex: *3 { a: 0,1,2 }\n"
                      "    Colors: *12 { a: 1,0,0,1, 0,1,0,1, 0,0,1,0.5 }\n"
                      "  }\n"
                      "}\n";
    EXPECT_TRUE(write_text_file(path, fbx), "ASCII FBX attribute fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses ASCII FBX attributes/materials");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_find_node(model, rt_const_cstr("AsciiNamed")) != nullptr,
                "ASCII FBX fallback preserves Model names");
    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3,
                "ASCII FBX attribute fixture imports mesh vertices");
    if (mesh && mesh->vertex_count == 3) {
        EXPECT_NEAR(mesh->vertices[1].uv[0], 1.0, 0.001, "ASCII FBX UV imports U");
        EXPECT_NEAR(mesh->vertices[2].uv[1], 0.0, 0.001, "ASCII FBX UV flips V");
        EXPECT_NEAR(mesh->vertices[2].color[2], 1.0, 0.001, "ASCII FBX Colors import blue");
        EXPECT_NEAR(mesh->vertices[2].color[3], 0.5, 0.001, "ASCII FBX Colors import alpha");
    }
    auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(mat != nullptr, "ASCII FBX material fixture imports a material");
    if (mat) {
        EXPECT_NEAR(mat->diffuse[0], 0.2, 0.001, "ASCII FBX DiffuseColor imports R");
        EXPECT_NEAR(mat->diffuse[2], 0.6, 0.001, "ASCII FBX DiffuseColor imports B");
    }
}

/// @brief Verify ASCII FBX produces the same multi-family asset graph as equivalent binary FBX.
static void test_fbx_ascii_typed_graph_matches_binary_multi_object_graph() {
    const char *ascii_path = "/tmp/zanna_fbx_typed_multi_ascii.fbx";
    const char *binary_path = "/tmp/zanna_fbx_typed_multi_binary.fbx";
    EXPECT_TRUE(write_fbx_multi_object_ascii_fixture(ascii_path),
                "Typed multi-object ASCII FBX fixture can be written");
    EXPECT_TRUE(write_fbx_multi_object_binary_fixture(binary_path),
                "Equivalent multi-object binary FBX fixture can be written");
    void *ascii_asset = rt_fbx_load(rt_const_cstr(ascii_path));
    void *binary_asset = rt_fbx_load(rt_const_cstr(binary_path));
    EXPECT_TRUE(ascii_asset != nullptr && binary_asset != nullptr,
                "ASCII and binary typed multi-object FBX graphs both load");
    if (ascii_asset && binary_asset) {
        EXPECT_TRUE(rt_fbx_mesh_count(ascii_asset) == 2 &&
                        rt_fbx_mesh_count(ascii_asset) == rt_fbx_mesh_count(binary_asset),
                    "ASCII and binary FBX retain the same two geometries");
        EXPECT_TRUE(rt_fbx_material_count(ascii_asset) == 2 &&
                        rt_fbx_material_count(ascii_asset) == rt_fbx_material_count(binary_asset),
                    "ASCII and binary FBX retain the same two materials");
        EXPECT_TRUE(rt_skeleton3d_get_bone_count(rt_fbx_get_skeleton(ascii_asset)) == 2 &&
                        rt_skeleton3d_get_bone_count(rt_fbx_get_skeleton(ascii_asset)) ==
                            rt_skeleton3d_get_bone_count(rt_fbx_get_skeleton(binary_asset)),
                    "ASCII and binary FBX retain the same two-bone skeleton");
        EXPECT_TRUE(rt_fbx_animation_count(ascii_asset) == 1 &&
                        rt_fbx_animation_count(ascii_asset) == rt_fbx_animation_count(binary_asset),
                    "ASCII and binary FBX retain the same keyed animation stack");
        EXPECT_TRUE(rt_scene_node3d_child_count(rt_fbx_get_scene_root(ascii_asset)) == 2 &&
                        rt_scene_node3d_child_count(rt_fbx_get_scene_root(ascii_asset)) ==
                            rt_scene_node3d_child_count(rt_fbx_get_scene_root(binary_asset)),
                    "ASCII and binary FBX build equivalent two-model scene roots");
    }
    if (ascii_asset && rt_obj_release_check0(ascii_asset))
        rt_obj_free(ascii_asset);
    if (binary_asset && rt_obj_release_check0(binary_asset))
        rt_obj_free(binary_asset);
    std::remove(ascii_path);
    std::remove(binary_path);
}

/// @brief Verify comments, strings, and sibling scopes cannot impersonate ASCII FBX nodes.
static void test_fbx_ascii_parser_ignores_decoy_identifiers_and_scopes() {
    const char *path = "/tmp/zanna_fbx_ascii_decoy_scopes.fbx";
    const char *text =
        "; FBX 7.4.0 project file\n"
        "; Geometry: 999, \"Geometry::CommentDecoy\", \"Mesh\" { Vertices: *3 { a: 99,99,99 }\n"
        "DocumentInfo: { Note: \"Vertices: *9 { a: 88,88,88 } Geometry: decoy\" }\n"
        "Objects: {\n"
        " Geometry: 1, \"Geometry::Real\", \"Mesh\" {\n"
        "  Metadata: { Vertices: *9 { a: 77,77,77, 77,77,77, 77,77,77 } }\n"
        "  Vertices: *9 { a: 5,0,0, 6,0,0, 5,1,0 }\n"
        "  PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
        " }\n"
        "}\n"
        "Connections: { }\n";
    EXPECT_TRUE(write_text_file(path, text), "ASCII FBX decoy-scope fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "Brace-scoped ASCII FBX with decoys still loads its real graph");
    auto *mesh = asset ? static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 0)) : nullptr;
    EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3,
                "Only the direct real Geometry arrays produce a mesh");
    if (mesh && mesh->vertex_count == 3)
        EXPECT_NEAR(mesh->vertices[0].pos[0],
                    5.0,
                    0.001,
                    "Comment/string/sibling Vertices decoys cannot satisfy direct lookup");
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

/// @brief Verify aggregate compressed-array expansion obeys the one-load FBX budget atomically.
static void test_fbx_aggregate_budget_rejects_many_valid_compressed_arrays() {
    const char *path = "/tmp/zanna_fbx_aggregate_budget.fbx";
    const uint64_t budget = 4u * 1024u * 1024u;
    EXPECT_TRUE(write_fbx_aggregate_budget_fixture(path),
                "Aggregate-budget compressed FBX fixture can be written");
    rt_fbx_test_set_load_budget_bytes(budget);
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset == nullptr,
                "Many individually valid compressed arrays fail before publishing a partial FBX");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_TOO_LARGE,
                "Aggregate FBX budget exhaustion reports the stable too-large diagnostic");
    EXPECT_TRUE(rt_fbx_test_get_last_budget_used_bytes() <= budget &&
                    rt_fbx_test_get_last_budget_used_bytes() > 2u * 1024u * 1024u,
                "FBX aggregate telemetry remains bounded and includes source plus expansions");
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

/// @brief Verify keyed FBX transforms retain pivots, pre-rotation, geometry, and source tick poses.
static void test_fbx_animation_uses_static_transform_stack_composer() {
    const char *path = "/tmp/zanna_fbx_animated_transform_stack.fbx";
    EXPECT_TRUE(write_fbx_transform_stack_animation_fixture(path),
                "Animated FBX transform-stack fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "Animated FBX transform-stack fixture loads");
    void *skeleton = asset ? rt_fbx_get_skeleton(asset) : nullptr;
    auto *animation =
        asset ? static_cast<rt_animation3d *>(rt_fbx_get_animation(asset, 0)) : nullptr;
    double bind_local[16] = {};
    EXPECT_TRUE(skeleton && rt_skeleton3d_get_bone_bind_local_raw(skeleton, 0, bind_local),
                "Transform-stack fixture exposes its static bind matrix");
    const vgfx3d_anim_channel_t *channel = nullptr;
    if (animation) {
        for (int32_t i = 0; i < animation->channel_count; i++)
            if (animation->channels[i].bone_index == 0)
                channel = &animation->channels[i];
    }
    EXPECT_TRUE(channel && channel->keyframe_count >= 2,
                "Transform-stack fixture produces keyed root-bone poses");
    if (channel && channel->keyframe_count >= 2) {
        const vgfx3d_keyframe_t &first = channel->keyframes[0];
        const vgfx3d_keyframe_t &last = channel->keyframes[channel->keyframe_count - 1];
        EXPECT_NEAR(first.position[0],
                    bind_local[3],
                    0.001,
                    "Animated first pose matches static pivot/geometric bind translation X");
        EXPECT_NEAR(first.position[1],
                    bind_local[7],
                    0.001,
                    "Animated first pose matches static pre-rotated pivot translation Y");
        EXPECT_NEAR(first.position[0], 3.0, 0.001, "Pivot plus geometric translation composes X");
        EXPECT_NEAR(
            first.position[1], -1.0, 0.001, "PreRotation rotates around the authored pivot");
        EXPECT_NEAR(std::fabs(first.rotation[2]),
                    std::sqrt(0.5),
                    0.001,
                    "Animated quaternion retains the static 90-degree PreRotation");
        EXPECT_NEAR(last.position[0],
                    4.0,
                    0.001,
                    "Keyed Lcl Translation replaces only its lane in the full transform stack");
    }
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

/// @brief Verify cubic FBX Hermite curves are adaptively sampled through extrema and tangents.
static void test_fbx_cubic_animation_adaptive_sampling_preserves_shape() {
    const char *path = "/tmp/zanna_fbx_cubic_adaptive.fbx";
    EXPECT_TRUE(write_fbx_cubic_animation_fixture(path),
                "Cubic FBX adaptive-sampling fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    auto *animation =
        asset ? static_cast<rt_animation3d *>(rt_fbx_get_animation(asset, 0)) : nullptr;
    const vgfx3d_anim_channel_t *channel = nullptr;
    if (animation) {
        for (int32_t i = 0; i < animation->channel_count; i++)
            if (animation->channels[i].bone_index == 0)
                channel = &animation->channels[i];
    }
    EXPECT_TRUE(channel && channel->keyframe_count >= 1001,
                "One-second cubic FBX segment subdivides to the one-millisecond bound");
    if (channel && channel->keyframe_count >= 3) {
        double maximum = -1e30;
        double maximum_gap = 0.0;
        for (int32_t i = 0; i < channel->keyframe_count; i++) {
            maximum = std::fmax(maximum, (double)channel->keyframes[i].position[0]);
            if (i > 0)
                maximum_gap = std::fmax(
                    maximum_gap, channel->keyframes[i].time - channel->keyframes[i - 1].time);
        }
        double first_slope =
            ((double)channel->keyframes[1].position[0] - channel->keyframes[0].position[0]) /
            (channel->keyframes[1].time - channel->keyframes[0].time);
        EXPECT_NEAR(maximum, 1.0, 0.0015, "Adaptive cubic samples preserve the Hermite extremum");
        EXPECT_NEAR(first_slope, 4.0, 0.03, "Adaptive cubic samples preserve outgoing tangent");
        EXPECT_TRUE(maximum_gap <= 0.001000001,
                    "Every adaptive cubic runtime chord spans at most one millisecond");
    }
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

/// @brief Verify indexed FBX object/connection traversal scales near-linearly on large graphs.
static void test_fbx_large_animation_graph_lookup_probes_are_near_linear() {
    const char *small_path = "/tmp/zanna_fbx_probe_small.fbx";
    const char *large_path = "/tmp/zanna_fbx_probe_large.fbx";
    EXPECT_TRUE(
        write_fbx_skinned_animation_fixture_ex(small_path, false, 0, 128, false, false, false),
        "Small indexed FBX probe fixture can be written");
    void *small_asset = rt_fbx_load(rt_const_cstr(small_path));
    uint64_t small_probes = rt_fbx_test_get_last_lookup_probe_count();
    EXPECT_TRUE(
        write_fbx_skinned_animation_fixture_ex(large_path, false, 0, 1024, false, false, false),
        "Large indexed FBX probe fixture can be written");
    void *large_asset = rt_fbx_load(rt_const_cstr(large_path));
    uint64_t large_probes = rt_fbx_test_get_last_lookup_probe_count();
    EXPECT_TRUE(small_asset != nullptr && large_asset != nullptr,
                "Both indexed FBX probe fixtures load completely");
    EXPECT_TRUE(small_probes > 0 && large_probes < small_probes * 12u,
                "Eightfold graph growth stays within a near-linear lookup-probe envelope");
    EXPECT_TRUE(large_probes < 250000u,
                "Large FBX animation graph avoids per-curve whole-graph rescans");
    if (small_asset && rt_obj_release_check0(small_asset))
        rt_obj_free(small_asset);
    if (large_asset && rt_obj_release_check0(large_asset))
        rt_obj_free(large_asset);
    std::remove(small_path);
    std::remove(large_path);
}

/// @brief Verify long equal-prefix display names stay distinct and curves bind only by object ID.
static void test_fbx_long_colliding_names_remain_distinct_and_id_bound() {
    const char *path = "/tmp/zanna_fbx_long_name_identity.fbx";
    std::string scene_a = make_fbx_long_collision_name("_scene_a");
    std::string scene_b = make_fbx_long_collision_name("_scene_b");
    std::string bone_a = make_fbx_long_collision_name("_bone_a");
    std::string bone_b = make_fbx_long_collision_name("_bone_b");
    EXPECT_TRUE(write_fbx_long_name_identity_fixture(path),
                "Long colliding-name FBX fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "Long colliding-name FBX fixture loads");
    void *root = asset ? rt_fbx_get_scene_root(asset) : nullptr;
    rt_string scene_a_string = rt_string_from_bytes(scene_a.data(), scene_a.size());
    rt_string scene_b_string = rt_string_from_bytes(scene_b.data(), scene_b.size());
    EXPECT_TRUE(root && rt_scene_node3d_find(root, scene_a_string) != nullptr &&
                    rt_scene_node3d_find(root, scene_b_string) != nullptr,
                "Scene nodes remain distinct beyond the former 128-byte name prefix");
    rt_string_unref(scene_a_string);
    rt_string_unref(scene_b_string);
    void *skeleton = asset ? rt_fbx_get_skeleton(asset) : nullptr;
    rt_string imported_bone_a = skeleton ? rt_skeleton3d_get_bone_name(skeleton, 0) : nullptr;
    rt_string imported_bone_b = skeleton ? rt_skeleton3d_get_bone_name(skeleton, 1) : nullptr;
    EXPECT_TRUE(imported_bone_a && imported_bone_b &&
                    std::string(rt_string_cstr(imported_bone_a)) == bone_a &&
                    std::string(rt_string_cstr(imported_bone_b)) == bone_b,
                "Skeleton names remain distinct beyond the former 64-byte name prefix");
    rt_string_unref(imported_bone_a);
    rt_string_unref(imported_bone_b);
    auto *animation =
        asset ? static_cast<rt_animation3d *>(rt_fbx_get_animation(asset, 0)) : nullptr;
    bool targets_second_bone = false;
    if (animation)
        for (int32_t i = 0; i < animation->channel_count; i++)
            targets_second_bone |= animation->channels[i].bone_index == 1;
    EXPECT_TRUE(targets_second_bone,
                "Long-name animation resolves the intended second bone by numeric object ID");
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

static void test_model3d_honors_fbx_rotation_order() {
    const char *path = "/tmp/zanna_model3d_rotation_order.fbx";
    EXPECT_TRUE(write_fbx_rotation_order_fixture(path), "FBX RotationOrder fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX RotationOrder properties");
    if (!model)
        return;
    void *default_node = rt_model3d_find_node(model, rt_const_cstr("DefaultOrder"));
    void *zyx_node = rt_model3d_find_node(model, rt_const_cstr("ZyxOrder"));
    EXPECT_TRUE(default_node != nullptr && zyx_node != nullptr,
                "FBX RotationOrder fixture imports both scene nodes");
    if (!default_node || !zyx_node)
        return;
    void *default_q = rt_scene_node3d_get_rotation(default_node);
    void *zyx_q = rt_scene_node3d_get_rotation(zyx_node);
    double diff = std::fabs(rt_quat_x(default_q) - rt_quat_x(zyx_q)) +
                  std::fabs(rt_quat_y(default_q) - rt_quat_y(zyx_q)) +
                  std::fabs(rt_quat_z(default_q) - rt_quat_z(zyx_q)) +
                  std::fabs(rt_quat_w(default_q) - rt_quat_w(zyx_q));
    EXPECT_TRUE(diff > 0.01, "FBX RotationOrder changes the imported node quaternion");
}

static void test_model3d_imports_fbx_skinning_and_grouped_animation() {
    const char *path = "/tmp/zanna_model3d_skinned_anim_fixture.fbx";
    bool wrote_fixture = write_fbx_skinned_animation_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Skinned FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses skinned FBX assets");
    if (!model)
        return;

    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "Skinned FBX exposes one mesh");
    EXPECT_TRUE(rt_model3d_get_skeleton_count(model) == 1, "Skinned FBX exposes one skeleton");
    EXPECT_TRUE(rt_model3d_get_animation_count(model) == 1, "Skinned FBX exposes one animation");

    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr, "Skinned FBX mesh is available");
    if (!mesh)
        return;
    EXPECT_TRUE(mesh->bone_count == 2, "FBX skin clusters set mesh bone_count");
    EXPECT_NEAR(
        mesh->vertices[0].bone_weights[0], 1.0, 0.001, "Root-only vertex keeps full root weight");
    EXPECT_TRUE(mesh->vertices[0].bone_indices[0] == 0, "Root-only vertex references root bone");
    EXPECT_NEAR(mesh->vertices[1].bone_weights[0], 0.25, 0.001, "Shared vertex keeps root weight");
    EXPECT_NEAR(mesh->vertices[1].bone_weights[1], 0.75, 0.001, "Shared vertex keeps child weight");
    EXPECT_TRUE(mesh->vertices[1].bone_indices[0] == 0 && mesh->vertices[1].bone_indices[1] == 1,
                "Shared vertex references both imported bones");
    EXPECT_NEAR(
        mesh->vertices[2].bone_weights[0], 1.0, 0.001, "Child-only vertex keeps full child weight");
    EXPECT_TRUE(mesh->vertices[2].bone_indices[0] == 1, "Child-only vertex references child bone");

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "Skinned FBX animation is available");
    if (!anim)
        return;
    EXPECT_NEAR(anim->duration, 1.0, 0.001, "FBX animation duration follows imported key time");

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "FBX animation creates a root-bone channel");
    if (!root_channel)
        return;
    EXPECT_TRUE(root_channel->keyframe_count == 2,
                "FBX animation groups X/Y component curves into shared keyframes");
    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "Grouped FBX animation preserves X translation");
    EXPECT_NEAR(root_channel->keyframes[1].position[1],
                20.0,
                0.001,
                "Grouped FBX animation preserves Y translation");
    EXPECT_NEAR(root_channel->keyframes[1].position[2],
                0.0,
                0.001,
                "Grouped FBX animation keeps untouched Z translation at bind value");
    EXPECT_NEAR(root_channel->keyframes[1].scale_xyz[0],
                1.0,
                0.001,
                "Grouped FBX animation preserves default scale");

    void *instance = rt_model3d_instantiate(model);
    EXPECT_TRUE(instance != nullptr, "SceneAsset.Instantiate creates an animated instance root");
    void *controller = instance ? rt_scene_node3d_get_animator(instance) : nullptr;
    EXPECT_TRUE(controller != nullptr,
                "SceneAsset.Instantiate auto-binds a controller for imported skeleton animations");
    if (controller) {
        EXPECT_TRUE(rt_anim_controller3d_get_state_count(controller) == 1,
                    "Auto-bound SceneAsset controller registers imported clips as states");
        rt_string state = rt_anim_controller3d_get_current_state(controller);
        const char *state_name = state ? rt_string_cstr(state) : "";
        EXPECT_TRUE(std::strcmp(state_name, "Walk") == 0,
                    "Auto-bound SceneAsset controller starts the first imported animation");
    }
}

static void test_fbx_duplicate_animation_curves_keep_first_component() {
    const char *path = "/tmp/zanna_model3d_duplicate_anim_curve_fixture.fbx";
    bool wrote_fixture = write_fbx_duplicate_animation_curve_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Duplicate-curve FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load parses FBX assets with duplicate animation curves");
    if (!model)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "Duplicate-curve FBX animation is available");
    if (!anim)
        return;

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "Duplicate-curve FBX animation creates a root channel");
    if (!root_channel || root_channel->keyframe_count < 2)
        return;

    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "FBX duplicate component curves keep the first valid X channel");
    EXPECT_NEAR(root_channel->keyframes[1].position[1],
                20.0,
                0.001,
                "FBX duplicate component curves do not disturb sibling Y channels");
}

static void test_fbx_mismatched_animation_curve_key_arrays_are_ignored() {
    const char *path = "/tmp/zanna_model3d_mismatched_anim_curve_fixture.fbx";
    bool wrote_fixture = write_fbx_mismatched_animation_curve_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Mismatched-curve FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load parses FBX assets with malformed animation curves");
    if (!model)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "FBX animation survives when a sibling component curve is valid");
    if (!anim)
        return;

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "Mismatched-curve FBX animation creates a root channel");
    if (!root_channel || root_channel->keyframe_count < 2)
        return;

    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                0.0,
                0.001,
                "FBX curves with mismatched KeyTime/KeyValueFloat counts do not drive X");
    EXPECT_NEAR(root_channel->keyframes[1].position[1],
                20.0,
                0.001,
                "FBX curves with valid sibling key arrays still drive Y");
}

static void test_fbx_bare_animation_curve_component_names_import() {
    const char *path = "/tmp/zanna_model3d_bare_anim_curve_component_fixture.fbx";
    bool wrote_fixture = write_fbx_bare_component_animation_curve_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Bare-component FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load parses FBX assets with bare animation components");
    if (!model)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "Bare-component FBX animation is available");
    if (!anim)
        return;

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "Bare-component FBX animation creates a root channel");
    if (!root_channel || root_channel->keyframe_count < 2)
        return;

    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "FBX bare X component connections drive translation X");
    EXPECT_NEAR(root_channel->keyframes[1].position[1],
                20.0,
                0.001,
                "FBX bare Y component connections drive translation Y");
}

static void test_fbx_lowercase_animation_curve_component_names_import() {
    const char *path = "/tmp/zanna_model3d_lowercase_anim_curve_component_fixture.fbx";
    bool wrote_fixture = write_fbx_lowercase_component_animation_curve_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Lowercase-component FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load parses FBX assets with lowercase animation components");
    if (!model)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "Lowercase-component FBX animation is available");
    if (!anim)
        return;

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr,
                "Lowercase-component FBX animation creates a root channel");
    if (!root_channel || root_channel->keyframe_count < 2)
        return;

    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "FBX lowercase X component connections drive translation X");
    EXPECT_NEAR(root_channel->keyframes[1].position[1],
                20.0,
                0.001,
                "FBX lowercase Y component connections drive translation Y");
}

static void test_fbx_negative_animation_key_times_normalize_to_clip_start() {
    const char *path = "/tmp/zanna_model3d_negative_time_anim_curve_fixture.fbx";
    bool wrote_fixture = write_fbx_negative_time_animation_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Negative-time FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX assets with negative animation keys");
    if (!model)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "Negative-time FBX animation is available");
    if (!anim)
        return;
    EXPECT_NEAR(anim->duration, 1.0, 0.001, "FBX negative key times keep the clip span");

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "Negative-time FBX animation creates a root channel");
    if (!root_channel || root_channel->keyframe_count < 2)
        return;
    EXPECT_NEAR(root_channel->keyframes[0].time,
                0.0,
                0.001,
                "FBX negative first key normalizes to engine clip time zero");
    EXPECT_NEAR(root_channel->keyframes[1].time,
                1.0,
                0.001,
                "FBX later key normalizes relative to the first source key");
    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "FBX negative-time X curve still drives translation X");
    EXPECT_NEAR(root_channel->keyframes[1].position[1],
                20.0,
                0.001,
                "FBX negative-time Y curve still drives translation Y");
}

static void test_fbx_animation_layers_beyond_fixed_cap_import() {
    const char *path = "/tmp/zanna_model3d_many_anim_layers_fixture.fbx";
    bool wrote_fixture = write_fbx_many_layer_animation_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Many-layer FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX assets with many animation layers");
    if (!model)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "FBX animation imports after many preceding layers");
    if (!anim)
        return;

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "Many-layer FBX animation creates a root channel");
    if (!root_channel || root_channel->keyframe_count < 2)
        return;
    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "FBX animation scans layers beyond the former fixed layer cap");
}

static void test_fbx_animation_curve_nodes_beyond_fixed_cap_import() {
    const char *path = "/tmp/zanna_model3d_many_curve_nodes_fixture.fbx";
    bool wrote_fixture = write_fbx_many_curve_node_animation_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Many-curve-node FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load parses FBX assets with many animation curve nodes");
    if (!model)
        return;

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "FBX animation imports after many preceding curve nodes");
    if (!anim)
        return;

    const vgfx3d_anim_channel_t *root_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 0) {
            root_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(root_channel != nullptr, "Many-curve-node FBX animation creates a root channel");
    if (!root_channel || root_channel->keyframe_count < 2)
        return;
    EXPECT_NEAR(root_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "FBX animation scans curve nodes beyond the former fixed curve-node cap");
}

static void test_fbx_duplicate_bone_names_resolve_by_model_id() {
    const char *path = "/tmp/zanna_model3d_duplicate_bone_names_fixture.fbx";
    bool wrote_fixture = write_fbx_duplicate_bone_name_child_animation_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Duplicate-bone-name FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses FBX assets with duplicate bone names");
    if (!model)
        return;

    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr, "Duplicate-bone-name FBX mesh is available");
    if (!mesh)
        return;
    EXPECT_TRUE(mesh->bone_count == 2, "Duplicate-bone-name FBX keeps both imported bones");
    EXPECT_TRUE(mesh->vertices[2].bone_indices[0] == 1,
                "FBX skin clusters resolve duplicate bone names by model ID");
    EXPECT_NEAR(mesh->vertices[2].bone_weights[0],
                1.0,
                0.001,
                "FBX skin clusters preserve child-bone weight after ID mapping");

    auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(model, 0));
    EXPECT_TRUE(anim != nullptr, "Duplicate-bone-name FBX animation is available");
    if (!anim)
        return;

    const vgfx3d_anim_channel_t *child_channel = nullptr;
    for (int32_t i = 0; i < anim->channel_count; i++) {
        if (anim->channels[i].bone_index == 1) {
            child_channel = &anim->channels[i];
            break;
        }
    }
    EXPECT_TRUE(child_channel != nullptr,
                "FBX animation curves resolve duplicate bone names by model ID");
    if (!child_channel || child_channel->keyframe_count < 2)
        return;
    EXPECT_NEAR(child_channel->keyframes[1].position[0],
                10.0,
                0.001,
                "FBX child animation preserves X translation after ID mapping");
    EXPECT_NEAR(child_channel->keyframes[1].position[1],
                20.0,
                0.001,
                "FBX child animation preserves Y translation after ID mapping");
}

static void test_fbx_asset_accessors_clamp_corrupt_counts() {
    const char *skinned_path = "/tmp/zanna_fbx_asset_corrupt_counts_skinned.fbx";
    EXPECT_TRUE(write_fbx_skinned_animation_fixture(skinned_path),
                "Skinned FBX fixture can be written for asset corruption test");
    void *skinned_asset = rt_fbx_load(rt_const_cstr(skinned_path));
    EXPECT_TRUE(skinned_asset != nullptr, "Direct FBX.Load parses skinned fixture");
    if (skinned_asset) {
        auto *view = static_cast<FbxAssetView *>(skinned_asset);
        EXPECT_TRUE(rt_fbx_mesh_count(skinned_asset) == 1, "FBX asset starts with one mesh");
        EXPECT_TRUE(rt_fbx_animation_count(skinned_asset) == 1,
                    "FBX asset starts with one animation");
        EXPECT_TRUE(rt_fbx_get_skeleton(skinned_asset) != nullptr,
                    "FBX asset starts with a skeleton");
        EXPECT_TRUE(rt_fbx_get_scene_root(skinned_asset) != nullptr,
                    "FBX asset starts with a scene root");

        void *wrong_material = rt_material3d_new_color(1.0, 0.0, 0.0);
        void *wrong_mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
        void *valid_morph = rt_morphtarget3d_new(3);
        EXPECT_TRUE(wrong_material != nullptr, "Wrong-class material fixture object is created");
        EXPECT_TRUE(wrong_mesh != nullptr, "Wrong-class mesh fixture object is created");
        EXPECT_TRUE(valid_morph != nullptr, "Valid morph target fixture object is created");

        void **saved_meshes = view->meshes;
        view->meshes = nullptr;
        view->mesh_count = 1;
        view->mesh_capacity = 1;
        EXPECT_TRUE(rt_fbx_mesh_count(skinned_asset) == 0,
                    "FBX mesh count rejects missing backing storage");
        EXPECT_TRUE(rt_fbx_get_mesh(skinned_asset, 0) == nullptr,
                    "FBX mesh accessor rejects missing backing storage");
        view->meshes = saved_meshes;
        view->mesh_count = 99;
        view->mesh_capacity = 1;
        EXPECT_TRUE(rt_fbx_mesh_count(skinned_asset) == 1,
                    "FBX mesh count clamps corrupt count to capacity");
        EXPECT_TRUE(rt_fbx_get_mesh(skinned_asset, 1) == nullptr,
                    "FBX mesh accessor rejects indexes past repaired count");
        void *saved_mesh = view->meshes[0];
        view->meshes[0] = wrong_material;
        EXPECT_TRUE(rt_fbx_get_mesh(skinned_asset, 0) == nullptr,
                    "FBX mesh accessor rejects wrong-class mesh slots");
        view->meshes[0] = saved_mesh;

        void **saved_animations = view->animations;
        view->animations = nullptr;
        view->animation_count = 1;
        view->animation_capacity = 1;
        EXPECT_TRUE(rt_fbx_animation_count(skinned_asset) == 0,
                    "FBX animation count rejects missing backing storage");
        view->animations = saved_animations;
        view->animation_count = 99;
        view->animation_capacity = 1;
        EXPECT_TRUE(rt_fbx_animation_count(skinned_asset) == 1,
                    "FBX animation count clamps corrupt count to capacity");
        EXPECT_TRUE(rt_fbx_get_animation(skinned_asset, 1) == nullptr,
                    "FBX animation accessor rejects indexes past repaired count");
        EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_fbx_get_animation_name(skinned_asset, 1)), "") ==
                        0,
                    "FBX animation names use empty fallback for repaired-invalid indexes");
        void *saved_animation = view->animations[0];
        view->animations[0] = wrong_mesh;
        EXPECT_TRUE(rt_fbx_get_animation(skinned_asset, 0) == nullptr,
                    "FBX animation accessor rejects wrong-class animation slots");
        EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_fbx_get_animation_name(skinned_asset, 0)), "") ==
                        0,
                    "FBX animation names use empty fallback for wrong-class animation slots");
        view->animations[0] = saved_animation;

        void *saved_skeleton = view->skeleton;
        view->skeleton = wrong_mesh;
        EXPECT_TRUE(rt_fbx_get_skeleton(skinned_asset) == nullptr,
                    "FBX skeleton accessor rejects wrong-class skeleton slots");
        view->skeleton = saved_skeleton;

        void *saved_scene_root = view->scene_root;
        view->scene_root = wrong_mesh;
        EXPECT_TRUE(rt_fbx_get_scene_root(skinned_asset) == nullptr,
                    "FBX scene root accessor rejects wrong-class root slots");
        view->scene_root = saved_scene_root;

        view->morph_count = 99;
        view->morph_capacity = 1;
        EXPECT_TRUE(rt_fbx_get_morph_target(skinned_asset, 1) == nullptr,
                    "FBX morph accessor rejects indexes past repaired count");
        void **saved_morph_targets = view->morph_targets;
        int32_t saved_morph_count = view->morph_count;
        int32_t saved_morph_capacity = view->morph_capacity;
        void *morph_slots[1] = {valid_morph};
        view->morph_targets = morph_slots;
        view->morph_count = 1;
        view->morph_capacity = 1;
        EXPECT_TRUE(rt_fbx_get_morph_target(skinned_asset, 0) == valid_morph,
                    "FBX morph accessor preserves valid morph slots");
        morph_slots[0] = wrong_material;
        EXPECT_TRUE(rt_fbx_get_morph_target(skinned_asset, 0) == nullptr,
                    "FBX morph accessor rejects wrong-class morph slots");
        view->morph_targets = saved_morph_targets;
        view->morph_count = saved_morph_count;
        view->morph_capacity = saved_morph_capacity;

        if (rt_obj_release_check0(skinned_asset))
            rt_obj_free(skinned_asset);
        if (valid_morph && rt_obj_release_check0(valid_morph))
            rt_obj_free(valid_morph);
        if (wrong_mesh && rt_obj_release_check0(wrong_mesh))
            rt_obj_free(wrong_mesh);
        if (wrong_material && rt_obj_release_check0(wrong_material))
            rt_obj_free(wrong_material);
    }

    const char *material_path = "/tmp/zanna_fbx_asset_corrupt_counts_material.fbx";
    EXPECT_TRUE(write_fbx_fixture(material_path),
                "Material FBX fixture can be written for asset corruption test");
    void *material_asset = rt_fbx_load(rt_const_cstr(material_path));
    EXPECT_TRUE(material_asset != nullptr, "Direct FBX.Load parses material fixture");
    if (material_asset) {
        auto *view = static_cast<FbxAssetView *>(material_asset);
        EXPECT_TRUE(rt_fbx_material_count(material_asset) == 1,
                    "FBX asset starts with one material");
        void **saved_materials = view->materials;
        view->materials = nullptr;
        view->material_count = 1;
        view->material_capacity = 1;
        EXPECT_TRUE(rt_fbx_material_count(material_asset) == 0,
                    "FBX material count rejects missing backing storage");
        view->materials = saved_materials;
        view->material_count = 99;
        view->material_capacity = 1;
        EXPECT_TRUE(rt_fbx_material_count(material_asset) == 1,
                    "FBX material count clamps corrupt count to capacity");
        EXPECT_TRUE(rt_fbx_get_material(material_asset, 1) == nullptr,
                    "FBX material accessor rejects indexes past repaired count");
        void *wrong_mesh = rt_mesh3d_new_box(1.0, 1.0, 1.0);
        EXPECT_TRUE(wrong_mesh != nullptr, "Wrong-class mesh fixture object is created");
        void *saved_material = view->materials[0];
        view->materials[0] = wrong_mesh;
        EXPECT_TRUE(rt_fbx_get_material(material_asset, 0) == nullptr,
                    "FBX material accessor rejects wrong-class material slots");
        view->materials[0] = saved_material;

        if (rt_obj_release_check0(material_asset))
            rt_obj_free(material_asset);
        if (wrong_mesh && rt_obj_release_check0(wrong_mesh))
            rt_obj_free(wrong_mesh);
    }
    std::remove(skinned_path);
    std::remove(material_path);
}

static void test_model3d_rejects_truncated_fbx() {
    const char *path = "/tmp/zanna_model3d_truncated_fixture.fbx";
    bool wrote_fixture = write_truncated_fbx_fixture(path);
    EXPECT_TRUE(wrote_fixture, "Truncated FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model == nullptr, "SceneAsset.Load returns null for truncated FBX input");
    EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                "SceneAsset.Load records an error for truncated FBX input");
    EXPECT_TRUE(rt_asset_error_get_message() != nullptr &&
                    std::strlen(rt_asset_error_get_message()) > 0,
                "SceneAsset.Load exposes a non-empty truncated FBX error");
    std::remove(path);
}

/// @brief Verify missing FBX files are recoverable through the high-level SceneAsset API.
/// @details Content loaders report missing files as NULL plus last-error state so fallback asset
/// flows can continue without aborting the program.
static void test_model3d_missing_fbx_returns_null_without_trap() {
    const char *path = "/tmp/zanna_model3d_missing_recoverable_fixture.fbx";
    std::remove(path);

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model == nullptr, "SceneAsset.Load returns null for missing FBX files");
    EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                "SceneAsset.Load records an error for missing FBX files");
}

static void test_model3d_loads_demo_fbx_textures() {
    const char *path = find_existing_path({
#ifdef ZANNA_SOURCE_DIR
        ZANNA_SOURCE_DIR "/examples/games/3dbaseball/model.fbx",
#endif
        "examples/games/3dbaseball/model.fbx",
        "../examples/games/3dbaseball/model.fbx"});
    if (!path) {
        skip_test("3dbaseball FBX fixture is not present in this checkout");
        return;
    }

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load parses the 3dbaseball FBX asset");
    if (!model)
        return;

    bool saw_textured_material = false;
    int64_t material_count = rt_model3d_get_material_count(model);
    for (int64_t i = 0; i < material_count; i++) {
        auto *mat = static_cast<rt_material3d *>(rt_model3d_get_material(model, i));
        if (mat && mat->texture) {
            saw_textured_material = true;
            break;
        }
    }

    EXPECT_TRUE(
        saw_textured_material,
        "FBX imports preserve demo diffuse textures when texture files sit beside the asset");
}

static void mesh_position_bounds(rt_mesh3d *mesh, float out_min[3], float out_max[3]) {
    for (int a = 0; a < 3; a++) {
        out_min[a] = 0.0f;
        out_max[a] = 0.0f;
    }
    if (!mesh || !mesh->vertices || mesh->vertex_count == 0)
        return;
    for (int a = 0; a < 3; a++) {
        out_min[a] = mesh->vertices[0].pos[a];
        out_max[a] = mesh->vertices[0].pos[a];
    }
    for (uint32_t v = 1; v < mesh->vertex_count; v++) {
        for (int a = 0; a < 3; a++) {
            if (mesh->vertices[v].pos[a] < out_min[a])
                out_min[a] = mesh->vertices[v].pos[a];
            if (mesh->vertices[v].pos[a] > out_max[a])
                out_max[a] = mesh->vertices[v].pos[a];
        }
    }
}

static void test_model3d_loads_gltfpack_meshopt_fixture() {
    /* Real gltfpack output (npx gltfpack -noq -c): EXT_meshopt_compression in
     * extensionsRequired, 7 ATTRIBUTES + 1 TRIANGLES compressed views, one skin and
     * two animation clips. The uncompressed source is its committed twin. */
    const char *compressed = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/skinned_agent_meshopt.glb",
         "../examples/3d/openworld_slice/assets/models/skinned_agent_meshopt.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/skinned_agent_meshopt.glb"
#endif
        });
    const char *reference = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/skinned_agent.gltf",
         "../examples/3d/openworld_slice/assets/models/skinned_agent.gltf",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/skinned_agent.gltf"
#endif
        });
    EXPECT_TRUE(compressed != nullptr && reference != nullptr,
                "gltfpack meshopt fixture and its uncompressed twin are present");
    if (!compressed || !reference)
        return;
    void *ref_model = rt_model3d_load(rt_const_cstr(reference));
    void *cmp_model = rt_model3d_load(rt_const_cstr(compressed));
    EXPECT_TRUE(ref_model != nullptr, "Uncompressed skinned twin loads");
    EXPECT_TRUE(cmp_model != nullptr, "gltfpack meshopt-compressed asset loads");
    if (!ref_model || !cmp_model)
        return;
    EXPECT_TRUE(rt_model3d_get_mesh_count(cmp_model) == rt_model3d_get_mesh_count(ref_model),
                "meshopt asset keeps the mesh count");
    EXPECT_TRUE(rt_model3d_get_skeleton_count(cmp_model) ==
                    rt_model3d_get_skeleton_count(ref_model),
                "meshopt asset keeps the skeleton count");
    EXPECT_TRUE(rt_model3d_get_animation_count(cmp_model) ==
                    rt_model3d_get_animation_count(ref_model),
                "meshopt asset keeps the animation count");
    auto *ref_mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(ref_model, 0));
    auto *cmp_mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(cmp_model, 0));
    EXPECT_TRUE(ref_mesh != nullptr && cmp_mesh != nullptr, "Both twins expose mesh 0");
    if (!ref_mesh || !cmp_mesh)
        return;
    EXPECT_TRUE(cmp_mesh->vertex_count == ref_mesh->vertex_count &&
                    cmp_mesh->index_count == ref_mesh->index_count,
                "meshopt mesh keeps vertex and index counts");
    /* gltfpack reorders vertices for cache locality; -noq keeps float payloads exact,
     * so the reorder-invariant position bounds must match bit-for-bit. */
    float ref_min[3];
    float ref_max[3];
    float cmp_min[3];
    float cmp_max[3];
    mesh_position_bounds(ref_mesh, ref_min, ref_max);
    mesh_position_bounds(cmp_mesh, cmp_min, cmp_max);
    EXPECT_TRUE(ref_min[0] == cmp_min[0] && ref_min[1] == cmp_min[1] && ref_min[2] == cmp_min[2] &&
                    ref_max[0] == cmp_max[0] && ref_max[1] == cmp_max[1] &&
                    ref_max[2] == cmp_max[2],
                "meshopt-decoded positions match the uncompressed twin exactly");
    EXPECT_TRUE(cmp_mesh->bone_count == ref_mesh->bone_count,
                "meshopt mesh keeps the skinning palette size");
    auto *ref_anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(ref_model, 0));
    auto *cmp_anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(cmp_model, 0));
    EXPECT_TRUE(ref_anim != nullptr && cmp_anim != nullptr, "Both twins expose animation 0");
    if (ref_anim && cmp_anim)
        EXPECT_NEAR(cmp_anim->duration,
                    ref_anim->duration,
                    0.0001,
                    "meshopt animation keeps its clip duration");
    if (rt_obj_release_check0(ref_model))
        rt_obj_free(ref_model);
    if (rt_obj_release_check0(cmp_model))
        rt_obj_free(cmp_model);
}

static void test_textureasset3d_decodes_basislz_etc1s_ktx2() {
    /* Real gltfpack -tc output: KTX2 supercompression scheme 1 (BasisLZ/ETC1S),
     * 16x16 with 5 mips, quadrant colors red/green/blue/white. */
    const char *path = find_existing_path(
        {"examples/3d/openworld_slice/assets/textures/quad_etc1s.ktx2",
         "../examples/3d/openworld_slice/assets/textures/quad_etc1s.ktx2",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/textures/quad_etc1s.ktx2"
#endif
        });
    EXPECT_TRUE(path != nullptr, "ETC1S KTX2 fixture is present");
    if (!path)
        return;
    void *asset = rt_textureasset3d_load_ktx2(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "BasisLZ/ETC1S KTX2 loads");
    if (!asset)
        return;
    EXPECT_TRUE(rt_textureasset3d_get_width(asset) == 16 &&
                    rt_textureasset3d_get_height(asset) == 16,
                "ETC1S texture keeps its dimensions");
    EXPECT_TRUE(rt_textureasset3d_get_mip_count(asset) == 5, "ETC1S texture keeps its mip chain");
    rt_string format = rt_textureasset3d_get_format(asset);
    EXPECT_TRUE(format && std::strcmp(rt_string_cstr(format), "rgba8") == 0,
                "ETC1S decodes to an rgba8 asset");
    void *pixels = rt_textureasset3d_get_pixels(asset);
    EXPECT_TRUE(pixels != nullptr, "ETC1S texture exposes decoded pixels");
    if (pixels) {
        struct {
            int64_t x;
            int64_t y;
            int r;
            int g;
            int b;
            const char *name;
        } quads[4] = {{3, 3, 255, 0, 0, "red"},
                      {12, 3, 0, 255, 0, "green"},
                      {3, 12, 0, 0, 255, "blue"},
                      {12, 12, 255, 255, 255, "white"}};

        for (int q = 0; q < 4; q++) {
            uint32_t rgba = (uint32_t)rt_pixels_get_rgba(pixels, quads[q].x, quads[q].y);
            int r = (int)((rgba >> 24) & 0xFF);
            int g = (int)((rgba >> 16) & 0xFF);
            int b = (int)((rgba >> 8) & 0xFF);
            char message[96];
            snprintf(message, sizeof(message), "ETC1S decodes the %s quadrant", quads[q].name);
            EXPECT_TRUE(std::abs(r - quads[q].r) <= 32 && std::abs(g - quads[q].g) <= 32 &&
                            std::abs(b - quads[q].b) <= 32,
                        message);
        }
    }
    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);

    /* BC6H (vkFormat 143): four flat mode-3 blocks with mid-range HDR values;
     * expectations are the validated decoder's own output (bit-exact against an
     * independent reference across all 14 modes). */
    const char *bc6h_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/textures/quad_bc6h.ktx2",
         "../examples/3d/openworld_slice/assets/textures/quad_bc6h.ktx2",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/textures/quad_bc6h.ktx2"
#endif
        });
    EXPECT_TRUE(bc6h_path != nullptr, "BC6H KTX2 fixture is present");
    if (bc6h_path) {
        void *bc6h_asset = rt_textureasset3d_load_ktx2(rt_const_cstr(bc6h_path));
        EXPECT_TRUE(bc6h_asset != nullptr, "BC6H KTX2 loads");
        if (bc6h_asset) {
            void *bc6h_pixels = rt_textureasset3d_get_pixels(bc6h_asset);
            EXPECT_TRUE(bc6h_pixels != nullptr, "BC6H decodes to pixels");
            if (bc6h_pixels) {
                struct {
                    int64_t x;
                    int64_t y;
                    int r;
                    int g;
                    int b;
                } blocks[4] = {
                    {1, 1, 64, 0, 0}, {5, 1, 0, 24, 0}, {1, 5, 0, 0, 4}, {5, 5, 64, 24, 4}};

                for (int q = 0; q < 4; q++) {
                    uint32_t rgba =
                        (uint32_t)rt_pixels_get_rgba(bc6h_pixels, blocks[q].x, blocks[q].y);
                    int r = (int)((rgba >> 24) & 0xFF);
                    int g = (int)((rgba >> 16) & 0xFF);
                    int b = (int)((rgba >> 8) & 0xFF);
                    EXPECT_TRUE(std::abs(r - blocks[q].r) <= 1 && std::abs(g - blocks[q].g) <= 1 &&
                                    std::abs(b - blocks[q].b) <= 1,
                                "BC6H block decodes to the expected clamped color");
                }
            }
            if (rt_obj_release_check0(bc6h_asset))
                rt_obj_free(bc6h_asset);
        }
    }

    /* UASTC twin (scheme 2 + UASTC DFD): same quadrants through the UASTC block decoder. */
    const char *uastc_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/textures/quad_uastc.ktx2",
         "../examples/3d/openworld_slice/assets/textures/quad_uastc.ktx2",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/textures/quad_uastc.ktx2"
#endif
        });
    EXPECT_TRUE(uastc_path != nullptr, "UASTC KTX2 fixture is present");
    if (uastc_path) {
        void *uastc_asset = rt_textureasset3d_load_ktx2(rt_const_cstr(uastc_path));
        EXPECT_TRUE(uastc_asset != nullptr, "UASTC KTX2 loads");
        if (uastc_asset) {
            void *uastc_pixels = rt_textureasset3d_get_pixels(uastc_asset);
            EXPECT_TRUE(uastc_pixels != nullptr, "UASTC texture exposes decoded pixels");
            if (uastc_pixels) {
                struct {
                    int64_t x;
                    int64_t y;
                    int r;
                    int g;
                    int b;
                    const char *name;
                } quads[4] = {{3, 3, 255, 0, 0, "red"},
                              {12, 3, 0, 255, 0, "green"},
                              {3, 12, 0, 0, 255, "blue"},
                              {12, 12, 255, 255, 255, "white"}};

                for (int q = 0; q < 4; q++) {
                    uint32_t rgba =
                        (uint32_t)rt_pixels_get_rgba(uastc_pixels, quads[q].x, quads[q].y);
                    int r = (int)((rgba >> 24) & 0xFF);
                    int g = (int)((rgba >> 16) & 0xFF);
                    int b = (int)((rgba >> 8) & 0xFF);
                    char message[96];
                    snprintf(
                        message, sizeof(message), "UASTC decodes the %s quadrant", quads[q].name);
                    EXPECT_TRUE(std::abs(r - quads[q].r) <= 16 && std::abs(g - quads[q].g) <= 16 &&
                                    std::abs(b - quads[q].b) <= 16,
                                message);
                }
            }
            if (rt_obj_release_check0(uastc_asset))
                rt_obj_free(uastc_asset);
        }
    }

    /* End-to-end: a GLB whose base color texture is that ETC1S KTX2 (KHR_texture_basisu). */
    const char *glb = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/quad_etc1s.glb",
         "../examples/3d/openworld_slice/assets/models/quad_etc1s.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/quad_etc1s.glb"
#endif
        });
    EXPECT_TRUE(glb != nullptr, "ETC1S GLB fixture is present");
    if (!glb)
        return;
    void *model = rt_model3d_load(rt_const_cstr(glb));
    EXPECT_TRUE(model != nullptr, "GLB with BasisU/ETC1S texture loads");
    if (!model)
        return;
    auto *material = static_cast<rt_material3d *>(rt_model3d_get_material(model, 0));
    EXPECT_TRUE(material != nullptr && material->texture != nullptr,
                "BasisU/ETC1S base color texture binds to the material");
    if (rt_obj_release_check0(model))
        rt_obj_free(model);
}

static void test_model3d_loads_draco_sequential_fixture() {
    /* Real gltf-pipeline output: KHR_draco_mesh_compression in extensionsRequired.
     * Both encodings must decode: sequential (compressionLevel 0) and standard
     * edgebreaker (compressionLevel 7). */
    const char *seq_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/quad_draco_seq.glb",
         "../examples/3d/openworld_slice/assets/models/quad_draco_seq.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/quad_draco_seq.glb"
#endif
        });
    EXPECT_TRUE(seq_path != nullptr, "Draco sequential fixture is present");
    if (!seq_path)
        return;
    void *model = rt_model3d_load(rt_const_cstr(seq_path));
    EXPECT_TRUE(model != nullptr, "Draco sequential-encoded GLB loads");
    if (model) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                    "Draco mesh decodes the triangle");
        if (mesh && mesh->vertex_count == 3) {
            /* Source geometry: (0,0,0) (1,0,0) (0,1,0), uv (0,0) (1,0) (0,1);
             * positions are quantized (14-bit default), so compare with tolerance. */
            float max_x = 0.0f;
            float max_y = 0.0f;
            float max_u = 0.0f;
            float max_v = 0.0f;
            for (uint32_t v = 0; v < 3; v++) {
                if (mesh->vertices[v].pos[0] > max_x)
                    max_x = mesh->vertices[v].pos[0];
                if (mesh->vertices[v].pos[1] > max_y)
                    max_y = mesh->vertices[v].pos[1];
                if (mesh->vertices[v].uv[0] > max_u)
                    max_u = mesh->vertices[v].uv[0];
                if (mesh->vertices[v].uv[1] > max_v)
                    max_v = mesh->vertices[v].uv[1];
            }
            EXPECT_NEAR(max_x, 1.0, 0.001, "Draco dequantizes position X");
            EXPECT_NEAR(max_y, 1.0, 0.001, "Draco dequantizes position Y");
            EXPECT_NEAR(max_u, 1.0, 0.001, "Draco dequantizes texcoord U");
            EXPECT_NEAR(max_v, 1.0, 0.001, "Draco dequantizes texcoord V");
        }
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
    }

    const char *eb_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/quad_draco_eb.glb",
         "../examples/3d/openworld_slice/assets/models/quad_draco_eb.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/quad_draco_eb.glb"
#endif
        });
    EXPECT_TRUE(eb_path != nullptr, "Draco edgebreaker fixture is present");
    if (eb_path) {
        void *eb_model = rt_model3d_load(rt_const_cstr(eb_path));
        EXPECT_TRUE(eb_model != nullptr, "Draco edgebreaker-encoded GLB loads");
        if (eb_model) {
            auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(eb_model, 0));
            EXPECT_TRUE(mesh != nullptr && mesh->vertex_count == 3 && mesh->index_count == 3,
                        "Draco edgebreaker mesh decodes the triangle");
            if (mesh && mesh->vertex_count == 3) {
                float max_x = 0.0f;
                float max_y = 0.0f;
                float max_u = 0.0f;
                float max_v = 0.0f;
                for (uint32_t v = 0; v < 3; v++) {
                    if (mesh->vertices[v].pos[0] > max_x)
                        max_x = mesh->vertices[v].pos[0];
                    if (mesh->vertices[v].pos[1] > max_y)
                        max_y = mesh->vertices[v].pos[1];
                    if (mesh->vertices[v].uv[0] > max_u)
                        max_u = mesh->vertices[v].uv[0];
                    if (mesh->vertices[v].uv[1] > max_v)
                        max_v = mesh->vertices[v].uv[1];
                }
                EXPECT_NEAR(max_x, 1.0, 0.001, "edgebreaker decode dequantizes position X");
                EXPECT_NEAR(max_y, 1.0, 0.001, "edgebreaker decode dequantizes position Y");
                EXPECT_NEAR(max_u, 1.0, 0.001, "edgebreaker decode dequantizes texcoord U");
                EXPECT_NEAR(max_v, 1.0, 0.001, "edgebreaker decode dequantizes texcoord V");
            }
            if (rt_obj_release_check0(eb_model))
                rt_obj_free(eb_model);
        }
    }
}

/// Validate a Draco-decoded unit sphere: exact point/face counts must match the
/// reference decoder, positions sit on the unit sphere within quantization error,
/// normals agree with the analytic direction, and UVs stay in range.
static void expect_draco_unit_sphere(const char *label,
                                     const char *path,
                                     uint32_t expected_points,
                                     uint32_t expected_faces) {
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, label);
    if (!model)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(model, 0));
    EXPECT_TRUE(mesh != nullptr, "Draco sphere has a mesh");
    if (mesh) {
        EXPECT_TRUE((uint32_t)mesh->vertex_count == expected_points,
                    "Draco sphere point count matches the reference decoder");
        EXPECT_TRUE((uint32_t)mesh->index_count == expected_faces * 3u,
                    "Draco sphere face count survives decode");
        int bad_radius = 0;
        int bad_normal = 0;
        int bad_uv = 0;
        for (uint32_t v = 0; v < (uint32_t)mesh->vertex_count; v++) {
            const float *pos = mesh->vertices[v].pos;
            const float *nrm = mesh->vertices[v].normal;
            const float *uv = mesh->vertices[v].uv;
            float radius = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
            if (radius < 0.995f || radius > 1.005f)
                bad_radius++;
            if (radius > 0.5f) {
                float dot = (nrm[0] * pos[0] + nrm[1] * pos[1] + nrm[2] * pos[2]) / radius;
                if (dot < 0.97f)
                    bad_normal++;
            }
            if (uv[0] < -0.01f || uv[0] > 1.01f || uv[1] < -0.01f || uv[1] > 1.01f)
                bad_uv++;
        }
        EXPECT_TRUE(bad_radius == 0, "every decoded position lies on the unit sphere");
        EXPECT_TRUE(bad_normal == 0, "every decoded normal matches the analytic direction");
        EXPECT_TRUE(bad_uv == 0, "every decoded texcoord stays in [0,1]");
    }
    if (rt_obj_release_check0(model))
        rt_obj_free(model);
}

static void test_model3d_loads_draco_edgebreaker_spheres() {
    /* Standard edgebreaker (gltf-pipeline -d, 256 faces): exercises C/R/L/S/E
     * symbols, interior-edge completion, a corner-mapped TEXCOORD decoder with
     * real seam bits, geometric-normal prediction, and multi-parallelogram. */
    const char *eb_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/sphere_draco_eb.glb",
         "../examples/3d/openworld_slice/assets/models/sphere_draco_eb.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/sphere_draco_eb.glb"
#endif
        });
    EXPECT_TRUE(eb_path != nullptr, "standard-edgebreaker sphere fixture is present");
    if (eb_path)
        expect_draco_unit_sphere("standard-edgebreaker Draco sphere loads", eb_path, 168, 256);

    /* Valence edgebreaker (reference draco_encoder -cl 7, 1104 faces so the
     * encoder picks the valence traversal): exercises the context-modeled symbol
     * path plus 23 S-symbol merges. */
    const char *valence_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/sphere_draco_valence.glb",
         "../examples/3d/openworld_slice/assets/models/sphere_draco_valence.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/sphere_draco_valence.glb"
#endif
        });
    EXPECT_TRUE(valence_path != nullptr, "valence-edgebreaker sphere fixture is present");
    if (valence_path)
        expect_draco_unit_sphere("valence-edgebreaker Draco sphere loads", valence_path, 623, 1104);

    /* Reference draco_encoder -cl 10 (speed 0): valence traversal plus the
     * prediction-degree position traverser. */
    const char *pd_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/sphere_draco_pd.glb",
         "../examples/3d/openworld_slice/assets/models/sphere_draco_pd.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/sphere_draco_pd.glb"
#endif
        });
    EXPECT_TRUE(pd_path != nullptr, "prediction-degree sphere fixture is present");
    if (pd_path)
        expect_draco_unit_sphere("prediction-degree Draco sphere loads", pd_path, 623, 1104);
}

static void test_model3d_vscn_v3_rig_roundtrip() {
    /* Bake pipeline round-trip: load a skinned glTF, instantiate, save as .vscn
     * (v3 adds skeletons + animation clips), reload through SceneAsset, and
     * verify the rig survived. */
    const char *src_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/skinned_agent.gltf",
         "../examples/3d/openworld_slice/assets/models/skinned_agent.gltf",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/skinned_agent.gltf"
#endif
        });
    EXPECT_TRUE(src_path != nullptr, "skinned agent fixture is present");
    if (!src_path)
        return;
    void *model = rt_model3d_load(rt_const_cstr(src_path));
    EXPECT_TRUE(model != nullptr, "skinned agent loads");
    if (!model)
        return;
    int64_t src_skeletons = rt_model3d_get_skeleton_count(model);
    EXPECT_TRUE(src_skeletons > 0, "skinned agent carries a skeleton");
    int64_t src_anims = rt_model3d_get_animation_count(model);
    EXPECT_TRUE(src_anims > 0, "skinned agent carries animation clips");
    void *scene = rt_model3d_instantiate_scene(model);
    EXPECT_TRUE(scene != nullptr, "skinned agent instantiates");
    const char *baked_path = "/tmp/zanna_model3d_baked_rig.vscn";
    if (scene) {
        EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(baked_path)) != 0,
                    "skinned scene saves as vscn");
        if (rt_obj_release_check0(scene))
            rt_obj_free(scene);
    }
    if (rt_obj_release_check0(model))
        rt_obj_free(model);

    void *baked = rt_model3d_load(rt_const_cstr(baked_path));
    EXPECT_TRUE(baked != nullptr, "baked v3 vscn loads as a SceneAsset");
    if (baked) {
        EXPECT_TRUE(rt_model3d_get_skeleton_count(baked) == src_skeletons,
                    "baked vscn exposes its mesh skeleton through SceneAsset");
        EXPECT_TRUE(rt_model3d_get_animation_count(baked) == src_anims,
                    "baked vscn preserves the animation clips");
        auto *mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(baked, 0));
        EXPECT_TRUE(mesh != nullptr && mesh->skeleton_ref != nullptr,
                    "baked vscn rebinds the skeleton to the mesh");
        EXPECT_TRUE(mesh != nullptr && rt_model3d_get_skeleton(baked, 0) == mesh->skeleton_ref,
                    "baked vscn enumerates the skeleton attached to its mesh");
        EXPECT_TRUE(mesh != nullptr && mesh->bone_count > 0,
                    "baked vscn keeps the mesh bone palette");
        void *instance = rt_model3d_instantiate(baked);
        auto *instance_root = static_cast<rt_scene_node3d *>(instance);
        EXPECT_TRUE(instance_root != nullptr && instance_root->bound_animator != nullptr,
                    "baked vscn automatically binds its skeletal animator");
        if (instance && rt_obj_release_check0(instance))
            rt_obj_free(instance);
        if (rt_obj_release_check0(baked))
            rt_obj_free(baked);
    }
    std::remove(baked_path);
}

static void test_scene3d_vscn_v5_source_texture_roundtrip() {
    struct SourceCase {
        const char *kind;
        const char *decode_name;
        std::vector<uint8_t> bytes;
    };

    const char *png_path =
        find_existing_path({"src/tests/fixtures/runtime/test_sprite_load.png",
                            "../src/tests/fixtures/runtime/test_sprite_load.png",
#ifdef ZANNA_SOURCE_DIR
                            ZANNA_SOURCE_DIR "/src/tests/fixtures/runtime/test_sprite_load.png"
#endif
        });
    const char *bmp_path =
        find_existing_path({"src/tests/fixtures/runtime/test_sprite_load.bmp",
                            "../src/tests/fixtures/runtime/test_sprite_load.bmp",
#ifdef ZANNA_SOURCE_DIR
                            ZANNA_SOURCE_DIR "/src/tests/fixtures/runtime/test_sprite_load.bmp"
#endif
        });
    const char *ktx2_path = find_existing_path(
        {"examples/3d/openworld_slice/assets/textures/checker_rgba8.ktx2",
         "../examples/3d/openworld_slice/assets/textures/checker_rgba8.ktx2",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/textures/checker_rgba8.ktx2"
#endif
        });
    static const char jpeg_base64[] =
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
    static const uint8_t gif_bytes[] = {'G',  'I',  'F',  '8',  '7',  'a',  0x01, 0x00, 0x01,
                                        0x00, 0x80, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
                                        0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3B};
    EXPECT_TRUE(png_path && bmp_path && ktx2_path, "VSCN v5 source-container fixtures are present");
    if (!png_path || !bmp_path || !ktx2_path)
        return;
    std::vector<SourceCase> cases = {{"png", "source.png", {}},
                                     {"jpeg", "source.jpg", {}},
                                     {"gif", "source.gif", {}},
                                     {"bmp", "source.bmp", {}},
                                     {"ktx2", "source.ktx2", {}}};
    EXPECT_TRUE(read_binary_file(png_path, cases[0].bytes), "VSCN v5 PNG fixture is readable");
    EXPECT_TRUE(base64_decode_fixture(jpeg_base64, cases[1].bytes),
                "VSCN v5 embedded JPEG fixture is decodable");
    cases[2].bytes.assign(gif_bytes, gif_bytes + sizeof(gif_bytes));
    EXPECT_TRUE(read_binary_file(bmp_path, cases[3].bytes), "VSCN v5 BMP fixture is readable");
    EXPECT_TRUE(read_binary_file(ktx2_path, cases[4].bytes), "VSCN v5 KTX2 fixture is readable");

    for (size_t index = 0; index < cases.size(); ++index) {
        const SourceCase &source_case = cases[index];
        const std::string path = "/tmp/zanna_vscn_v5_source_" + std::to_string(index) + ".vscn";
        void *decoded = nullptr;
        void *texture = nullptr;
        if (std::strcmp(source_case.kind, "ktx2") == 0) {
            texture = rt_textureasset3d_load_ktx2_memory_strict(source_case.bytes.data(),
                                                                source_case.bytes.size());
        } else {
            decoded = rt_asset_decode_typed(
                source_case.decode_name, source_case.bytes.data(), source_case.bytes.size());
            if (decoded)
                texture = rt_textureasset3d_wrap_encoded_pixels(
                    decoded, source_case.bytes.data(), source_case.bytes.size(), source_case.kind);
        }
        EXPECT_TRUE(texture != nullptr, "supported VSCN source fixture decodes and wraps");
        if (!texture) {
            release_test_ref(decoded);
            continue;
        }
        void *material = rt_material3d_new();
        void *node = rt_scene_node3d_new();
        void *scene = rt_scene3d_new();
        rt_material3d_set_texture(material, texture);
        rt_material3d_set_normal_map(material, texture);
        rt_scene_node3d_set_name(node, rt_const_cstr("SourceTextureNode"));
        rt_scene_node3d_set_material(node, material);
        rt_scene3d_add(scene, node);
        EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path.c_str())) == 1,
                    "Scene3D.Save emits a VSCN v5 source texture");
        std::vector<uint8_t> serialized;
        EXPECT_TRUE(read_binary_file(path.c_str(), serialized),
                    "VSCN v5 source output is readable");
        std::string serialized_text(serialized.begin(), serialized.end());
        size_t first_source = serialized_text.find("\"sourceBase64\"");
        EXPECT_TRUE(serialized_text.find("\"version\": 5") != std::string::npos &&
                        serialized_text.find("\"kind\": \"source\"") != std::string::npos &&
                        first_source != std::string::npos &&
                        serialized_text.find("\"sourceBase64\"", first_source + 1u) ==
                            std::string::npos,
                    "VSCN v5 emits one deduplicated discriminated source entry");
        void *loaded = rt_scene3d_load(rt_const_cstr(path.c_str()));
        auto *loaded_node = loaded ? static_cast<rt_scene_node3d *>(rt_scene3d_find(
                                         loaded, rt_const_cstr("SourceTextureNode")))
                                   : nullptr;
        auto *loaded_material =
            loaded_node ? static_cast<rt_material3d *>(loaded_node->material) : nullptr;
        EXPECT_TRUE(loaded_material && loaded_material->texture == loaded_material->normal_map,
                    "VSCN v5 restores one shared texture identity across material slots");
        EXPECT_TRUE(loaded_material && texture_source_equals(loaded_material->texture,
                                                             source_case.kind,
                                                             source_case.bytes),
                    "VSCN v5 restores exact source-container kind and bytes");
        if (std::strcmp(source_case.kind, "ktx2") == 0) {
            EXPECT_TRUE(loaded_material &&
                            rt_textureasset3d_get_mip_count(loaded_material->texture) ==
                                rt_textureasset3d_get_mip_count(texture) &&
                            std::strcmp(rt_string_cstr(
                                            rt_textureasset3d_get_format(loaded_material->texture)),
                                        rt_string_cstr(rt_textureasset3d_get_format(texture))) == 0,
                        "VSCN v5 restores KTX2 mip hierarchy and native format");
        }
        if (index == 0) {
            const char *corrupt_path = "/tmp/zanna_vscn_v5_source_corrupt.vscn";
            auto expect_source_rejected = [&](const std::string &mutated,
                                              bool mutation_applied,
                                              const char *mutation_message,
                                              const char *rejection_message) {
                EXPECT_TRUE(mutation_applied, mutation_message);
                if (!mutation_applied)
                    return;
                EXPECT_TRUE(write_text_file(corrupt_path, mutated),
                            "Malformed VSCN v5 source fixture can be written");
                void *rejected = rt_scene3d_load(rt_const_cstr(corrupt_path));
                EXPECT_TRUE(rejected == nullptr, rejection_message);
                EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                            "Malformed VSCN v5 source records a recoverable asset error");
                release_test_ref(rejected);

                void *recovered = rt_scene3d_load(rt_const_cstr(path.c_str()));
                auto *recovered_node =
                    recovered ? static_cast<rt_scene_node3d *>(
                                    rt_scene3d_find(recovered, rt_const_cstr("SourceTextureNode")))
                              : nullptr;
                auto *recovered_material =
                    recovered_node ? static_cast<rt_material3d *>(recovered_node->material)
                                   : nullptr;
                EXPECT_TRUE(recovered_material && texture_source_equals(recovered_material->texture,
                                                                        "png",
                                                                        source_case.bytes),
                            "A rejected VSCN v5 source leaves the next complete load intact");
                release_test_ref(recovered);
            };

            std::string mismatched_magic = serialized_text;
            bool mismatched_magic_applied = replace_first_text(
                mismatched_magic, "\"container\": \"png\"", "\"container\": \"jpeg\"");
            expect_source_rejected(mismatched_magic,
                                   mismatched_magic_applied,
                                   "VSCN v5 source magic mismatch mutation finds its target",
                                   "VSCN v5 rejects source bytes whose magic mismatches the kind");

            std::string unsupported_container = serialized_text;
            bool unsupported_container_applied = replace_first_text(
                unsupported_container, "\"container\": \"png\"", "\"container\": \"tga\"");
            expect_source_rejected(unsupported_container,
                                   unsupported_container_applied,
                                   "VSCN v5 unsupported-container mutation finds its target",
                                   "VSCN v5 rejects an unsupported source container");

            std::string invalid_kind = serialized_text;
            bool invalid_kind_applied =
                replace_first_text(invalid_kind, "\"kind\": \"source\"", "\"kind\": \"unknown\"");
            expect_source_rejected(invalid_kind,
                                   invalid_kind_applied,
                                   "VSCN v5 source-kind mutation finds its target",
                                   "VSCN v5 rejects an unknown texture-entry kind");

            std::string invalid_base64 = serialized_text;
            bool invalid_base64_applied =
                replace_first_json_string_value(invalid_base64, "sourceBase64", "%%%%");
            expect_source_rejected(invalid_base64,
                                   invalid_base64_applied,
                                   "VSCN v5 invalid-Base64 mutation finds its target",
                                   "VSCN v5 rejects malformed source Base64");

            std::string truncated_source = serialized_text;
            bool truncated_source_applied =
                replace_first_json_string_value(truncated_source, "sourceBase64", "AAAA");
            expect_source_rejected(truncated_source,
                                   truncated_source_applied,
                                   "VSCN v5 truncated-source mutation finds its target",
                                   "VSCN v5 rejects a truncated encoded source image");
            std::remove(corrupt_path);
        }
        release_test_ref(loaded);
        release_test_ref(scene);
        release_test_ref(node);
        release_test_ref(material);
        release_test_ref(texture);
        release_test_ref(decoded);
        std::remove(path.c_str());
    }

    /* A changed decoded surface must serialize current RGBA rather than stale encoded bytes. */
    void *decoded =
        rt_asset_decode_typed("source.png", cases[0].bytes.data(), cases[0].bytes.size());
    void *texture = decoded ? rt_textureasset3d_wrap_encoded_pixels(
                                  decoded, cases[0].bytes.data(), cases[0].bytes.size(), "png")
                            : nullptr;
    EXPECT_TRUE(texture != nullptr, "mutation-fallback source texture wraps");
    if (texture) {
        void *texture_pixels = rt_textureasset3d_get_pixels(texture);
        rt_pixels_set(texture_pixels, 0, 0, 0x13579BDFll);
        void *material = rt_material3d_new();
        void *node = rt_scene_node3d_new();
        void *scene = rt_scene3d_new();
        void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
        const char *path = "/tmp/zanna_vscn_v5_mutated_source.vscn";
        rt_material3d_set_texture(material, texture);
        rt_scene_node3d_set_name(node, rt_const_cstr("MutatedTextureNode"));
        rt_scene_node3d_set_material(node, material);
        rt_scene_node3d_set_camera(node, camera);
        rt_scene3d_add(scene, node);
        EXPECT_TRUE(rt_scene3d_save(scene, rt_const_cstr(path)) == 1,
                    "changed source texture scene saves");
        std::vector<uint8_t> serialized;
        EXPECT_TRUE(read_binary_file(path, serialized),
                    "changed source texture output is readable");
        std::string serialized_text(serialized.begin(), serialized.end());
        EXPECT_TRUE(serialized_text.find("\"kind\": \"rgba8\"") != std::string::npos &&
                        serialized_text.find("\"sourceBase64\"") == std::string::npos,
                    "changed decoded Pixels serialize as current RGBA8, not stale source bytes");
        void *loaded = rt_scene3d_load(rt_const_cstr(path));
        auto *loaded_node = loaded ? static_cast<rt_scene_node3d *>(rt_scene3d_find(
                                         loaded, rt_const_cstr("MutatedTextureNode")))
                                   : nullptr;
        auto *loaded_material =
            loaded_node ? static_cast<rt_material3d *>(loaded_node->material) : nullptr;
        EXPECT_TRUE(loaded_material && loaded_material->texture &&
                        rt_pixels_get(loaded_material->texture, 0, 0) == 0x13579BDFll,
                    "mutation fallback reloads the current decoded texels");
        release_test_ref(loaded);
        release_test_ref(scene);
        release_test_ref(node);
        release_test_ref(material);
        release_test_ref(camera);
        std::remove(path);
    }
    release_test_ref(texture);
    release_test_ref(decoded);

    const char tiny_document[] = "{}";
    void *oversized = rt_scene3d_load_from_memory(
        rt_const_cstr("oversized.vscn"), tiny_document, (size_t)256u * 1024u * 1024u + 1u);
    EXPECT_TRUE(oversized == nullptr && rt_asset_error_get_code() == RT_ASSET_ERROR_TOO_LARGE,
                "VSCN memory loading rejects oversized documents before reading the payload");
    release_test_ref(oversized);
}

static void test_model3d_vscn_v5_full_scene_asset_roundtrip() {
    const char *source_path = find_existing_path(
        {"src/tests/fixtures/runtime/assets/gltf/asset_bake_lossy.gltf",
         "../src/tests/fixtures/runtime/assets/gltf/asset_bake_lossy.gltf",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/src/tests/fixtures/runtime/assets/gltf/asset_bake_lossy.gltf"
#endif
        });
    const char *baked_path = "/tmp/zanna_model3d_baked_full_asset_v5.vscn";
    EXPECT_TRUE(source_path != nullptr, "VSCN v5 full-fidelity glTF fixture is present");
    if (!source_path)
        return;
    void *source = rt_model3d_load(rt_const_cstr(source_path));
    EXPECT_TRUE(source != nullptr, "VSCN v5 source SceneAsset loads");
    if (!source)
        return;
    auto *source_mover =
        static_cast<rt_scene_node3d *>(rt_model3d_find_node(source, rt_const_cstr("Mover")));
    auto *source_camera_node =
        static_cast<rt_scene_node3d *>(rt_model3d_find_node(source, rt_const_cstr("CameraNode")));
    auto *source_camera = static_cast<rt_camera3d *>(rt_model3d_get_camera(source, 0, 0));
    auto *source_material = static_cast<rt_material3d *>(rt_model3d_get_material(source, 0));
    EXPECT_TRUE(source_mover != nullptr && source_mover->import_index == 0,
                "VSCN v5 fixture carries stable importer node indexes");
    EXPECT_TRUE(source_material != nullptr, "VSCN v5 fixture carries a material payload");
    EXPECT_TRUE(source_camera_node && source_camera && source_camera_node->camera == source_camera,
                "VSCN v5 fixture binds its authored camera to CameraNode");
    if (source_mover) {
        rt_scene_node3d_set_static(source_mover, 1);
        rt_scene_node3d_set_sync_mode(source_mover,
                                      RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION);
    }
    if (source_material) {
        rt_material3d_set_custom_param(source_material, 8, 0.625);
        rt_material3d_set_custom_param(source_material, 9, -0.375);
        rt_material3d_set_anisotropy(source_material, 8);
    }
    void *light_position = rt_vec3_new(1.0, 2.0, 3.0);
    void *light_direction = rt_vec3_new(0.25, -1.0, 0.5);
    void *source_light_handle = rt_light3d_new_area_rectangle(
        light_position, light_direction, 2.5, 1.25, 0.2, 0.4, 0.8, 0.02, 8.0);
    if (source_light_handle) {
        rt_light3d_set_intensity(source_light_handle, 3.5);
        rt_light3d_set_decay_type(source_light_handle, 3);
        rt_light3d_set_enabled(source_light_handle, 0);
        rt_light3d_set_casts_shadows(source_light_handle, 1);
    }
    if (source_mover)
        rt_scene_node3d_set_light(source_mover, source_light_handle);
    release_test_ref(source_light_handle);
    release_test_ref(light_direction);
    release_test_ref(light_position);
    auto *source_light = source_mover ? static_cast<rt_light3d *>(source_mover->light) : nullptr;
    EXPECT_TRUE(source_light && source_light->type == 4,
                "VSCN v5 fixture carries a native rectangle light");

    auto *extended_animation =
        static_cast<rt_node_animation3d *>(rt_model3d_get_node_animation(source, 0));
    if (extended_animation) {
        const double times[2] = {0.0, 1.0};
        const int64_t paths[] = {RT_NODE_ANIM_PATH_CAMERA_FOV,
                                 RT_NODE_ANIM_PATH_CAMERA_ASPECT,
                                 RT_NODE_ANIM_PATH_CAMERA_NEAR,
                                 RT_NODE_ANIM_PATH_CAMERA_FAR,
                                 RT_NODE_ANIM_PATH_CAMERA_ORTHO_SIZE,
                                 RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE};
        const int64_t interpolations[] = {RT_NODE_ANIM_INTERP_LINEAR,
                                          RT_NODE_ANIM_INTERP_LINEAR,
                                          RT_NODE_ANIM_INTERP_LINEAR,
                                          RT_NODE_ANIM_INTERP_LINEAR,
                                          RT_NODE_ANIM_INTERP_LINEAR,
                                          RT_NODE_ANIM_INTERP_STEP};
        const float values[][2] = {{60.0f, 45.0f},
                                   {1.7777778f, 1.3333334f},
                                   {0.1f, 0.2f},
                                   {100.0f, 200.0f},
                                   {2.0f, 4.0f},
                                   {0.0f, 1.0f}};
        int added_camera_channels = 0;
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
            int64_t channel = rt_node_animation3d_add_channel(extended_animation,
                                                              rt_const_cstr("CameraNode"),
                                                              paths[i],
                                                              interpolations[i],
                                                              2,
                                                              1,
                                                              times,
                                                              values[i]);
            if (channel >= 0) {
                rt_node_animation3d_set_channel_target_node_index(extended_animation, channel, 1);
                added_camera_channels++;
            }
        }
        EXPECT_TRUE(added_camera_channels == 6, "VSCN v5 fixture adds every camera animation path");
    }
    EXPECT_TRUE(rt_model3d_save(nullptr, rt_const_cstr(baked_path)) == 0,
                "SceneAsset.Save rejects a null receiver");
    EXPECT_TRUE(rt_model3d_save(source, rt_const_cstr(baked_path)) == 1,
                "SceneAsset.Save writes a complete VSCN v5 asset");
    std::vector<uint8_t> baked_bytes;
    EXPECT_TRUE(read_binary_file(baked_path, baked_bytes), "VSCN v5 output is readable");
    if (!baked_bytes.empty()) {
        std::string baked_text(reinterpret_cast<const char *>(baked_bytes.data()),
                               baked_bytes.size());
        EXPECT_TRUE(baked_text.find("\"version\": 5") != std::string::npos,
                    "SceneAsset.Save emits VSCN version 5");
        EXPECT_TRUE(baked_text.find("\"nodeAnimations\"") != std::string::npos &&
                        baked_text.find("\"morphTargets\"") != std::string::npos &&
                        baked_text.find("\"importIndex\"") != std::string::npos &&
                        baked_text.find("\"variantMaterials\"") != std::string::npos &&
                        baked_text.find("\"scenes\"") != std::string::npos,
                    "VSCN v5 document carries animation, morph, variant, and scene sections");
    }

    void *baked = rt_model3d_load(rt_const_cstr(baked_path));
    EXPECT_TRUE(baked != nullptr, "VSCN v5 reloads as a SceneAsset");
    if (!baked) {
        if (rt_obj_release_check0(source))
            rt_obj_free(source);
        std::remove(baked_path);
        return;
    }
    EXPECT_TRUE(rt_model3d_get_mesh_count(baked) == rt_model3d_get_mesh_count(source),
                "VSCN v5 preserves the complete mesh inventory");
    EXPECT_TRUE(rt_model3d_get_material_count(baked) == rt_model3d_get_material_count(source),
                "VSCN v5 preserves the complete material inventory");
    EXPECT_TRUE(rt_model3d_get_scene_count(baked) == 2, "VSCN v5 preserves all immutable scenes");
    EXPECT_TRUE(rt_model3d_get_camera_count(baked, 0) == 1 &&
                    rt_model3d_get_camera_count(baked, 1) == 0,
                "VSCN v5 preserves per-scene camera membership");
    EXPECT_TRUE(rt_model3d_get_node_animation_count(baked) == 1,
                "VSCN v5 preserves node and morph animation clips");
    EXPECT_TRUE(rt_model3d_get_morph_target_count(source) > 0 &&
                    rt_model3d_get_morph_target_count(baked) ==
                        rt_model3d_get_morph_target_count(source),
                "VSCN v5 preserves every mesh-attached morph container");
    EXPECT_TRUE(rt_model3d_get_morph_shape_count(source) > 0 &&
                    rt_model3d_get_morph_shape_count(baked) ==
                        rt_model3d_get_morph_shape_count(source),
                "VSCN v5 preserves the aggregate morph-shape inventory");
    EXPECT_TRUE(rt_model3d_get_morph_target(baked, -1) == nullptr &&
                    rt_model3d_get_morph_target(baked, rt_model3d_get_mesh_count(baked)) == nullptr,
                "SceneAsset.GetMorphTarget rejects out-of-range mesh indexes");
    EXPECT_TRUE(rt_model3d_get_variant_count(baked) == 2,
                "VSCN v5 preserves material-variant names");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_model3d_get_scene_name(baked, 0)), "PrimaryScene") == 0 &&
            std::strcmp(rt_string_cstr(rt_model3d_get_scene_name(baked, 1)), "SecondaryScene") == 0,
        "VSCN v5 preserves immutable scene names and order");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_model3d_get_variant_name(baked, 0)), "day") == 0 &&
                    std::strcmp(rt_string_cstr(rt_model3d_get_variant_name(baked, 1)), "night") ==
                        0,
                "VSCN v5 preserves ordered variant display names");
    auto *baked_mover =
        static_cast<rt_scene_node3d *>(rt_model3d_find_node(baked, rt_const_cstr("Mover")));
    auto *baked_camera_node =
        static_cast<rt_scene_node3d *>(rt_model3d_find_node(baked, rt_const_cstr("CameraNode")));
    auto *baked_material = static_cast<rt_material3d *>(rt_model3d_get_material(baked, 0));
    EXPECT_TRUE(baked_mover && source_mover &&
                    baked_mover->import_index == source_mover->import_index &&
                    rt_scene_node3d_get_static(baked_mover) == 1 &&
                    rt_scene_node3d_get_sync_mode(baked_mover) ==
                        RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION,
                "VSCN v5 preserves node import identity and authored flags");
    EXPECT_TRUE(baked_material && source_material &&
                    baked_material->custom_params[8] == source_material->custom_params[8] &&
                    baked_material->custom_params[9] == source_material->custom_params[9] &&
                    rt_material3d_get_anisotropy(baked_material) == 8,
                "VSCN v5 preserves extended material parameters and anisotropy");
    auto *baked_light = baked_mover ? static_cast<rt_light3d *>(baked_mover->light) : nullptr;
    auto vec3_near = [](const double *a, const double *b) {
        return a && b && std::fabs(a[0] - b[0]) <= 1e-12 && std::fabs(a[1] - b[1]) <= 1e-12 &&
               std::fabs(a[2] - b[2]) <= 1e-12;
    };
    EXPECT_TRUE(source_light && baked_light && baked_light->type == source_light->type &&
                    baked_light->enabled == source_light->enabled &&
                    baked_light->casts_shadows == source_light->casts_shadows &&
                    baked_light->decay_type == source_light->decay_type &&
                    baked_light->intensity == source_light->intensity &&
                    baked_light->attenuation == source_light->attenuation &&
                    baked_light->width == source_light->width &&
                    baked_light->height == source_light->height &&
                    baked_light->radius == source_light->radius &&
                    baked_light->range == source_light->range &&
                    vec3_near(baked_light->direction, source_light->direction) &&
                    vec3_near(baked_light->position, source_light->position) &&
                    vec3_near(baked_light->color, source_light->color) &&
                    vec3_near(baked_light->basis_u, source_light->basis_u) &&
                    vec3_near(baked_light->basis_v, source_light->basis_v),
                "VSCN v5 preserves complete native rectangle-light state");

    int64_t mesh_count = rt_model3d_get_mesh_count(source);
    int morph_meshes = 0;
    for (int64_t mesh_index = 0; mesh_index < mesh_count; ++mesh_index) {
        auto *source_mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(source, mesh_index));
        auto *baked_mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(baked, mesh_index));
        EXPECT_TRUE(source_mesh != nullptr && baked_mesh != nullptr,
                    "VSCN v5 mesh slots remain index-aligned");
        if (!source_mesh || !baked_mesh || !source_mesh->morph_targets_ref)
            continue;
        morph_meshes++;
        EXPECT_TRUE(
            rt_model3d_get_morph_target(source, mesh_index) == source_mesh->morph_targets_ref &&
                rt_model3d_get_morph_target(baked, mesh_index) == baked_mesh->morph_targets_ref,
            "SceneAsset.GetMorphTarget remains aligned with mesh indexes");
        EXPECT_TRUE(baked_mesh->morph_targets_ref != nullptr,
                    "VSCN v5 preserves attached static morph containers");
        int64_t shape_count = rt_morphtarget3d_get_shape_count(source_mesh->morph_targets_ref);
        EXPECT_TRUE(rt_morphtarget3d_get_shape_count(baked_mesh->morph_targets_ref) == shape_count,
                    "VSCN v5 preserves every morph shape");
        for (int64_t shape = 0; shape < shape_count; ++shape) {
            rt_morphtarget3d_shape_view_internal source_view = {};
            rt_morphtarget3d_shape_view_internal baked_view = {};
            bool views_ok = rt_morphtarget3d_get_shape_view_internal(
                                source_mesh->morph_targets_ref, shape, &source_view) &&
                            rt_morphtarget3d_get_shape_view_internal(
                                baked_mesh->morph_targets_ref, shape, &baked_view);
            EXPECT_TRUE(views_ok, "VSCN v5 morph shape views remain valid");
            if (!views_ok)
                continue;
            size_t delta_bytes = static_cast<size_t>(source_view.vertex_count) * 3u * sizeof(float);
            EXPECT_TRUE(std::strcmp(source_view.name, baked_view.name) == 0,
                        "VSCN v5 preserves morph shape names");
            EXPECT_TRUE(source_view.weight == baked_view.weight,
                        "VSCN v5 preserves static morph weights");
            EXPECT_TRUE(source_view.vertex_count == baked_view.vertex_count &&
                            std::memcmp(source_view.position_deltas,
                                        baked_view.position_deltas,
                                        delta_bytes) == 0,
                        "VSCN v5 preserves position morph deltas exactly");
            EXPECT_TRUE((source_view.normal_deltas == nullptr) ==
                                (baked_view.normal_deltas == nullptr) &&
                            (!source_view.normal_deltas || std::memcmp(source_view.normal_deltas,
                                                                       baked_view.normal_deltas,
                                                                       delta_bytes) == 0),
                        "VSCN v5 preserves optional normal morph deltas exactly");
            EXPECT_TRUE((source_view.tangent_deltas == nullptr) ==
                                (baked_view.tangent_deltas == nullptr) &&
                            (!source_view.tangent_deltas || std::memcmp(source_view.tangent_deltas,
                                                                        baked_view.tangent_deltas,
                                                                        delta_bytes) == 0),
                        "VSCN v5 preserves optional tangent morph deltas exactly");
        }
    }
    EXPECT_TRUE(morph_meshes > 0, "VSCN v5 fixture exercises static morph persistence");

    auto *source_animation =
        static_cast<rt_node_animation3d *>(rt_model3d_get_node_animation(source, 0));
    auto *baked_animation =
        static_cast<rt_node_animation3d *>(rt_model3d_get_node_animation(baked, 0));
    EXPECT_TRUE(source_animation && baked_animation &&
                    source_animation->channel_count == baked_animation->channel_count,
                "VSCN v5 preserves node-animation channel counts");
    if (source_animation && baked_animation) {
        EXPECT_TRUE(source_animation->duration == baked_animation->duration &&
                        source_animation->looping == baked_animation->looping,
                    "VSCN v5 preserves node-animation timing metadata");
        int32_t channel_count = scene3d_node_animation_channel_count(source_animation);
        for (int32_t channel_index = 0; channel_index < channel_count; ++channel_index) {
            const rt_node_anim_channel3d &source_channel =
                source_animation->channels[channel_index];
            const rt_node_anim_channel3d &baked_channel = baked_animation->channels[channel_index];
            size_t value_count = static_cast<size_t>(source_channel.key_count) *
                                 static_cast<size_t>(source_channel.value_width);
            EXPECT_TRUE(source_channel.path == baked_channel.path &&
                            source_channel.interpolation == baked_channel.interpolation &&
                            source_channel.key_count == baked_channel.key_count &&
                            source_channel.value_width == baked_channel.value_width &&
                            source_channel.target_node_index == baked_channel.target_node_index &&
                            std::strcmp(rt_string_cstr(source_channel.target_name),
                                        rt_string_cstr(baked_channel.target_name)) == 0,
                        "VSCN v5 preserves node-animation channel identity");
            EXPECT_TRUE(
                std::memcmp(source_channel.times,
                            baked_channel.times,
                            static_cast<size_t>(source_channel.key_count) * sizeof(double)) == 0 &&
                    std::memcmp(source_channel.values,
                                baked_channel.values,
                                value_count * sizeof(float)) == 0,
                "VSCN v5 preserves animation key times and values exactly");
        }
    }

    auto *baked_camera = static_cast<rt_camera3d *>(rt_model3d_get_camera(baked, 0, 0));
    EXPECT_TRUE(
        source_camera && baked_camera && source_camera->is_ortho == baked_camera->is_ortho &&
            source_camera->fov == baked_camera->fov &&
            source_camera->aspect == baked_camera->aspect &&
            source_camera->near_plane == baked_camera->near_plane &&
            source_camera->far_plane == baked_camera->far_plane &&
            std::memcmp(source_camera->eye, baked_camera->eye, sizeof(source_camera->eye)) == 0 &&
            std::memcmp(source_camera->view, baked_camera->view, sizeof(source_camera->view)) == 0,
        "VSCN v5 preserves camera projection and world transform exactly");
    EXPECT_TRUE(source_camera_node && baked_camera_node && source_camera && baked_camera &&
                    source_camera_node->camera == source_camera &&
                    baked_camera_node->camera == baked_camera,
                "VSCN v5 preserves camera-to-node attachment identity");

    void *primary = rt_model3d_instantiate_scene_at(baked, 0);
    void *secondary = rt_model3d_instantiate_scene_at(baked, 1);
    EXPECT_TRUE(primary && rt_scene3d_find(primary, rt_const_cstr("Mover")) != nullptr &&
                    rt_scene3d_find(primary, rt_const_cstr("SecondaryMover")) == nullptr,
                "VSCN v5 default scene contains only its authored hierarchy");
    EXPECT_TRUE(secondary &&
                    rt_scene3d_find(secondary, rt_const_cstr("SecondaryMover")) != nullptr &&
                    rt_scene3d_find(secondary, rt_const_cstr("Mover")) == nullptr,
                "VSCN v5 secondary scene remains independently instantiable");
    EXPECT_TRUE(primary && rt_model3d_apply_variant(baked, primary, 1) == 1,
                "VSCN v5 preserves per-node material-variant mappings");

    const char *padded_variant_path = "/tmp/zanna_model3d_vscn_v5_padded_variants.vscn";
    int32_t source_variant_count = source_mover ? source_mover->variant_material_count : 0;
    if (source_mover && source_variant_count == 2)
        source_mover->variant_material_count = 1;
    int64_t padded_saved = source_mover && source_variant_count == 2
                               ? rt_model3d_save(source, rt_const_cstr(padded_variant_path))
                               : 0;
    if (source_mover)
        source_mover->variant_material_count = source_variant_count;
    EXPECT_TRUE(padded_saved == 1,
                "VSCN v5 pads a short node variant table to the global variant inventory");
    void *padded_variant_asset =
        padded_saved ? rt_model3d_load(rt_const_cstr(padded_variant_path)) : nullptr;
    auto *padded_variant_node = padded_variant_asset
                                    ? static_cast<rt_scene_node3d *>(rt_model3d_find_node(
                                          padded_variant_asset, rt_const_cstr("Mover")))
                                    : nullptr;
    EXPECT_TRUE(padded_variant_node && padded_variant_node->variant_material_count == 2 &&
                    padded_variant_node->variant_materials &&
                    padded_variant_node->variant_materials[0] != nullptr &&
                    padded_variant_node->variant_materials[1] == nullptr,
                "A padded VSCN v5 variant table reloads with missing mappings as defaults");
    if (padded_variant_asset && rt_obj_release_check0(padded_variant_asset))
        rt_obj_free(padded_variant_asset);
    std::remove(padded_variant_path);

    const char *atomic_path = "/tmp/zanna_model3d_vscn_v5_atomic_failure.vscn";
    const std::string sentinel = "existing-destination";
    bool sentinel_written = write_text_file(atomic_path, sentinel);
    int32_t source_import_index = source_mover ? source_mover->import_index : -1;
    if (source_mover)
        source_mover->import_index = -2;
    int64_t invalid_saved = source_mover ? rt_model3d_save(source, rt_const_cstr(atomic_path)) : 1;
    if (source_mover)
        source_mover->import_index = source_import_index;
    std::vector<uint8_t> atomic_bytes;
    bool sentinel_read = read_binary_file(atomic_path, atomic_bytes);
    std::string atomic_text(atomic_bytes.begin(), atomic_bytes.end());
    EXPECT_TRUE(sentinel_written && invalid_saved == 0 && sentinel_read && atomic_text == sentinel,
                "SceneAsset.Save validation failure leaves an existing destination untouched");
    std::remove(atomic_path);
    if (primary && rt_obj_release_check0(primary))
        rt_obj_free(primary);
    if (secondary && rt_obj_release_check0(secondary))
        rt_obj_free(secondary);
    if (rt_obj_release_check0(baked))
        rt_obj_free(baked);
    if (rt_obj_release_check0(source))
        rt_obj_free(source);
    std::remove(baked_path);
}

static void test_model3d_vscn_v5_corruption_rolls_back() {
    const char *source_path = find_existing_path(
        {"src/tests/fixtures/runtime/assets/gltf/asset_bake_lossy.gltf",
         "../src/tests/fixtures/runtime/assets/gltf/asset_bake_lossy.gltf",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/src/tests/fixtures/runtime/assets/gltf/asset_bake_lossy.gltf"
#endif
        });
    const char *valid_path = "/tmp/zanna_model3d_vscn_v5_rollback_valid.vscn";
    const char *corrupt_path = "/tmp/zanna_model3d_vscn_v5_rollback_corrupt.vscn";
    EXPECT_TRUE(source_path != nullptr, "VSCN v5 rollback fixture is present");
    if (!source_path)
        return;

    void *source = rt_model3d_load(rt_const_cstr(source_path));
    EXPECT_TRUE(source != nullptr, "VSCN v5 rollback source loads");
    if (!source)
        return;
    bool saved = rt_model3d_save(source, rt_const_cstr(valid_path)) == 1;
    EXPECT_TRUE(saved, "VSCN v5 rollback baseline can be saved");
    std::vector<uint8_t> bytes;
    bool read = saved && read_binary_file(valid_path, bytes);
    EXPECT_TRUE(read, "VSCN v5 rollback baseline can be read");
    if (!read) {
        if (rt_obj_release_check0(source))
            rt_obj_free(source);
        std::remove(valid_path);
        return;
    }
    const std::string baseline(reinterpret_cast<const char *>(bytes.data()), bytes.size());

    auto expect_rejected = [&](const std::string &mutated,
                               bool mutation_applied,
                               const char *mutation_message,
                               const char *rejection_message) {
        EXPECT_TRUE(mutation_applied, mutation_message);
        if (!mutation_applied)
            return;
        bool wrote = write_text_file(corrupt_path, mutated);
        EXPECT_TRUE(wrote, "Malformed VSCN v5 fixture can be written");
        if (!wrote)
            return;
        void *rejected = rt_model3d_load(rt_const_cstr(corrupt_path));
        EXPECT_TRUE(rejected == nullptr, rejection_message);
        EXPECT_TRUE(rt_asset_error_get_code() != RT_ASSET_ERROR_NONE,
                    "Malformed VSCN v5 load records a recoverable asset error");
        if (rejected && rt_obj_release_check0(rejected))
            rt_obj_free(rejected);

        void *recovered = rt_model3d_load(rt_const_cstr(valid_path));
        EXPECT_TRUE(recovered != nullptr && rt_model3d_get_scene_count(recovered) == 2 &&
                        rt_model3d_get_morph_shape_count(recovered) == 2,
                    "A rejected VSCN v5 document leaves the next complete load intact");
        if (recovered && rt_obj_release_check0(recovered))
            rt_obj_free(recovered);
    };

    std::string invalid_camera = baseline;
    bool camera_mutated =
        replace_first_text(invalid_camera, "\"cameras\": [0]", "\"cameras\": [999]");
    expect_rejected(invalid_camera,
                    camera_mutated,
                    "VSCN v5 camera-index mutation finds its target",
                    "VSCN v5 rejects an out-of-range per-scene camera index atomically");

    std::string invalid_morph = baseline;
    bool morph_mutated = replace_first_json_string_value(invalid_morph, "positionBase64", "AA==");
    expect_rejected(invalid_morph,
                    morph_mutated,
                    "VSCN v5 morph-payload mutation finds its target",
                    "VSCN v5 rejects a truncated morph delta payload atomically");

    std::string invalid_variant = baseline;
    bool variant_mutated = replace_first_text(
        invalid_variant, "\"variantMaterials\": [0, 1]", "\"variantMaterials\": [0, 999]");
    expect_rejected(invalid_variant,
                    variant_mutated,
                    "VSCN v5 variant-index mutation finds its target",
                    "VSCN v5 rejects an out-of-range variant material atomically");

    std::string invalid_animation = baseline;
    bool animation_mutated =
        replace_first_json_string_value(invalid_animation, "timesBase64", "AA==");
    expect_rejected(invalid_animation,
                    animation_mutated,
                    "VSCN v5 animation-payload mutation finds its target",
                    "VSCN v5 rejects a truncated animation sample payload atomically");

    EXPECT_TRUE(rt_model3d_get_scene_count(source) == 2 &&
                    rt_model3d_get_morph_shape_count(source) == 2,
                "Rejected VSCN v5 documents do not mutate an existing SceneAsset");
    if (rt_obj_release_check0(source))
        rt_obj_free(source);
    std::remove(corrupt_path);
    std::remove(valid_path);
}

static void test_model3d_draco_corrupt_payloads_fail_cleanly() {
    /* Byte-flip a sweep of positions inside the valence sphere's BIN chunk (the
     * Draco payload). Every variant must either load or fail with a diagnostic —
     * never crash or hang (exercises the decoder's bounds and iteration guards). */
    const char *path = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/sphere_draco_valence.glb",
         "../examples/3d/openworld_slice/assets/models/sphere_draco_valence.glb",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/sphere_draco_valence.glb"
#endif
        });
    if (!path)
        return;
    std::vector<uint8_t> glb;
    if (!read_binary_file(path, glb) || glb.size() < 32)
        return;
    /* Locate the BIN chunk: GLB header (12) + JSON chunk header (8) + JSON. */
    uint32_t json_len = 0;
    std::memcpy(&json_len, glb.data() + 12, sizeof(json_len));
    size_t bin_start = 12u + 8u + json_len + 8u;
    if (bin_start >= glb.size())
        return;
    size_t bin_len = glb.size() - bin_start;
    const char *tmp_path = "/tmp/zanna_model3d_draco_corrupt.glb";
    int survived = 0;
    const int kVariants = 24;
    for (int i = 0; i < kVariants; i++) {
        std::vector<uint8_t> mutated = glb;
        size_t at = bin_start + (bin_len * (size_t)i) / kVariants;
        mutated[at] ^= 0xFFu;
        if (!write_binary_file(tmp_path, mutated))
            return;
        void *model = rt_model3d_load(rt_const_cstr(tmp_path));
        if (model) {
            if (rt_obj_release_check0(model))
                rt_obj_free(model);
        }
        survived++;
    }
    std::remove(tmp_path);
    EXPECT_TRUE(survived == kVariants,
                "corrupt Draco payload variants all return without crashing");
}

static rt_scene_node3d *find_first_mesh_node(rt_scene_node3d *node, int depth) {
    if (!node || depth > 8)
        return nullptr;
    if (node->mesh)
        return node;
    for (int32_t i = 0; i < node->child_count; i++) {
        auto *child = (rt_scene_node3d *)node->children[i];
        rt_scene_node3d *found = find_first_mesh_node(child, depth + 1);
        if (found)
            return found;
    }
    return nullptr;
}

static void test_model3d_loads_gltfpack_quantized_fixtures() {
    /* Real gltfpack output requiring KHR_mesh_quantization + EXT_meshopt_compression:
     * skinned_agent_quantized.glb stores USHORT positions with a dequantization node
     * scale; skinned_agent_filtered.glb keeps float positions through the EXPONENTIAL
     * filter and 16-bit octahedral-filtered normals. */
    const char *reference = find_existing_path(
        {"examples/3d/openworld_slice/assets/models/skinned_agent.gltf",
         "../examples/3d/openworld_slice/assets/models/skinned_agent.gltf",
#ifdef ZANNA_SOURCE_DIR
         ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/skinned_agent.gltf"
#endif
        });
    EXPECT_TRUE(reference != nullptr, "Quantized-fixture reference twin is present");
    if (!reference)
        return;
    void *ref_model = rt_model3d_load(rt_const_cstr(reference));
    EXPECT_TRUE(ref_model != nullptr, "Quantized-fixture reference twin loads");
    if (!ref_model)
        return;
    auto *ref_mesh = static_cast<rt_mesh3d *>(rt_model3d_get_mesh(ref_model, 0));
    if (!ref_mesh)
        return;
    float ref_min[3];
    float ref_max[3];
    mesh_position_bounds(ref_mesh, ref_min, ref_max);

    const struct {
        const char *name;
        const char *paths[3];
    } fixtures[2] = {
        {"quantized",
         {"examples/3d/openworld_slice/assets/models/skinned_agent_quantized.glb",
          "../examples/3d/openworld_slice/assets/models/skinned_agent_quantized.glb",
#ifdef ZANNA_SOURCE_DIR
          ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/skinned_agent_quantized.glb"
#endif
         }},
        {"filtered",
         {"examples/3d/openworld_slice/assets/models/skinned_agent_filtered.glb",
          "../examples/3d/openworld_slice/assets/models/skinned_agent_filtered.glb",
#ifdef ZANNA_SOURCE_DIR
          ZANNA_SOURCE_DIR "/examples/3d/openworld_slice/assets/models/skinned_agent_filtered.glb"
#endif
         }},
    };
    for (int f = 0; f < 2; f++) {
        char message[160];
        const char *path =
            find_existing_path({fixtures[f].paths[0], fixtures[f].paths[1], fixtures[f].paths[2]});
        snprintf(message, sizeof(message), "gltfpack %s fixture is present", fixtures[f].name);
        EXPECT_TRUE(path != nullptr, message);
        if (!path)
            continue;
        void *model = rt_model3d_load(rt_const_cstr(path));
        snprintf(message, sizeof(message), "gltfpack %s fixture loads", fixtures[f].name);
        EXPECT_TRUE(model != nullptr, message);
        if (!model)
            continue;
        snprintf(message, sizeof(message), "gltfpack %s fixture keeps counts", fixtures[f].name);
        EXPECT_TRUE(rt_model3d_get_mesh_count(model) == rt_model3d_get_mesh_count(ref_model) &&
                        rt_model3d_get_skeleton_count(model) ==
                            rt_model3d_get_skeleton_count(ref_model),
                    message);
        void *inst = rt_model3d_instantiate(model);
        rt_scene_node3d *mesh_node = find_first_mesh_node((rt_scene_node3d *)inst, 0);
        snprintf(
            message, sizeof(message), "gltfpack %s fixture exposes a mesh node", fixtures[f].name);
        EXPECT_TRUE(mesh_node != nullptr, message);
        if (mesh_node) {
            auto *mesh = static_cast<rt_mesh3d *>(mesh_node->mesh);
            float cmp_min[3];
            float cmp_max[3];
            mesh_position_bounds(mesh, cmp_min, cmp_max);
            /* Compare dequantized extents (reorder- and translation-invariant): the
             * node scale undoes position quantization per KHR_mesh_quantization. */
            int extents_ok = 1;
            for (int a = 0; a < 3; a++) {
                double scaled = (double)(cmp_max[a] - cmp_min[a]) * mesh_node->scale_xyz[a];
                double expected = (double)(ref_max[a] - ref_min[a]);
                if (std::fabs(scaled - expected) > 0.02)
                    extents_ok = 0;
            }
            snprintf(message,
                     sizeof(message),
                     "gltfpack %s fixture dequantizes to the reference extents",
                     fixtures[f].name);
            EXPECT_TRUE(extents_ok, message);
        }
        if (inst && rt_obj_release_check0(inst))
            rt_obj_free(inst);
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
    }
    if (rt_obj_release_check0(ref_model))
        rt_obj_free(ref_model);
}

static void test_model3d_generate_lods_builds_chains() {
    const char *obj_path = "/tmp/zanna_model3d_generate_lods.obj";
    std::string obj_text = "v -1 -1 -1\n"
                           "v 1 -1 -1\n"
                           "v 1 1 -1\n"
                           "v -1 1 -1\n"
                           "v -1 -1 1\n"
                           "v 1 -1 1\n"
                           "v 1 1 1\n"
                           "v -1 1 1\n"
                           "f 1 2 3\nf 1 3 4\n"
                           "f 5 7 6\nf 5 8 7\n"
                           "f 1 5 6\nf 1 6 2\n"
                           "f 2 6 7\nf 2 7 3\n"
                           "f 3 7 8\nf 3 8 4\n"
                           "f 4 8 5\nf 4 5 1\n";
    EXPECT_TRUE(write_text_file(obj_path, obj_text), "GenerateLODs OBJ fixture can be created");
    void *model = rt_model3d_load(rt_const_cstr(obj_path));
    EXPECT_TRUE(model != nullptr, "GenerateLODs OBJ fixture loads");
    if (!model)
        return;
    int64_t chained = rt_model3d_generate_lods(model, 2, 0.5);
    EXPECT_TRUE(chained == 1, "SceneAsset.GenerateLODs chains the box mesh node");

    void *inst = rt_model3d_instantiate(model);
    EXPECT_TRUE(inst != nullptr, "LOD-chained model instantiates");
    if (!inst)
        return;
    EXPECT_TRUE(rt_scene_node3d_child_count(inst) == 1, "Instance carries the mesh node");
    auto *mesh_node = (rt_scene_node3d *)rt_scene_node3d_get_child(inst, 0);
    EXPECT_TRUE(mesh_node != nullptr, "Instance mesh node is reachable");
    if (!mesh_node)
        return;
    EXPECT_TRUE(mesh_node->lod_count >= 1, "Instantiated nodes inherit the generated LOD chain");
    EXPECT_TRUE(mesh_node->auto_lod_enabled == 1,
                "Instantiated nodes inherit auto screen-error LOD selection");

    EXPECT_TRUE(rt_model3d_generate_lods(model, 2, 0.5) == 0,
                "GenerateLODs skips nodes that already carry LOD chains");
    EXPECT_TRUE(rt_model3d_generate_lods(nullptr, 2, 0.5) == 0,
                "GenerateLODs rejects invalid handles");
    if (rt_obj_release_check0(inst))
        rt_obj_free(inst);
    if (rt_obj_release_check0(model))
        rt_obj_free(model);
}

static void test_model3d_applies_material_variants() {
    const char *gltf_path = "/tmp/zanna_model3d_material_variants.gltf";
    std::vector<uint8_t> gltf_buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    for (float v : positions)
        append_bytes(gltf_buffer, v);
    for (uint16_t v : indices)
        append_bytes(gltf_buffer, v);
    std::string buffer_b64 = base64_encode(gltf_buffer.data(), gltf_buffer.size());
    std::string gltf_json =
        "{\"asset\":{\"version\":\"2.0\"},"
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
    EXPECT_TRUE(write_text_file(gltf_path, gltf_json),
                "Material-variants SceneAsset fixture can be created");
    void *model = rt_model3d_load(rt_const_cstr(gltf_path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load accepts KHR_materials_variants assets");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_variant_count(model) == 2, "SceneAsset exposes the variant count");
    rt_string night = rt_model3d_get_variant_name(model, 1);
    EXPECT_TRUE(night && std::strcmp(rt_string_cstr(night), "night") == 0,
                "SceneAsset exposes variant names");
    rt_string out_of_range = rt_model3d_get_variant_name(model, 9);
    EXPECT_TRUE(out_of_range && rt_string_cstr(out_of_range)[0] == '\0',
                "SceneAsset out-of-range variant name is empty");

    void *inst = rt_model3d_instantiate(model);
    EXPECT_TRUE(inst != nullptr, "Variant fixture instantiates");
    if (!inst)
        return;
    EXPECT_TRUE(rt_scene_node3d_child_count(inst) == 1, "Instance has the glTF node");
    auto *mesh_node = (rt_scene_node3d *)rt_scene_node3d_get_child(inst, 0);
    EXPECT_TRUE(mesh_node != nullptr && mesh_node->mesh != nullptr,
                "Instance child carries the primitive mesh");
    if (!mesh_node)
        return;
    void *default_material = mesh_node->material;
    EXPECT_TRUE(default_material != nullptr, "Primitive default material is bound");

    int64_t changed = rt_model3d_apply_variant(model, inst, 1);
    EXPECT_TRUE(changed == 1, "ApplyVariant updates the mapped node");
    EXPECT_TRUE(mesh_node->material != nullptr && mesh_node->material != default_material,
                "ApplyVariant swaps in the variant material");
    EXPECT_TRUE(mesh_node->material == rt_model3d_get_material(model, 1),
                "Variant material is enumerable through GetMaterial");

    changed = rt_model3d_apply_variant(model, inst, 0);
    EXPECT_TRUE(changed == 1 && mesh_node->material == default_material,
                "Unmapped variant restores the primitive default material");

    EXPECT_TRUE(rt_model3d_apply_variant(model, inst, 5) == 0,
                "Out-of-range variant index applies nothing");
    EXPECT_TRUE(rt_model3d_apply_variant(model, nullptr, 1) == 0,
                "ApplyVariant with no target applies nothing");
    if (rt_obj_release_check0(inst))
        rt_obj_free(inst);
    if (rt_obj_release_check0(model))
        rt_obj_free(model);
}

static void test_model3d_autoplays_gltf_node_and_morph_animation() {
    const char *path = "/tmp/zanna_model3d_node_animation.gltf";
    std::vector<uint8_t> buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    const float morph_positions[9] = {0.0f, 0.0f, 0.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.3f, 0.0f};
    const float times[2] = {0.0f, 1.0f};
    const float translations[6] = {0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f};
    const float weights[2] = {0.0f, 1.0f};

    size_t pos_off = buffer.size();
    for (float v : positions)
        append_bytes(buffer, v);
    size_t idx_off = buffer.size();
    for (uint16_t v : indices)
        append_bytes(buffer, v);
    while (buffer.size() % 4 != 0)
        buffer.push_back(0);
    size_t morph_off = buffer.size();
    for (float v : morph_positions)
        append_bytes(buffer, v);
    size_t times_off = buffer.size();
    for (float v : times)
        append_bytes(buffer, v);
    size_t translation_off = buffer.size();
    for (float v : translations)
        append_bytes(buffer, v);
    size_t weights_off = buffer.size();
    for (float v : weights)
        append_bytes(buffer, v);

    std::string buffer_b64 = base64_encode(buffer.data(), buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(pos_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(idx_off) +
        ",\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(times_off) +
        ",\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(translation_off) +
        ",\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(weights_off) +
        ",\"byteLength\":8}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"weights\":[0.0],\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"indices\":1,\"targets\":[{\"POSITION\":2}]}]}],"
        "\"nodes\":[{\"name\":\"Mover\",\"mesh\":0,\"weights\":[0.0]}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"animations\":[{\"name\":\"MoveAndSmile\",\"samplers\":[{\"input\":3,\"output\":4},"
        "{\"input\":3,\"output\":5}],\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]"
        "}";
    EXPECT_TRUE(write_text_file(path, gltf_json), "Node-animation glTF fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load imports glTF node animation clips");
    if (!model)
        return;
    EXPECT_TRUE(rt_model3d_get_node_animation_count(model) == 1,
                "SceneAsset.NodeAnimationCount exposes imported glTF node clips");
    void *borrowed_clip = rt_model3d_get_node_animation(model, 0);
    EXPECT_TRUE(borrowed_clip != nullptr,
                "SceneAsset.GetNodeAnimation borrows imported node clips");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_model3d_get_node_animation_name(model, 0)),
                            "MoveAndSmile") == 0,
                "SceneAsset.GetNodeAnimationName exposes imported node clip names");
    void *loaded_node_clip = rt_model3d_load_node_animation(rt_const_cstr(path), 0);
    EXPECT_TRUE(loaded_node_clip != nullptr,
                "SceneAsset.LoadNodeAnimation returns a retained external node clip");
    if (loaded_node_clip) {
        EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_node_animation3d_get_name(loaded_node_clip)),
                                "MoveAndSmile") == 0,
                    "SceneAsset.LoadNodeAnimation preserves node clip names");
        if (rt_obj_release_check0(loaded_node_clip))
            rt_obj_free(loaded_node_clip);
    }
    EXPECT_TRUE(rt_model3d_load_animation(rt_const_cstr(path), 0) == nullptr,
                "SceneAsset.LoadAnimation returns null when a file has no skeletal clips");
    void *scene = rt_model3d_instantiate_scene(model);
    EXPECT_TRUE(scene != nullptr, "SceneAsset.InstantiateScene creates a scene for node animation");
    if (!scene)
        return;
    rt_scene3d_sync_bindings(scene, 0.5);
    void *node = rt_scene3d_find(scene, rt_const_cstr("Mover"));
    EXPECT_TRUE(node != nullptr, "Instantiated scene preserves animated node name");
    if (!node)
        return;
    EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(node)),
                1.0,
                0.001,
                "SceneGraph.SyncBindings advances glTF node translation animation");
    auto *mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(node));
    EXPECT_TRUE(mesh != nullptr && mesh->morph_targets_ref != nullptr,
                "Animated glTF node keeps an instance-local morph target");
    if (mesh && mesh->morph_targets_ref) {
        EXPECT_NEAR(rt_morphtarget3d_get_weight(mesh->morph_targets_ref, 0),
                    0.5,
                    0.001,
                    "SceneGraph.SyncBindings advances glTF morph weight animation");
    }
    std::remove(path);
}

static void test_gltf_short_node_weights_clear_morph_tail() {
    const char *path = "/tmp/zanna_gltf_short_node_weights.gltf";
    std::vector<uint8_t> buffer;
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    const float morph_a[9] = {0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const float morph_b[9] = {0.0f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    size_t pos_off = buffer.size();
    for (float v : positions)
        append_bytes(buffer, v);
    size_t idx_off = buffer.size();
    for (uint16_t v : indices)
        append_bytes(buffer, v);
    while (buffer.size() % 4 != 0)
        buffer.push_back(0);
    size_t morph_a_off = buffer.size();
    for (float v : morph_a)
        append_bytes(buffer, v);
    size_t morph_b_off = buffer.size();
    for (float v : morph_b)
        append_bytes(buffer, v);

    std::string buffer_b64 = base64_encode(buffer.data(), buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(pos_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(idx_off) +
        ",\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_a_off) +
        ",\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morph_b_off) +
        ",\"byteLength\":36}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
        "],"
        "\"meshes\":[{\"weights\":[0.0,1.0],\"primitives\":[{\"attributes\":{\"POSITION\":0},"
        "\"indices\":1,\"targets\":[{\"POSITION\":2},{\"POSITION\":3}]}]}],"
        "\"nodes\":[{\"name\":\"ShortWeights\",\"mesh\":0,\"weights\":[0.0]}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";
    EXPECT_TRUE(write_text_file(path, gltf_json), "Short node-weights glTF fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load imports glTF with short node weights");
    if (!model)
        return;
    void *scene = rt_model3d_instantiate_scene(model);
    EXPECT_TRUE(scene != nullptr, "SceneAsset.InstantiateScene creates short-weights scene");
    if (!scene)
        return;
    void *node = rt_scene3d_find(scene, rt_const_cstr("ShortWeights"));
    EXPECT_TRUE(node != nullptr, "Short-weights node is present");
    if (!node)
        return;
    auto *mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(node));
    EXPECT_TRUE(mesh != nullptr && mesh->morph_targets_ref != nullptr,
                "Short-weights node has instance-local morph targets");
    if (mesh && mesh->morph_targets_ref) {
        EXPECT_NEAR(rt_morphtarget3d_get_weight(mesh->morph_targets_ref, 0),
                    0.0,
                    0.001,
                    "Short node weights apply provided weight");
        EXPECT_NEAR(rt_morphtarget3d_get_weight(mesh->morph_targets_ref, 1),
                    0.0,
                    0.001,
                    "Short node weights clear inherited mesh-default tail weights");
    }
    std::remove(path);
}

static void test_gltf_rejects_unknown_animation_interpolation() {
    const char *path = "/tmp/zanna_gltf_unknown_animation_interpolation.gltf";
    std::vector<uint8_t> buffer;
    const float times[2] = {0.0f, 1.0f};
    const float translations[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

    size_t times_off = buffer.size();
    for (float v : times)
        append_bytes(buffer, v);
    size_t translations_off = buffer.size();
    for (float v : translations)
        append_bytes(buffer, v);

    std::string buffer_b64 = base64_encode(buffer.data(), buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(buffer.size()) +
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
        "\"nodes\":[{\"name\":\"BadInterpolationTarget\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"animations\":[{\"name\":\"BadInterpolation\",\"samplers\":[{\"input\":0,"
        "\"output\":1,\"interpolation\":\"BOGUS\"}],\"channels\":[{\"sampler\":0,"
        "\"target\":{\"node\":0,\"path\":\"translation\"}}]}]"
        "}";

    EXPECT_TRUE(write_text_file(path, gltf_json),
                "Unknown-interpolation glTF fixture can be written");
    void *asset = rt_gltf_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "glTF asset with a bad animation sampler still loads");
    if (asset) {
        EXPECT_TRUE(
            rt_gltf_node_animation_count(asset) == 0,
            "glTF rejects unknown animation interpolation instead of treating it as linear");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(path);
}

static void test_gltf_rejects_oversized_node_animation_key_count_before_scan() {
    const char *path = "/tmp/zanna_gltf_oversized_node_animation_keys.gltf";
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"accessors\":["
        "{\"componentType\":5126,\"count\":1000001,\"type\":\"SCALAR\"},"
        "{\"componentType\":5126,\"count\":1000001,\"type\":\"VEC3\"}"
        "],"
        "\"nodes\":[{\"name\":\"HugeAnimationTarget\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"animations\":[{\"name\":\"TooManyKeys\",\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]"
        "}";

    EXPECT_TRUE(write_text_file(path, gltf_json),
                "Oversized-key-count glTF fixture can be written");
    void *asset = rt_gltf_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "glTF asset with oversized animation key count still loads");
    if (asset) {
        EXPECT_TRUE(rt_gltf_node_animation_count(asset) == 0,
                    "glTF skips node animation channels before scanning oversized key accessors");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(path);
}

static void test_gltf_rejects_oversized_morph_weight_animation_width() {
    const char *path = "/tmp/zanna_gltf_oversized_weight_animation.gltf";
    std::vector<uint8_t> buffer;
    const float time = 0.0f;
    size_t time_off = buffer.size();
    append_bytes(buffer, time);
    size_t weights_off = buffer.size();
    for (int i = 0; i < 4097; i++) {
        const float weight = 0.0f;
        append_bytes(buffer, weight);
    }

    std::string buffer_b64 = base64_encode(buffer.data(), buffer.size());
    std::string gltf_json =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"uri\":\"data:application/octet-stream;base64," +
        buffer_b64 + "\",\"byteLength\":" + std::to_string(buffer.size()) +
        "}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(time_off) +
        ",\"byteLength\":4},"
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(weights_off) +
        ",\"byteLength\":" + std::to_string(4097 * (int)sizeof(float)) +
        "}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4097,\"type\":\"SCALAR\"}"
        "],"
        "\"nodes\":[{\"name\":\"TooManyWeights\"}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0,"
        "\"animations\":[{\"name\":\"TooWide\",\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]"
        "}";

    EXPECT_TRUE(write_text_file(path, gltf_json),
                "Oversized morph-weight glTF fixture can be written");
    void *asset = rt_gltf_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "glTF asset with oversized weight animation still loads");
    if (asset) {
        EXPECT_TRUE(rt_gltf_node_animation_count(asset) == 0,
                    "glTF skips morph-weight channels wider than NodeAnimation3D can play");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(path);
}

static void test_model3d_rejects_wrong_handle_types() {
    void *node = rt_scene_node3d_new();
    EXPECT_TRUE(rt_model3d_get_mesh_count(node) == 0,
                "SceneAsset.GetMeshCount rejects non-SceneAsset handles");
    EXPECT_TRUE(rt_model3d_get_scene_count(node) == 0,
                "SceneAsset.SceneCount rejects non-SceneAsset handles");
    EXPECT_TRUE(rt_model3d_get_camera_count(node, 0) == 0,
                "SceneAsset.GetCameraCount rejects non-SceneAsset handles");
    EXPECT_TRUE(rt_model3d_get_camera(node, 0, 0) == nullptr,
                "SceneAsset.GetCamera rejects non-SceneAsset handles");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_model3d_get_scene_name(node, 0)), "") == 0,
                "SceneAsset.GetSceneName rejects non-SceneAsset handles");
    EXPECT_TRUE(rt_model3d_get_mesh(node, 0) == nullptr,
                "SceneAsset.GetMesh rejects non-SceneAsset handles");
    EXPECT_TRUE(rt_model3d_instantiate(node) == nullptr,
                "SceneAsset.Instantiate rejects non-SceneAsset handles");
    EXPECT_TRUE(rt_model3d_instantiate_scene(node) == nullptr,
                "SceneAsset.InstantiateScene rejects non-SceneAsset handles");
    EXPECT_TRUE(rt_model3d_instantiate_scene_at(node, 0) == nullptr,
                "SceneAsset.InstantiateSceneAt rejects non-SceneAsset handles");
}

static void test_model3d_binds_first_valid_default_skeletal_animator_for_multiple_skeletons() {
    const char *path = "/tmp/zanna_model3d_multi_skeleton_autobind.obj";
    const char *obj = "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "f 1 2 3\n";
    EXPECT_TRUE(write_text_file(path, obj), "SceneAsset multi-skeleton OBJ fixture is written");

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load creates fixture for multi-skeleton auto-bind test");
    if (!model)
        return;

    auto *view = static_cast<SceneAssetView *>(model);
    view->skeletons = static_cast<void **>(std::calloc(2, sizeof(void *)));
    view->animations = static_cast<void **>(std::calloc(2, sizeof(void *)));
    EXPECT_TRUE(view->skeletons && view->animations,
                "SceneAsset multi-skeleton auto-bind arrays allocate");
    if (!view->skeletons || !view->animations)
        return;

    view->skeletons[0] = rt_skeleton3d_new();
    view->skeletons[1] = rt_skeleton3d_new();
    EXPECT_TRUE(view->skeletons[0] && view->skeletons[1],
                "SceneAsset multi-skeleton auto-bind skeletons allocate");
    if (!view->skeletons[0] || !view->skeletons[1])
        return;
    rt_skeleton3d_add_bone(view->skeletons[0], rt_const_cstr("root_a"), -1, rt_mat4_identity());
    rt_skeleton3d_add_bone(view->skeletons[1], rt_const_cstr("root_b"), -1, rt_mat4_identity());
    rt_skeleton3d_compute_inverse_bind(view->skeletons[0]);
    rt_skeleton3d_compute_inverse_bind(view->skeletons[1]);
    view->skeleton_count = 2;
    view->skeleton_capacity = 2;

    view->animations[0] = rt_animation3d_new(rt_const_cstr("clip_a"), 1.0);
    view->animations[1] = rt_animation3d_new(rt_const_cstr("clip_b"), 1.0);
    view->animation_count = 2;
    view->animation_capacity = 2;

    auto *instance = static_cast<rt_scene_node3d *>(rt_model3d_instantiate(model));
    EXPECT_TRUE(instance != nullptr, "SceneAsset.Instantiate succeeds for multi-skeleton fixture");
    void *controller = instance ? rt_scene_node3d_get_animator(instance) : nullptr;
    EXPECT_TRUE(controller != nullptr,
                "SceneAsset binds a default skeletal animator when multiple skeletons are present");
    if (controller) {
        EXPECT_TRUE(rt_anim_controller3d_get_skeleton(controller) == view->skeletons[0],
                    "SceneAsset default animator uses the first valid imported skeleton");
        EXPECT_TRUE(
            rt_anim_controller3d_get_state_count(controller) == 2,
            "SceneAsset default animator keeps all imported clips for multi-skeleton assets");
        EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_anim_controller3d_get_current_state(controller)),
                                "clip_a") == 0,
                    "SceneAsset default animator starts the first imported clip");
    }

    std::remove(path);
}

static void test_model3d_clamps_corrupt_counts_and_child_walks() {
    const char *path = "/tmp/zanna_model3d_corrupt_counts.obj";
    const char *obj = "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "f 1 2 3\n";
    EXPECT_TRUE(write_text_file(path, obj), "SceneAsset corrupt-count OBJ fixture is written");

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset.Load creates fixture for corrupt-count test");
    if (!model)
        return;

    auto *view = static_cast<SceneAssetView *>(model);
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1, "SceneAsset fixture starts with one mesh");
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 1,
                "SceneAsset fixture starts with one material");
    EXPECT_TRUE(rt_model3d_get_scene_count(model) == 1, "SceneAsset fixture starts with one scene");

    view->mesh_count = 50;
    view->mesh_capacity = 1;
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1,
                "SceneAsset mesh count clamps to backing capacity");
    EXPECT_TRUE(rt_model3d_get_mesh(model, 0) != nullptr,
                "SceneAsset still exposes valid mesh within clamped count");
    EXPECT_TRUE(rt_model3d_get_mesh(model, 1) == nullptr,
                "SceneAsset rejects mesh index beyond clamped count");
    if (view->meshes && view->materials) {
        void *saved_mesh_entry = view->meshes[0];
        void *saved_material_entry = view->materials[0];
        view->meshes[0] = saved_material_entry;
        EXPECT_TRUE(rt_model3d_get_mesh(model, 0) == nullptr,
                    "SceneAsset.GetMesh rejects wrong-class entries in corrupt mesh tables");
        view->meshes[0] = saved_mesh_entry;
        view->materials[0] = saved_mesh_entry;
        EXPECT_TRUE(
            rt_model3d_get_material(model, 0) == nullptr,
            "SceneAsset.GetMaterial rejects wrong-class entries in corrupt material tables");
        view->materials[0] = saved_material_entry;
    }

    view->material_count = -5;
    EXPECT_TRUE(rt_model3d_get_material_count(model) == 0,
                "SceneAsset negative material count reads as empty");
    EXPECT_TRUE(rt_model3d_get_material(model, 0) == nullptr,
                "SceneAsset rejects material access when count is corrupt-negative");

    view->scene_count = 50;
    view->scene_capacity = 1;
    EXPECT_TRUE(rt_model3d_get_scene_count(model) == 1,
                "SceneAsset scene count clamps to backing capacity");
    EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_model3d_get_scene_name(model, 1)), "") == 0,
                "SceneAsset rejects scene-name access beyond clamped count");
    EXPECT_TRUE(rt_model3d_instantiate_scene_at(model, 1) == nullptr,
                "SceneAsset rejects scene instantiation beyond clamped count");

    if (view->scenes) {
        view->scenes[0].camera_count = 99;
        view->scenes[0].camera_capacity = 0;
        view->scenes[0].cameras = nullptr;
        EXPECT_TRUE(rt_model3d_get_camera_count(model, 0) == 0,
                    "SceneAsset camera count clamps to a missing camera array");
        EXPECT_TRUE(rt_model3d_get_camera(model, 0, 0) == nullptr,
                    "SceneAsset rejects camera access when camera array is missing");
        view->scenes[0].cameras = static_cast<void **>(std::calloc(1, sizeof(void *)));
        EXPECT_TRUE(view->scenes[0].cameras != nullptr,
                    "SceneAsset corrupt camera table test array allocates");
        if (view->scenes[0].cameras) {
            view->scenes[0].cameras[0] = rt_material3d_new_color(0.8, 0.2, 0.2);
            view->scenes[0].camera_count = 1;
            view->scenes[0].camera_capacity = 1;
            EXPECT_TRUE(rt_model3d_get_camera_count(model, 0) == 1,
                        "SceneAsset camera count exposes corrupt but backed camera table");
            EXPECT_TRUE(
                rt_model3d_get_camera(model, 0, 0) == nullptr,
                "SceneAsset.GetCamera rejects wrong-class entries in corrupt camera tables");
        }
    }

    if (view->template_root) {
        view->template_root->child_count = 99;
        view->template_root->child_capacity = 1;
        EXPECT_TRUE(rt_model3d_get_node_count(model) == 1,
                    "SceneAsset node counting clamps corrupt root child count");
        EXPECT_TRUE(rt_model3d_find_node(model, rt_const_cstr("mesh_0")) != nullptr,
                    "SceneAsset.FindNode still finds valid children under clamped count");
        EXPECT_TRUE(rt_model3d_instantiate(model) != nullptr,
                    "SceneAsset.Instantiate clamps corrupt root child count while cloning");
        EXPECT_TRUE(rt_model3d_instantiate_scene_at(model, 0) != nullptr,
                    "SceneAsset.InstantiateSceneAt clamps corrupt scene root child count");

        rt_scene_node3d *saved_child = view->template_root->children[0];
        view->template_root->children[0] = nullptr;
        void *empty_instance = rt_model3d_instantiate(model);
        EXPECT_TRUE(empty_instance != nullptr,
                    "SceneAsset.Instantiate skips null child slots in corrupt templates");
        EXPECT_TRUE(rt_scene_node3d_child_count(empty_instance) == 0,
                    "SceneAsset null child slots do not appear in cloned instances");
        EXPECT_TRUE(rt_model3d_instantiate_scene_at(model, 0) != nullptr,
                    "SceneAsset.InstantiateSceneAt skips null child slots in corrupt scenes");
        view->template_root->children[0] = saved_child;

        if (saved_child && view->materials && view->materials[0] && view->meshes &&
            view->meshes[0]) {
            void *saved_mesh = saved_child->mesh;
            saved_child->mesh = view->materials[0];
            auto *bad_mesh_instance = static_cast<rt_scene_node3d *>(rt_model3d_instantiate(model));
            auto *bad_mesh_child = bad_mesh_instance && bad_mesh_instance->child_count > 0
                                       ? bad_mesh_instance->children[0]
                                       : nullptr;
            EXPECT_TRUE(bad_mesh_child && bad_mesh_child->mesh == nullptr,
                        "SceneAsset.Instantiate drops wrong-class template mesh slots");
            saved_child->mesh = saved_mesh;

            rt_scene_node3d_add_lod(saved_child, 4.0, view->meshes[0]);
            if (saved_child->lod_levels && saved_child->lod_count > 0) {
                void *saved_lod_mesh = saved_child->lod_levels[0].mesh;
                saved_child->lod_levels[0].mesh = view->materials[0];
                auto *bad_lod_instance =
                    static_cast<rt_scene_node3d *>(rt_model3d_instantiate(model));
                auto *bad_lod_child = bad_lod_instance && bad_lod_instance->child_count > 0
                                          ? bad_lod_instance->children[0]
                                          : nullptr;
                EXPECT_TRUE(bad_lod_child && rt_scene_node3d_get_lod_count(bad_lod_child) == 0,
                            "SceneAsset.Instantiate drops wrong-class template LOD mesh slots");
                saved_child->lod_levels[0].mesh = saved_lod_mesh;
            }
        }
    }

    view->skeletons = static_cast<void **>(std::calloc(2, sizeof(void *)));
    view->animations = static_cast<void **>(std::calloc(2, sizeof(void *)));
    view->node_animations = static_cast<void **>(std::calloc(2, sizeof(void *)));
    EXPECT_TRUE(view->skeletons && view->animations && view->node_animations,
                "SceneAsset corrupt animation-list test arrays allocate");
    if (view->skeletons && view->animations && view->node_animations) {
        view->skeletons[0] = rt_material3d_new_color(0.3, 0.4, 0.5);
        view->skeletons[1] = rt_skeleton3d_new();
        view->animations[0] = rt_material3d_new_color(0.5, 0.4, 0.3);
        view->animations[1] = rt_animation3d_new(rt_const_cstr("idle"), 1.0);
        view->node_animations[0] = rt_material3d_new_color(0.2, 0.5, 0.4);
        view->node_animations[1] = rt_node_animation3d_new(rt_const_cstr("node_idle"), 1.0);
        view->skeleton_count = 99;
        view->skeleton_capacity = 2;
        view->animation_count = 99;
        view->animation_capacity = 2;
        view->node_animation_count = 99;
        view->node_animation_capacity = 2;
        EXPECT_TRUE(
            rt_model3d_get_skeleton(model, 0) == nullptr,
            "SceneAsset.GetSkeleton rejects wrong-class entries in corrupt skeleton tables");
        EXPECT_TRUE(rt_model3d_get_skeleton(model, 1) == view->skeletons[1],
                    "SceneAsset.GetSkeleton still returns valid clamped skeleton entries");
        EXPECT_TRUE(
            rt_model3d_get_animation(model, 0) == nullptr,
            "SceneAsset.GetAnimation rejects wrong-class entries in corrupt animation tables");
        EXPECT_TRUE(rt_model3d_get_animation(model, 1) == view->animations[1],
                    "SceneAsset.GetAnimation still returns valid clamped animation entries");

        void *animated_instance = rt_model3d_instantiate(model);
        auto *animated_root = static_cast<rt_scene_node3d *>(animated_instance);
        EXPECT_TRUE(animated_root && animated_root->bound_animator,
                    "SceneAsset default skeletal animator skips null/corrupt animation slots");
        EXPECT_TRUE(animated_root && animated_root->bound_node_animator,
                    "SceneAsset default node animator compacts null/corrupt clip slots");
    }

    std::remove(path);
}

static void test_fbx_imports_cameras_lights_and_object_animation() {
    const char *path = "/tmp/zanna_fbx_camera_light_animation.fbx";
    const char *ascii_path = "/tmp/zanna_fbx_camera_light_animation_ascii.fbx";
    EXPECT_TRUE(write_fbx_camera_light_animation_fixture(path),
                "FBX camera/light/object-animation fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX.Load imports typed camera/light scene attributes");
    if (!asset) {
        std::remove(path);
        return;
    }
    EXPECT_TRUE(rt_fbx_camera_count(asset) == 1, "FBX exposes imported Camera3D objects");
    void *camera = rt_fbx_get_camera(asset, 0);
    EXPECT_TRUE(camera != nullptr && rt_camera3d_is_ortho(camera) == 0,
                "FBX perspective camera retains its projection type");
    EXPECT_NEAR(rt_camera3d_get_fov(camera), 40.0, 0.001, "FBX camera retains vertical FOV");
    EXPECT_NEAR(
        rt_camera3d_get_near_plane(camera), 0.25, 0.001, "FBX camera retains its near plane");
    EXPECT_NEAR(
        rt_camera3d_get_far_plane(camera), 500.0, 0.001, "FBX camera retains its far plane");
    EXPECT_NEAR(rt_vec3_x(rt_camera3d_get_position(camera)),
                2.0,
                0.001,
                "FBX camera uses the evaluated model position");
    EXPECT_NEAR(rt_vec3_z(rt_camera3d_get_forward(camera)),
                -1.0,
                0.001,
                "FBX camera honors its LookAtProperty target");
    EXPECT_TRUE(rt_fbx_get_camera(asset, 1) == nullptr,
                "FBX camera accessor rejects out-of-range indices");

    void *scene_root = rt_fbx_get_scene_root(asset);
    void *camera_node =
        scene_root ? rt_scene_node3d_find(scene_root, rt_const_cstr("Camera01")) : nullptr;
    EXPECT_TRUE(camera_node && rt_scene_node3d_get_camera(camera_node) == camera,
                "FBX camera attaches to its owning SceneNode");
    void *light_node =
        scene_root ? rt_scene_node3d_find(scene_root, rt_const_cstr("KeyLight")) : nullptr;
    void *light = light_node ? rt_scene_node3d_get_light(light_node) : nullptr;
    EXPECT_TRUE(light != nullptr, "FBX light NodeAttribute attaches to its SceneNode3D");
    EXPECT_TRUE(rt_light3d_get_type(light) == 3, "FBX spot lights retain their light type");
    EXPECT_NEAR(rt_light3d_get_intensity(light),
                2.5,
                0.001,
                "FBX percentage intensity maps to runtime intensity");
    EXPECT_TRUE(rt_light3d_get_casts_shadows(light) == 1,
                "FBX CastShadows maps to the runtime light");
    void *point_node =
        scene_root ? rt_scene_node3d_find(scene_root, rt_const_cstr("FillLight")) : nullptr;
    void *directional_node =
        scene_root ? rt_scene_node3d_find(scene_root, rt_const_cstr("SunLight")) : nullptr;
    void *area_node =
        scene_root ? rt_scene_node3d_find(scene_root, rt_const_cstr("PanelLight")) : nullptr;
    void *sphere_area_node =
        scene_root ? rt_scene_node3d_find(scene_root, rt_const_cstr("OrbLight")) : nullptr;
    void *volume_node =
        scene_root ? rt_scene_node3d_find(scene_root, rt_const_cstr("FogLight")) : nullptr;
    EXPECT_TRUE(point_node && rt_light3d_get_type(rt_scene_node3d_get_light(point_node)) == 1,
                "FBX point lights map to native point lights");
    EXPECT_TRUE(directional_node &&
                    rt_light3d_get_type(rt_scene_node3d_get_light(directional_node)) == 0,
                "FBX directional lights map to native directional lights");
    void *area_light = area_node ? rt_scene_node3d_get_light(area_node) : nullptr;
    EXPECT_TRUE(area_light && rt_light3d_get_type(area_light) == 4,
                "FBX rectangle area lights map to native rectangle emitters");
    EXPECT_NEAR(rt_light3d_get_width(area_light), 4.0, 0.001, "FBX area width is preserved");
    EXPECT_NEAR(rt_light3d_get_height(area_light), 2.0, 0.001, "FBX area height is preserved");
    EXPECT_NEAR(rt_light3d_get_range(area_light), 15.0, 0.001, "FBX area range is preserved");
    EXPECT_TRUE(rt_light3d_get_decay_type(area_light) == 2,
                "FBX rectangle decay type is preserved");
    void *sphere_area_light =
        sphere_area_node ? rt_scene_node3d_get_light(sphere_area_node) : nullptr;
    EXPECT_TRUE(sphere_area_light && rt_light3d_get_type(sphere_area_light) == 5,
                "FBX sphere area lights map to native sphere emitters");
    EXPECT_NEAR(rt_light3d_get_radius(sphere_area_light),
                2.5,
                0.001,
                "FBX sphere area radius alias is preserved");
    EXPECT_NEAR(
        rt_light3d_get_range(sphere_area_light), 20.0, 0.001, "FBX sphere area range is preserved");
    EXPECT_TRUE(rt_light3d_get_decay_type(sphere_area_light) == 1,
                "FBX sphere area linear decay is preserved");
    void *volume_light = volume_node ? rt_scene_node3d_get_light(volume_node) : nullptr;
    EXPECT_TRUE(volume_light && rt_light3d_get_type(volume_light) == 6,
                "FBX volume lights map to native volume emitters");
    EXPECT_NEAR(
        rt_light3d_get_radius(volume_light), 6.0, 0.001, "FBX volume radius alias is preserved");
    EXPECT_NEAR(rt_light3d_get_range(volume_light), 9.0, 0.001, "FBX volume range is preserved");
    EXPECT_TRUE(rt_light3d_get_decay_type(volume_light) == 0,
                "FBX volume no-decay mode is preserved");
    EXPECT_TRUE(rt_light3d_get_casts_shadows(volume_light) == 0,
                "FBX volume lights never claim unsupported shadow slots");

    EXPECT_TRUE(rt_fbx_node_animation_count(asset) == 1,
                "FBX exposes non-skeletal object animation clips");
    auto *clip = static_cast<rt_node_animation3d *>(rt_fbx_get_node_animation(asset, 0));
    EXPECT_TRUE(clip != nullptr && scene3d_node_animation_channel_count(clip) == 3,
                "FBX object animation emits complete T/R/S channels");
    EXPECT_TRUE(
        std::strcmp(rt_string_cstr(rt_fbx_get_node_animation_name(asset, 0)), "MoveLight") == 0,
        "FBX exposes object animation names");
    if (clip && scene3d_node_animation_channel_count(clip) == 3) {
        EXPECT_TRUE(clip->channels[0].target_node_index == 2,
                    "FBX object channels bind by stable Objects ordinal");
        EXPECT_NEAR(clip->channels[0].values[0],
                    1.0,
                    0.001,
                    "FBX object animation begins at the authored transform");
        EXPECT_NEAR(clip->channels[0].values[3],
                    5.0,
                    0.001,
                    "FBX object animation ends at the authored transform");
    }
    {
        auto *view = static_cast<FbxAssetView *>(asset);
        int32_t saved_node_animation_count = view->node_animation_count;
        int32_t saved_node_animation_capacity = view->node_animation_capacity;
        int32_t saved_camera_count = view->camera_count;
        int32_t saved_camera_capacity = view->camera_capacity;
        void *saved_node_animation = view->node_animations[0];
        void *saved_camera = view->cameras[0];
        view->node_animation_count = 99;
        view->node_animation_capacity = 1;
        view->camera_count = 99;
        view->camera_capacity = 1;
        EXPECT_TRUE(rt_fbx_node_animation_count(asset) == 1 && rt_fbx_camera_count(asset) == 1,
                    "FBX new accessor counts clamp corrupt counts to capacity");
        view->node_animations[0] = light;
        view->cameras[0] = light;
        EXPECT_TRUE(rt_fbx_get_node_animation(asset, 0) == nullptr &&
                        rt_fbx_get_camera(asset, 0) == nullptr,
                    "FBX new accessors reject wrong-class array entries");
        EXPECT_TRUE(std::strcmp(rt_string_cstr(rt_fbx_get_node_animation_name(asset, 0)), "") == 0,
                    "FBX node-animation names use an empty wrong-class fallback");
        view->node_animations[0] = saved_node_animation;
        view->cameras[0] = saved_camera;
        view->node_animation_count = saved_node_animation_count;
        view->node_animation_capacity = saved_node_animation_capacity;
        view->camera_count = saved_camera_count;
        view->camera_capacity = saved_camera_capacity;
    }

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset adapts complete FBX scene attributes");
    if (model) {
        EXPECT_TRUE(rt_model3d_get_node_animation_count(model) == 1,
                    "SceneAsset retains FBX object animation clips");
        EXPECT_TRUE(rt_model3d_get_scene_count(model) == 1 &&
                        rt_model3d_get_camera_count(model, 0) == 1,
                    "SceneAsset associates FBX cameras with its default scene");
        void *scene = rt_model3d_instantiate_scene(model);
        EXPECT_TRUE(scene != nullptr, "FBX SceneAsset scene instantiates with default animators");
        if (scene) {
            void *instanced_camera_node = rt_scene3d_find(scene, rt_const_cstr("Camera01"));
            void *instanced_camera =
                instanced_camera_node ? rt_scene_node3d_get_camera(instanced_camera_node) : nullptr;
            void *inventory_camera = rt_model3d_get_camera(model, 0, 0);
            EXPECT_TRUE(instanced_camera && instanced_camera != inventory_camera,
                        "SceneAsset instantiation clones attached cameras independently");
            if (instanced_camera && inventory_camera) {
                rt_camera3d_set_fov(instanced_camera, 75.0);
                EXPECT_NEAR(
                    rt_camera3d_get_fov(inventory_camera),
                    40.0,
                    0.001,
                    "Instanced camera projection changes do not mutate the asset inventory");
            }
            rt_scene3d_sync_bindings(scene, 0.5);
            if (instanced_camera) {
                void *instanced_position = rt_camera3d_get_position(instanced_camera);
                EXPECT_NEAR(rt_vec3_x(instanced_position),
                            2.0,
                            0.001,
                            "SceneGraph.SyncBindings couples an FBX camera to its node");
            }
            void *animated_light = rt_scene3d_find(scene, rt_const_cstr("KeyLight"));
            EXPECT_NEAR(rt_vec3_x(rt_scene_node3d_get_position(animated_light)),
                        3.0,
                        0.001,
                        "SceneGraph.SyncBindings advances FBX object animation");
            if (rt_obj_release_check0(scene))
                rt_obj_free(scene);
        }
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
    }
    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);
    EXPECT_TRUE(write_fbx_camera_light_animation_ascii_fixture(ascii_path),
                "ASCII FBX camera/light/object-animation fixture can be written");
    void *ascii_asset = rt_fbx_load(rt_const_cstr(ascii_path));
    EXPECT_TRUE(ascii_asset != nullptr,
                "ASCII FBX imports camera/light/object animation through the typed graph");
    if (ascii_asset) {
        void *ascii_camera = rt_fbx_get_camera(ascii_asset, 0);
        EXPECT_TRUE(rt_fbx_camera_count(ascii_asset) == 1 && ascii_camera != nullptr &&
                        rt_camera3d_is_ortho(ascii_camera) == 1,
                    "ASCII FBX retains orthographic camera attributes");
        EXPECT_TRUE(rt_fbx_node_animation_count(ascii_asset) == 1,
                    "ASCII FBX retains non-skeletal animation stacks");
        void *ascii_light_node =
            rt_scene_node3d_find(rt_fbx_get_scene_root(ascii_asset), rt_const_cstr("KeyLight"));
        EXPECT_TRUE(ascii_light_node && rt_scene_node3d_get_light(ascii_light_node),
                    "ASCII FBX retains light NodeAttributes");
        if (rt_obj_release_check0(ascii_asset))
            rt_obj_free(ascii_asset);
    }
    std::remove(path);
    std::remove(ascii_path);
}

static void test_fbx_animates_complete_camera_projection_state() {
    const char *path = "/tmp/zanna_fbx_camera_projection_animation.fbx";
    EXPECT_TRUE(write_fbx_camera_projection_animation_fixture(path),
                "FBX camera projection-animation fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX imports coupled camera scalar animation");
    if (asset) {
        auto *clip = static_cast<rt_node_animation3d *>(rt_fbx_get_node_animation(asset, 0));
        const rt_node_anim_channel3d *channels[6] = {};
        if (clip)
            for (int32_t i = 0; i < scene3d_node_animation_channel_count(clip); ++i) {
                const rt_node_anim_channel3d &channel = clip->channels[i];
                if (channel.path >= RT_NODE_ANIM_PATH_CAMERA_FOV &&
                    channel.path <= RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE) {
                    int slot = -1;
                    if (channel.path == RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE)
                        slot = 0;
                    else if (channel.path == RT_NODE_ANIM_PATH_CAMERA_ASPECT)
                        slot = 1;
                    else if (channel.path == RT_NODE_ANIM_PATH_CAMERA_FOV)
                        slot = 2;
                    else if (channel.path == RT_NODE_ANIM_PATH_CAMERA_NEAR)
                        slot = 3;
                    else if (channel.path == RT_NODE_ANIM_PATH_CAMERA_FAR)
                        slot = 4;
                    else if (channel.path == RT_NODE_ANIM_PATH_CAMERA_ORTHO_SIZE)
                        slot = 5;
                    if (slot >= 0)
                        channels[slot] = &channel;
                }
            }
        EXPECT_TRUE(clip && scene3d_node_animation_channel_count(clip) == 6,
                    "FBX emits exactly six derived camera projection channels");
        EXPECT_TRUE(channels[0] && channels[1] && channels[2] && channels[3] && channels[4] &&
                        channels[5],
                    "Camera clip contains projection, aspect, FOV, clip-plane, and ortho paths");
        if (channels[0]) {
            EXPECT_TRUE(channels[0]->interpolation == RT_NODE_ANIM_INTERP_STEP,
                        "Camera projection mode always uses step interpolation");
            EXPECT_TRUE(channels[0]->key_count == 5,
                        "Discrete camera modes retain pre-switch FBX ticks");
        }
        for (const rt_node_anim_channel3d *channel : channels)
            if (channel)
                EXPECT_TRUE(channel->value_width == 1,
                            "Every private camera animation path is scalar");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset adapts complete FBX camera projection animation");
    if (model) {
        void *scene = rt_model3d_instantiate_scene(model);
        EXPECT_TRUE(scene != nullptr, "Animated FBX camera SceneAsset instantiates");
        if (scene) {
            rt_scene3d_sync_bindings(scene, 0.5);
            void *node = rt_scene3d_find(scene, rt_const_cstr("AnimatedCamera"));
            void *camera = node ? rt_scene_node3d_get_camera(node) : nullptr;
            EXPECT_TRUE(camera != nullptr && rt_camera3d_is_ortho(camera) == 1,
                        "Projection step switches the attached camera to orthographic");
            if (camera) {
                auto *camera_view = static_cast<rt_camera3d *>(camera);
                EXPECT_NEAR(camera_view->aspect,
                            3.0,
                            0.001,
                            "AspectWidth/AspectHeight curves evaluate as a coupled ratio");
                EXPECT_NEAR(rt_camera3d_get_near_plane(camera),
                            0.375,
                            0.001,
                            "Model-targeted FBX near-plane animation reaches Camera3D");
                EXPECT_NEAR(rt_camera3d_get_far_plane(camera),
                            750.0,
                            0.01,
                            "FBX far-plane animation reaches Camera3D");
                EXPECT_NEAR(rt_camera3d_get_ortho_size(camera),
                            15.0,
                            0.001,
                            "CameraOrthoZoom alias animation reaches the retained ortho size");
                const double focal = 37.5;
                const double film_width_inches = 0.9;
                const double horizontal = 2.0 * std::atan((film_width_inches * 25.4 * 0.5) / focal);
                const double expected_vertical = 2.0 * std::atan(std::tan(horizontal * 0.5) / 3.0) *
                                                 180.0 / 3.14159265358979323846;
                rt_camera3d_set_is_ortho(camera, 0);
                EXPECT_NEAR(
                    rt_camera3d_get_fov(camera),
                    expected_vertical,
                    0.002,
                    "Animated focal length and film aperture produce retained vertical FOV");
                EXPECT_TRUE(std::fabs(rt_camera3d_get_fov(camera) - 60.0) > 1.0,
                            "Focal-length aperture mode ignores an unrelated FieldOfViewY curve");
            }
            if (rt_obj_release_check0(scene))
                rt_obj_free(scene);
        }
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
    }
    std::remove(path);

    const char *invalid_path = "/tmp/zanna_fbx_camera_projection_invalid.fbx";
    EXPECT_TRUE(write_fbx_camera_projection_animation_fixture(invalid_path, true),
                "Invalid FBX camera projection fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(invalid_path));
    EXPECT_TRUE(asset == nullptr, "Unknown animated FBX projection modes reject the whole asset");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT &&
                    rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "camera projection") != nullptr,
                "Invalid camera animation reports a stable projection-specific diagnostic");
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(invalid_path);
}

static void test_fbx_composes_animation_layers_for_nodes_and_bones() {
    const char *path = "/tmp/zanna_fbx_animation_layers.fbx";
    EXPECT_TRUE(write_fbx_animation_layer_composition_fixture(path),
                "FBX animation-layer composition fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX composes every active animation layer transactionally");
    if (asset) {
        auto *node_clip = static_cast<rt_node_animation3d *>(rt_fbx_get_node_animation(asset, 0));
        const rt_node_anim_channel3d *translation = nullptr;
        const rt_node_anim_channel3d *rotation = nullptr;
        const rt_node_anim_channel3d *scale = nullptr;
        if (node_clip)
            for (int32_t i = 0; i < scene3d_node_animation_channel_count(node_clip); ++i) {
                const rt_node_anim_channel3d &channel = node_clip->channels[i];
                const char *name = channel.target_name ? rt_string_cstr(channel.target_name) : "";
                if (std::strcmp(name, "LayerNode") != 0)
                    continue;
                if (channel.path == RT_NODE_ANIM_PATH_TRANSLATION)
                    translation = &channel;
                else if (channel.path == RT_NODE_ANIM_PATH_ROTATION)
                    rotation = &channel;
                else if (channel.path == RT_NODE_ANIM_PATH_SCALE)
                    scale = &channel;
            }
        EXPECT_TRUE(node_clip && translation && rotation && scale,
                    "Layered scene model emits composed translation, rotation, and scale channels");
        if (translation && rotation && scale) {
            EXPECT_TRUE(translation->key_count == 3 && rotation->key_count == 3 &&
                            scale->key_count == 3,
                        "Animated layer weight joins the shared sample-time union");
            EXPECT_NEAR(translation->times[0],
                        0.0,
                        0.0001,
                        "Negative FBX layer time normalizes to clip start");
            EXPECT_NEAR(translation->values[0],
                        5.5,
                        0.002,
                        "Base, additive, and explicit override layers compose at zero weight");
            EXPECT_NEAR(translation->values[3],
                        7.875,
                        0.002,
                        "Animated override-passthrough weight composes at its midpoint");
            EXPECT_NEAR(translation->values[6],
                        10.0,
                        0.002,
                        "Full passthrough weight overrides preceding translation layers");
            EXPECT_NEAR(scale->values[0],
                        4.0,
                        0.002,
                        "Multiply scale accumulation exponentiates by layer weight");
            EXPECT_NEAR(scale->values[3],
                        6.0,
                        0.002,
                        "Scale override-passthrough interpolates the accumulated scale");
            EXPECT_NEAR(scale->values[6],
                        8.0,
                        0.002,
                        "Full scale passthrough reaches the authored layer value");
            EXPECT_NEAR(scale->values[1],
                        2.0,
                        0.002,
                        "Additive scale mode applies weighted delta from unit scale");
            EXPECT_NEAR(rotation->values[2],
                        std::sin(15.0 * 3.14159265358979323846 / 180.0),
                        0.003,
                        "Rotation-by-channel adds weighted Euler lanes on the base layer");
            EXPECT_NEAR(rotation->values[6],
                        std::sin(33.75 * 3.14159265358979323846 / 180.0),
                        0.003,
                        "Rotation-by-layer uses shortest-path quaternion interpolation");
            EXPECT_NEAR(rotation->values[10],
                        std::sin(50.0 * 3.14159265358979323846 / 180.0),
                        0.003,
                        "Full rotation layer reaches its authored quaternion");
        }
        auto *skeleton_clip = static_cast<rt_animation3d *>(rt_fbx_get_animation(asset, 0));
        const vgfx3d_anim_channel_t *bone_channel = nullptr;
        if (skeleton_clip)
            for (int32_t i = 0; i < skeleton_clip->channel_count; ++i)
                if (skeleton_clip->channels[i].bone_index == 0)
                    bone_channel = &skeleton_clip->channels[i];
        EXPECT_TRUE(bone_channel && bone_channel->keyframe_count == 3,
                    "The same layer evaluator emits three skeletal samples");
        if (bone_channel && bone_channel->keyframe_count == 3) {
            EXPECT_NEAR(bone_channel->keyframes[1].position[0],
                        7.875,
                        0.002,
                        "Skeletal and scene-node layer composition produce identical translation");
            EXPECT_NEAR(bone_channel->keyframes[1].rotation[2],
                        std::sin(33.75 * 3.14159265358979323846 / 180.0),
                        0.003,
                        "Skeletal and scene-node layer composition produce identical rotation");
            EXPECT_NEAR(bone_channel->keyframes[1].scale_xyz[0],
                        6.0,
                        0.002,
                        "Skeletal and scene-node layer composition produce identical scale");
        }
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(path);

    const char *solo_path = "/tmp/zanna_fbx_animation_layer_solo.fbx";
    EXPECT_TRUE(write_fbx_animation_layer_solo_fixture(solo_path),
                "FBX solo/mute layer fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(solo_path));
    EXPECT_TRUE(asset != nullptr, "FBX evaluates solo and mute layer state");
    if (asset) {
        auto *clip = static_cast<rt_node_animation3d *>(rt_fbx_get_node_animation(asset, 0));
        const rt_node_anim_channel3d *translation = nullptr;
        if (clip)
            for (int32_t i = 0; i < scene3d_node_animation_channel_count(clip); ++i)
                if (clip->channels[i].path == RT_NODE_ANIM_PATH_TRANSLATION)
                    translation = &clip->channels[i];
        EXPECT_TRUE(translation && translation->key_count >= 2,
                    "Solo layer retains its authored transform channel");
        if (translation)
            EXPECT_NEAR(translation->values[0],
                        9.0,
                        0.001,
                        "Active solo suppresses non-solo and muted-solo layers");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(solo_path);

    const char *invalid_path = "/tmp/zanna_fbx_animation_layer_nonfinite.fbx";
    EXPECT_TRUE(write_fbx_animation_layer_solo_fixture(invalid_path, true),
                "Non-finite FBX layer-weight fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(invalid_path));
    EXPECT_TRUE(asset == nullptr, "Non-finite animation-layer weight rejects the complete load");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT &&
                    rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "animation layer") != nullptr,
                "Malformed animation layers use a stable layer-specific diagnostic");
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(invalid_path);
}

static void test_fbx_imports_general_constraints_and_animated_dependencies() {
    const char *path = "/tmp/zanna_fbx_constraint_zoo.fbx";
    EXPECT_TRUE(write_fbx_constraint_zoo_fixture(path),
                "FBX built-in constraint zoo fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr,
                "FBX imports every supported built-in constraint transactionally");
    if (!asset) {
        std::remove(path);
        return;
    }
    void *root = rt_fbx_get_scene_root(asset);
    void *position = root ? rt_scene_node3d_find(root, rt_const_cstr("PositionTarget")) : nullptr;
    void *rotation = root ? rt_scene_node3d_find(root, rt_const_cstr("RotationTarget")) : nullptr;
    void *scale = root ? rt_scene_node3d_find(root, rt_const_cstr("ScaleTarget")) : nullptr;
    void *parent = root ? rt_scene_node3d_find(root, rt_const_cstr("ParentTarget")) : nullptr;
    void *aim = root ? rt_scene_node3d_find(root, rt_const_cstr("AimTarget")) : nullptr;
    void *inactive = root ? rt_scene_node3d_find(root, rt_const_cstr("InactiveTarget")) : nullptr;
    void *ik_first = root ? rt_scene_node3d_find(root, rt_const_cstr("IkFirst")) : nullptr;
    void *ik_middle = root ? rt_scene_node3d_find(root, rt_const_cstr("IkMiddle")) : nullptr;
    void *ik_end = root ? rt_scene_node3d_find(root, rt_const_cstr("IkEnd")) : nullptr;
    double matrix[16] = {};
    EXPECT_TRUE(position && rt_scene_node3d_get_world_matrix_components(position, matrix),
                "Position-constrained model remains in the imported scene hierarchy");
    EXPECT_NEAR(matrix[3], 3.0, 0.001, "Position constraint blends weighted sources and offset");
    EXPECT_NEAR(matrix[7], 0.0, 0.001, "Position constraint honors a disabled Y affect mask");
    EXPECT_NEAR(matrix[11], 4.0, 0.001, "Position constraint blends its remaining world axes");

    EXPECT_TRUE(rotation && rt_scene_node3d_get_world_matrix_components(rotation, matrix),
                "Rotation-constrained model remains addressable by name");
    EXPECT_NEAR(matrix[0], -0.5, 0.002, "Rotation constraint composes its authored offset");
    EXPECT_NEAR(matrix[4], 0.8660254, 0.002, "Rotation constraint retains world orientation");

    EXPECT_TRUE(scale && rt_scene_node3d_get_world_matrix_components(scale, matrix),
                "Scale-constrained model has a finite world matrix");
    EXPECT_NEAR(matrix[0], 1.0, 0.002, "Scale constraint multiplies the X offset before blending");
    EXPECT_NEAR(matrix[5], 3.5, 0.002, "Scale constraint blends weighted Y scale");
    EXPECT_NEAR(matrix[10], 2.5, 0.002, "Scale constraint blends weighted Z scale");

    EXPECT_TRUE(parent && rt_scene_node3d_get_world_matrix_components(parent, matrix),
                "Parent-constrained model has an evaluated world transform");
    EXPECT_NEAR(matrix[3], 11.0, 0.003, "Parent offset translation is evaluated in source space");
    EXPECT_NEAR(matrix[7], 1.7320508, 0.003, "Parent offset follows the source orientation");
    EXPECT_NEAR(matrix[0], 0.0, 0.003, "Parent constraint composes source and offset rotation");
    EXPECT_NEAR(matrix[4], 1.0, 0.003, "Parent rotation uses normalized quaternion blending");

    EXPECT_TRUE(aim && rt_scene_node3d_get_world_matrix_components(aim, matrix),
                "Aim-constrained model has an evaluated orthonormal basis");
    EXPECT_NEAR(matrix[0], 0.0, 0.003, "Aim maps the authored local aim X axis away from world X");
    EXPECT_NEAR(matrix[4], 0.0, 0.003, "Aim direction remains orthogonal to its up object");
    EXPECT_NEAR(matrix[8], -1.0, 0.003, "Aim maps the local X axis toward its weighted target");
    EXPECT_NEAR(matrix[5], 1.0, 0.003, "Aim honors object-position world-up mode");

    EXPECT_TRUE(inactive && rt_scene_node3d_get_world_matrix_components(inactive, matrix),
                "Inactive constrained model remains in the scene");
    EXPECT_NEAR(matrix[3], 1.0, 0.001, "Inactive constraint does not alter translation");
    EXPECT_NEAR(matrix[7], 2.0, 0.001, "Inactive constraint preserves every local axis");
    EXPECT_NEAR(matrix[11], 3.0, 0.001, "Inactive constraint preserves Z translation");

    EXPECT_TRUE(ik_end && rt_scene_node3d_get_world_matrix_components(ik_end, matrix),
                "Single-chain IK end joint remains part of its original hierarchy");
    EXPECT_NEAR(matrix[3], 1.0, 0.002, "FABRIK reaches the effector X coordinate");
    EXPECT_NEAR(matrix[7], 1.0, 0.002, "FABRIK reaches the effector Y coordinate");
    EXPECT_NEAR(matrix[11], 0.0, 0.002, "FABRIK preserves the constrained solve plane");
    double first_world[16] = {};
    double middle_world[16] = {};
    double end_world[16] = {};
    EXPECT_TRUE(ik_first && ik_middle &&
                    rt_scene_node3d_get_world_matrix_components(ik_first, first_world) &&
                    rt_scene_node3d_get_world_matrix_components(ik_middle, middle_world) &&
                    rt_scene_node3d_get_world_matrix_components(ik_end, end_world),
                "Single-chain IK publishes every solved joint transform");
    auto segment_length = [](const double a[16], const double b[16]) {
        const double x = b[3] - a[3];
        const double y = b[7] - a[7];
        const double z = b[11] - a[11];
        return std::sqrt(x * x + y * y + z * z);
    };
    EXPECT_NEAR(segment_length(first_world, middle_world),
                1.0,
                0.002,
                "FABRIK preserves the first authored segment length");
    EXPECT_NEAR(segment_length(middle_world, end_world),
                1.0,
                0.002,
                "Pole and twist guidance preserve the second segment length");
    EXPECT_TRUE(std::fabs(middle_world[11]) > 0.05,
                "Pole-object and twist properties guide the internal IK joint out of plane");
    EXPECT_TRUE(asset_warning_contains("custom constraint", nullptr) ||
                    asset_warning_contains("Vendor Twist", nullptr),
                "Unknown custom constraints are preserved as bounded import warnings");

    auto *clip = static_cast<rt_node_animation3d *>(rt_fbx_get_node_animation(asset, 0));
    bool found_constrained_translation = false;
    if (clip) {
        for (int32_t i = 0; i < scene3d_node_animation_channel_count(clip); ++i) {
            const rt_node_anim_channel3d &channel = clip->channels[i];
            if (channel.target_node_index == 0 && channel.path == RT_NODE_ANIM_PATH_TRANSLATION &&
                channel.key_count >= 2 && channel.value_width == 3) {
                found_constrained_translation = true;
                EXPECT_NEAR(channel.values[(size_t)(channel.key_count - 1) * 3u],
                            4.0,
                            0.002,
                            "Constraint source keys propagate into constrained-node samples");
            }
        }
    }
    EXPECT_TRUE(found_constrained_translation,
                "Constraint-only animated targets emit baked NodeAnimation3D channels");
    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

static void test_fbx_constraint_validation_and_bounded_ik_failure() {
    const struct {
        FbxBadConstraintFixture kind;
        const char *suffix;
    } malformed[] = {{FbxBadConstraintFixture::MissingTarget, "missing_target"},
                     {FbxBadConstraintFixture::Cycle, "cycle"},
                     {FbxBadConstraintFixture::WeightMismatch, "weights"},
                     {FbxBadConstraintFixture::NonFiniteOffset, "nonfinite"},
                     {FbxBadConstraintFixture::MismatchedIkChain, "ik_chain"},
                     {FbxBadConstraintFixture::DescendantSource, "descendant_source"}};

    for (const auto &entry : malformed) {
        std::string path = std::string("/tmp/zanna_fbx_bad_constraint_") + entry.suffix + ".fbx";
        EXPECT_TRUE(write_fbx_bad_constraint_fixture(path.c_str(), entry.kind),
                    "Malformed FBX constraint fixture can be written");
        void *asset = rt_fbx_load(rt_const_cstr(path.c_str()));
        EXPECT_TRUE(asset == nullptr, "Malformed active FBX constraint rejects the complete load");
        EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                    "Malformed active FBX constraint reports a content error");
        EXPECT_TRUE(rt_asset_error_get_message() &&
                        std::strstr(rt_asset_error_get_message(), "constraint") != nullptr,
                    "Constraint validation uses a stable constraint-specific diagnostic");
        if (asset && rt_obj_release_check0(asset))
            rt_obj_free(asset);
        std::remove(path.c_str());
    }

    const char *ancestor_path = "/tmp/zanna_fbx_constraint_ancestor_order.fbx";
    EXPECT_TRUE(write_fbx_constraint_ancestor_order_fixture(ancestor_path),
                "Ancestor-dependent FBX constraint fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(ancestor_path));
    EXPECT_TRUE(asset != nullptr,
                "Constraint ordering follows full source ancestry rather than object order");
    if (asset) {
        void *target = rt_scene_node3d_find(rt_fbx_get_scene_root(asset), rt_const_cstr("Target"));
        double world[16] = {};
        EXPECT_TRUE(target && rt_scene_node3d_get_world_matrix_components(target, world),
                    "Ancestor-dependent constrained target remains addressable");
        EXPECT_NEAR(world[3],
                    12.0,
                    0.001,
                    "Producer constraint runs before a consumer of its nested source");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(ancestor_path);

    const char *unreachable_path = "/tmp/zanna_fbx_unreachable_ik.fbx";
    EXPECT_TRUE(write_fbx_constraint_zoo_fixture(unreachable_path, true),
                "Unreachable FBX IK fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(unreachable_path));
    EXPECT_TRUE(asset != nullptr, "Unreachable but valid IK keeps the closest finite pose");
    if (asset) {
        void *end = rt_scene_node3d_find(rt_fbx_get_scene_root(asset), rt_const_cstr("IkEnd"));
        double world[16] = {};
        EXPECT_TRUE(end && rt_scene_node3d_get_world_matrix_components(end, world),
                    "Unreachable IK publishes a finite end-joint transform");
        EXPECT_NEAR(world[3], 2.0, 0.002, "Unreachable IK preserves total segment length");
        EXPECT_NEAR(world[7], 0.0, 0.002, "Unreachable IK stretches toward the effector only");
        EXPECT_TRUE(asset_warning_contains("IK", "unreachable"),
                    "Unreachable IK emits one bounded degradation warning");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(unreachable_path);

    const char *partial_path = "/tmp/zanna_fbx_partial_ik.fbx";
    EXPECT_TRUE(write_fbx_constraint_zoo_fixture(partial_path, false, 50.0),
                "Partial-weight FBX IK fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(partial_path));
    EXPECT_TRUE(asset != nullptr, "Partial-weight IK evaluates to a finite blended chain");
    if (asset) {
        void *root = rt_fbx_get_scene_root(asset);
        void *first = rt_scene_node3d_find(root, rt_const_cstr("IkFirst"));
        void *middle = rt_scene_node3d_find(root, rt_const_cstr("IkMiddle"));
        void *end = rt_scene_node3d_find(root, rt_const_cstr("IkEnd"));
        double first_world[16] = {};
        double middle_world[16] = {};
        double end_world[16] = {};
        EXPECT_TRUE(first && middle && end &&
                        rt_scene_node3d_get_world_matrix_components(first, first_world) &&
                        rt_scene_node3d_get_world_matrix_components(middle, middle_world) &&
                        rt_scene_node3d_get_world_matrix_components(end, end_world),
                    "Partial-weight IK retains the complete chain hierarchy");
        auto segment_length = [](const double a[16], const double b[16]) {
            const double x = b[3] - a[3];
            const double y = b[7] - a[7];
            const double z = b[11] - a[11];
            return std::sqrt(x * x + y * y + z * z);
        };
        EXPECT_NEAR(segment_length(first_world, middle_world),
                    1.0,
                    0.002,
                    "Partial IK weight preserves the first segment length");
        EXPECT_NEAR(segment_length(middle_world, end_world),
                    1.0,
                    0.002,
                    "Partial IK weight preserves the second segment length");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(partial_path);

    const char *z_up_path = "/tmp/zanna_fbx_z_up_constraints.fbx";
    EXPECT_TRUE(write_fbx_constraint_zoo_fixture(z_up_path, false, 100.0, true),
                "Z-up FBX constraint fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(z_up_path));
    EXPECT_TRUE(asset != nullptr, "Z-up constraints import through the common basis conversion");
    if (asset) {
        void *position =
            rt_scene_node3d_find(rt_fbx_get_scene_root(asset), rt_const_cstr("PositionTarget"));
        double world[16] = {};
        EXPECT_TRUE(position && rt_scene_node3d_get_world_matrix_components(position, world),
                    "Z-up constrained target publishes a finite converted transform");
        EXPECT_NEAR(world[3], 3.0, 0.002, "Z-up translation offsets retain source X");
        EXPECT_NEAR(world[7], 4.0, 0.002, "Z-up translation offsets map source Z to runtime Y");
        EXPECT_NEAR(
            world[11], 0.0, 0.002, "Z-up affect masks map the disabled source Y axis to runtime Z");
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(z_up_path);
}

static void test_fbx_constraints_drive_static_and_animated_skeleton_poses() {
    const char *path = "/tmp/zanna_fbx_animated_skeleton_constraint.fbx";
    EXPECT_TRUE(write_fbx_animated_skeleton_constraint_fixture(path),
                "Animated skeletal FBX constraint fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "Animated non-bone source can constrain an FBX skeleton bone");
    if (asset) {
        void *skeleton = rt_fbx_get_skeleton(asset);
        double bind_local[16] = {};
        EXPECT_TRUE(skeleton && rt_skeleton3d_get_bone_count(skeleton) == 1,
                    "Constrained skeletal fixture retains its single bone");
        EXPECT_TRUE(skeleton && rt_skeleton3d_get_bone_bind_local_raw(skeleton, 0, bind_local),
                    "Constrained skeleton publishes a finite static bind pose");
        EXPECT_NEAR(bind_local[3], 3.0, 0.001, "Static bone pose includes its point constraint");
        auto *animation = static_cast<rt_animation3d *>(rt_fbx_get_animation(asset, 0));
        const vgfx3d_anim_channel_t *channel = nullptr;
        if (animation)
            for (int32_t i = 0; i < animation->channel_count; ++i)
                if (animation->channels[i].bone_index == 0)
                    channel = &animation->channels[i];
        EXPECT_TRUE(channel && channel->keyframe_count >= 2,
                    "Constraint-only bone motion emits skeletal animation keyframes");
        if (channel && channel->keyframe_count >= 2) {
            EXPECT_NEAR(channel->keyframes[0].position[0],
                        3.0,
                        0.001,
                        "Skeletal constraint begins at the evaluated source pose");
            EXPECT_NEAR(channel->keyframes[channel->keyframe_count - 1].position[0],
                        11.0,
                        0.001,
                        "Animated external driver propagates through the bone constraint");
        }
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(path);
}

static void test_fbx_tessellates_binary_and_ascii_procedural_surfaces() {
    const char *path = "/tmp/zanna_fbx_procedural_surfaces.fbx";
    const char *ascii_path = "/tmp/zanna_fbx_procedural_surface_ascii.fbx";
    EXPECT_TRUE(write_fbx_procedural_surface_fixture(path),
                "Binary FBX procedural-surface fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX.Load tessellates NURBS surfaces and patches");
    if (asset) {
        EXPECT_TRUE(rt_fbx_mesh_count(asset) == 2,
                    "FBX procedural Geometry records become ordinary mesh resources");
        auto *nurbs = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 0));
        auto *patch = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 1));
        EXPECT_TRUE(nurbs && nurbs->vertex_count == 9 && nurbs->index_count == 24,
                    "Open two-step NURBS emits a shared 3x3 grid and eight triangles");
        if (nurbs && nurbs->vertex_count == 9) {
            EXPECT_NEAR(nurbs->vertices[4].pos[0],
                        0.6,
                        0.0001,
                        "NURBS tessellation applies rational control-point weights in U");
            EXPECT_NEAR(nurbs->vertices[4].pos[1],
                        0.6,
                        0.0001,
                        "NURBS tessellation applies rational control-point weights in V");
            EXPECT_TRUE(std::isfinite(nurbs->vertices[4].normal[0]) &&
                            std::isfinite(nurbs->vertices[4].normal[1]) &&
                            std::isfinite(nurbs->vertices[4].normal[2]) &&
                            nurbs->vertices[4].normal[2] > 0.99f,
                        "NURBS tessellation emits finite derivative normals");
            EXPECT_NEAR(nurbs->vertices[4].uv[0],
                        0.5,
                        0.0001,
                        "NURBS tessellation emits normalized row-major U coordinates");
            EXPECT_NEAR(nurbs->vertices[4].uv[1],
                        0.5,
                        0.0001,
                        "NURBS tessellation emits normalized row-major V coordinates");
        }
        EXPECT_TRUE(patch && patch->vertex_count == 9 && patch->index_count == 24,
                    "Linear FBX Patch emits a shared 3x3 grid and eight triangles");
        if (patch && patch->vertex_count == 9) {
            EXPECT_NEAR(patch->vertices[4].pos[0],
                        0.5,
                        0.0001,
                        "Patch tensor-product evaluation preserves bilinear U");
            EXPECT_NEAR(patch->vertices[4].pos[1],
                        0.5,
                        0.0001,
                        "Patch tensor-product evaluation preserves bilinear V");
        }
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset adapts tessellated FBX procedural geometry");
    if (model) {
        void *nurbs_node = rt_model3d_find_node(model, rt_const_cstr("RationalSurface"));
        void *patch_node = rt_model3d_find_node(model, rt_const_cstr("LinearPatch"));
        EXPECT_TRUE(nurbs_node && rt_scene_node3d_get_mesh(nurbs_node),
                    "Tessellated NURBS binds to its authored model node");
        auto *surface_material =
            nurbs_node ? static_cast<rt_material3d *>(rt_scene_node3d_get_material(nurbs_node))
                       : nullptr;
        EXPECT_TRUE(surface_material != nullptr,
                    "Tessellated NURBS uses the ordinary connected-material path");
        if (surface_material)
            EXPECT_NEAR(surface_material->diffuse[2],
                        0.6,
                        0.001,
                        "Tessellated NURBS retains its connected FBX material values");
        EXPECT_TRUE(patch_node && rt_scene_node3d_get_mesh(patch_node),
                    "Tessellated patch binds to its authored model node");
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
    }

    EXPECT_TRUE(write_fbx_ascii_procedural_surface_fixture(ascii_path),
                "ASCII FBX procedural-surface fixture can be written");
    void *ascii_asset = rt_fbx_load(rt_const_cstr(ascii_path));
    EXPECT_TRUE(ascii_asset != nullptr && rt_fbx_mesh_count(ascii_asset) == 1,
                "ASCII FBX uses the same procedural-surface tessellator as binary FBX");
    if (ascii_asset) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(ascii_asset, 0));
        EXPECT_TRUE(mesh && mesh->vertex_count == 9 && mesh->index_count == 24,
                    "ASCII rational NURBS retains the binary grid topology");
        if (rt_obj_release_check0(ascii_asset))
            rt_obj_free(ascii_asset);
    }
    std::remove(path);
    std::remove(ascii_path);
}

static void test_fbx_tessellates_surface_forms_and_every_patch_basis() {
    const char *path = "/tmp/zanna_fbx_procedural_surface_variants.fbx";
    EXPECT_TRUE(write_fbx_procedural_surface_variants_fixture(path),
                "FBX procedural-surface variant fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX imports closed/periodic NURBS and every patch basis");
    if (!asset) {
        std::remove(path);
        return;
    }
    EXPECT_TRUE(rt_fbx_mesh_count(asset) == 9,
                "Every renderable procedural Geometry becomes one mesh resource");
    for (int64_t mesh_index = 0; mesh_index < rt_fbx_mesh_count(asset); ++mesh_index) {
        auto *mesh = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, mesh_index));
        bool finite = mesh && mesh->vertex_count > 0 && mesh->index_count >= 3;
        if (mesh) {
            for (uint32_t vertex = 0; finite && vertex < mesh->vertex_count; ++vertex) {
                for (int lane = 0; lane < 3; ++lane) {
                    finite = finite && std::isfinite(mesh->vertices[vertex].pos[lane]) &&
                             std::isfinite(mesh->vertices[vertex].normal[lane]);
                }
            }
        }
        EXPECT_TRUE(finite, "Every procedural basis emits finite positions and normals");
    }
    auto *closed = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 0));
    auto *periodic = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 1));
    auto *generated_knots = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 2));
    auto *closed_patch = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 8));
    EXPECT_TRUE(closed && closed->vertex_count == 6 && closed->index_count == 18,
                "Closed NURBS omits its duplicated U seam vertex and wraps topology");
    EXPECT_TRUE(periodic && periodic->vertex_count == 6 && periodic->index_count == 18,
                "Periodic NURBS omits its duplicated U seam vertex and wraps topology");
    EXPECT_TRUE(generated_knots && generated_knots->vertex_count == 6 &&
                    generated_knots->index_count == 12,
                "Missing legal open knot vectors are generated deterministically");
    const double expected_patch_mid_z[] = {
        0.09375,
        0.125,
        0.140625,
        0.1197916667,
        0.125,
    };
    for (int basis = 0; basis < 5; ++basis) {
        auto *basis_mesh = static_cast<rt_mesh3d *>(rt_fbx_get_mesh(asset, 3 + basis));
        EXPECT_TRUE(basis_mesh && basis_mesh->vertex_count == 6 && basis_mesh->index_count == 12,
                    "Open single-span patch basis preserves shared two-step topology");
        if (basis_mesh && basis_mesh->vertex_count > 1) {
            EXPECT_NEAR(basis_mesh->vertices[1].pos[2],
                        expected_patch_mid_z[basis],
                        0.0001,
                        "Patch basis tensor evaluates the authored midpoint weights");
        }
    }
    EXPECT_TRUE(closed_patch && closed_patch->vertex_count == 16 && closed_patch->index_count == 48,
                "Closed Patch shares its seam vertices across wrapped quads");
    if (closed_patch && closed_patch->index_count >= 6) {
        bool found_wrapped_edge = false;
        for (uint32_t index = 0; index + 1 < closed_patch->index_count; ++index) {
            uint32_t a = closed_patch->indices[index];
            uint32_t b = closed_patch->indices[index + 1];
            if ((a == 7u && b == 0u) || (a == 0u && b == 7u)) {
                found_wrapped_edge = true;
                break;
            }
        }
        EXPECT_TRUE(found_wrapped_edge,
                    "Closed Patch topology references the first row vertex from the final span");
    }
    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);
}

static void test_fbx_rejects_malformed_procedural_surfaces_transactionally() {
    const char *step_path = "/tmp/zanna_fbx_bad_nurbs_step.fbx";
    const char *knot_path = "/tmp/zanna_fbx_bad_nurbs_knots.fbx";
    const char *control_path = "/tmp/zanna_fbx_bad_nurbs_controls.fbx";
    const char *trim_path = "/tmp/zanna_fbx_trimmed_nurbs.fbx";
    const char *budget_path = "/tmp/zanna_fbx_budgeted_nurbs.fbx";
    EXPECT_TRUE(write_fbx_procedural_surface_fixture(step_path, 257),
                "Oversized NURBS-step fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(step_path));
    EXPECT_TRUE(asset == nullptr, "FBX rejects NURBS step counts above the audited maximum");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "Oversized NURBS step reports corrupt authored content");
    EXPECT_TRUE(rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "step") != nullptr,
                "Oversized NURBS step diagnostic identifies the invalid field");

    EXPECT_TRUE(write_fbx_procedural_surface_fixture(knot_path, 2, 2, true),
                "Descending NURBS-knot fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(knot_path));
    EXPECT_TRUE(asset == nullptr, "FBX rejects descending NURBS knot vectors transactionally");
    EXPECT_TRUE(rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "knot") != nullptr,
                "Descending-knot diagnostic identifies the invalid field");

    EXPECT_TRUE(write_fbx_bad_surface_control_count_fixture(control_path),
                "Mismatched NURBS control-net fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(control_path));
    EXPECT_TRUE(asset == nullptr,
                "FBX rejects a procedural control net that does not match Dimensions");
    EXPECT_TRUE(rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "control net") != nullptr,
                "Mismatched control-net diagnostic identifies the invalid payload");

    EXPECT_TRUE(write_fbx_trimmed_surface_fixture(trim_path),
                "Connected NURBS trim-boundary fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(trim_path));
    EXPECT_TRUE(asset == nullptr,
                "FBX rejects active NURBS trim graphs instead of changing the boundary");
    EXPECT_TRUE(rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "trimmed NURBS") != nullptr,
                "Trim rejection uses the stable procedural-surface diagnostic");

    EXPECT_TRUE(write_fbx_procedural_surface_fixture(budget_path, 256, 256),
                "High-resolution NURBS budget fixture can be written");
    rt_fbx_test_set_load_budget_bytes(1024u * 1024u);
    asset = rt_fbx_load(rt_const_cstr(budget_path));
    EXPECT_TRUE(asset == nullptr,
                "Generated procedural mesh storage participates in the aggregate FBX budget");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_TOO_LARGE,
                "Generated procedural mesh budget exhaustion reports too-large content");
    asset = rt_fbx_load(rt_const_cstr(step_path));
    EXPECT_TRUE(asset == nullptr && rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "The procedural mesh test budget is consumed once and cannot leak to later loads");
    if (asset && rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(step_path);
    std::remove(knot_path);
    std::remove(control_path);
    std::remove(trim_path);
    std::remove(budget_path);
}

static void test_fbx_rejects_invalid_native_light_dimensions() {
    const char *path = "/tmp/zanna_fbx_invalid_area_light.fbx";
    EXPECT_TRUE(write_fbx_camera_light_animation_fixture(path, 0.0),
                "Malformed FBX area-light fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset == nullptr, "FBX rejects non-positive native area-light dimensions");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_CORRUPT,
                "Malformed native FBX light reports a corrupt-content error");
    EXPECT_TRUE(rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "rectangle width") != nullptr,
                "Malformed native FBX light diagnostic names the invalid dimension");
    std::remove(path);
}

/// @brief Write an `Objects` record holding far more children than the former 16384 cap.
/// @details Commercial mocap packages place one record per AnimationCurve/AnimationCurveNode
///          under a single `Objects` node; a 218-clip package was observed with 67,056 children.
///          The filler records use a structural name the extractor does not recognize, so the
///          fixture isolates the parser's per-record child capacity from object extraction.
/// @param path Destination fixture path.
/// @param filler_children Number of unrecognized filler records to emit before the mesh.
/// @return True when the binary fixture was written.
static bool write_fbx_wide_objects_record_fixture(const char *path, int filler_children) {
    static const int64_t kGeometryId = 7801;
    static const int64_t kModelId = 7802;
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    objects.name = "Objects";
    for (int i = 0; i < filler_children; i++) {
        FbxNodeFixture filler;
        filler.name = "ZannaTestFiller";
        filler.props.push_back(fbx_prop_i64_fixture((int64_t)i + 100000));
        objects.children.push_back(std::move(filler));
    }
    objects.children.push_back(make_fbx_triangle_geometry_fixture(kGeometryId, "WideMesh", 0.0));
    objects.children.push_back(
        make_fbx_model_fixture(kModelId, "WideMeshModel", "Mesh", 0.0, 0.0, 0.0));
    connections.name = "Connections";
    connections.children.push_back(make_fbx_connection_fixture(kModelId, 0));
    connections.children.push_back(make_fbx_connection_fixture(kGeometryId, kModelId));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief A single FBX record may hold far more children than the former fixed cap.
/// @details Regression for the 218-clip mocap package that failed to import: the parser capped
///          any record at 16384 children and reported the overflow as corrupt content.
static void test_fbx_objects_record_beyond_former_child_cap_imports() {
    const char *path = "/tmp/zanna_fbx_wide_objects_record.fbx";
    /* Comfortably past the former 16384 cap without making the fixture slow to write. */
    bool wrote_fixture = write_fbx_wide_objects_record_fixture(path, 40000);
    EXPECT_TRUE(wrote_fixture, "Wide-Objects-record FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr,
                "SceneAsset.Load parses an FBX record with more children than the former cap");
    if (!model) {
        std::remove(path);
        return;
    }
    EXPECT_TRUE(rt_model3d_get_mesh_count(model) == 1,
                "The mesh past 40000 sibling records is still extracted");
    std::remove(path);
}

/// @brief Write a well-formed document whose `Objects` record nests past the depth cap.
/// @details Depth is the cheapest structural cap to exercise; it shares the reporting path with
///          the child- and property-count caps, so it pins the diagnostic without materializing
///          a million records.
/// @param path Destination fixture path.
/// @param depth Nesting levels to emit below `Objects`.
/// @return True when the binary fixture was written.
static bool write_fbx_overdeep_fixture(const char *path, int depth) {
    FbxNodeFixture objects;
    FbxNodeFixture connections;
    FbxNodeFixture chain;
    objects.name = "Objects";
    connections.name = "Connections";
    chain.name = "ZannaTestNest";
    for (int i = 0; i < depth; i++) {
        FbxNodeFixture parent;
        parent.name = "ZannaTestNest";
        parent.children.push_back(std::move(chain));
        chain = std::move(parent);
    }
    objects.children.push_back(std::move(chain));
    return write_fbx_document_fixture(path, objects, connections);
}

/// @brief Exceeding a hard structural cap reports a size diagnostic, never "malformed".
/// @details The old code funneled every capacity failure into the corrupt-content branch, so a
///          structurally valid file that outgrew a parser bound was reported as truncated.
static void test_fbx_structural_limit_reports_size_error_not_corruption() {
    const char *path = "/tmp/zanna_fbx_structural_limit.fbx";
    /* Comfortably past FBX_MAX_NODE_DEPTH (512): valid FBX, beyond what the parser will nest. */
    bool wrote_fixture = write_fbx_overdeep_fixture(path, 600);
    EXPECT_TRUE(wrote_fixture, "Over-deep FBX fixture can be written");
    if (!wrote_fixture)
        return;

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model == nullptr, "FBX rejects a document that exceeds the hard nesting cap");
    EXPECT_TRUE(rt_asset_error_get_code() == RT_ASSET_ERROR_TOO_LARGE,
                "A structural cap overflow reports a size error, not corrupt content");
    EXPECT_TRUE(rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "structural limit") != nullptr,
                "The structural-cap diagnostic names the limit rather than claiming corruption");
    std::remove(path);
}

/// @brief Many-channel stacks resolve through the channel index without changing sampled values.
/// @details The `(layer_index, target_id)` bucket index replaced whole-array scans in the
///          duplicate check, channel lookup, and scalar composition. A wide graph must therefore
///          import identically to the narrow one it was grown from, and stay well inside the
///          near-linear probe envelope.
static void test_fbx_channel_index_matches_linear_scan_results() {
    const char *narrow_path = "/tmp/zanna_fbx_channel_index_narrow.fbx";
    const char *wide_path = "/tmp/zanna_fbx_channel_index_wide.fbx";
    EXPECT_TRUE(
        write_fbx_skinned_animation_fixture_ex(narrow_path, false, 0, 8, false, false, false),
        "Narrow channel-index fixture can be written");
    EXPECT_TRUE(
        write_fbx_skinned_animation_fixture_ex(wide_path, false, 0, 2048, false, false, false),
        "Wide channel-index fixture can be written");

    void *narrow = rt_model3d_load(rt_const_cstr(narrow_path));
    void *wide = rt_model3d_load(rt_const_cstr(wide_path));
    EXPECT_TRUE(narrow != nullptr && wide != nullptr,
                "Both channel-index fixtures import completely");
    if (!narrow || !wide) {
        std::remove(narrow_path);
        std::remove(wide_path);
        return;
    }

    auto *narrow_anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(narrow, 0));
    auto *wide_anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(wide, 0));
    EXPECT_TRUE(narrow_anim != nullptr && wide_anim != nullptr,
                "Both channel-index fixtures produce an animation");
    if (!narrow_anim || !wide_anim) {
        std::remove(narrow_path);
        std::remove(wide_path);
        return;
    }
    EXPECT_TRUE(narrow_anim->channel_count == wide_anim->channel_count,
                "Channel indexing does not change the imported channel count");

    /* Padding curve nodes must not perturb the animated root: same keys, same values. */
    const vgfx3d_anim_channel_t *narrow_root = nullptr;
    const vgfx3d_anim_channel_t *wide_root = nullptr;
    for (int32_t i = 0; i < narrow_anim->channel_count; i++)
        if (narrow_anim->channels[i].bone_index == 0)
            narrow_root = &narrow_anim->channels[i];
    for (int32_t i = 0; i < wide_anim->channel_count; i++)
        if (wide_anim->channels[i].bone_index == 0)
            wide_root = &wide_anim->channels[i];
    EXPECT_TRUE(narrow_root != nullptr && wide_root != nullptr,
                "Both channel-index fixtures animate the root bone");
    if (narrow_root && wide_root) {
        EXPECT_TRUE(narrow_root->keyframe_count == wide_root->keyframe_count,
                    "Channel indexing preserves the sampled keyframe count");
        int32_t shared = narrow_root->keyframe_count < wide_root->keyframe_count
                             ? narrow_root->keyframe_count
                             : wide_root->keyframe_count;
        for (int32_t i = 0; i < shared; i++) {
            EXPECT_NEAR(wide_root->keyframes[i].position[0],
                        narrow_root->keyframes[i].position[0],
                        1e-9,
                        "Indexed lookup samples the same root translation as the linear scan");
            EXPECT_NEAR(wide_root->keyframes[i].time,
                        narrow_root->keyframes[i].time,
                        1e-9,
                        "Indexed lookup samples at the same times as the linear scan");
        }
    }

    if (rt_obj_release_check0(narrow))
        rt_obj_free(narrow);
    if (rt_obj_release_check0(wide))
        rt_obj_free(wide);
    std::remove(narrow_path);
    std::remove(wide_path);
}

/// @brief The specialized translation/scale composes agree with the general 4x4 product.
/// @details The transform stack applies six translations and two scales per bone per sample
///          without a general multiply. This sweeps awkward accumulators — negative zeros,
///          denormals, clamp boundaries, sign mixes — through both paths and requires elementwise
///          equality, so the optimization cannot silently drift from the algebra it replaces.
static void test_fbx_specialized_matrix_composes_match_general_product() {
    /* Mirrors FBX_NUMERIC_ABS_MAX in rt_fbx_loader.c, which is loader-internal. */
    static const double kFbxNumericAbsMax = 1000000000000.0;
    static const double kValues[] = {0.0,
                                     -0.0,
                                     1.0,
                                     -1.0,
                                     0.5,
                                     -37.25,
                                     1e-300,
                                     -1e-300,
                                     1e11,
                                     -1e11,
                                     kFbxNumericAbsMax,
                                     -kFbxNumericAbsMax,
                                     kFbxNumericAbsMax * 2.0};
    const size_t value_count = sizeof(kValues) / sizeof(kValues[0]);
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    int translation_ok = 1;
    int scale_ok = 1;

    for (size_t trial = 0; trial < 4096u && (translation_ok || scale_ok); trial++) {
        double acc[16];
        for (int i = 0; i < 16; i++) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            switch ((rng >> 59) & 3u) {
                case 0:
                    acc[i] = kValues[(rng >> 13) % value_count];
                    break;
                case 1:
                    acc[i] = -0.0;
                    break;
                case 2:
                    acc[i] = (double)(int64_t)(rng >> 40) * 1e-6;
                    break;
                default:
                    acc[i] = (double)(int64_t)(rng >> 32) * 1.5;
                    break;
            }
        }
        double x = kValues[(rng >> 3) % value_count];
        double y = kValues[(rng >> 17) % value_count];
        double z = kValues[(rng >> 29) % value_count];
        if (!rt_fbx_test_append_matches_general_product(acc, x, y, z, 0))
            translation_ok = 0;
        if (!rt_fbx_test_append_matches_general_product(acc, x, y, z, 1))
            scale_ok = 0;
    }
    EXPECT_TRUE(translation_ok,
                "Specialized translation compose equals the general 4x4 product elementwise");
    EXPECT_TRUE(scale_ok, "Specialized scale compose equals the general 4x4 product elementwise");
}

/// @brief An unevaluated constraint must not change the imported animation at all.
/// @details "Preserved but not evaluated" constraints (unknown kinds such as a MotionBuilder
///          Character rig definition) previously forced the whole file onto the constraint-aware
///          bake path — a massive slowdown, and a divergence risk if the two paths ever sampled
///          differently. Extraction now routes on the evaluated-constraint count, so the two
///          fixtures below must import byte-for-byte identically.
static void test_fbx_unknown_constraint_matches_constraint_free_import() {
    const char *plain_path = "/tmp/zanna_fbx_no_constraint.fbx";
    const char *rigdef_path = "/tmp/zanna_fbx_unknown_constraint.fbx";
    EXPECT_TRUE(write_fbx_skinned_animation_fixture_ex(
                    plain_path, false, 0, 0, false, true, false, false, false, 0, false),
                "Constraint-free skinned fixture can be written");
    EXPECT_TRUE(write_fbx_skinned_animation_fixture_ex(
                    rigdef_path, false, 0, 0, false, true, false, false, false, 0, true),
                "Unknown-constraint skinned fixture can be written");

    void *plain = rt_model3d_load(rt_const_cstr(plain_path));
    void *rigdef = rt_model3d_load(rt_const_cstr(rigdef_path));
    EXPECT_TRUE(plain != nullptr && rigdef != nullptr,
                "Both unknown-constraint comparison fixtures import");
    if (plain && rigdef) {
        auto *plain_anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(plain, 0));
        auto *rigdef_anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(rigdef, 0));
        EXPECT_TRUE(plain_anim != nullptr && rigdef_anim != nullptr,
                    "Both comparison fixtures produce an animation clip");
        if (plain_anim && rigdef_anim) {
            EXPECT_TRUE(plain_anim->channel_count == rigdef_anim->channel_count,
                        "An unevaluated constraint does not change the imported channel count");
            int32_t shared_channels = plain_anim->channel_count < rigdef_anim->channel_count
                                          ? plain_anim->channel_count
                                          : rigdef_anim->channel_count;
            for (int32_t c = 0; c < shared_channels; c++) {
                const vgfx3d_anim_channel_t *a = &plain_anim->channels[c];
                const vgfx3d_anim_channel_t *b = &rigdef_anim->channels[c];
                EXPECT_TRUE(a->bone_index == b->bone_index &&
                                a->keyframe_count == b->keyframe_count,
                            "An unevaluated constraint preserves channel targets and key counts");
                int32_t shared_keys =
                    a->keyframe_count < b->keyframe_count ? a->keyframe_count : b->keyframe_count;
                for (int32_t k = 0; k < shared_keys; k++) {
                    EXPECT_TRUE(a->keyframes[k].time == b->keyframes[k].time,
                                "An unevaluated constraint preserves every key time");
                    for (int component = 0; component < 3; component++) {
                        EXPECT_TRUE(a->keyframes[k].position[component] ==
                                        b->keyframes[k].position[component],
                                    "An unevaluated constraint preserves sampled translations");
                        EXPECT_TRUE(a->keyframes[k].scale_xyz[component] ==
                                        b->keyframes[k].scale_xyz[component],
                                    "An unevaluated constraint preserves sampled scales");
                    }
                    for (int component = 0; component < 4; component++) {
                        EXPECT_TRUE(a->keyframes[k].rotation[component] ==
                                        b->keyframes[k].rotation[component],
                                    "An unevaluated constraint preserves sampled rotations");
                    }
                }
            }
        }
    }
    if (plain && rt_obj_release_check0(plain))
        rt_obj_free(plain);
    if (rigdef && rt_obj_release_check0(rigdef))
        rt_obj_free(rigdef);
    std::remove(plain_path);
    std::remove(rigdef_path);
}

/// @brief Clip-subset filtering keeps the selected clips and survives a save/reload roundtrip.
/// @details Backs `zanna asset bake --clips`: multi-clip packages exceed the VSCN document size
///          limit when baked whole, so per-clip bakes must preserve exactly the chosen clips.
static void test_model3d_keep_animation_subset_filters_and_roundtrips() {
    const char *source_path = "/tmp/zanna_fbx_two_stack_subset.fbx";
    const char *baked_path = "/tmp/zanna_fbx_two_stack_subset.scene3d";
    EXPECT_TRUE(write_fbx_skinned_animation_fixture_ex(
                    source_path, false, 0, 0, false, true, false, false, false, 0, false, true),
                "Two-stack subset fixture can be written");

    void *model = rt_model3d_load(rt_const_cstr(source_path));
    EXPECT_TRUE(model != nullptr, "Two-stack subset fixture imports");
    if (!model) {
        std::remove(source_path);
        return;
    }
    EXPECT_TRUE(rt_model3d_get_animation_count(model) == 2,
                "Two independent stacks import as two skeletal clips");

    /* Invalid selections leave the model untouched. */
    const int64_t descending[2] = {1, 0};
    EXPECT_TRUE(rt_model3d_keep_animation_subset(model, descending, 2, nullptr, 0) == 0,
                "A non-ascending kept-index list is rejected");
    const int64_t out_of_range[1] = {2};
    EXPECT_TRUE(rt_model3d_keep_animation_subset(model, out_of_range, 1, nullptr, 0) == 0,
                "An out-of-range kept index is rejected");
    EXPECT_TRUE(rt_model3d_get_animation_count(model) == 2,
                "Rejected subset selections change nothing");

    const int64_t keep_run[1] = {1};
    EXPECT_TRUE(rt_model3d_keep_animation_subset(model, keep_run, 1, nullptr, 0) == 1,
                "Keeping the second clip succeeds");
    EXPECT_TRUE(rt_model3d_get_animation_count(model) == 1,
                "Exactly one clip remains after subsetting");
    {
        void *anim = rt_model3d_get_animation(model, 0);
        rt_string name = anim ? rt_animation3d_get_name(anim) : nullptr;
        const char *text = name ? rt_string_cstr(name) : nullptr;
        EXPECT_TRUE(text && std::strcmp(text, "Run") == 0,
                    "The kept clip is the selected 'Run' stack");
        rt_string_unref(name);
    }

    EXPECT_TRUE(rt_model3d_save(model, rt_const_cstr(baked_path)) == 1,
                "A clip-subset model saves");
    void *baked = rt_model3d_load(rt_const_cstr(baked_path));
    EXPECT_TRUE(baked != nullptr, "A clip-subset bake reloads");
    if (baked) {
        EXPECT_TRUE(rt_model3d_get_animation_count(baked) == 1,
                    "The reloaded bake holds exactly the kept clip");
        auto *anim = static_cast<rt_animation3d *>(rt_model3d_get_animation(baked, 0));
        bool found_run_key = false;
        for (int32_t c = 0; anim && c < anim->channel_count && !found_run_key; c++) {
            for (int32_t k = 0; k < anim->channels[c].keyframe_count; k++) {
                if (std::fabs(anim->channels[c].keyframes[k].position[0] - 20.0) < 1e-4) {
                    found_run_key = true;
                    break;
                }
            }
        }
        EXPECT_TRUE(found_run_key, "The reloaded clip carries the 'Run' stack's keyframe values");
        if (rt_obj_release_check0(baked))
            rt_obj_free(baked);
    }
    if (rt_obj_release_check0(model))
        rt_obj_free(model);
    std::remove(source_path);
    std::remove(baked_path);
}

/// @brief A failed VSCN save reports a diagnostic instead of failing silently.
/// @details The save path historically returned zero from every failure with no recorded
///          message, so `zanna asset bake` printed `stage:"save"` and nothing else.
static void test_model3d_save_reports_diagnostics_on_failure() {
    const char *source_path = "/tmp/zanna_fbx_save_diagnostic.fbx";
    EXPECT_TRUE(
        write_fbx_skinned_animation_fixture_ex(source_path, false, 0, 0, false, true, false),
        "Save-diagnostic fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(source_path));
    EXPECT_TRUE(model != nullptr, "Save-diagnostic fixture imports");
    if (!model) {
        std::remove(source_path);
        return;
    }
    int64_t saved =
        rt_model3d_save(model, rt_const_cstr("/tmp/zanna_no_such_dir_for_vscn_save/out.scene3d"));
    EXPECT_TRUE(saved == 0, "Saving into a missing directory fails");
    EXPECT_TRUE(rt_asset_error_get_message() &&
                    std::strstr(rt_asset_error_get_message(), "VSCN.Save") != nullptr,
                "A failed VSCN save records a VSCN.Save diagnostic");
    if (rt_obj_release_check0(model))
        rt_obj_free(model);
    std::remove(source_path);
}

/// @brief Constraint OP links to bones must not displace their hierarchy parents.
/// @details Biped exports list a Character constraint's `*Link` property connections before
///          each core bone's real parent link; parent resolution previously took the first
///          connection blindly, silently flattening the imported skeleton (and the
///          node-animation, pose, and scene hierarchies) for every such file.
static void test_fbx_constraint_property_links_do_not_flatten_the_skeleton() {
    const char *path = "/tmp/zanna_fbx_constraint_link_hierarchy.fbx";
    EXPECT_TRUE(write_fbx_skinned_animation_fixture_ex(
                    path, false, 0, 0, false, true, false, false, false, 0, true, false, true),
                "Constraint-link hierarchy fixture can be written");
    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "Constraint-link hierarchy fixture imports");
    if (!model) {
        std::remove(path);
        return;
    }
    void *skel = rt_model3d_get_skeleton(model, 0);
    EXPECT_TRUE(skel != nullptr, "Constraint-link fixture yields a skeleton");
    if (skel) {
        int64_t root_index = -1;
        int64_t child_index = -1;
        int64_t bone_count = rt_skeleton3d_get_bone_count(skel);
        for (int64_t i = 0; i < bone_count; i++) {
            rt_string name = rt_skeleton3d_get_bone_name(skel, i);
            const char *text = name ? rt_string_cstr(name) : nullptr;
            if (text && std::strcmp(text, "RootBone") == 0)
                root_index = i;
            if (text && std::strcmp(text, "ChildBone") == 0)
                child_index = i;
            rt_string_unref(name);
        }
        EXPECT_TRUE(root_index >= 0 && child_index >= 0,
                    "Both fixture bones import despite the constraint links");
        if (root_index >= 0 && child_index >= 0) {
            EXPECT_TRUE(rt_skeleton3d_get_bone_parent(skel, root_index) == -1,
                        "The root bone stays a root");
            EXPECT_TRUE(rt_skeleton3d_get_bone_parent(skel, child_index) == root_index,
                        "The child bone keeps its hierarchy parent, not the constraint");
        }
    }
    if (rt_obj_release_check0(model))
        rt_obj_free(model);
    std::remove(path);
}

static void test_fbx_imports_progressive_morph_fidelity_and_animation() {
    const char *path = "/tmp/zanna_fbx_progressive_morph.fbx";
    EXPECT_TRUE(write_fbx_progressive_morph_fixture(path),
                "FBX progressive morph fixture can be written");
    void *asset = rt_fbx_load(rt_const_cstr(path));
    EXPECT_TRUE(asset != nullptr, "FBX.Load imports progressive BlendShapeChannels");
    if (!asset) {
        std::remove(path);
        return;
    }
    void *morph = rt_fbx_get_morph_target(asset, 0);
    EXPECT_TRUE(morph != nullptr && rt_morphtarget3d_get_shape_count(morph) == 2,
                "FBX imports every progressive morph shape");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph, 0),
                0.5,
                0.001,
                "FBX DeformPercent initializes the first progressive shape");
    EXPECT_NEAR(rt_morphtarget3d_get_weight(morph, 1),
                0.0,
                0.001,
                "FBX DeformPercent leaves later progressive shapes inactive below threshold");
    rt_morphtarget3d_shape_view_internal shape_view{};
    EXPECT_TRUE(rt_morphtarget3d_get_shape_view_internal(morph, 0, &shape_view) == 1,
                "FBX imported morph shape exposes complete delta payloads");
    if (shape_view.position_deltas) {
        EXPECT_NEAR(shape_view.position_deltas[0],
                    1.0,
                    0.001,
                    "FBX float position deltas retain their value");
        EXPECT_TRUE(shape_view.normal_deltas != nullptr && shape_view.tangent_deltas != nullptr,
                    "FBX imports optional normal and tangent shape deltas");
    }
    EXPECT_TRUE(rt_fbx_node_animation_count(asset) == 1,
                "FBX DeformPercent curves become node animation clips");
    auto *clip = static_cast<rt_node_animation3d *>(rt_fbx_get_node_animation(asset, 0));
    EXPECT_TRUE(clip != nullptr && scene3d_node_animation_channel_count(clip) == 1,
                "FBX morph-only stack emits one combined weight-vector channel");
    if (clip && scene3d_node_animation_channel_count(clip) == 1) {
        const rt_node_anim_channel3d &channel = clip->channels[0];
        EXPECT_TRUE(channel.path == RT_NODE_ANIM_PATH_WEIGHTS && channel.value_width == 2,
                    "FBX morph animation targets the full mesh-local shape vector");
        EXPECT_TRUE(channel.target_node_index == 3,
                    "FBX morph channel binds to the owning Model's Objects ordinal");
        EXPECT_NEAR(channel.values[0],
                    0.5,
                    0.001,
                    "FBX morph animation evaluates the first FullWeights threshold");
        EXPECT_TRUE(channel.key_count == 5,
                    "FBX morph animation inserts every progressive threshold crossing");
        EXPECT_NEAR(channel.values[4],
                    0.5,
                    0.001,
                    "FBX progressive cross-fade retains the preceding shape");
        EXPECT_NEAR(channel.values[5],
                    0.5,
                    0.001,
                    "FBX progressive cross-fade activates the following shape");
        EXPECT_NEAR(channel.values[8],
                    0.0,
                    0.001,
                    "FBX progressive morph clears preceding shapes beyond the final threshold");
        EXPECT_NEAR(channel.values[9],
                    1.0,
                    0.001,
                    "FBX progressive morph holds the final target beyond its threshold");
    }

    void *model = rt_model3d_load(rt_const_cstr(path));
    EXPECT_TRUE(model != nullptr, "SceneAsset adapts animated FBX morph targets");
    if (model) {
        void *template_node = rt_model3d_find_node(model, rt_const_cstr("MorphModel"));
        EXPECT_TRUE(template_node != nullptr && rt_scene_node3d_child_count(template_node) == 2,
                    "FBX multi-material morph mesh remains split into two render nodes");
        if (template_node) {
            for (int64_t child_index = 0; child_index < rt_scene_node3d_child_count(template_node);
                 ++child_index) {
                auto *mesh = static_cast<rt_mesh3d *>(rt_scene_node3d_get_mesh(
                    rt_scene_node3d_get_child(template_node, child_index)));
                EXPECT_TRUE(mesh != nullptr && mesh->morph_targets_ref != nullptr,
                            "FBX material-split meshes preserve remapped morph payloads");
            }
        }
        void *scene = rt_model3d_instantiate_scene(model);
        EXPECT_TRUE(scene != nullptr, "Animated FBX morph SceneAsset instantiates");
        if (scene) {
            rt_scene3d_sync_bindings(scene, 0.75);
            auto *instance_node =
                static_cast<rt_scene_node3d *>(rt_scene3d_find(scene, rt_const_cstr("MorphModel")));
            auto *child = instance_node && scene3d_node_child_count(instance_node) > 0
                              ? instance_node->children[0]
                              : nullptr;
            auto *mesh = child ? static_cast<rt_mesh3d *>(child->mesh) : nullptr;
            EXPECT_TRUE(mesh != nullptr && mesh->morph_targets_ref != nullptr,
                        "FBX instantiated split mesh retains instance-local morph state");
            if (mesh && mesh->morph_targets_ref) {
                EXPECT_NEAR(rt_morphtarget3d_get_weight(mesh->morph_targets_ref, 0),
                            0.75,
                            0.001,
                            "FBX progressive animation cross-fades the lower shape");
                EXPECT_NEAR(rt_morphtarget3d_get_weight(mesh->morph_targets_ref, 1),
                            0.25,
                            0.001,
                            "FBX progressive animation cross-fades the upper shape");
            }
            if (rt_obj_release_check0(scene))
                rt_obj_free(scene);
        }
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
    }
    if (rt_obj_release_check0(asset))
        rt_obj_free(asset);
    std::remove(path);

    const char *layered_path = "/tmp/zanna_fbx_progressive_morph_layers.fbx";
    EXPECT_TRUE(write_fbx_progressive_morph_fixture(layered_path, true),
                "Layer-composed progressive morph fixture can be written");
    asset = rt_fbx_load(rt_const_cstr(layered_path));
    EXPECT_TRUE(asset != nullptr,
                "FBX composes progressive morph percentages across every active layer");
    if (asset) {
        clip = static_cast<rt_node_animation3d *>(rt_fbx_get_node_animation(asset, 0));
        const rt_node_anim_channel3d *channel =
            clip && scene3d_node_animation_channel_count(clip) == 1 ? &clip->channels[0] : nullptr;
        EXPECT_TRUE(channel && channel->key_count == 5,
                    "Composed morph layers insert their own threshold-crossing samples");
        if (channel && channel->key_count == 5) {
            EXPECT_NEAR(channel->values[0],
                        0.75,
                        0.001,
                        "Additive layer weight contributes to the initial progressive shape");
            EXPECT_NEAR(channel->times[1],
                        0.25,
                        0.000001,
                        "First FullWeights crossing is solved from the composed layer value");
            EXPECT_NEAR(channel->values[2],
                        1.0,
                        0.001,
                        "Composed first threshold reaches the lower shape exactly");
            EXPECT_NEAR(channel->values[3],
                        0.0,
                        0.001,
                        "Composed first threshold has not activated the upper shape");
            EXPECT_NEAR(channel->times[3],
                        1.25,
                        0.000001,
                        "Second FullWeights crossing is solved from the composed layer value");
            EXPECT_NEAR(channel->values[6],
                        0.0,
                        0.001,
                        "Composed final threshold clears the preceding shape exactly");
            EXPECT_NEAR(channel->values[7],
                        1.0,
                        0.001,
                        "Composed final threshold activates the final shape exactly");
        }
        if (rt_obj_release_check0(asset))
            rt_obj_free(asset);
    }
    std::remove(layered_path);
}

static void test_bc6h_all_modes_are_finite_and_deterministic() {
    static const uint8_t modes[] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15};
    for (int signed_mode = 0; signed_mode <= 1; signed_mode++) {
        for (uint8_t mode : modes) {
            uint8_t block[16];
            uint16_t first[16][3];
            uint16_t second[16][3];
            for (size_t i = 0; i < sizeof(block); i++)
                block[i] = (uint8_t)(0x5Au + (uint8_t)(i * 37u) + mode * 11u);
            if (mode < 2)
                block[0] = (uint8_t)((block[0] & ~UINT8_C(3)) | mode);
            else
                block[0] = (uint8_t)((block[0] & ~UINT8_C(31)) | mode);

            bc6h_decode_block(block, signed_mode, first);
            bc6h_decode_block(block, signed_mode, second);
            EXPECT_TRUE(std::memcmp(first, second, sizeof(first)) == 0,
                        "BC6H mode decoding is deterministic");
            bool finite = true;
            for (int texel = 0; texel < 16; texel++) {
                for (int channel = 0; channel < 3; channel++) {
                    if ((first[texel][channel] & UINT16_C(0x7C00)) == UINT16_C(0x7C00))
                        finite = false;
                }
            }
            EXPECT_TRUE(finite, "BC6H mode decoding never synthesizes NaN or infinity");
        }
    }

    uint8_t reserved[16] = {19};
    uint16_t decoded[16][3];
    bc6h_decode_block(reserved, 0, decoded);
    uint16_t zero[16][3] = {};
    EXPECT_TRUE(std::memcmp(decoded, zero, sizeof(decoded)) == 0,
                "BC6H reserved modes fail closed to black");
}

int main() {
    test_bc6h_all_modes_are_finite_and_deterministic();
    test_model3d_rejects_wrong_handle_types();
    test_model3d_binds_first_valid_default_skeletal_animator_for_multiple_skeletons();
    test_model3d_clamps_corrupt_counts_and_child_walks();
    test_model3d_roundtrips_vscn_assets();
    test_model3d_accepts_canonical_scene3d_extension();
    test_model3d_load_text_result_loads_without_disk_document();
    test_model3d_load_text_result_resolves_prefabs_beside_virtual_path();
    test_model3d_find_node_rejects_wrong_string_handles();
    test_model3d_adapts_gltf_scene_graphs();
    test_model3d_rejects_gltf_accessor_overrun_of_buffer_view();
    test_gltf_asset_accessors_clamp_corrupt_counts();
    test_model3d_load_asset_resolves_mounted_gltf_dependencies();
    test_model3d_load_asset_diagnostics_name_missing_dependency();
    test_model3d_load_asset_resolves_vscn_obj_and_stl_packages();
    test_model3d_adapts_fbx_scene_graphs();
    test_model3d_imports_fbx_nodes_with_many_properties();
    test_model3d_loads_preloaded_fbx_bytes();
    test_model3d_loads_obj_as_template_asset();
    test_model3d_preserves_obj_mtl_material_groups();
    test_model3d_imports_obj_mtl_texture_maps();
    test_model3d_imports_quoted_obj_mtl_references();
    test_model3d_sanitizes_obj_mtl_values_and_rejects_uri_maps();
    test_model3d_preserves_empty_gltf_scene_without_synth_nodes();
    test_model3d_loads_stl_as_template_asset();
    test_model3d_loads_minimal_ascii_fbx();
    test_model3d_splits_fbx_layer_element_materials();
    test_model3d_triangulates_large_fbx_ngons();
    test_model3d_imports_fbx_embedded_textures();
    test_model3d_imports_fbx_texture_aliases_and_absolute_basename_fallback();
    test_model3d_imports_fbx_material_scalar_aliases_and_allsame_uvs();
    test_model3d_imports_fbx_secondary_uvs_and_vertex_colors();
    test_fbx_cluster_transform_link_drives_bind_pose();
    test_fbx_procedural_surface_interpolates_skin_weights();
    test_fbx_procedural_surface_interpolates_morph_deltas();
    test_fbx_imports_skeletons_beyond_draw_palette_limit();
    test_fbx_constant_animation_curve_preserves_step();
    test_model3d_loads_ascii_fbx_attributes_and_materials();
    test_fbx_ascii_typed_graph_matches_binary_multi_object_graph();
    test_fbx_ascii_parser_ignores_decoy_identifiers_and_scopes();
    test_fbx_aggregate_budget_rejects_many_valid_compressed_arrays();
    test_fbx_animation_uses_static_transform_stack_composer();
    test_fbx_cubic_animation_adaptive_sampling_preserves_shape();
    test_fbx_large_animation_graph_lookup_probes_are_near_linear();
    test_fbx_long_colliding_names_remain_distinct_and_id_bound();
    test_model3d_honors_fbx_rotation_order();
    test_model3d_imports_fbx_skinning_and_grouped_animation();
    test_fbx_duplicate_animation_curves_keep_first_component();
    test_fbx_mismatched_animation_curve_key_arrays_are_ignored();
    test_fbx_bare_animation_curve_component_names_import();
    test_fbx_lowercase_animation_curve_component_names_import();
    test_fbx_negative_animation_key_times_normalize_to_clip_start();
    test_fbx_animation_layers_beyond_fixed_cap_import();
    test_fbx_animation_curve_nodes_beyond_fixed_cap_import();
    test_fbx_duplicate_bone_names_resolve_by_model_id();
    test_fbx_asset_accessors_clamp_corrupt_counts();
    test_fbx_imports_cameras_lights_and_object_animation();
    test_fbx_animates_complete_camera_projection_state();
    test_fbx_composes_animation_layers_for_nodes_and_bones();
    test_fbx_imports_general_constraints_and_animated_dependencies();
    test_fbx_constraint_validation_and_bounded_ik_failure();
    test_fbx_constraints_drive_static_and_animated_skeleton_poses();
    test_fbx_tessellates_binary_and_ascii_procedural_surfaces();
    test_fbx_tessellates_surface_forms_and_every_patch_basis();
    test_fbx_rejects_malformed_procedural_surfaces_transactionally();
    test_fbx_rejects_invalid_native_light_dimensions();
    test_fbx_objects_record_beyond_former_child_cap_imports();
    test_fbx_structural_limit_reports_size_error_not_corruption();
    test_fbx_channel_index_matches_linear_scan_results();
    test_fbx_specialized_matrix_composes_match_general_product();
    test_fbx_unknown_constraint_matches_constraint_free_import();
    test_model3d_keep_animation_subset_filters_and_roundtrips();
    test_model3d_save_reports_diagnostics_on_failure();
    test_fbx_constraint_property_links_do_not_flatten_the_skeleton();
    test_fbx_imports_progressive_morph_fidelity_and_animation();
    test_model3d_rejects_truncated_fbx();
    test_model3d_missing_fbx_returns_null_without_trap();
    test_model3d_loads_demo_fbx_textures();
    test_gltf_rejects_unknown_animation_interpolation();
    test_gltf_rejects_oversized_node_animation_key_count_before_scan();
    test_gltf_rejects_oversized_morph_weight_animation_width();
    test_model3d_loads_gltfpack_meshopt_fixture();
    test_model3d_loads_gltfpack_quantized_fixtures();
    test_textureasset3d_decodes_basislz_etc1s_ktx2();
    test_model3d_loads_draco_sequential_fixture();
    test_model3d_loads_draco_edgebreaker_spheres();
    test_model3d_vscn_v3_rig_roundtrip();
    test_scene3d_vscn_v5_source_texture_roundtrip();
    test_model3d_vscn_v5_full_scene_asset_roundtrip();
    test_model3d_vscn_v5_corruption_rolls_back();
    test_model3d_draco_corrupt_payloads_fail_cleanly();
    test_model3d_generate_lods_builds_chains();
    test_model3d_applies_material_variants();
    test_model3d_autoplays_gltf_node_and_morph_animation();
    test_gltf_short_node_weights_clear_morph_tail();
    std::printf("SceneAsset tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
