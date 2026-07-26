//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/tests/runtime/RTSceneEditorTests.cpp
// Purpose: Tests for scene-owned editor primitives, complete Tiled JSON/TMX
//   import, projected-map retention, and scaled tile hit testing.
//
// Key invariants:
//   - Canonical SceneDocument editing and compatibility loads remain stable.
//   - Bounded flood fill is four-connected, exact-counted, and no-op safe.
//   - Tiled imports preserve chunks, projections, GID transforms, mixed
//     tilesets, objects, properties, collision, animation, and render state.
//
// Ownership/Lifetime:
//   - Test-owned runtime handles live for the process-length test invocation.
//   - Temporary external-scene fixtures are removed before the test exits.
//
// Links: src/runtime/game/rt_scene_editor.cpp,
//   src/runtime/game/rt_tiled_import.cpp, docs/adr/0140-tiled-map-and-scene-import.md,
//   docs/adr/0144-complete-tiled-map-import.md,
//   docs/adr/0174-scene-object-authoring-metadata-and-duplication.md,
//   docs/adr/0158-scene-level-property-authoring.md,
//   docs/adr/0164-backward-compatible-2d-scene-object-hierarchy.md,
//   docs/adr/0171-bounded-scene-flood-fill-and-studio-tile-tools.md,
//   docs/adr/0176-typed-tile-behavior-sections.md,
//   docs/adr/0177-scene-camera-and-lighting-sections.md
//
//===----------------------------------------------------------------------===//

// This boundary suite intentionally uses assert as its executable check
// primitive, including around Result unwrapping and filesystem operations.
// Keep those checks active in Release installer validation.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "rt_asset.h"
#include "rt_crc32.h"
#include "rt_graphics.h"
#include "rt_graphics2d.h"
#include "rt_pixels.h"
#include "rt_scene_editor.h"
#include "rt_tilemap.h"

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ZpakWriter.hpp"

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static std::string to_std(rt_string value) {
    std::string out(rt_string_cstr(value), (size_t)rt_str_len(value));
    rt_string_unref(value);
    return out;
}

static std::string read_text(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    assert(in && "fixture should exist");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void write_text(const std::filesystem::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out && "fixture output should open");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(out.good() && "fixture output should be complete");
}

static std::vector<uint8_t> read_bytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    assert(in && "binary fixture should exist");
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

static std::vector<uint8_t> tiled_gid_bytes(const std::vector<uint32_t> &gids) {
    std::vector<uint8_t> out;
    out.reserve(gids.size() * 4);
    for (uint32_t gid : gids) {
        out.push_back(static_cast<uint8_t>(gid));
        out.push_back(static_cast<uint8_t>(gid >> 8));
        out.push_back(static_cast<uint8_t>(gid >> 16));
        out.push_back(static_cast<uint8_t>(gid >> 24));
    }
    return out;
}

static uint32_t adler32(const std::vector<uint8_t> &bytes) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : bytes) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static std::vector<uint8_t> stored_deflate(const std::vector<uint8_t> &bytes) {
    assert(bytes.size() <= 65535u);
    uint16_t length = static_cast<uint16_t>(bytes.size());
    uint16_t inverse = static_cast<uint16_t>(~length);
    std::vector<uint8_t> out = {0x01,
                                static_cast<uint8_t>(length),
                                static_cast<uint8_t>(length >> 8),
                                static_cast<uint8_t>(inverse),
                                static_cast<uint8_t>(inverse >> 8)};
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

static std::vector<uint8_t> zlib_stored(const std::vector<uint8_t> &bytes) {
    std::vector<uint8_t> out = {0x78, 0x01};
    std::vector<uint8_t> deflate = stored_deflate(bytes);
    out.insert(out.end(), deflate.begin(), deflate.end());
    uint32_t checksum = adler32(bytes);
    out.push_back(static_cast<uint8_t>(checksum >> 24));
    out.push_back(static_cast<uint8_t>(checksum >> 16));
    out.push_back(static_cast<uint8_t>(checksum >> 8));
    out.push_back(static_cast<uint8_t>(checksum));
    return out;
}

static std::vector<uint8_t> gzip_stored(const std::vector<uint8_t> &bytes) {
    std::vector<uint8_t> out = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff};
    std::vector<uint8_t> deflate = stored_deflate(bytes);
    out.insert(out.end(), deflate.begin(), deflate.end());
    uint32_t checksum = rt_crc32_compute(bytes.data(), bytes.size());
    uint32_t length = static_cast<uint32_t>(bytes.size());
    for (int shift = 0; shift <= 24; shift += 8)
        out.push_back(static_cast<uint8_t>(checksum >> shift));
    for (int shift = 0; shift <= 24; shift += 8)
        out.push_back(static_cast<uint8_t>(length >> shift));
    return out;
}

static std::vector<uint8_t> zstd_raw_frame(const std::vector<uint8_t> &bytes) {
    assert(bytes.size() <= 255u);
    std::vector<uint8_t> out = {0x28, 0xb5, 0x2f, 0xfd, 0x20, static_cast<uint8_t>(bytes.size())};
    uint32_t blockHeader = (static_cast<uint32_t>(bytes.size()) << 3u) | 1u;
    out.push_back(static_cast<uint8_t>(blockHeader));
    out.push_back(static_cast<uint8_t>(blockHeader >> 8));
    out.push_back(static_cast<uint8_t>(blockHeader >> 16));
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

static std::string base64(const std::vector<uint8_t> &bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t index = 0; index < bytes.size(); index += 3u) {
        uint32_t value = static_cast<uint32_t>(bytes[index]) << 16u;
        bool hasSecond = index + 1u < bytes.size();
        bool hasThird = index + 2u < bytes.size();
        if (hasSecond)
            value |= static_cast<uint32_t>(bytes[index + 1u]) << 8u;
        if (hasThird)
            value |= bytes[index + 2u];
        out.push_back(alphabet[(value >> 18u) & 63u]);
        out.push_back(alphabet[(value >> 12u) & 63u]);
        out.push_back(hasSecond ? alphabet[(value >> 6u) & 63u] : '=');
        out.push_back(hasThird ? alphabet[value & 63u] : '=');
    }
    return out;
}

static void *load_text(const std::string &text) {
    rt_string source = rt_string_from_bytes(text.data(), text.size());
    void *scene = rt_game_scene_load_json(source);
    rt_string_unref(source);
    return scene;
}

static void *load_fixture(const std::filesystem::path &fixture_dir, const char *name) {
    return load_text(read_text(fixture_dir / name));
}

static std::string scene_json(void *scene) {
    return to_std(rt_game_scene_to_json(scene));
}

static std::string map_str(void *map, const char *key) {
    rt_string key_s = rt_const_cstr(key);
    rt_string value = rt_map_get_str(map, key_s);
    rt_string_unref(key_s);
    return to_std(value);
}

static int64_t map_int(void *map, const char *key) {
    rt_string key_s = rt_const_cstr(key);
    int64_t value = rt_map_get_int(map, key_s);
    rt_string_unref(key_s);
    return value;
}

static std::string diagnostic_code(void *scene, int64_t index) {
    void *records = rt_game_scene_diagnostic_records(scene);
    assert(rt_seq_len(records) > index);
    return map_str(rt_seq_get(records, index), "code");
}

static std::string diagnostic_severity(void *scene, int64_t index) {
    void *records = rt_game_scene_diagnostic_records(scene);
    assert(rt_seq_len(records) > index);
    return map_str(rt_seq_get(records, index), "severity");
}

static std::string last_error(void *scene) {
    return to_std(rt_game_scene_last_error(scene));
}

static bool has_descriptor(void *descriptors,
                           const char *path,
                           const char *kind,
                           const char *owner,
                           const char *section,
                           const char *key) {
    int64_t count = rt_seq_len(descriptors);
    for (int64_t i = 0; i < count; ++i) {
        void *desc = rt_seq_get(descriptors, i);
        if (map_str(desc, "path") == path && map_str(desc, "kind") == kind &&
            map_str(desc, "owner") == owner && map_str(desc, "section") == section &&
            map_str(desc, "key") == key)
            return true;
    }
    return false;
}

int main() {
    void *scene = rt_game_scene_new(4, 3, 16, 16);
    assert(rt_game_scene_get_width(scene) == 4);
    assert(rt_game_scene_layer_count(scene) == 1);

    int64_t fg = rt_game_scene_add_layer(scene, rt_const_cstr("foreground"));
    rt_game_scene_set_layer_asset(scene, fg, rt_const_cstr("tiles/terrain.png"));
    rt_game_scene_fill_tiles(scene, fg, 1, 1, 2, 1, 7);
    assert(rt_game_scene_get_tile(scene, fg, 1, 1) == 7);
    assert(rt_game_scene_get_tile(scene, fg, 2, 1) == 7);
    rt_game_scene_set_layer_visible(scene, fg, 0);
    assert(rt_game_scene_layer_visible(scene, fg) == 0);

    // ADR 0171: Flood fill replaces only the four-connected source region,
    // reports the exact count, and leaves invalid or equal requests untouched.
    {
        void *fill_scene = rt_game_scene_new(5, 4, 16, 16);
        rt_game_scene_fill_tiles(fill_scene, 0, 0, 0, 5, 4, 1);
        rt_game_scene_fill_tiles(fill_scene, 0, 2, 0, 1, 4, 2);
        rt_game_scene_set_tile(fill_scene, 0, 3, 2, 2);
        assert(rt_game_scene_flood_fill_tiles(fill_scene, 0, 0, 0, 9) == 8);
        assert(rt_game_scene_get_tile(fill_scene, 0, 1, 3) == 9);
        assert(rt_game_scene_get_tile(fill_scene, 0, 2, 3) == 2);
        assert(rt_game_scene_get_tile(fill_scene, 0, 3, 3) == 1);
        assert(rt_game_scene_flood_fill_tiles(fill_scene, 0, 0, 0, 9) == 0);
        assert(rt_game_scene_flood_fill_tiles(fill_scene, -1, 0, 0, 3) == 0);
        assert(rt_game_scene_flood_fill_tiles(fill_scene, 0, -1, 0, 3) == 0);

        void *diagonal = rt_game_scene_new(3, 3, 16, 16);
        rt_game_scene_set_tile(diagonal, 0, 0, 0, 4);
        rt_game_scene_set_tile(diagonal, 0, 1, 1, 4);
        assert(rt_game_scene_flood_fill_tiles(diagonal, 0, 0, 0, 6) == 1);
        assert(rt_game_scene_get_tile(diagonal, 0, 0, 0) == 6);
        assert(rt_game_scene_get_tile(diagonal, 0, 1, 1) == 4);

        void *large = rt_game_scene_new(1024, 1024, 16, 16);
        assert(rt_game_scene_flood_fill_tiles(large, 0, 0, 0, 5) == 1024 * 1024);
        assert(rt_game_scene_get_tile(large, 0, 1023, 1023) == 5);
    }

    // ADR 0176: typed tile-behavior authoring, canonical serialization, legacy
    // section loads, retained unknown members, and BuildTilemap equivalence.
    {
        auto make_int_seq = [](std::initializer_list<int64_t> values) {
            void *seq = rt_seq_new_owned();
            for (int64_t value : values) {
                void *boxed = rt_box_i64(value);
                rt_seq_push(seq, boxed);
                if (boxed && rt_obj_release_check0(boxed))
                    rt_obj_free(boxed);
            }
            return seq;
        };

        void *typed = rt_game_scene_new(4, 4, 16, 16);
        rt_game_scene_set_tile_collision(typed, 7, 1);
        rt_game_scene_set_tile_collision(typed, 3, 2);
        rt_game_scene_set_collision_layer(typed, 0);
        assert(rt_game_scene_tile_collision(typed, 7) == 1);
        assert(rt_game_scene_tile_collision(typed, 3) == 2);
        assert(rt_game_scene_tile_collision(typed, 99) == 0);
        void *collision_tiles = rt_game_scene_collision_tiles(typed);
        assert(rt_seq_len(collision_tiles) == 2);
        assert(rt_unbox_i64(rt_seq_get(collision_tiles, 0)) == 3);
        assert(rt_unbox_i64(rt_seq_get(collision_tiles, 1)) == 7);

        rt_game_scene_set_tile_property_int(typed, 7, rt_const_cstr("damage"), 3);
        rt_game_scene_set_tile_property_bool(typed, 7, rt_const_cstr("slippery"), 1);
        assert(to_std(rt_game_scene_tile_property_kind(typed, 7, rt_const_cstr("damage"))) ==
               "int");
        assert(to_std(rt_game_scene_tile_property_kind(typed, 7, rt_const_cstr("slippery"))) ==
               "bool");
        assert(rt_game_scene_tile_property_int(typed, 7, rt_const_cstr("damage"), -1) == 3);
        assert(rt_game_scene_tile_property_bool(typed, 7, rt_const_cstr("slippery"), 0) == 1);
        assert(rt_game_scene_tile_property_int(typed, 7, rt_const_cstr("slippery"), -1) == -1);
        void *prop_tiles = rt_game_scene_tile_property_tiles(typed);
        assert(rt_seq_len(prop_tiles) == 1);
        void *prop_keys = rt_game_scene_tile_property_keys(typed, 7);
        assert(rt_seq_len(prop_keys) == 2);
        assert(to_std(rt_seq_get_str(prop_keys, 0)) == "damage");

        void *anim_frames = make_int_seq({5, 6, 7});
        void *anim_durations = make_int_seq({120, 90, 120});
        rt_game_scene_set_tile_anim(typed, 5, anim_frames, anim_durations);
        void *frames_out = rt_game_scene_tile_anim_frames(typed, 5);
        void *durations_out = rt_game_scene_tile_anim_durations(typed, 5);
        assert(rt_seq_len(frames_out) == 3);
        assert(rt_unbox_i64(rt_seq_get(frames_out, 2)) == 7);
        assert(rt_unbox_i64(rt_seq_get(durations_out, 1)) == 90);

        void *variants = make_int_seq({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        rt_game_scene_set_autotile_rule(typed, 9, variants);
        void *variants_out = rt_game_scene_autotile_variants(typed, 9);
        assert(rt_seq_len(variants_out) == 16);
        assert(rt_unbox_i64(rt_seq_get(variants_out, 15)) == 16);

        // Setter rejections are warning no-ops (ADR 0176).
        int64_t before = rt_seq_len(rt_game_scene_diagnostic_records(typed));
        rt_game_scene_set_tile_collision(typed, 0, 1);
        rt_game_scene_set_tile_collision(typed, 4096, 1);
        rt_game_scene_set_tile_collision(typed, 7, 3);
        rt_game_scene_set_collision_layer(typed, 16);
        void *short_variants = make_int_seq({1, 2, 3});
        rt_game_scene_set_autotile_rule(typed, 9, short_variants);
        void *bad_durations = make_int_seq({120, 0, 120});
        rt_game_scene_set_tile_anim(typed, 5, anim_frames, bad_durations);
        void *mismatched = make_int_seq({120});
        rt_game_scene_set_tile_anim(typed, 5, anim_frames, mismatched);
        assert(rt_seq_len(rt_game_scene_diagnostic_records(typed)) == before + 7);
        assert(rt_game_scene_tile_collision(typed, 7) == 1);
        assert(rt_seq_len(rt_game_scene_autotile_variants(typed, 9)) == 16);
        assert(rt_unbox_i64(rt_seq_get(rt_game_scene_tile_anim_durations(typed, 5), 1)) == 90);

        // Canonical serialization and reload.
        std::string typed_json = scene_json(typed);
        assert(typed_json.find("\"collision\"") != std::string::npos);
        assert(typed_json.find("\"solid\":[7]") != std::string::npos);
        assert(typed_json.find("\"oneWayUp\":[3]") != std::string::npos);
        assert(typed_json.find("\"layer\":0") != std::string::npos);
        assert(typed_json.find("\"durations\":[120,90,120]") != std::string::npos);
        assert(typed_json.find("msPerFrame") == std::string::npos);
        void *reloaded = load_text(typed_json);
        assert(rt_game_scene_tile_collision(reloaded, 3) == 2);
        assert(rt_game_scene_tile_property_int(reloaded, 7, rt_const_cstr("damage"), -1) == 3);
        assert(rt_game_scene_tile_property_bool(reloaded, 7, rt_const_cstr("slippery"), 0) == 1);
        assert(rt_seq_len(rt_game_scene_tile_anim_frames(reloaded, 5)) == 3);
        assert(rt_seq_len(rt_game_scene_autotile_variants(reloaded, 9)) == 16);
        // Reload → save is byte-stable.
        assert(scene_json(reloaded) == typed_json);

        // BuildTilemap applies typed state exactly as the legacy JSON path did.
        void *tilemap = rt_game_scene_build_tilemap(typed);
        assert(rt_tilemap_get_collision(tilemap, 7) == 1);
        assert(rt_tilemap_get_collision(tilemap, 3) == 2);
        assert(rt_tilemap_get_tile_property(tilemap, 7, rt_const_cstr("damage"), -1) == 3);
        assert(rt_tilemap_get_tile_property(tilemap, 7, rt_const_cstr("slippery"), -1) == 1);
        assert(rt_tilemap_resolve_anim_tile(tilemap, 5) == 5);

        // Removal empties the sections and drops them from JSON entirely.
        rt_game_scene_set_tile_collision(typed, 7, 0);
        rt_game_scene_set_tile_collision(typed, 3, 0);
        rt_game_scene_remove_tile_property(typed, 7, rt_const_cstr("damage"));
        rt_game_scene_remove_tile_property(typed, 7, rt_const_cstr("slippery"));
        rt_game_scene_remove_tile_anim(typed, 5);
        rt_game_scene_remove_autotile_rule(typed, 9);
        std::string cleared_json = scene_json(typed);
        assert(cleared_json.find("\"tileProperties\"") == std::string::npos);
        assert(cleared_json.find("\"animations\"") == std::string::npos);
        assert(cleared_json.find("\"autotiles\"") == std::string::npos);
        // The authored collision layer keeps the collision section alive.
        assert(cleared_json.find("\"collision\": {\"layer\":0}") != std::string::npos);

        // Legacy uniform-duration animations, unknown members, non-numeric
        // tileProperties keys, and out-of-range ids survive loading.
        void *legacy =
            load_text("{\"version\":1,\"name\":\"legacy\",\"width\":2,\"height\":2,"
                      "\"tileWidth\":16,\"tileHeight\":16,\"tilesetAsset\":\"\","
                      "\"properties\":{},"
                      "\"layers\":[{\"name\":\"base\",\"visible\":true,\"asset\":\"\","
                      "\"tiles\":[0,0,0,0]},{\"name\":\"walls\",\"visible\":true,\"asset\":\"\","
                      "\"tiles\":[0,0,0,0]}],\"objects\":[],"
                      "\"collision\":{\"layer\":1,\"solid\":[9,70000],\"future\":true},"
                      "\"tileProperties\":{\"4\":{\"hp\":2,\"note\":\"soft\"},\"x\":{\"a\":1}},"
                      "\"animations\":[{\"baseTile\":6,\"frames\":[6,7],\"frameCount\":2,"
                      "\"msPerFrame\":150,\"tag\":\"lava\"}],"
                      "\"autotiles\":[{\"baseTile\":8,\"variants\":"
                      "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]}]}");
        assert(rt_game_scene_tile_collision(legacy, 9) == 1);
        assert(rt_game_scene_tile_collision(legacy, 70000) == 1);
        assert(rt_game_scene_collision_layer(legacy) == 1);
        assert(rt_game_scene_tile_property_int(legacy, 4, rt_const_cstr("hp"), -1) == 2);
        void *legacy_durations = rt_game_scene_tile_anim_durations(legacy, 6);
        assert(rt_seq_len(legacy_durations) == 2);
        assert(rt_unbox_i64(rt_seq_get(legacy_durations, 0)) == 150);
        assert(rt_seq_len(rt_game_scene_autotile_variants(legacy, 8)) == 16);
        std::string legacy_json = scene_json(legacy);
        assert(legacy_json.find("\"future\":true") != std::string::npos);
        assert(legacy_json.find("\"note\":\"soft\"") != std::string::npos);
        assert(legacy_json.find("\"x\":{\"a\":1}") != std::string::npos);
        assert(legacy_json.find("\"tag\":\"lava\"") != std::string::npos);
        assert(legacy_json.find("70000") != std::string::npos);
        assert(legacy_json.find("\"durations\":[150,150]") != std::string::npos);
        assert(legacy_json.find("msPerFrame") == std::string::npos);
        void *legacy_reload = load_text(legacy_json);
        assert(scene_json(legacy_reload) == legacy_json);
        void *legacy_map = rt_game_scene_build_tilemap(legacy);
        assert(rt_tilemap_get_collision(legacy_map, 9) == 1);
        assert(rt_tilemap_get_collision_layer(legacy_map) == 1);
        assert(rt_tilemap_get_tile_property(legacy_map, 4, rt_const_cstr("hp"), -1) == 2);
    }

    // ADR 0177: camera/lighting typed fields, validation, unknown retention.
    {
        void *scenic = rt_game_scene_new(4, 4, 16, 16);
        rt_game_scene_camera_set_str(scenic, rt_const_cstr("mode"), rt_const_cstr("follow"));
        rt_game_scene_camera_set_int(scenic, rt_const_cstr("minX"), 0);
        rt_game_scene_camera_set_int(scenic, rt_const_cstr("maxX"), 9600);
        rt_game_scene_camera_set_int(scenic, rt_const_cstr("zoomPct"), 150);
        rt_game_scene_lighting_set_int(scenic, rt_const_cstr("darkness"), 160);
        rt_game_scene_lighting_set_int(scenic, rt_const_cstr("playerLightRadius"), 220);
        assert(to_std(rt_game_scene_camera_get_str(
                   scenic, rt_const_cstr("mode"), rt_const_cstr(""))) == "follow");
        assert(rt_game_scene_camera_get_int(scenic, rt_const_cstr("maxX"), -1) == 9600);
        assert(to_std(rt_game_scene_camera_field_kind(scenic, rt_const_cstr("mode"))) == "string");
        assert(to_std(rt_game_scene_camera_field_kind(scenic, rt_const_cstr("absent"))) == "");
        assert(rt_game_scene_lighting_get_int(scenic, rt_const_cstr("darkness"), -1) == 160);
        assert(rt_seq_len(rt_game_scene_camera_keys(scenic)) == 4);
        assert(rt_seq_len(rt_game_scene_lighting_keys(scenic)) == 2);

        // Known-field validation rejects out-of-range values as warning no-ops.
        int64_t before = rt_seq_len(rt_game_scene_diagnostic_records(scenic));
        rt_game_scene_camera_set_str(scenic, rt_const_cstr("mode"), rt_const_cstr("chase"));
        rt_game_scene_camera_set_int(scenic, rt_const_cstr("zoomPct"), 5000);
        rt_game_scene_lighting_set_int(scenic, rt_const_cstr("darkness"), 300);
        assert(rt_seq_len(rt_game_scene_diagnostic_records(scenic)) == before + 3);
        assert(to_std(rt_game_scene_camera_get_str(
                   scenic, rt_const_cstr("mode"), rt_const_cstr(""))) == "follow");
        assert(rt_game_scene_camera_get_int(scenic, rt_const_cstr("zoomPct"), -1) == 150);
        assert(rt_game_scene_lighting_get_int(scenic, rt_const_cstr("darkness"), -1) == 160);

        std::string scenic_json = scene_json(scenic);
        assert(scenic_json.find("\"camera\"") != std::string::npos);
        assert(scenic_json.find("\"mode\":\"follow\"") != std::string::npos);
        assert(scenic_json.find("\"darkness\":160") != std::string::npos);
        void *scenic_reload = load_text(scenic_json);
        assert(rt_game_scene_camera_get_int(scenic_reload, rt_const_cstr("minX"), -1) == 0);
        assert(scene_json(scenic_reload) == scenic_json);

        // Non-int/string members of loaded sections are retained verbatim.
        void *odd = load_text(
            "{\"version\":1,\"name\":\"odd\",\"width\":1,\"height\":1,"
            "\"tileWidth\":16,\"tileHeight\":16,\"tilesetAsset\":\"\","
            "\"properties\":{},"
            "\"layers\":[{\"name\":\"base\",\"visible\":true,\"asset\":\"\",\"tiles\":[0]}],"
            "\"objects\":[],"
            "\"camera\":{\"mode\":\"fixed\",\"weights\":[1,2]},"
            "\"lighting\":{\"darkness\":40,\"curve\":0.5}}");
        assert(to_std(rt_game_scene_camera_get_str(
                   odd, rt_const_cstr("mode"), rt_const_cstr(""))) == "fixed");
        assert(rt_game_scene_lighting_get_int(odd, rt_const_cstr("darkness"), -1) == 40);
        std::string odd_json = scene_json(odd);
        assert(odd_json.find("\"weights\":[1,2]") != std::string::npos);
        assert(odd_json.find("\"curve\":0.5") != std::string::npos);
        assert(scene_json(load_text(odd_json)) == odd_json);

        // Removing every field drops the sections from JSON.
        rt_game_scene_camera_remove(odd, rt_const_cstr("mode"));
        rt_game_scene_lighting_remove(odd, rt_const_cstr("darkness"));
        std::string trimmed = scene_json(odd);
        assert(trimmed.find("\"weights\"") != std::string::npos);
        assert(trimmed.find("\"mode\"") == std::string::npos);
        assert(trimmed.find("\"darkness\"") == std::string::npos);
    }

    int64_t obj =
        rt_game_scene_add_object(scene, rt_const_cstr("Player"), rt_const_cstr("player"), 10, 20);
    rt_game_scene_set_object_property(
        scene, obj, rt_const_cstr("sprite"), rt_const_cstr("hero.png"));
    assert(to_std(rt_game_scene_get_object_property(scene, obj, rt_const_cstr("sprite"))) ==
           "hero.png");
    rt_game_scene_set_property(scene, rt_const_cstr("tileset"), rt_const_cstr("tiles/base.png"));
    void *assets = rt_game_scene_asset_paths(scene);
    assert(rt_seq_len(assets) == 3);

    rt_string json = rt_game_scene_to_json(scene);
    std::string json_text = to_std(rt_string_ref(json));
    assert(json_text.find("\"version\": 1") != std::string::npos);
    assert(json_text.find("\"tiles\"") != std::string::npos);
    assert(json_text.find("\"data\"") == std::string::npos);
    void *loaded = rt_game_scene_load_json(json);
    rt_string_unref(json);
    assert(rt_game_scene_get_width(loaded) == 4);
    assert(rt_game_scene_layer_count(loaded) == 2);
    assert(rt_game_scene_get_tile(loaded, 1, 1, 1) == 7);
    assert(rt_game_scene_object_count(loaded) == 1);
    assert(to_std(rt_game_scene_object_id(loaded, 0)) == "player");
    assert(to_std(rt_game_scene_get_object_property(loaded, 0, rt_const_cstr("sprite"))) ==
           "hero.png");

    rt_game_scene_set_int(scene, rt_const_cstr("playerStartX"), 96);
    rt_game_scene_set_float(scene, rt_const_cstr("gravity"), 0.75);
    rt_game_scene_set_bool(scene, rt_const_cstr("dark"), 1);
    rt_game_scene_set_str(scene, rt_const_cstr("theme"), rt_const_cstr("descent"));
    rt_game_scene_set_null(scene, rt_const_cstr("nextObjective"));
    assert(rt_game_scene_get_int(scene, rt_const_cstr("playerStartX"), -1) == 96);
    assert(rt_game_scene_get_int(scene, rt_const_cstr("theme"), -1) == -1);
    assert(rt_game_scene_get_bool(scene, rt_const_cstr("dark"), 0) == 1);
    assert(rt_game_scene_get_float(scene, rt_const_cstr("gravity"), -1.0) > 0.7);
    assert(to_std(rt_game_scene_get_str(scene, rt_const_cstr("theme"), rt_const_cstr(""))) ==
           "descent");
    assert(to_std(rt_game_scene_property_kind(scene, rt_const_cstr("playerStartX"))) == "int");
    assert(to_std(rt_game_scene_property_kind(scene, rt_const_cstr("gravity"))) == "float");
    assert(to_std(rt_game_scene_property_kind(scene, rt_const_cstr("dark"))) == "bool");
    assert(to_std(rt_game_scene_property_kind(scene, rt_const_cstr("theme"))) == "string");
    assert(to_std(rt_game_scene_property_kind(scene, rt_const_cstr("nextObjective"))) == "null");
    assert(to_std(rt_game_scene_property_kind(scene, rt_const_cstr("missing"))).empty());
    void *scene_keys = rt_game_scene_keys(scene);
    assert(rt_seq_len(scene_keys) == 6);
    assert(to_std(rt_seq_get_str(scene_keys, 0)) == "dark");
    assert(to_std(rt_seq_get_str(scene_keys, 1)) == "gravity");
    assert(to_std(rt_seq_get_str(scene_keys, 2)) == "nextObjective");
    assert(to_std(rt_seq_get_str(scene_keys, 3)) == "playerStartX");
    assert(to_std(rt_seq_get_str(scene_keys, 4)) == "theme");
    assert(to_std(rt_seq_get_str(scene_keys, 5)) == "tileset");
    rt_game_scene_object_set_int(scene, obj, rt_const_cstr("hp"), 3);
    rt_game_scene_object_set_bool(scene, obj, rt_const_cstr("elite"), 1);
    rt_game_scene_object_set_null(scene, obj, rt_const_cstr("target"));
    assert(rt_game_scene_object_get_int(scene, obj, rt_const_cstr("hp"), -1) == 3);
    assert(rt_game_scene_object_get_bool(scene, obj, rt_const_cstr("elite"), 0) == 1);
    assert(to_std(rt_game_scene_object_property_kind(scene, obj, rt_const_cstr("hp"))) == "int");
    assert(to_std(rt_game_scene_object_property_kind(scene, obj, rt_const_cstr("elite"))) ==
           "bool");
    assert(to_std(rt_game_scene_object_property_kind(scene, obj, rt_const_cstr("sprite"))) ==
           "string");
    assert(to_std(rt_game_scene_object_property_kind(scene, obj, rt_const_cstr("target"))) ==
           "null");
    assert(
        to_std(rt_game_scene_object_property_kind(scene, obj, rt_const_cstr("missing"))).empty());
    assert(to_std(rt_game_scene_object_property_kind(scene, -1, rt_const_cstr("hp"))).empty());
    assert(rt_game_scene_find_object(scene, rt_const_cstr("player")) == obj);
    assert(rt_game_scene_count_of_type(scene, rt_const_cstr("Player")) == 1);
    assert(rt_game_scene_object_of_type(scene, rt_const_cstr("Player"), 0) == obj);
    void *keys = rt_game_scene_object_keys(scene, obj);
    assert(rt_seq_len(keys) >= 3);

    void *descriptors = rt_game_scene_asset_descriptors(scene);
    assert(rt_seq_len(descriptors) == 3);
    void *first_desc = rt_seq_get(descriptors, 0);
    assert(to_std(rt_map_get_str(first_desc, rt_const_cstr("path"))).size() > 0);

    void *render = rt_game_scene_build_tilemap(scene);
    assert(rt_tilemap_get_width(render) == 4);
    assert(rt_tilemap_get_layer_count(render) == 2);
    assert(rt_tilemap_get_tile_layer(render, 1, 1, 1) == 7);
    rt_tilemap_set_tile_layer(render, 1, 1, 1, 99);
    assert(rt_game_scene_get_tile(scene, 1, 1, 1) == 7);

    rt_game_scene_set_object_metadata(scene, obj, rt_const_cstr("Actor"), rt_const_cstr("hero"));
    assert(to_std(rt_game_scene_object_type(scene, obj)) == "Actor");
    assert(to_std(rt_game_scene_object_id(scene, obj)) == "hero");
    assert(rt_game_scene_find_object(scene, rt_const_cstr("player")) == -1);
    assert(rt_game_scene_find_object(scene, rt_const_cstr("hero")) == obj);
    int64_t duplicate = rt_game_scene_duplicate_object(scene, obj, rt_const_cstr("hero-copy"));
    assert(duplicate == obj + 1);
    assert(rt_game_scene_object_count(scene) == 2);
    assert(to_std(rt_game_scene_object_type(scene, duplicate)) == "Actor");
    assert(to_std(rt_game_scene_object_id(scene, duplicate)) == "hero-copy");
    assert(rt_game_scene_object_x(scene, duplicate) == rt_game_scene_object_x(scene, obj));
    assert(rt_game_scene_object_y(scene, duplicate) == rt_game_scene_object_y(scene, obj));
    assert(rt_game_scene_object_get_int(scene, duplicate, rt_const_cstr("hp"), -1) == 3);
    assert(rt_game_scene_object_get_bool(scene, duplicate, rt_const_cstr("elite"), 0) == 1);
    assert(to_std(rt_game_scene_object_property_kind(scene, duplicate, rt_const_cstr("target"))) ==
           "null");
    assert(to_std(rt_game_scene_object_get_str(
               scene, duplicate, rt_const_cstr("sprite"), rt_const_cstr(""))) == "hero.png");
    rt_game_scene_object_set_int(scene, duplicate, rt_const_cstr("hp"), 9);
    rt_game_scene_object_set_str(
        scene, duplicate, rt_const_cstr("target"), rt_const_cstr("changed"));
    rt_game_scene_set_object_position(scene, duplicate, 144, 160);
    assert(rt_game_scene_object_get_int(scene, obj, rt_const_cstr("hp"), -1) == 3);
    assert(to_std(rt_game_scene_object_property_kind(scene, obj, rt_const_cstr("target"))) ==
           "null");
    assert(rt_game_scene_object_x(scene, obj) == 10);
    assert(rt_game_scene_object_y(scene, obj) == 20);
    assert(rt_game_scene_duplicate_object(scene, -1, rt_const_cstr("invalid")) == -1);

    // ADR 0164: SceneDocument owns a cycle-safe organizational hierarchy while
    // retaining absolute scene-space positions and version-1 compatibility.
    {
        void *hierarchy = rt_game_scene_new(2, 2, 16, 16);
        int64_t root = rt_game_scene_add_object(
            hierarchy, rt_const_cstr("Group"), rt_const_cstr("root"), 4, 8);
        int64_t child = rt_game_scene_add_object(
            hierarchy, rt_const_cstr("Actor"), rt_const_cstr("child"), 12, 16);
        int64_t grandchild = rt_game_scene_add_object(
            hierarchy, rt_const_cstr("Actor"), rt_const_cstr("grandchild"), 20, 24);
        int64_t peer = rt_game_scene_add_object(
            hierarchy, rt_const_cstr("Actor"), rt_const_cstr("peer"), 28, 32);

        assert(rt_game_scene_object_parent(hierarchy, root) == -1);
        assert(rt_game_scene_object_parent(hierarchy, child) == -1);
        assert(rt_game_scene_object_parent(hierarchy, -1) == -1);
        assert(rt_game_scene_try_set_object_parent(hierarchy, child, root) == 1);
        assert(rt_game_scene_try_set_object_parent(hierarchy, grandchild, child) == 1);
        assert(rt_game_scene_object_parent(hierarchy, child) == root);
        assert(rt_game_scene_object_parent(hierarchy, grandchild) == child);
        assert(rt_game_scene_object_x(hierarchy, child) == 12);
        assert(rt_game_scene_object_y(hierarchy, child) == 16);

        std::string before_rejection = scene_json(hierarchy);
        assert(rt_game_scene_try_set_object_parent(hierarchy, -1, root) == 0);
        assert(rt_game_scene_try_set_object_parent(hierarchy, child, 99) == 0);
        assert(rt_game_scene_try_set_object_parent(hierarchy, child, child) == 0);
        assert(rt_game_scene_try_set_object_parent(hierarchy, root, grandchild) == 0);
        assert(scene_json(hierarchy) == before_rejection);

        constexpr const char *parent_key = "zanna.hierarchy.parentIndex";
        rt_game_scene_object_set_int(hierarchy, child, rt_const_cstr(parent_key), peer);
        assert(rt_game_scene_object_parent(hierarchy, child) == root);
        assert(rt_game_scene_object_has(hierarchy, child, rt_const_cstr(parent_key)) == 0);
        assert(rt_game_scene_object_get_int(hierarchy, child, rt_const_cstr(parent_key), -77) ==
               -77);
        assert(
            to_std(rt_game_scene_object_property_kind(hierarchy, child, rt_const_cstr(parent_key)))
                .empty());
        void *public_keys = rt_game_scene_object_keys(hierarchy, child);
        assert(rt_seq_len(public_keys) == 0);
        rt_game_scene_object_remove(hierarchy, child, rt_const_cstr(parent_key));
        assert(rt_game_scene_object_parent(hierarchy, child) == root);

        std::string hierarchy_json = scene_json(hierarchy);
        assert(hierarchy_json.find("\"zanna.hierarchy.parentIndex\": 0") != std::string::npos);
        void *hierarchy_roundtrip = load_text(hierarchy_json);
        assert(rt_game_scene_has_errors(hierarchy_roundtrip) == 0);
        assert(rt_game_scene_object_parent(hierarchy_roundtrip, 1) == 0);
        assert(rt_game_scene_object_parent(hierarchy_roundtrip, 2) == 1);
        assert(rt_game_scene_object_has(hierarchy_roundtrip, 1, rt_const_cstr(parent_key)) == 0);

        // The reserved hierarchy entry consumes one of the 256 serialized
        // property slots. Parenting a full root must be an exact rejection,
        // and a child at capacity may replace but not add public properties.
        void *property_limits = rt_game_scene_new(1, 1, 16, 16);
        int64_t limit_root = rt_game_scene_add_object(
            property_limits, rt_const_cstr("Group"), rt_const_cstr("limit-root"), 0, 0);
        int64_t limit_child = rt_game_scene_add_object(
            property_limits, rt_const_cstr("Actor"), rt_const_cstr("limit-child"), 0, 0);
        for (int64_t property = 0; property < 256; ++property) {
            std::string key = "property-" + std::to_string(property);
            rt_string key_string = rt_string_from_bytes(key.data(), key.size());
            rt_game_scene_object_set_int(property_limits, limit_child, key_string, property);
            rt_string_unref(key_string);
        }
        assert(rt_seq_len(rt_game_scene_object_keys(property_limits, limit_child)) == 256);
        assert(rt_game_scene_try_set_object_parent(property_limits, limit_child, limit_root) == 0);
        assert(rt_game_scene_object_parent(property_limits, limit_child) == -1);

        rt_game_scene_object_remove(property_limits, limit_child, rt_const_cstr("property-255"));
        assert(rt_game_scene_try_set_object_parent(property_limits, limit_child, limit_root) == 1);
        assert(rt_seq_len(rt_game_scene_object_keys(property_limits, limit_child)) == 255);
        rt_game_scene_object_set_int(property_limits, limit_child, rt_const_cstr("overflow"), 1);
        assert(rt_game_scene_object_has(property_limits, limit_child, rt_const_cstr("overflow")) ==
               0);
        rt_game_scene_object_set_int(
            property_limits, limit_child, rt_const_cstr("property-0"), 999);
        assert(rt_game_scene_object_get_int(
                   property_limits, limit_child, rt_const_cstr("property-0"), -1) == 999);
        std::string limited_json = scene_json(property_limits);
        void *limited_roundtrip = load_text(limited_json);
        assert(rt_game_scene_has_errors(limited_roundtrip) == 0);
        assert(rt_game_scene_object_parent(limited_roundtrip, limit_child) == limit_root);
        assert(rt_seq_len(rt_game_scene_object_keys(limited_roundtrip, limit_child)) == 255);

        // Moving root to the end applies one old-to-new permutation to objects
        // and every structural parent reference.
        rt_game_scene_move_object(hierarchy, root, peer);
        assert(to_std(rt_game_scene_object_id(hierarchy, 0)) == "child");
        assert(to_std(rt_game_scene_object_id(hierarchy, 1)) == "grandchild");
        assert(to_std(rt_game_scene_object_id(hierarchy, 2)) == "peer");
        assert(to_std(rt_game_scene_object_id(hierarchy, 3)) == "root");
        assert(rt_game_scene_object_parent(hierarchy, 0) == 3);
        assert(rt_game_scene_object_parent(hierarchy, 1) == 0);
        assert(rt_game_scene_object_parent(hierarchy, 2) == -1);
        assert(rt_game_scene_object_parent(hierarchy, 3) == -1);
    }

    {
        void *removal = rt_game_scene_new(2, 2, 16, 16);
        int64_t root =
            rt_game_scene_add_object(removal, rt_const_cstr("Group"), rt_const_cstr("root"), 0, 0);
        int64_t child =
            rt_game_scene_add_object(removal, rt_const_cstr("Group"), rt_const_cstr("child"), 0, 0);
        int64_t grandchild = rt_game_scene_add_object(
            removal, rt_const_cstr("Actor"), rt_const_cstr("grandchild"), 0, 0);
        int64_t sibling = rt_game_scene_add_object(
            removal, rt_const_cstr("Actor"), rt_const_cstr("sibling"), 0, 0);
        assert(rt_game_scene_try_set_object_parent(removal, child, root) == 1);
        assert(rt_game_scene_try_set_object_parent(removal, grandchild, child) == 1);
        assert(rt_game_scene_try_set_object_parent(removal, sibling, root) == 1);

        rt_game_scene_remove_object(removal, child);
        assert(rt_game_scene_object_count(removal) == 3);
        assert(to_std(rt_game_scene_object_id(removal, 1)) == "grandchild");
        assert(rt_game_scene_object_parent(removal, 1) == 0);
        assert(rt_game_scene_object_parent(removal, 2) == 0);

        rt_game_scene_remove_object(removal, 0);
        assert(rt_game_scene_object_count(removal) == 2);
        assert(rt_game_scene_object_parent(removal, 0) == -1);
        assert(rt_game_scene_object_parent(removal, 1) == -1);
    }

    {
        void *duplication = rt_game_scene_new(2, 2, 16, 16);
        int64_t root = rt_game_scene_add_object(
            duplication, rt_const_cstr("Group"), rt_const_cstr("root"), 0, 0);
        int64_t child = rt_game_scene_add_object(
            duplication, rt_const_cstr("Actor"), rt_const_cstr("child"), 0, 0);
        int64_t grandchild = rt_game_scene_add_object(
            duplication, rt_const_cstr("Actor"), rt_const_cstr("grandchild"), 0, 0);
        assert(rt_game_scene_try_set_object_parent(duplication, child, root) == 1);
        assert(rt_game_scene_try_set_object_parent(duplication, grandchild, child) == 1);

        int64_t root_copy =
            rt_game_scene_duplicate_object(duplication, root, rt_const_cstr("root-copy"));
        assert(root_copy == 1);
        assert(rt_game_scene_object_parent(duplication, root_copy) == -1);
        assert(to_std(rt_game_scene_object_id(duplication, 2)) == "child");
        assert(to_std(rt_game_scene_object_id(duplication, 3)) == "grandchild");
        assert(rt_game_scene_object_parent(duplication, 2) == 0);
        assert(rt_game_scene_object_parent(duplication, 3) == 2);

        int64_t child_copy =
            rt_game_scene_duplicate_object(duplication, 2, rt_const_cstr("child-copy"));
        assert(child_copy == 3);
        assert(rt_game_scene_object_parent(duplication, child_copy) == 0);
        assert(to_std(rt_game_scene_object_id(duplication, 4)) == "grandchild");
        assert(rt_game_scene_object_parent(duplication, 4) == 2);
    }

    {
        constexpr const char *malformed_hierarchy =
            "{\"version\":1,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[{\"name\":\"base\",\"visible\":true,\"asset\":\"\",\"tiles\":[0]}],"
            "\"objects\":["
            "{\"type\":\"Group\",\"id\":\"bad-type\",\"x\":0,\"y\":0,\"properties\":{"
            "\"zanna.hierarchy.parentIndex\":\"nope\"}},"
            "{\"type\":\"Group\",\"id\":\"bad-range\",\"x\":0,\"y\":0,\"properties\":{"
            "\"zanna.hierarchy.parentIndex\":99}},"
            "{\"type\":\"Group\",\"id\":\"self\",\"x\":0,\"y\":0,\"properties\":{"
            "\"zanna.hierarchy.parentIndex\":2}},"
            "{\"type\":\"Group\",\"id\":\"cycle-a\",\"x\":0,\"y\":0,\"properties\":{"
            "\"zanna.hierarchy.parentIndex\":4}},"
            "{\"type\":\"Group\",\"id\":\"cycle-b\",\"x\":0,\"y\":0,\"properties\":{"
            "\"zanna.hierarchy.parentIndex\":3}}]}";
        void *malformed_hierarchy_scene = load_text(malformed_hierarchy);
        assert(rt_game_scene_has_errors(malformed_hierarchy_scene) == 1);
        assert(rt_seq_len(rt_game_scene_diagnostic_records(malformed_hierarchy_scene)) >= 4);
        for (int64_t index = 0; index < 5; ++index)
            assert(rt_game_scene_object_parent(malformed_hierarchy_scene, index) == -1);
        assert(scene_json(malformed_hierarchy_scene).find("zanna.hierarchy.parentIndex") ==
               std::string::npos);
    }

    rt_string bad_json = rt_const_cstr("{\"version\":1,");
    void *bad = rt_game_scene_load_json(bad_json);
    rt_string_unref(bad_json);
    assert(rt_game_scene_has_errors(bad) == 1);
    void *diag_records = rt_game_scene_diagnostic_records(bad);
    assert(rt_seq_len(diag_records) >= 1);
    void *diag0 = rt_seq_get(diag_records, 0);
    assert(to_std(rt_map_get_str(diag0, rt_const_cstr("code"))) == "scene.parse.malformed_json");

    rt_string non_object_json = rt_const_cstr("[1,2,3]");
    void *non_object = rt_game_scene_load_json(non_object_json);
    rt_string_unref(non_object_json);
    assert(rt_game_scene_has_errors(non_object) == 1);

    void *too_big = rt_game_scene_new(5000, 5000, 16, 16);
    assert(rt_game_scene_has_errors(too_big) == 1);
    int64_t long_layer =
        rt_game_scene_add_layer(scene, rt_const_cstr("this-layer-name-is-longer-than-thirty-one"));
    assert(long_layer == -1);
    assert(rt_game_scene_has_errors(scene) == 0);
    assert(diagnostic_code(scene, 0) == "scene.edit.rejected");
    assert(diagnostic_severity(scene, 0) == "warning");
    rt_game_scene_clear_diagnostics(scene);
    assert(rt_game_scene_has_errors(scene) == 0);

    // VDOC-254: clearing diagnostics on a structurally invalid (error-severity)
    // scene must NOT flip it to valid — validity state is separate from the
    // diagnostic queue, so HasErrors stays true after the messages are cleared.
    rt_string invalid_json = rt_const_cstr("{\"version\":1,");
    void *invalid_scene = rt_game_scene_load_json(invalid_json);
    rt_string_unref(invalid_json);
    assert(rt_game_scene_has_errors(invalid_scene) == 1);
    rt_game_scene_clear_diagnostics(invalid_scene);
    assert(rt_game_scene_has_errors(invalid_scene) == 1);

    // VDOC-255: the Result Err message is the first error diagnostic, not lastError
    // (the newest of any severity), so a trailing warning cannot mask the real error.
    {
        const char *mixedText =
            "{\"version\":1,\"width\":-5,\"height\":2,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[],\"bogusUnknownField\":123}";
        void *doc = rt_game_scene_load_json(rt_const_cstr(mixedText));
        void *records = rt_game_scene_diagnostic_records(doc);
        const int64_t n = rt_seq_len(records);
        std::string firstError;
        std::string lastMsg;
        for (int64_t i = 0; i < n; ++i) {
            void *rec = rt_seq_get(records, i);
            std::string sev = map_str(rec, "severity");
            std::string msg = map_str(rec, "message");
            if (firstError.empty() && sev == "error")
                firstError = msg;
            lastMsg = msg;
        }
        assert(!firstError.empty());
        // lastError holds the newest diagnostic (the trailing warning), which differs
        // from the first error — that is exactly the masking hazard.
        assert(last_error(doc) == lastMsg);
        assert(lastMsg != firstError);

        // The Result Err carries the first error message, not the trailing warning.
        void *res = rt_game_scene_load_json_result(rt_const_cstr(mixedText));
        assert(rt_result_is_err(res) == 1);
        assert(to_std(rt_result_unwrap_err_str(res)) == firstError);
    }

    rt_string legacy_short =
        rt_const_cstr("{\"width\":2,\"height\":2,\"tileWidth\":16,\"tileHeight\":16,"
                      "\"layers\":[{\"name\":\"base\",\"visible\":1,\"data\":[5]}]}");
    void *legacy = rt_game_scene_load_json(legacy_short);
    rt_string_unref(legacy_short);
    assert(rt_game_scene_has_errors(legacy) == 0);
    assert(rt_game_scene_get_tile(legacy, 0, 0, 0) == 5);
    assert(rt_game_scene_get_tile(legacy, 0, 1, 1) == 0);
    assert(rt_seq_len(rt_game_scene_diagnostic_records(legacy)) == 1);

#ifdef ZANNA_SOURCE_DIR
    std::filesystem::path fixture_dir =
        std::filesystem::path(ZANNA_SOURCE_DIR) / "src/tests/data/game/scenes";
    std::string minimal_text = read_text(fixture_dir / "v1_minimal.scene");
    rt_string minimal_s = rt_string_from_bytes(minimal_text.data(), minimal_text.size());
    void *minimal = rt_game_scene_load_json(minimal_s);
    rt_string_unref(minimal_s);
    assert(rt_game_scene_has_errors(minimal) == 0);
    assert(rt_game_scene_get_tile(minimal, 0, 1, 0) == 1);
    assert(to_std(rt_game_scene_to_json(minimal)) ==
           read_text(fixture_dir / "v1_minimal.golden.scene"));

    void *full = load_fixture(fixture_dir, "v1_full.scene");
    assert(rt_game_scene_has_errors(full) == 0);
    assert(rt_game_scene_get_width(full) == 3);
    assert(rt_game_scene_layer_count(full) == 2);
    assert(rt_game_scene_get_tile(full, 0, 1, 0) == 1);
    assert(rt_game_scene_get_tile(full, 1, 2, 1) == 4);
    assert(rt_game_scene_layer_visible(full, 1) == 0);
    assert(to_std(rt_game_scene_get_str(full, rt_const_cstr("theme"), rt_const_cstr(""))) ==
           "cavern");
    assert(to_std(rt_game_scene_object_get_str(
               full, 0, rt_const_cstr("sprite"), rt_const_cstr(""))) == "sprites/slime.png");
    std::string full_once = scene_json(full);
    void *full_reloaded = load_text(full_once);
    assert(rt_game_scene_has_errors(full_reloaded) == 0);
    assert(scene_json(full_reloaded) == full_once);

    void *full_tilemap = rt_game_scene_build_tilemap(full);
    assert(rt_tilemap_get_layer_count(full_tilemap) == 2);
    assert(rt_tilemap_get_layer_visible(full_tilemap, 1) == 0);
    assert(rt_tilemap_get_collision_layer(full_tilemap) == 0);
    assert(rt_tilemap_get_collision(full_tilemap, 1) == RT_TILE_COLLISION_SOLID);
    assert(rt_tilemap_get_collision(full_tilemap, 2) == RT_TILE_COLLISION_SOLID);
    assert(rt_tilemap_get_collision(full_tilemap, 3) == RT_TILE_COLLISION_ONE_WAY_UP);
    assert(rt_tilemap_get_tile_property(full_tilemap, 1, rt_const_cstr("solid"), 0) == 1);
    assert(rt_tilemap_resolve_anim_tile(full_tilemap, 4) == 4);
    rt_tilemap_update_anims(full_tilemap, 120);
    assert(rt_tilemap_resolve_anim_tile(full_tilemap, 4) == 5);
    rt_tilemap_set_tile(full_tilemap, 1, 0, 8);
    rt_tilemap_set_tile(full_tilemap, 2, 0, 8);
    rt_tilemap_apply_autotile(full_tilemap);
    assert(rt_tilemap_get_tile(full_tilemap, 1, 0) == 10);
    assert(rt_tilemap_get_tile(full_tilemap, 2, 0) == 16);
    void *full_descriptors = rt_game_scene_asset_descriptors(full);
    assert(has_descriptor(full_descriptors,
                          "textures/light-mask.png",
                          "image",
                          "section",
                          "lighting",
                          "textureAsset"));

    void *leveldata = load_fixture(fixture_dir, "legacy_leveldata.json");
    assert(rt_game_scene_has_errors(leveldata) == 0);
    assert(rt_game_scene_get_int(leveldata, rt_const_cstr("playerStartX"), -1) == 16);
    assert(rt_game_scene_object_count(leveldata) == 1);

    void *legacy_current = load_fixture(fixture_dir, "legacy_current_generated.json");
    assert(rt_game_scene_has_errors(legacy_current) == 0);
    assert(rt_game_scene_get_tile(legacy_current, 0, 1, 0) == 7);
    assert(to_std(rt_game_scene_get_str(
               legacy_current, rt_const_cstr("tileset"), rt_const_cstr(""))) == "tiles/base.png");

    void *legacy_nested = load_fixture(fixture_dir, "legacy_nested_tilemap.json");
    assert(rt_game_scene_has_errors(legacy_nested) == 0);
    assert(rt_game_scene_get_width(legacy_nested) == 2);
    assert(rt_game_scene_get_tile(legacy_nested, 0, 1, 0) == 4);
    assert(rt_game_scene_object_count(legacy_nested) == 1);
    assert(has_descriptor(rt_game_scene_asset_descriptors(legacy_nested),
                          "tiles/nested.png",
                          "tileset",
                          "scene",
                          "",
                          "tilesetAsset"));

    void *legacy_flat = load_fixture(fixture_dir, "legacy_flat_objects.json");
    assert(rt_game_scene_has_errors(legacy_flat) == 0);
    assert(rt_game_scene_object_get_int(legacy_flat, 0, rt_const_cstr("hp"), -1) == 5);
    assert(rt_game_scene_object_get_bool(legacy_flat, 0, rt_const_cstr("elite"), 0) == 1);
    assert(to_std(rt_game_scene_object_get_str(
               legacy_flat, 0, rt_const_cstr("sprite"), rt_const_cstr(""))) ==
           "sprites/player.png");

    std::string long_key(129, 'k');
    std::string long_value(64 * 1024 + 1, 'v');
    std::string legacy_flat_limits =
        "{\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
        "\"layers\":[{\"name\":\"base\",\"data\":[0]}],\"objects\":[{\"type\":\"Thing\","
        "\"id\":\"bad\",\"x\":0,\"y\":0,\"" +
        long_key + "\":1,\"sprite\":\"" + long_value + "\"}]}";
    void *legacy_flat_rejected = load_text(legacy_flat_limits);
    assert(rt_game_scene_has_errors(legacy_flat_rejected) == 1);
    rt_string long_key_s = rt_string_from_bytes(long_key.data(), long_key.size());
    assert(rt_game_scene_object_has(legacy_flat_rejected, 0, long_key_s) == 0);
    rt_string_unref(long_key_s);
    assert(to_std(rt_game_scene_object_get_str(
               legacy_flat_rejected, 0, rt_const_cstr("sprite"), rt_const_cstr(""))) == "");

    std::string warning_flood =
        "{\"version\":1,\"name\":\"flood\",\"width\":1,\"height\":1,"
        "\"tileWidth\":16,\"tileHeight\":16,\"tilesetAsset\":\"\","
        "\"properties\":{},\"layers\":[{\"name\":\"base\",\"tiles\":[0]}],\"objects\":[]";
    for (int i = 0; i < 260; ++i)
        warning_flood += ",\"unknown" + std::to_string(i) + "\":0";
    warning_flood += "}";
    void *warning_scene = load_text(warning_flood);
    void *warning_records = rt_game_scene_diagnostic_records(warning_scene);
    assert(rt_game_scene_has_errors(warning_scene) == 0);
    assert(rt_seq_len(warning_records) == 256);
    assert(diagnostic_code(warning_scene, 255) == "scene.diagnostics.truncated");
    assert(last_error(warning_scene) ==
           "too many scene diagnostics; later diagnostics were dropped");

    void *invalid_version = load_fixture(fixture_dir, "invalid_version.scene");
    assert(rt_game_scene_has_errors(invalid_version) == 1);
    assert(diagnostic_code(invalid_version, 0) == "scene.schema.unsupported_version");

    void *invalid_malformed = load_fixture(fixture_dir, "invalid_malformed.scene");
    assert(rt_game_scene_has_errors(invalid_malformed) == 1);
    assert(diagnostic_code(invalid_malformed, 0) == "scene.parse.malformed_json");

    void *invalid_count = load_fixture(fixture_dir, "invalid_tile_count.scene");
    assert(rt_game_scene_has_errors(invalid_count) == 1);
#endif

    std::filesystem::path path =
        std::filesystem::temp_directory_path() / "zanna_scene_editor_test.json";
    std::string path_text = path.string();
    rt_string path_s = rt_string_from_bytes(path_text.data(), path_text.size());
    assert(rt_game_scene_save_file(scene, path_s) == 1);
    void *from_file = rt_game_scene_load_file(path_s);
    rt_string_unref(path_s);
    assert(rt_game_scene_get_tile(from_file, 1, 2, 1) == 7);
    std::filesystem::remove(path);

    std::filesystem::path blocked_path =
        std::filesystem::temp_directory_path() / "zanna_scene_editor_save_target_dir";
    std::filesystem::remove_all(blocked_path);
    std::filesystem::create_directory(blocked_path);
    std::string blocked_text = blocked_path.string();
    rt_string blocked_s = rt_string_from_bytes(blocked_text.data(), blocked_text.size());
    assert(rt_game_scene_save_file(scene, blocked_s) == 0);
    rt_string_unref(blocked_s);
    assert(std::filesystem::is_directory(blocked_path));
    assert(rt_game_scene_has_errors(scene) == 1);
    rt_game_scene_clear_diagnostics(scene);
    std::filesystem::remove_all(blocked_path);

    // ADR 0140: external Tiled JSON/TMX imports preserve tile layers, objects,
    // typed properties, tileset metadata, animation, and collision. The loader
    // convenience returns a render-ready Tilemap using the same normalized data.
    std::filesystem::path tiled_dir =
        std::filesystem::temp_directory_path() / "zanna_tiled_import_test";
    std::filesystem::remove_all(tiled_dir);
    std::filesystem::create_directories(tiled_dir);
    std::filesystem::path tiled_png = tiled_dir / "tiles.png";
    void *tiled_pixels = rt_pixels_new(7, 7);
    assert(tiled_pixels != nullptr);
    rt_pixels_fill(tiled_pixels, 0xff8040ff);
    assert(rt_pixels_save_png(tiled_pixels, rt_const_cstr(tiled_png.string().c_str())) == 1);

    write_text(tiled_dir / "terrain.tsj",
               R"json({
  "type":"tileset","name":"terrain","tilewidth":2,"tileheight":2,
  "tilecount":4,"columns":2,"margin":1,"spacing":1,
  "image":"tiles.png","imagewidth":7,"imageheight":7,
  "tiles":[{
    "id":1,
    "properties":[{"name":"damage","type":"int","value":3}],
    "objectgroup":{"objects":[{"id":1,"x":0,"y":0,"width":2,"height":2}]},
    "animation":[{"tileid":1,"duration":100},{"tileid":2,"duration":200}]
  }]
})json");
    write_text(tiled_dir / "spawn.tj",
               R"json({
  "type":"template",
  "object":{"class":"Enemy","width":2,"height":2,
    "properties":[{"name":"team","type":"string","value":"red"},
                  {"name":"hp","type":"int","value":2}]}
})json");
    write_text(tiled_dir / "level.tmj",
               R"json({
  "type":"map","tiledversion":"1.11.2","orientation":"orthogonal","infinite":false,
  "width":3,"height":2,"tilewidth":2,"tileheight":2,
  "properties":[{"name":"theme","type":"string","value":"cavern"},
                {"name":"config","type":"file","value":"config/game.json"}],
  "tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
  "layers":[
    {"id":1,"type":"group","name":"World","visible":true,
     "properties":[{"name":"category","type":"string","value":"terrain"}],
     "layers":[
       {"id":2,"type":"tilelayer","name":"Ground","width":2,"height":2,"x":1,
        "visible":true,"properties":[{"name":"kind","type":"string","value":"floor"}],
        "data":[2,1,3,4]}
     ]},
    {"id":3,"type":"objectgroup","name":"Objects","objects":[
      {"id":7,"name":"spawn-a","class":"Spawn","x":2,"y":4,"width":2,"height":2,
       "properties":[{"name":"team","type":"string","value":"blue"},
                     {"name":"hp","type":"int","value":5}]},
      {"id":8,"name":"spawn-a","template":"spawn.tj","x":2.5,"y":1,
       "properties":[{"name":"hp","type":"int","value":9}]}
    ]},
    {"id":4,"type":"imagelayer","name":"Backdrop","image":"tiles.png","visible":false}
  ]
})json");

    void *tiled_scene_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "level.tmj").string().c_str()));
    assert(rt_result_is_ok(tiled_scene_result) == 1);
    void *tiled_scene = rt_result_unwrap(tiled_scene_result);
    assert(rt_game_scene_get_width(tiled_scene) == 3);
    assert(rt_game_scene_get_height(tiled_scene) == 2);
    assert(rt_game_scene_get_tile_width(tiled_scene) == 2);
    assert(rt_game_scene_layer_count(tiled_scene) == 1);
    assert(rt_game_scene_get_tile(tiled_scene, 0, 1, 0) == 2);
    assert(rt_game_scene_object_count(tiled_scene) == 3);
    assert(to_std(rt_game_scene_object_type(tiled_scene, 0)) == "Spawn");
    assert(to_std(rt_game_scene_object_id(tiled_scene, 0)) == "spawn-a");
    assert(rt_game_scene_object_get_int(tiled_scene, 0, rt_const_cstr("hp"), -1) == 5);
    assert(to_std(rt_game_scene_object_get_str(
               tiled_scene, 0, rt_const_cstr("team"), rt_const_cstr(""))) == "blue");
    assert(to_std(rt_game_scene_get_str(tiled_scene, rt_const_cstr("theme"), rt_const_cstr(""))) ==
           "cavern");
    assert(to_std(rt_game_scene_get_str(tiled_scene,
                                        rt_const_cstr("tiled.layer.World.category"),
                                        rt_const_cstr(""))) == "terrain");
    assert(to_std(rt_game_scene_get_str(tiled_scene,
                                        rt_const_cstr("tiled.layer.World/Ground.kind"),
                                        rt_const_cstr(""))) == "floor");
    assert(to_std(rt_game_scene_get_str(tiled_scene, rt_const_cstr("config"), rt_const_cstr(""))) ==
           (tiled_dir / "config/game.json").lexically_normal().string());
    assert(to_std(rt_game_scene_object_type(tiled_scene, 1)) == "Enemy");
    assert(to_std(rt_game_scene_object_id(tiled_scene, 1)) == "8");
    assert(rt_game_scene_object_get_int(tiled_scene, 1, rt_const_cstr("hp"), -1) == 9);
    assert(to_std(rt_game_scene_object_get_str(
               tiled_scene, 1, rt_const_cstr("team"), rt_const_cstr(""))) == "red");
    assert(rt_game_scene_object_get_float(tiled_scene, 1, rt_const_cstr("tiled.sourceX"), -1.0) ==
           2.5);
    assert(to_std(rt_game_scene_object_get_str(
               tiled_scene, 1, rt_const_cstr("tiled.template"), rt_const_cstr(""))) ==
           (tiled_dir / "spawn.tj").string());
    assert(to_std(rt_game_scene_object_type(tiled_scene, 2)) == "tiled.image-layer");
    void *tiled_render = rt_game_scene_build_tilemap(tiled_scene);
    assert(rt_tilemap_get_collision(tiled_render, 2) == RT_TILE_COLLISION_SOLID);
    assert(rt_tilemap_get_tile_property(tiled_render, 2, rt_const_cstr("damage"), -1) == 3);
    assert(rt_tilemap_resolve_anim_tile(tiled_render, 2) == 2);
    rt_tilemap_update_anims(tiled_render, 100);
    assert(rt_tilemap_resolve_anim_tile(tiled_render, 2) == 3);
    rt_tilemap_update_anims(tiled_render, 100);
    assert(rt_tilemap_resolve_anim_tile(tiled_render, 2) == 3);
    rt_tilemap_update_anims(tiled_render, 100);
    assert(rt_tilemap_resolve_anim_tile(tiled_render, 2) == 2);

    void *tiled_loader = rt_tiledmaploader_new();
    void *tiled_map_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "level.tmj").string().c_str()));
    assert(rt_result_is_ok(tiled_map_result) == 1);
    void *loaded_tiled_map = rt_result_unwrap(tiled_map_result);
    assert(rt_tilemap_get_width(loaded_tiled_map) == 3);
    assert(rt_tilemap_get_tile_count(loaded_tiled_map) == 4);
    assert(rt_tilemap_get_tile(loaded_tiled_map, 2, 1) == 4);

    write_text(tiled_dir / "level.tmx",
               R"xml(<?xml version="1.0" encoding="UTF-8"?>
<map version="1.10" tiledversion="1.11.2" orientation="orthogonal"
     width="2" height="2" tilewidth="2" tileheight="2" infinite="0">
  <properties><property name="theme" value="tmx-cave"/></properties>
  <tileset firstgid="1" source="terrain.tsx"/>
  <layer id="1" name="Ground" width="2" height="2">
    <properties><property name="kind" value="tmx-floor"/></properties>
    <data encoding="csv">1,0,2,4</data>
  </layer>
  <objectgroup id="2" name="Objects">
    <object id="9" name="door-a" type="Door" x="2" y="2" width="2" height="2">
      <properties><property name="locked" type="bool" value="true"/></properties>
    </object>
    <object id="10" name="switch-a" template="door.tx" x="1.5" y="3">
      <properties><property name="channel" type="int" value="7"/></properties>
    </object>
  </objectgroup>
</map>)xml");
    write_text(tiled_dir / "terrain.tsx",
               R"xml(<?xml version="1.0" encoding="UTF-8"?>
<tileset version="1.10" name="terrain" tilewidth="2" tileheight="2"
         tilecount="4" columns="2" margin="1" spacing="1">
  <image source="tiles.png" width="7" height="7"/>
</tileset>)xml");
    write_text(tiled_dir / "door.tx",
               R"xml(<?xml version="1.0" encoding="UTF-8"?>
<template>
  <object type="Switch" width="2" height="2">
    <properties><property name="channel" type="int" value="1"/></properties>
  </object>
</template>)xml");
    void *tmx_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "level.tmx").string().c_str()));
    if (rt_result_is_err(tmx_result))
        std::fprintf(stderr,
                     "TMX import failed: %s\n",
                     to_std(rt_result_unwrap_err_str(tmx_result)).c_str());
    assert(rt_result_is_ok(tmx_result) == 1);
    void *tmx_scene = rt_result_unwrap(tmx_result);
    assert(rt_game_scene_get_tile(tmx_scene, 0, 1, 1) == 4);
    assert(to_std(rt_game_scene_object_type(tmx_scene, 0)) == "Door");
    assert(rt_game_scene_object_get_bool(tmx_scene, 0, rt_const_cstr("locked"), 0) == 1);
    assert(to_std(rt_game_scene_get_str(tmx_scene,
                                        rt_const_cstr("tiled.layer.Ground.kind"),
                                        rt_const_cstr(""))) == "tmx-floor");
    assert(to_std(rt_game_scene_object_type(tmx_scene, 1)) == "Switch");
    assert(rt_game_scene_object_get_int(tmx_scene, 1, rt_const_cstr("channel"), -1) == 7);
    assert(rt_game_scene_object_get_float(tmx_scene, 1, rt_const_cstr("tiled.sourceX"), -1.0) ==
           1.5);

    // Every encoded Tiled payload path is decoded against the exact cell-byte
    // bound. These tiny stored frames keep the fixture dependency-free.
    std::vector<uint8_t> gid_bytes = tiled_gid_bytes({1, 2, 3, 4});

    struct EncodedCase {
        const char *name;
        const char *compression;
        std::vector<uint8_t> bytes;
    };

    std::vector<EncodedCase> encoded_cases = {
        {"raw", "", gid_bytes},
        {"zlib", "zlib", zlib_stored(gid_bytes)},
        {"gzip", "gzip", gzip_stored(gid_bytes)},
        {"zstd", "zstd", zstd_raw_frame(gid_bytes)},
    };
    for (const EncodedCase &encoded_case : encoded_cases) {
        std::string compression_field =
            encoded_case.compression[0]
                ? std::string(",\"compression\":\"") + encoded_case.compression + "\""
                : std::string();
        std::string encoded_json =
            "{\"type\":\"map\",\"orientation\":\"orthogonal\",\"infinite\":false,"
            "\"width\":2,\"height\":2,\"tilewidth\":2,\"tileheight\":2,"
            "\"tilesets\":[{\"firstgid\":1,\"source\":\"terrain.tsj\"}],"
            "\"layers\":[{\"type\":\"tilelayer\",\"name\":\"encoded\","
            "\"width\":2,\"height\":2,\"encoding\":\"base64\"" +
            compression_field + ",\"data\":\"" + base64(encoded_case.bytes) + "\"}]}";
        std::filesystem::path encoded_path =
            tiled_dir / (std::string("encoded-") + encoded_case.name + ".tmj");
        write_text(encoded_path, encoded_json);
        void *encoded_result =
            rt_game_scene_import_tiled_result(rt_const_cstr(encoded_path.string().c_str()));
        if (rt_result_is_err(encoded_result))
            std::fprintf(stderr,
                         "%s import failed: %s\n",
                         encoded_case.name,
                         to_std(rt_result_unwrap_err_str(encoded_result)).c_str());
        assert(rt_result_is_ok(encoded_result) == 1);
        void *encoded_scene = rt_result_unwrap(encoded_result);
        assert(rt_game_scene_get_tile(encoded_scene, 0, 1, 1) == 4);
    }

    // ADR 0144: infinite layers are flattened against their signed chunk
    // union. Negative source coordinates remain available in tiledRuntime.
    write_text(tiled_dir / "infinite.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":true,
"width":0,"height":0,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"chunks","width":0,"height":0,
"chunks":[{"x":-2,"y":-1,"width":2,"height":2,"data":[1,0,0,2]},
          {"x":0,"y":-1,"width":1,"height":2,"data":[3,4]}]}]})json");
    void *infinite_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "infinite.tmj").string().c_str()));
    if (rt_result_is_err(infinite_result))
        std::fprintf(stderr,
                     "infinite import failed: %s\n",
                     to_std(rt_result_unwrap_err_str(infinite_result)).c_str());
    assert(rt_result_is_ok(infinite_result) == 1);
    void *infinite_scene = rt_result_unwrap(infinite_result);
    assert(rt_game_scene_get_width(infinite_scene) == 3);
    assert(rt_game_scene_get_height(infinite_scene) == 2);
    assert(rt_game_scene_get_tile(infinite_scene, 0, 0, 0) == 1);
    assert(rt_game_scene_get_tile(infinite_scene, 0, 2, 0) == 3);
    assert(rt_game_scene_get_tile(infinite_scene, 0, 1, 1) == 2);
    assert(rt_game_scene_get_tile(infinite_scene, 0, 2, 1) == 4);
    std::string infinite_json = scene_json(infinite_scene);
    assert(infinite_json.find("\"originTileX\":-2") != std::string::npos);
    assert(infinite_json.find("\"originTileY\":-1") != std::string::npos);
    void *infinite_tilemap = rt_game_scene_build_tilemap(infinite_scene);
    int64_t infinite_pixel_x = 0;
    int64_t infinite_pixel_y = 0;
    rt_tilemap_tile_to_pixel(infinite_tilemap, 0, 0, &infinite_pixel_x, &infinite_pixel_y);
    assert(infinite_pixel_x == -4 && infinite_pixel_y == -2);

    write_text(tiled_dir / "infinite-empty.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":true,
"width":0,"height":0,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"empty","width":0,"height":0,"chunks":[]}]})json");
    void *empty_infinite_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "infinite-empty.tmj").string().c_str()));
    assert(rt_result_is_ok(empty_infinite_result) == 1);
    void *empty_infinite_scene = rt_result_unwrap(empty_infinite_result);
    assert(rt_game_scene_get_width(empty_infinite_scene) == 1);
    assert(rt_game_scene_get_height(empty_infinite_scene) == 1);
    assert(rt_game_scene_get_tile(empty_infinite_scene, 0, 0, 0) == 0);

    write_text(tiled_dir / "infinite-isometric.tmj",
               R"json({"type":"map","orientation":"isometric","infinite":true,
"width":0,"height":0,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"iso","width":0,"height":0,
"chunks":[{"x":0,"y":0,"width":1,"height":1,"data":[1]}]}]})json");
    void *infinite_iso_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "infinite-isometric.tmj").string().c_str()));
    assert(rt_result_is_ok(infinite_iso_result) == 1);
    void *infinite_iso_tilemap = rt_game_scene_build_tilemap(rt_result_unwrap(infinite_iso_result));
    int64_t infinite_iso_x = -1;
    int64_t infinite_iso_y = -1;
    rt_tilemap_tile_to_pixel(infinite_iso_tilemap, 0, 0, &infinite_iso_x, &infinite_iso_y);
    assert(infinite_iso_x == 0 && infinite_iso_y == 0);

    write_text(tiled_dir / "infinite.tmx",
               R"xml(<?xml version="1.0" encoding="UTF-8"?>
<map version="1.10" orientation="orthogonal" infinite="1"
     width="0" height="0" tilewidth="2" tileheight="2">
  <tileset firstgid="1" source="terrain.tsx"/>
  <layer id="1" name="chunks" width="0" height="0">
    <data encoding="csv">
      <chunk x="-1" y="-1" width="2" height="1">1,2</chunk>
      <chunk x="-1" y="0" width="2" height="1">3,4</chunk>
    </data>
  </layer>
</map>)xml");
    void *infinite_tmx_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "infinite.tmx").string().c_str()));
    assert(rt_result_is_ok(infinite_tmx_result) == 1);
    void *infinite_tmx_scene = rt_result_unwrap(infinite_tmx_result);
    assert(rt_game_scene_get_width(infinite_tmx_scene) == 2);
    assert(rt_game_scene_get_height(infinite_tmx_scene) == 2);
    assert(rt_game_scene_get_tile(infinite_tmx_scene, 0, 0, 0) == 1);
    assert(rt_game_scene_get_tile(infinite_tmx_scene, 0, 1, 1) == 4);
    assert(scene_json(infinite_tmx_scene).find("\"originTileX\":-1") != std::string::npos);

    std::vector<uint8_t> chunk_payload = zlib_stored(tiled_gid_bytes({2, 3}));
    write_text(tiled_dir / "infinite-encoded.tmj",
               std::string("{\"type\":\"map\",\"orientation\":\"orthogonal\","
                           "\"infinite\":true,\"width\":0,\"height\":0,"
                           "\"tilewidth\":2,\"tileheight\":2,"
                           "\"tilesets\":[{\"firstgid\":1,\"source\":\"terrain.tsj\"}],"
                           "\"layers\":[{\"type\":\"tilelayer\",\"name\":\"encoded\","
                           "\"width\":0,\"height\":0,\"encoding\":\"base64\","
                           "\"compression\":\"zlib\",\"chunks\":[{\"x\":-3,\"y\":4,"
                           "\"width\":2,\"height\":1,\"data\":\"") +
                   base64(chunk_payload) + "\"}]}]}");
    void *encoded_chunk_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "infinite-encoded.tmj").string().c_str()));
    assert(rt_result_is_ok(encoded_chunk_result) == 1);
    void *encoded_chunk_scene = rt_result_unwrap(encoded_chunk_result);
    assert(rt_game_scene_get_width(encoded_chunk_scene) == 2);
    assert(rt_game_scene_get_height(encoded_chunk_scene) == 1);
    assert(rt_game_scene_get_tile(encoded_chunk_scene, 0, 0, 0) == 2);
    assert(rt_game_scene_get_tile(encoded_chunk_scene, 0, 1, 0) == 3);

    write_text(tiled_dir / "overlapping-chunks.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":true,
"width":0,"height":0,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"overlap","width":0,"height":0,
"chunks":[{"x":0,"y":0,"width":2,"height":1,"data":[1,2]},
          {"x":1,"y":0,"width":1,"height":1,"data":[3]}]}]})json");
    void *overlap_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "overlapping-chunks.tmj").string().c_str()));
    assert(rt_result_is_err(overlap_result) == 1);
    assert(to_std(rt_result_unwrap_err_str(overlap_result)).find("overlap") != std::string::npos);

    write_text(tiled_dir / "overflowing-chunk.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":true,
"width":0,"height":0,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"overflow","width":0,"height":0,
"chunks":[{"x":9223372036854775807,"y":0,"width":2,"height":1,"data":[1,2]}]}]})json");
    void *overflow_chunk_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "overflowing-chunk.tmj").string().c_str()));
    assert(rt_result_is_err(overflow_chunk_result) == 1);

    struct ProjectionCase {
        const char *name;
        const char *orientation;
        const char *extra;
        int64_t tile_x;
        int64_t tile_y;
        int64_t pixel_x;
        int64_t pixel_y;
    };

    const ProjectionCase projection_cases[] = {
        {"isometric", "isometric", "", 1, 0, 3, 1},
        {"staggered-y-odd",
         "staggered",
         ",\"staggeraxis\":\"y\",\"staggerindex\":\"odd\"",
         0,
         1,
         1,
         1},
        {"staggered-x-even",
         "staggered",
         ",\"staggeraxis\":\"x\",\"staggerindex\":\"even\"",
         0,
         0,
         0,
         1},
        {"hexagonal-x-even",
         "hexagonal",
         ",\"staggeraxis\":\"x\",\"staggerindex\":\"even\",\"hexsidelength\":1",
         0,
         0,
         0,
         1},
        {"hexagonal-y-odd",
         "hexagonal",
         ",\"staggeraxis\":\"y\",\"staggerindex\":\"odd\",\"hexsidelength\":1",
         0,
         1,
         1,
         1},
        {"oblique", "oblique", ",\"skewx\":1.0,\"skewy\":-0.5", 1, 1, 3, 2},
    };
    for (const ProjectionCase &projection_case : projection_cases) {
        std::string projected = std::string("{\"type\":\"map\",\"orientation\":\"") +
                                projection_case.orientation +
                                "\",\"infinite\":false,\"width\":2,\"height\":2,"
                                "\"tilewidth\":2,\"tileheight\":2" +
                                projection_case.extra +
                                ",\"tilesets\":[{\"firstgid\":1,\"source\":\"terrain.tsj\"}],"
                                "\"layers\":[{\"type\":\"tilelayer\",\"name\":\"projected\","
                                "\"width\":2,\"height\":2,\"data\":[1,2,3,4]}]}";
        std::filesystem::path projected_path =
            tiled_dir / (std::string(projection_case.name) + ".tmj");
        write_text(projected_path, projected);
        void *projected_result =
            rt_game_scene_import_tiled_result(rt_const_cstr(projected_path.string().c_str()));
        if (rt_result_is_err(projected_result))
            std::fprintf(stderr,
                         "%s import failed: %s\n",
                         projection_case.orientation,
                         to_std(rt_result_unwrap_err_str(projected_result)).c_str());
        assert(rt_result_is_ok(projected_result) == 1);
        std::string projected_json = scene_json(rt_result_unwrap(projected_result));
        assert(projected_json.find(std::string("\"orientation\":\"") + projection_case.orientation +
                                   "\"") != std::string::npos);
        void *projected_tilemap = rt_game_scene_build_tilemap(rt_result_unwrap(projected_result));
        int64_t projected_x = 0;
        int64_t projected_y = 0;
        rt_tilemap_tile_to_pixel(projected_tilemap,
                                 projection_case.tile_x,
                                 projection_case.tile_y,
                                 &projected_x,
                                 &projected_y);
        assert(projected_x == projection_case.pixel_x && projected_y == projection_case.pixel_y);
        void *reloaded_projected = load_text(projected_json);
        void *roundtrip_tilemap = rt_game_scene_build_tilemap(reloaded_projected);
        rt_tilemap_tile_to_pixel(roundtrip_tilemap,
                                 projection_case.tile_x,
                                 projection_case.tile_y,
                                 &projected_x,
                                 &projected_y);
        assert(projected_x == projection_case.pixel_x && projected_y == projection_case.pixel_y);
    }

    write_text(tiled_dir / "fractional-image.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"imagelayer","name":"fractional","image":"tiles.png",
"offsetx":1.5,"offsety":-1.5}]})json");
    void *fractional_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "fractional-image.tmj").string().c_str()));
    assert(rt_result_is_ok(fractional_result) == 1);
    void *fractional_scene = rt_result_unwrap(fractional_result);
    assert(rt_game_scene_object_count(fractional_scene) == 1);
    assert(rt_game_scene_object_x(fractional_scene, 0) == 2);
    assert(rt_game_scene_object_y(fractional_scene, 0) == -2);
    assert(rt_game_scene_object_get_float(
               fractional_scene, 0, rt_const_cstr("tiled.sourceX"), 0.0) == 1.5);
    assert(rt_game_scene_object_get_float(
               fractional_scene, 0, rt_const_cstr("tiled.sourceY"), 0.0) == -1.5);

    // The asset-mode entry points must resolve the entire map/tileset/template/
    // image graph from a mounted package after loose-file paths are irrelevant.
    std::filesystem::path tiled_pack = tiled_dir.parent_path() / "zanna_tiled_import_test.zpak";
    zanna::asset::ZpakWriter tiled_writer;
    auto add_text_entry = [&](const char *name, const std::filesystem::path &path) {
        std::string value = read_text(path);
        tiled_writer.addEntry(
            name, reinterpret_cast<const uint8_t *>(value.data()), value.size(), false);
    };
    add_text_entry("maps/level.tmj", tiled_dir / "level.tmj");
    add_text_entry("maps/terrain.tsj", tiled_dir / "terrain.tsj");
    add_text_entry("maps/spawn.tj", tiled_dir / "spawn.tj");
    const std::string unsafe_asset_map =
        R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"../../terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"bad","width":1,"height":1,"data":[1]}]})json";
    tiled_writer.addEntry("maps/unsafe.tmj",
                          reinterpret_cast<const uint8_t *>(unsafe_asset_map.data()),
                          unsafe_asset_map.size(),
                          false);
    std::vector<uint8_t> packed_png = read_bytes(tiled_png);
    tiled_writer.addEntry("maps/tiles.png", packed_png.data(), packed_png.size(), false);
    std::string pack_error;
    assert(tiled_writer.writeToFile(tiled_pack.string(), pack_error));
    assert(rt_asset_mount(rt_const_cstr(tiled_pack.string().c_str())) == 1);
    void *asset_scene_result =
        rt_game_scene_import_tiled_asset_result(rt_const_cstr("maps/level.tmj"));
    assert(rt_result_is_ok(asset_scene_result) == 1);
    void *asset_scene = rt_result_unwrap(asset_scene_result);
    assert(rt_game_scene_get_tile(asset_scene, 0, 2, 1) == 4);
    assert(to_std(rt_game_scene_object_type(asset_scene, 1)) == "Enemy");
    void *asset_map_result =
        rt_tiledmaploader_load_asset_result(tiled_loader, rt_const_cstr("maps/level.tmj"));
    assert(rt_result_is_ok(asset_map_result) == 1);
    assert(rt_tilemap_get_tile_count(rt_result_unwrap(asset_map_result)) == 4);
    void *unsafe_asset_result =
        rt_game_scene_import_tiled_asset_result(rt_const_cstr("maps/unsafe.tmj"));
    assert(rt_result_is_err(unsafe_asset_result) == 1);
    assert(to_std(rt_result_unwrap_err_str(unsafe_asset_result)).find("escapes") !=
           std::string::npos);
    assert(rt_asset_unmount(rt_const_cstr(tiled_pack.string().c_str())) == 1);
    std::filesystem::remove(tiled_pack);

    void *wrong_loader_result = rt_tiledmaploader_load_result(
        tiled_pixels, rt_const_cstr((tiled_dir / "level.tmj").string().c_str()));
    assert(rt_result_is_err(wrong_loader_result) == 1);

    write_text(tiled_dir / "flipped.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"bad","width":1,"height":1,
"data":[2147483649]}]})json");
    void *flipped_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "flipped.tmj").string().c_str()));
    assert(rt_result_is_ok(flipped_result) == 1);
    void *flipped_scene = rt_result_unwrap(flipped_result);
    int64_t flipped_id = rt_game_scene_get_tile(flipped_scene, 0, 0, 0);
    assert(flipped_id > 4);

    write_text(tiled_dir / "mixed.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":2,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"},
            {"firstgid":5,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"mixed","width":2,"height":1,"data":[1,5]}]})json");
    void *mixed_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "mixed.tmj").string().c_str()));
    assert(rt_result_is_ok(mixed_result) == 1);
    void *mixed_scene = rt_result_unwrap(mixed_result);
    assert(rt_game_scene_get_tile(mixed_scene, 0, 0, 0) == 1);
    assert(rt_game_scene_get_tile(mixed_scene, 0, 1, 0) == 5);
    void *mixed_map_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "mixed.tmj").string().c_str()));
    assert(rt_result_is_ok(mixed_map_result) == 1);
    assert(rt_tilemap_get_tile_count(rt_result_unwrap(mixed_map_result)) == 8);

    void *transform_pixels = rt_pixels_new(2, 2);
    rt_pixels_set(transform_pixels, 0, 0, 0xff0000ff);
    rt_pixels_set(transform_pixels, 1, 0, 0x00ff00ff);
    rt_pixels_set(transform_pixels, 0, 1, 0x0000ffff);
    rt_pixels_set(transform_pixels, 1, 1, 0xffffffff);
    assert(rt_pixels_save_png(transform_pixels,
                              rt_const_cstr((tiled_dir / "transform.png").string().c_str())) == 1);
    write_text(tiled_dir / "transform.tsj",
               R"json({"type":"tileset","name":"transform","tilewidth":2,"tileheight":2,
"tilecount":1,"columns":1,"image":"transform.png","imagewidth":2,"imageheight":2})json");
    write_text(tiled_dir / "transform.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"transform.tsj"}],
"layers":[{"type":"tilelayer","name":"flipped","width":1,"height":1,
"data":[2147483649]}]})json");
    void *transform_map_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "transform.tmj").string().c_str()));
    assert(rt_result_is_ok(transform_map_result) == 1);
    void *transform_canvas = rt_canvas_new(rt_const_cstr("transform-test"), 2, 2);
    rt_tilemap_draw(rt_result_unwrap(transform_map_result), transform_canvas, 0, 0);
    assert(rt_canvas_get_pixel(transform_canvas, 0, 0) == 0x00ff00);
    assert(rt_canvas_get_pixel(transform_canvas, 1, 0) == 0xff0000);
    assert(rt_canvas_get_pixel(transform_canvas, 0, 1) == 0xffffff);
    assert(rt_canvas_get_pixel(transform_canvas, 1, 1) == 0x0000ff);

    write_text(tiled_dir / "diagonal.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"transform.tsj"}],
"layers":[{"type":"tilelayer","name":"diagonal","width":1,"height":1,
"data":[536870913]}]})json");
    void *diagonal_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "diagonal.tmj").string().c_str()));
    assert(rt_result_is_ok(diagonal_result) == 1);
    void *diagonal_canvas = rt_canvas_new(rt_const_cstr("diagonal-test"), 2, 2);
    rt_tilemap_draw(rt_result_unwrap(diagonal_result), diagonal_canvas, 0, 0);
    assert(rt_canvas_get_pixel(diagonal_canvas, 0, 0) == 0xffffff);
    assert(rt_canvas_get_pixel(diagonal_canvas, 1, 0) == 0x00ff00);
    assert(rt_canvas_get_pixel(diagonal_canvas, 0, 1) == 0x0000ff);
    assert(rt_canvas_get_pixel(diagonal_canvas, 1, 1) == 0xff0000);

    struct TransformCase {
        const char *name;
        uint32_t gid;
        uint32_t expected[4];
    };

    const TransformCase transform_cases[] = {
        {"vertical", UINT32_C(1073741825), {0x0000ff, 0xffffff, 0xff0000, 0x00ff00}},
        {"diagonal-horizontal", UINT32_C(2684354561), {0x00ff00, 0xffffff, 0xff0000, 0x0000ff}},
        {"diagonal-vertical", UINT32_C(1610612737), {0x0000ff, 0xff0000, 0xffffff, 0x00ff00}},
        {"diagonal-both", UINT32_C(3758096385), {0xff0000, 0x0000ff, 0x00ff00, 0xffffff}},
    };
    for (const TransformCase &transform_case : transform_cases) {
        std::filesystem::path transform_path =
            tiled_dir / (std::string(transform_case.name) + ".tmj");
        write_text(transform_path,
                   std::string("{\"type\":\"map\",\"orientation\":\"orthogonal\","
                               "\"infinite\":false,\"width\":1,\"height\":1,"
                               "\"tilewidth\":2,\"tileheight\":2,"
                               "\"tilesets\":[{\"firstgid\":1,"
                               "\"source\":\"transform.tsj\"}],"
                               "\"layers\":[{\"type\":\"tilelayer\","
                               "\"name\":\"transform\",\"width\":1,\"height\":1,"
                               "\"data\":[") +
                       std::to_string(transform_case.gid) + "]}]}");
        void *result = rt_tiledmaploader_load_result(
            tiled_loader, rt_const_cstr(transform_path.string().c_str()));
        assert(rt_result_is_ok(result) == 1);
        void *canvas = rt_canvas_new(rt_const_cstr(transform_case.name), 2, 2);
        rt_tilemap_draw(rt_result_unwrap(result), canvas, 0, 0);
        for (int64_t y = 0; y < 2; ++y) {
            for (int64_t x = 0; x < 2; ++x) {
                assert(static_cast<uint32_t>(rt_canvas_get_pixel(canvas, x, y)) ==
                       transform_case.expected[y * 2 + x]);
            }
        }
    }

    write_text(tiled_dir / "ignored-bit.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"ignored","width":1,"height":1,
"data":[268435457]}]})json");
    void *ignored_bit_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "ignored-bit.tmj").string().c_str()));
    assert(rt_result_is_ok(ignored_bit_result) == 1);
    assert(rt_game_scene_get_tile(rt_result_unwrap(ignored_bit_result), 0, 0, 0) == 1);

    void *white_pixel = rt_pixels_new(1, 1);
    rt_pixels_fill(white_pixel, 0xffffffff);
    assert(rt_pixels_save_png(white_pixel,
                              rt_const_cstr((tiled_dir / "white.png").string().c_str())) == 1);
    write_text(tiled_dir / "white.tsj",
               R"json({"type":"tileset","name":"white","tilewidth":1,"tileheight":1,
"tilecount":1,"columns":1,"image":"white.png","imagewidth":1,"imageheight":1})json");
    write_text(tiled_dir / "styled.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":1,"tileheight":1,
"tilesets":[{"firstgid":1,"source":"white.tsj"}],
"layers":[{"type":"group","name":"style","opacity":0.5,"tintcolor":"#80ffff",
"parallaxx":0.5,"layers":[{"type":"tilelayer","name":"tile","width":1,"height":1,
"offsetx":1.5,"offsety":1.5,"tintcolor":"#ff80ff","data":[1]}]}]})json");
    void *styled_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "styled.tmj").string().c_str()));
    assert(rt_result_is_ok(styled_result) == 1);
    void *styled_canvas = rt_canvas_new(rt_const_cstr("styled-test"), 4, 4);
    rt_tilemap_draw(rt_result_unwrap(styled_result), styled_canvas, 0, 0);
    void *styled_copy = rt_canvas_copy_rect(styled_canvas, 2, 2, 1, 1);
    assert(static_cast<uint32_t>(rt_pixels_get(styled_copy, 0, 0)) == UINT32_C(0x8080ff80));

    write_text(tiled_dir / "object-gid.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"objectgroup","name":"objects","objects":[
{"id":42,"name":"tile-object","gid":2147483649,"x":0,"y":2}]}]})json");
    void *object_gid_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "object-gid.tmj").string().c_str()));
    assert(rt_result_is_ok(object_gid_result) == 1);
    void *object_gid_scene = rt_result_unwrap(object_gid_result);
    assert(rt_game_scene_object_get_int(object_gid_scene, 0, rt_const_cstr("tiled.rawGid"), 0) ==
           2147483649LL);
    assert(rt_game_scene_object_get_int(object_gid_scene, 0, rt_const_cstr("tiled.gidFlags"), 0) ==
           2147483648LL);

    write_text(tiled_dir / "hex-rotation.tmj",
               R"json({"type":"map","orientation":"hexagonal","infinite":false,
"width":1,"height":1,"tilewidth":1,"tileheight":1,"staggeraxis":"x",
"staggerindex":"odd","hexsidelength":1,
"tilesets":[{"firstgid":1,"source":"white.tsj"}],
"layers":[{"type":"tilelayer","name":"hex","width":1,"height":1,
"data":[805306369]}]})json");
    void *hex_rotation_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "hex-rotation.tmj").string().c_str()));
    assert(rt_result_is_ok(hex_rotation_result) == 1);

    void *hex_pixels = rt_pixels_new(3, 3);
    const uint32_t hex_source[] = {0xff0000ff,
                                   0x00ff00ff,
                                   0x0000ffff,
                                   0x00ffffff,
                                   0xff00ffff,
                                   0xffff00ff,
                                   0x101010ff,
                                   0xffffffff,
                                   0x804000ff};
    for (int64_t y = 0; y < 3; ++y) {
        for (int64_t x = 0; x < 3; ++x)
            rt_pixels_set(hex_pixels, x, y, hex_source[y * 3 + x]);
    }
    assert(rt_pixels_save_png(hex_pixels,
                              rt_const_cstr((tiled_dir / "hex-source.png").string().c_str())) == 1);
    write_text(tiled_dir / "hex-source.tsj",
               R"json({"type":"tileset","name":"hex-source","tilewidth":3,"tileheight":3,
"tilecount":1,"columns":1,"image":"hex-source.png","imagewidth":3,"imageheight":3})json");

    struct HexRotationCase {
        const char *name;
        uint32_t gid;
        uint32_t expected[25];
    };

    const HexRotationCase hex_rotation_cases[] = {
        {"hex-60",
         UINT32_C(536870913),
         {0,        0, 0,        0,        0,        0, 0x00ffff, 0xff0000, 0x00ff00,
          0,        0, 0x101010, 0xff00ff, 0x0000ff, 0, 0,        0xffffff, 0x804000,
          0xffff00, 0, 0,        0,        0,        0, 0}},
        {"hex-120",
         UINT32_C(268435457),
         {0,        0, 0,        0,        0,        0, 0xffffff, 0x00ffff, 0x00ffff,
          0,        0, 0xffffff, 0xff00ff, 0x00ff00, 0, 0,        0xffff00, 0xffff00,
          0x00ff00, 0, 0,        0,        0,        0, 0}},
    };
    for (const HexRotationCase &rotation_case : hex_rotation_cases) {
        std::filesystem::path rotation_path =
            tiled_dir / (std::string(rotation_case.name) + ".tmj");
        write_text(rotation_path,
                   std::string("{\"type\":\"map\",\"orientation\":\"hexagonal\","
                               "\"infinite\":false,\"width\":1,\"height\":1,"
                               "\"tilewidth\":3,\"tileheight\":3,"
                               "\"staggeraxis\":\"x\",\"staggerindex\":\"odd\","
                               "\"hexsidelength\":1,\"tilesets\":[{\"firstgid\":1,"
                               "\"source\":\"hex-source.tsj\"}],"
                               "\"layers\":[{\"type\":\"tilelayer\",\"name\":\"hex\","
                               "\"width\":1,\"height\":1,\"data\":[") +
                       std::to_string(rotation_case.gid) + "]}]}");
        void *result = rt_tiledmaploader_load_result(tiled_loader,
                                                     rt_const_cstr(rotation_path.string().c_str()));
        assert(rt_result_is_ok(result) == 1);
        void *canvas = rt_canvas_new(rt_const_cstr(rotation_case.name), 5, 5);
        rt_tilemap_draw(rt_result_unwrap(result), canvas, 1, 1);
        for (int64_t y = 0; y < 5; ++y) {
            for (int64_t x = 0; x < 5; ++x) {
                assert(static_cast<uint32_t>(rt_canvas_get_pixel(canvas, x, y)) ==
                       rotation_case.expected[y * 5 + x]);
            }
        }
    }

    void *collection_a = rt_pixels_new(2, 3);
    void *collection_b = rt_pixels_new(3, 1);
    rt_pixels_fill(collection_a, 0xff00ffff);
    rt_pixels_fill(collection_b, 0xffff00ff);
    assert(rt_pixels_save_png(collection_a,
                              rt_const_cstr((tiled_dir / "collection-a.png").string().c_str())) ==
           1);
    assert(rt_pixels_save_png(collection_b,
                              rt_const_cstr((tiled_dir / "collection-b.png").string().c_str())) ==
           1);
    write_text(tiled_dir / "collection.tsj",
               R"json({"type":"tileset","name":"collection","tilewidth":2,"tileheight":2,
"tilecount":2,"columns":0,"tileoffset":{"x":1,"y":-1},
"tiles":[{"id":0,"image":"collection-a.png","imagewidth":2,"imageheight":3},
         {"id":1,"image":"collection-b.png","imagewidth":3,"imageheight":1}]})json");
    write_text(tiled_dir / "collection.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":2,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"collection.tsj"}],
"layers":[{"type":"tilelayer","name":"collection","width":2,"height":1,
"data":[1,2]}]})json");
    void *collection_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "collection.tmj").string().c_str()));
    assert(rt_result_is_ok(collection_result) == 1);
    void *collection_map = rt_result_unwrap(collection_result);
    assert(rt_tilemap_get_tile_count(collection_map) == 2);
    void *collection_canvas = rt_canvas_new(rt_const_cstr("collection-placement"), 8, 6);
    rt_tilemap_draw(collection_map, collection_canvas, 0, 2);
    assert(rt_canvas_get_pixel(collection_canvas, 0, 0) == 0);
    assert(rt_canvas_get_pixel(collection_canvas, 1, 0) == 0xff00ff);
    assert(rt_canvas_get_pixel(collection_canvas, 3, 2) == 0xffff00);
    void *collection_scene_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "collection.tmj").string().c_str()));
    assert(rt_result_is_ok(collection_scene_result) == 1);
    std::string collection_json = scene_json(rt_result_unwrap(collection_scene_result));
    assert(collection_json.find("collection-a.png") != std::string::npos);
    assert(collection_json.find("collection-b.png") != std::string::npos);

    write_text(tiled_dir / "collection.tsx",
               R"xml(<?xml version="1.0" encoding="UTF-8"?>
<tileset version="1.10" name="collection" tilewidth="2" tileheight="2"
         tilecount="2" columns="0">
  <tileoffset x="1" y="-1"/>
  <tile id="0"><image source="collection-a.png" width="2" height="3"/></tile>
  <tile id="1"><image source="collection-b.png" width="3" height="1"/></tile>
</tileset>)xml");
    write_text(tiled_dir / "collection.tmx",
               R"xml(<?xml version="1.0" encoding="UTF-8"?>
<map version="1.10" orientation="orthogonal" infinite="0"
     width="2" height="1" tilewidth="2" tileheight="2">
  <tileset firstgid="1" source="collection.tsx"/>
  <layer id="1" name="collection" width="2" height="1">
    <data encoding="csv">1,2</data>
  </layer>
</map>)xml");
    void *collection_tmx_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "collection.tmx").string().c_str()));
    assert(rt_result_is_ok(collection_tmx_result) == 1);
    assert(rt_tilemap_get_tile_count(rt_result_unwrap(collection_tmx_result)) == 2);

    write_text(tiled_dir / "missing-collection.tsj",
               R"json({"type":"tileset","name":"missing","tilewidth":1,"tileheight":1,
"tilecount":2,"columns":0,"tiles":[
{"id":0,"image":"white.png","imagewidth":1,"imageheight":1},
{"id":1,"image":"does-not-exist.png","imagewidth":1,"imageheight":1}]})json");
    write_text(tiled_dir / "unreachable-missing.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":1,"tileheight":1,
"tilesets":[{"firstgid":1,"source":"missing-collection.tsj"}],
"layers":[{"type":"tilelayer","name":"reachable-zero","width":1,"height":1,
"data":[1]}]})json");
    void *unreachable_missing_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "unreachable-missing.tmj").string().c_str()));
    assert(rt_result_is_ok(unreachable_missing_result) == 1);
    assert(rt_tilemap_get_tile_count(rt_result_unwrap(unreachable_missing_result)) == 2);
    write_text(tiled_dir / "reachable-missing.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":1,"tileheight":1,
"tilesets":[{"firstgid":1,"source":"missing-collection.tsj"}],
"layers":[{"type":"tilelayer","name":"missing","width":1,"height":1,"data":[2]}]})json");
    void *reachable_missing_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "reachable-missing.tmj").string().c_str()));
    assert(rt_result_is_err(reachable_missing_result) == 1);

    write_text(tiled_dir / "offset-a.tsj",
               R"json({"type":"tileset","name":"a","tilewidth":1,"tileheight":1,
"tilecount":1,"columns":1,"image":"white.png","imagewidth":1,"imageheight":1})json");
    write_text(tiled_dir / "offset-b.tsj",
               R"json({"type":"tileset","name":"b","tilewidth":1,"tileheight":1,
"tilecount":1,"columns":1,"tileoffset":{"x":70000000,"y":0},
"image":"white.png","imagewidth":1,"imageheight":1})json");
    write_text(tiled_dir / "oversized-atlas.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":2,"height":1,"tilewidth":1,"tileheight":1,
"tilesets":[{"firstgid":1,"source":"offset-a.tsj"},
            {"firstgid":2,"source":"offset-b.tsj"}],
"layers":[{"type":"tilelayer","name":"huge-frame","width":2,"height":1,
"data":[1,2]}]})json");
    void *oversized_atlas_result = rt_tiledmaploader_load_result(
        tiled_loader, rt_const_cstr((tiled_dir / "oversized-atlas.tmj").string().c_str()));
    assert(rt_result_is_err(oversized_atlas_result) == 1);
    assert(to_std(rt_result_unwrap_err_str(oversized_atlas_result)).find("256 MiB") !=
           std::string::npos);

    write_text(tiled_dir / "bad-base64.tmj",
               R"json({"type":"map","orientation":"orthogonal","infinite":false,
"width":1,"height":1,"tilewidth":2,"tileheight":2,
"tilesets":[{"firstgid":1,"source":"terrain.tsj"}],
"layers":[{"type":"tilelayer","name":"bad","width":1,"height":1,
"encoding":"base64","data":"AA=="}]})json");
    void *bad_base64_result = rt_game_scene_import_tiled_result(
        rt_const_cstr((tiled_dir / "bad-base64.tmj").string().c_str()));
    assert(rt_result_is_err(bad_base64_result) == 1);
    assert(to_std(rt_result_unwrap_err_str(bad_base64_result)).find("wrong byte length") !=
           std::string::npos);
    assert(rt_game_scene_import_tiled(
               rt_const_cstr((tiled_dir / "does-not-exist.tmj").string().c_str())) == nullptr);
    std::filesystem::remove_all(tiled_dir);

    void *tilemap = rt_tilemap_new(10, 10, 16, 16);
    rt_tilemap_set_tile(tilemap, 4, 3, 9);
    void *hit = rt_tilemap_hit_test_scaled(tilemap, 40, 30, -24, -18, 100);
    assert(rt_map_get_bool(hit, rt_const_cstr("inBounds")) == 1);
    assert(rt_map_get_int(hit, rt_const_cstr("tileX")) == 4);
    assert(rt_map_get_int(hit, rt_const_cstr("tileY")) == 3);
    assert(rt_map_get_int(hit, rt_const_cstr("tile")) == 9);
    hit = rt_tilemap_hit_test_scaled(tilemap, 16, 16, 0, 0, 200);
    assert(rt_map_get_int(hit, rt_const_cstr("tileX")) == 0);
    assert(rt_map_get_int(hit, rt_const_cstr("tileY")) == 0);

    void *projected_tilemap = rt_tilemap_new(2, 2, 16, 8);
    assert(rt_tilemap_configure_import_layout(projected_tilemap,
                                              RT_TILEMAP_IMPORT_ISOMETRIC,
                                              0,
                                              0,
                                              24,
                                              16,
                                              -4,
                                              -8,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              2) == 1);
    assert(rt_tilemap_configure_import_layer(projected_tilemap, 0, 1.5, -1.5, 1.0, 1.0) == 1);
    rt_tilemap_set_tile(projected_tilemap, 1, 0, 17);
    hit = rt_tilemap_hit_test_scaled(projected_tilemap, 53, 9, 10, 20, 200);
    assert(rt_map_get_bool(hit, rt_const_cstr("inBounds")) == 1);
    assert(rt_map_get_int(hit, rt_const_cstr("tileX")) == 1);
    assert(rt_map_get_int(hit, rt_const_cstr("tileY")) == 0);
    assert(rt_map_get_int(hit, rt_const_cstr("tile")) == 17);

    void *hex_hit_map = rt_tilemap_new(3, 3, 10, 8);
    assert(rt_tilemap_configure_import_layout(hex_hit_map,
                                              RT_TILEMAP_IMPORT_HEXAGONAL,
                                              0,
                                              0,
                                              10,
                                              8,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              2,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              3) == 1);
    rt_tilemap_set_tile(hex_hit_map, 1, 1, 23);
    hit = rt_tilemap_hit_test_scaled(hex_hit_map, 20, 9, 0, 0, 100);
    assert(rt_map_get_int(hit, rt_const_cstr("tileX")) == 1);
    assert(rt_map_get_int(hit, rt_const_cstr("tileY")) == 1);
    assert(rt_map_get_int(hit, rt_const_cstr("tile")) == 23);

    void *staggered_hit_map = rt_tilemap_new(3, 3, 10, 8);
    assert(rt_tilemap_configure_import_layout(staggered_hit_map,
                                              RT_TILEMAP_IMPORT_STAGGERED,
                                              0,
                                              0,
                                              10,
                                              8,
                                              0,
                                              0,
                                              RT_TILEMAP_IMPORT_RIGHT_DOWN,
                                              1,
                                              0,
                                              0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              0.0,
                                              3) == 1);
    rt_tilemap_set_tile(staggered_hit_map, 1, 1, 29);
    hit = rt_tilemap_hit_test_scaled(staggered_hit_map, 20, 8, 0, 0, 100);
    assert(rt_map_get_int(hit, rt_const_cstr("tileX")) == 1);
    assert(rt_map_get_int(hit, rt_const_cstr("tileY")) == 1);
    assert(rt_map_get_int(hit, rt_const_cstr("tile")) == 29);

    // ADR 0192: optional object transforms. Defaults serialize nothing, so
    // an untransformed document's bytes are identical before and after the
    // fields existed; authored values round-trip with sanitization.
    {
        void *transform_scene = rt_game_scene_new(4, 3, 16, 16);
        int64_t plain =
            rt_game_scene_add_object(transform_scene, rt_const_cstr("entity"),
                                     rt_const_cstr("plain"), 5, 6);
        assert(plain == 0);
        assert(rt_game_scene_object_rotation(transform_scene, plain) == 0.0);
        assert(rt_game_scene_object_scale_x(transform_scene, plain) == 1.0);
        assert(rt_game_scene_object_scale_y(transform_scene, plain) == 1.0);
        assert(rt_game_scene_object_flip_x(transform_scene, plain) == 0);
        assert(rt_game_scene_object_flip_y(transform_scene, plain) == 0);
        assert(rt_game_scene_object_tint(transform_scene, plain) == 0xFFFFFFFF);
        assert(rt_game_scene_object_pivot_x(transform_scene, plain) == 0.5);
        assert(rt_game_scene_object_pivot_y(transform_scene, plain) == 0.5);
        std::string default_json = scene_json(transform_scene);
        assert(default_json.find("rotation") == std::string::npos);
        assert(default_json.find("scaleX") == std::string::npos);
        assert(default_json.find("flipX") == std::string::npos);
        assert(default_json.find("tint") == std::string::npos);
        assert(default_json.find("pivotX") == std::string::npos);
        // Round-tripping the default document keeps its exact bytes.
        void *default_reload = load_text(default_json);
        assert(scene_json(default_reload) == default_json);

        rt_game_scene_set_object_rotation(transform_scene, plain, -90.0);
        assert(rt_game_scene_object_rotation(transform_scene, plain) == 270.0);
        rt_game_scene_set_object_scale(transform_scene, plain, 2.0, 0.0);
        assert(rt_game_scene_object_scale_x(transform_scene, plain) == 2.0);
        assert(rt_game_scene_object_scale_y(transform_scene, plain) == 1.0);
        rt_game_scene_set_object_scale(transform_scene, plain, 2.0, -3.5);
        rt_game_scene_set_object_flip(transform_scene, plain, 1, 0);
        rt_game_scene_set_object_tint(transform_scene, plain,
                                      static_cast<int64_t>(0x11223344));
        rt_game_scene_set_object_pivot(transform_scene, plain, -0.5, 2.0);
        assert(rt_game_scene_object_pivot_x(transform_scene, plain) == 0.0);
        assert(rt_game_scene_object_pivot_y(transform_scene, plain) == 1.0);

        std::string authored_json = scene_json(transform_scene);
        void *authored_reload = load_text(authored_json);
        assert(authored_reload);
        assert(rt_game_scene_object_rotation(authored_reload, 0) == 270.0);
        assert(rt_game_scene_object_scale_x(authored_reload, 0) == 2.0);
        assert(rt_game_scene_object_scale_y(authored_reload, 0) == -3.5);
        assert(rt_game_scene_object_flip_x(authored_reload, 0) == 1);
        assert(rt_game_scene_object_flip_y(authored_reload, 0) == 0);
        assert(rt_game_scene_object_tint(authored_reload, 0) == 0x11223344);
        assert(rt_game_scene_object_pivot_x(authored_reload, 0) == 0.0);
        assert(rt_game_scene_object_pivot_y(authored_reload, 0) == 1.0);
        // The authored document round-trips byte-stably too.
        assert(scene_json(authored_reload) == authored_json);

        // Duplication copies the transform; transform keys never leak into
        // the property bag.
        int64_t copy = rt_game_scene_duplicate_object(
            transform_scene, plain, rt_const_cstr("copy"));
        assert(copy == 1);
        assert(rt_game_scene_object_rotation(transform_scene, copy) == 270.0);
        assert(rt_game_scene_object_flip_x(transform_scene, copy) == 1);
        rt_string rotation_key = rt_const_cstr("rotation");
        assert(rt_game_scene_object_has(transform_scene, plain, rotation_key) == 0);
        rt_string_unref(rotation_key);
    }
    return 0;
}
