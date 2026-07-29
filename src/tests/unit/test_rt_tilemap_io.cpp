//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_tilemap_io.cpp
// Purpose: Unit tests for tilemap file I/O, imported-layout persistence, CSV
//   import, auto-tiling, and tile properties.
//
// Key invariants:
//   - JSON round-trip preserves tile data, layer structure, imported projection,
//     source-frame geometry, parallax, and exact composed-atlas tile count.
//   - CSV import produces correct tilemap dimensions and tile values.
//   - Auto-tiling computes correct 4-bit neighbor bitmasks.
//   - Tile properties are stored/retrieved by key.
//
// Ownership/Lifetime:
//   - Uses runtime library. Tilemap objects are GC-managed.
//
// Links: src/runtime/graphics/2d/rt_tilemap.h,
//   docs/adr/0144-complete-tiled-map-import.md
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_internal.h"
#include "rt_pixels.h"
#include "rt_string.h"
#include "rt_tilemap.h"
#include "rt_tilemap_internal.h"
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static int tests_passed = 0;
static int tests_total = 0;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        tests_total++;                                                                             \
        printf("  [%d] %s... ", tests_total, name);                                                \
    } while (0)
#define PASS()                                                                                     \
    do {                                                                                           \
        tests_passed++;                                                                            \
        printf("ok\n");                                                                            \
    } while (0)

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static rt_string make_bytes(const char *bytes, size_t length) {
    return rt_string_from_bytes(bytes, length);
}

static void test_tile_properties(void) {
    TEST("Tile properties set/get/has");
    void *tm = rt_tilemap_new(4, 4, 16, 16);
    rt_string key = make_str("damage");
    rt_tilemap_set_tile_property(tm, 5, key, 10);
    assert(rt_tilemap_get_tile_property(tm, 5, key, 0) == 10);
    assert(rt_tilemap_has_tile_property(tm, 5, key) == 1);

    // Non-existent property returns default
    rt_string other = make_str("speed");
    assert(rt_tilemap_get_tile_property(tm, 5, other, -1) == -1);
    assert(rt_tilemap_has_tile_property(tm, 5, other) == 0);
    PASS();
}

static void test_tile_property_update(void) {
    TEST("Tile property update overwrites");
    void *tm = rt_tilemap_new(4, 4, 16, 16);
    rt_string key = make_str("hp");
    rt_tilemap_set_tile_property(tm, 1, key, 100);
    assert(rt_tilemap_get_tile_property(tm, 1, key, 0) == 100);
    rt_tilemap_set_tile_property(tm, 1, key, 200);
    assert(rt_tilemap_get_tile_property(tm, 1, key, 0) == 200);
    PASS();
}

static void test_tile_property_rejects_embedded_nul_alias(void) {
    TEST("Tile property rejects embedded-NUL key aliases");
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    rt_string key = make_str("damage");
    const char alias_bytes[] = "damage\0alias";
    rt_string alias = make_bytes(alias_bytes, sizeof(alias_bytes) - 1);
    rt_tilemap_set_tile_property(tm, 1, key, 10);
    rt_tilemap_set_tile_property(tm, 1, alias, 99);
    assert(rt_tilemap_get_tile_property(tm, 1, key, 0) == 10);
    assert(rt_tilemap_get_tile_property(tm, 1, alias, -1) == -1);
    assert(rt_tilemap_has_tile_property(tm, 1, alias) == 0);
    PASS();
}

static void test_autotile_basic(void) {
    TEST("Auto-tiling basic neighbor computation");
    void *tm = rt_tilemap_new(5, 1, 16, 16);
    // Place base tile (ID=1) at x=1,2,3 (horizontal strip)
    rt_tilemap_set_tile(tm, 1, 0, 1);
    rt_tilemap_set_tile(tm, 2, 0, 1);
    rt_tilemap_set_tile(tm, 3, 0, 1);

    // Set autotile rules: variant[mask] = 100 + mask
    // So we can check which mask was computed
    rt_tilemap_set_autotile_lo(tm, 1, 100, 101, 102, 103, 104, 105, 106, 107);
    rt_tilemap_set_autotile_hi(tm, 1, 108, 109, 110, 111, 112, 113, 114, 115);

    rt_tilemap_apply_autotile(tm);

    // x=1: right neighbor(2) → mask = 2
    assert(rt_tilemap_get_tile(tm, 1, 0) == 102);
    // x=2: left(8) + right(2) → mask = 10
    assert(rt_tilemap_get_tile(tm, 2, 0) == 110);
    // x=3: left(8) → mask = 8
    assert(rt_tilemap_get_tile(tm, 3, 0) == 108);
    PASS();
}

static void test_autotile_isolated(void) {
    TEST("Auto-tiling isolated cell");
    void *tm = rt_tilemap_new(3, 3, 16, 16);
    rt_tilemap_set_tile(tm, 1, 1, 1);

    rt_tilemap_set_autotile_lo(tm, 1, 50, 51, 52, 53, 54, 55, 56, 57);
    rt_tilemap_set_autotile_hi(tm, 1, 58, 59, 60, 61, 62, 63, 64, 65);

    rt_tilemap_apply_autotile(tm);

    // No neighbors → mask = 0
    assert(rt_tilemap_get_tile(tm, 1, 1) == 50);
    PASS();
}

static void test_autotile_partial_rule_falls_back_to_base(void) {
    TEST("Auto-tiling partial rule falls back to base tile");
    void *tm = rt_tilemap_new(2, 1, 16, 16);
    rt_tilemap_set_tile(tm, 0, 0, 1);
    rt_tilemap_set_tile(tm, 1, 0, 1);

    rt_tilemap_set_autotile_lo(tm, 1, 100, 101, 102, 103, 104, 105, 106, 107);
    rt_tilemap_apply_autotile(tm);

    assert(rt_tilemap_get_tile(tm, 0, 0) == 102);
    assert(rt_tilemap_get_tile(tm, 1, 0) == 1);
    PASS();
}

static void test_clear_autotile_reclaims_rule_capacity(void) {
    TEST("Cleared autotile rules reclaim fixed capacity");
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    for (int64_t base = 1; base <= MAX_AUTOTILE_RULES; ++base) {
        rt_tilemap_set_autotile_lo(tm, base, 0, 0, 0, 0, 0, 0, 0, 0);
        rt_tilemap_clear_autotile(tm, base);
    }
    auto *impl = static_cast<rt_tilemap_impl *>(tm);
    assert(impl->autotile_count == 0);
    rt_tilemap_set_tile(tm, 0, 0, 999);
    rt_tilemap_set_autotile_lo(tm, 999, 777, 0, 0, 0, 0, 0, 0, 0);
    rt_tilemap_apply_autotile(tm);
    assert(rt_tilemap_get_tile(tm, 0, 0) == 777);
    PASS();
}

static void test_json_save_load(void) {
    TEST("JSON save/load round-trip");
    void *tm = rt_tilemap_new(3, 3, 16, 16);
    rt_tilemap_set_tile(tm, 0, 0, 5);
    rt_tilemap_set_tile(tm, 1, 1, 10);
    rt_tilemap_set_tile(tm, 2, 2, 15);

    rt_string path = make_str("/tmp/test_tilemap_roundtrip.json");
    int8_t saved = rt_tilemap_save_to_file(tm, path);
    assert(saved == 1);

    void *loaded = rt_tilemap_load_from_file(path);
    assert(loaded != NULL);
    assert(rt_tilemap_get_width(loaded) == 3);
    assert(rt_tilemap_get_height(loaded) == 3);
    assert(rt_tilemap_get_tile(loaded, 0, 0) == 5);
    assert(rt_tilemap_get_tile(loaded, 1, 1) == 10);
    assert(rt_tilemap_get_tile(loaded, 2, 2) == 15);
    PASS();
}

static void test_json_save_load_preserves_extended_state(void) {
    TEST("JSON save/load preserves layers, props, autotile, collision, animations");
    void *tm = rt_tilemap_new(3, 3, 16, 16);
    int64_t fg = rt_tilemap_add_layer(tm, make_str("fg"));
    assert(fg == 1);
    rt_tilemap_set_layer_visible(tm, fg, 0);
    rt_tilemap_set_tile_layer(tm, fg, 1, 1, 7);
    rt_tilemap_set_collision_layer(tm, fg);
    rt_tilemap_set_collision(tm, 7, 2);
    rt_tilemap_set_tile_property(tm, 7, make_str("damage"), 42);

    rt_tilemap_set_tile(tm, 1, 1, 3);
    rt_tilemap_set_autotile_lo(tm, 3, 50, 51, 52, 53, 54, 55, 56, 57);
    rt_tilemap_set_autotile_hi(tm, 3, 58, 59, 60, 61, 62, 63, 64, 65);

    rt_tilemap_set_tile_anim(tm, 7, 2, 100);
    rt_tilemap_set_tile_anim_frame(tm, 7, 0, 7);
    rt_tilemap_set_tile_anim_frame(tm, 7, 1, 8);
    rt_tilemap_update_anims(tm, 100);
    assert(rt_tilemap_resolve_anim_tile(tm, 7) == 8);

    rt_string path = make_str("/tmp/test_tilemap_extended_roundtrip.json");
    assert(rt_tilemap_save_to_file(tm, path) == 1);

    void *loaded = rt_tilemap_load_from_file(path);
    assert(loaded != NULL);
    assert(rt_tilemap_get_layer_count(loaded) == 2);
    assert(rt_tilemap_get_layer_by_name(loaded, make_str("fg")) == 1);
    assert(rt_tilemap_get_layer_visible(loaded, fg) == 0);
    assert(rt_tilemap_get_tile_layer(loaded, fg, 1, 1) == 7);
    assert(rt_tilemap_get_collision_layer(loaded) == fg);
    assert(rt_tilemap_get_collision(loaded, 7) == 2);
    assert(rt_tilemap_get_tile_property(loaded, 7, make_str("damage"), -1) == 42);
    assert(rt_tilemap_resolve_anim_tile(loaded, 7) == 8);

    rt_tilemap_apply_autotile(loaded);
    assert(rt_tilemap_get_tile(loaded, 1, 1) == 50);
    PASS();
}

static void test_csv_import(void) {
    TEST("CSV import");
    // Write a test CSV
    const char *csv_path = "/tmp/test_tilemap.csv";
    FILE *f = fopen(csv_path, "w");
    assert(f != NULL);
    fprintf(f, "0,0,1\n0,2,1\n1,1,0\n");
    fclose(f);

    rt_string path = make_str(csv_path);
    void *tm = rt_tilemap_load_csv(path, 16, 16);
    assert(tm != NULL);
    assert(rt_tilemap_get_width(tm) == 3);
    assert(rt_tilemap_get_height(tm) == 3);
    assert(rt_tilemap_get_tile(tm, 2, 0) == 1);
    assert(rt_tilemap_get_tile(tm, 1, 1) == 2);
    assert(rt_tilemap_get_tile(tm, 0, 2) == 1);
    PASS();
}

static void test_csv_import_rejects_overflow_values(void) {
    TEST("CSV import rejects overflowing integer values");
    const char *csv_path = "/tmp/test_tilemap_overflow.csv";
    FILE *f = fopen(csv_path, "w");
    assert(f != NULL);
    fprintf(f, "999999999999999999999999999999,-999999999999999999999999999999\n");
    fclose(f);

    assert(rt_tilemap_load_csv(make_str(csv_path), 16, 16) == NULL);
    PASS();
}

static void test_csv_import_rejects_overlong_line(void) {
    TEST("CSV import rejects overlong line");
    const char *csv_path = "/tmp/test_tilemap_overlong.csv";
    FILE *f = fopen(csv_path, "w");
    assert(f != NULL);
    for (int i = 0; i < 17000; i++)
        fputc('1', f);
    fclose(f);

    assert(rt_tilemap_load_csv(make_str(csv_path), 16, 16) == NULL);
    PASS();
}

static void test_csv_rejects_embedded_nul_and_invalid_tile_size(void) {
    TEST("CSV import rejects embedded NUL and invalid tile size");
    const char *csv_path = "/tmp/test_tilemap_nul.csv";
    FILE *f = fopen(csv_path, "wb");
    assert(f != NULL);
    const char row[] = {'1', ',', '2', '\0', ',', '3', '\n'};
    assert(fwrite(row, 1, sizeof(row), f) == sizeof(row));
    fclose(f);

    assert(rt_tilemap_load_csv(make_str(csv_path), 16, 16) == NULL);

    f = fopen(csv_path, "wb");
    assert(f != NULL);
    fputs("1,2\n", f);
    fclose(f);
    assert(rt_tilemap_load_csv(make_str(csv_path), 0, 16) == NULL);
    assert(rt_tilemap_load_csv(make_str(csv_path), 16, -1) == NULL);
    PASS();
}

static void test_file_apis_reject_embedded_nul_paths(void) {
    TEST("Tilemap file APIs reject embedded-NUL paths");
    const char path_bytes[] = "/tmp/test_tilemap_path.json\0ignored";
    rt_string path = make_bytes(path_bytes, sizeof(path_bytes) - 1);
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    assert(rt_tilemap_save_to_file(tm, path) == 0);
    assert(rt_tilemap_load_from_file(path) == NULL);
    assert(rt_tilemap_load_csv(path, 16, 16) == NULL);
    PASS();
}

static void test_json_requires_supported_version(void) {
    TEST("JSON load requires format version 1");
    const char *path = "/tmp/test_tilemap_bad_version.json";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "{"
            "\"version\":2,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[{\"tiles\":[0],\"visible\":1,\"name\":\"base\"}]"
            "}");
    fclose(f);
    assert(rt_tilemap_load_from_file(make_str(path)) == NULL);

    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "{"
            "\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[{\"tiles\":[0],\"visible\":1,\"name\":\"base\"}]"
            "}");
    fclose(f);
    assert(rt_tilemap_load_from_file(make_str(path)) == NULL);
    PASS();
}

static void test_json_rejects_embedded_nul_and_oversized_layer_name(void) {
    TEST("JSON load rejects NUL payloads and oversized layer names");
    const char *path = "/tmp/test_tilemap_nul_json.json";
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    const char json[] =
        "{\"version\":1,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
        "\"layers\":[{\"tiles\":[0],\"visible\":1,\"name\":\"base\"}]}";
    assert(fwrite(json, 1, sizeof(json) - 1, f) == sizeof(json) - 1);
    fputc('\0', f);
    fputs("ignored", f);
    fclose(f);
    assert(rt_tilemap_load_from_file(make_str(path)) == NULL);

    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "{"
            "\"version\":1,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[{\"tiles\":[0],\"visible\":1,"
            "\"name\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]"
            "}");
    fclose(f);
    assert(rt_tilemap_load_from_file(make_str(path)) == NULL);
    PASS();
}

static void test_json_negative_anim_frame_normalizes(void) {
    TEST("JSON load normalizes negative animation frame");
    const char *path = "/tmp/test_tilemap_negative_frame.json";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "{"
            "\"version\":1,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[{\"tiles\":[7],\"visible\":1,\"name\":\"base\"}],"
            "\"collision\":{\"layer\":0,\"types\":[]},"
            "\"tileProperties\":[],\"autotiles\":[],"
            "\"animations\":[{\"baseTile\":7,\"frameCount\":2,\"msPerFrame\":100,"
            "\"timer\":0,\"currentFrame\":-1,\"frames\":[7,8]}]"
            "}");
    fclose(f);

    void *tm = rt_tilemap_load_from_file(make_str(path));
    assert(tm != NULL);
    assert(rt_tilemap_resolve_anim_tile(tm, 7) == 8);
    PASS();
}

static void test_json_duplicate_animation_state_applies_to_replaced_base(void) {
    TEST("JSON duplicate animation state applies to replaced base");
    const char *path = "/tmp/test_tilemap_duplicate_anim_state.json";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "{"
            "\"version\":1,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[{\"tiles\":[5],\"visible\":1,\"name\":\"base\"}],"
            "\"collision\":{\"layer\":0,\"types\":[]},"
            "\"tileProperties\":[],\"autotiles\":[],"
            "\"animations\":["
            "{\"baseTile\":5,\"frameCount\":2,\"msPerFrame\":100,"
            "\"timer\":0,\"currentFrame\":0,\"frames\":[5,6]},"
            "{\"baseTile\":6,\"frameCount\":2,\"msPerFrame\":100,"
            "\"timer\":0,\"currentFrame\":0,\"frames\":[60,61]},"
            "{\"baseTile\":5,\"frameCount\":2,\"msPerFrame\":100,"
            "\"timer\":0,\"currentFrame\":1,\"frames\":[50,51]}"
            "]"
            "}");
    fclose(f);

    void *tm = rt_tilemap_load_from_file(make_str(path));
    assert(tm != NULL);
    assert(rt_tilemap_resolve_anim_tile(tm, 5) == 51);
    assert(rt_tilemap_resolve_anim_tile(tm, 6) == 60);
    PASS();
}

static void test_tile_anim_sequential_frames_reject_overflow(void) {
    TEST("Tile animation sequential defaults reject ID overflow");
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    rt_tilemap_set_tile_anim(tm, INT64_MAX - 1, 3, 100);
    assert(rt_tilemap_resolve_anim_tile(tm, INT64_MAX - 1) == INT64_MAX - 1);
    rt_tilemap_update_anims(tm, 100);
    assert(rt_tilemap_resolve_anim_tile(tm, INT64_MAX - 1) == INT64_MAX - 1);
    PASS();
}

static void test_tile_anim_duplicate_base_replaces(void) {
    TEST("Tile animation duplicate base replaces existing animation");
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    rt_tilemap_set_tile_anim(tm, 5, 2, 100);
    rt_tilemap_set_tile_anim_frame(tm, 5, 1, 9);
    rt_tilemap_update_anims(tm, 100);
    assert(rt_tilemap_resolve_anim_tile(tm, 5) == 9);

    rt_tilemap_set_tile_anim(tm, 5, 2, 100);
    rt_tilemap_set_tile_anim_frame(tm, 5, 1, 12);
    rt_tilemap_update_anims(tm, 100);
    assert(rt_tilemap_resolve_anim_tile(tm, 5) == 12);
    PASS();
}

static void test_variable_tile_animation_round_trip(void) {
    TEST("Variable-duration tile animation JSON round-trip");
    void *tm = rt_tilemap_new(1, 1, 16, 16);
    const int64_t frames[] = {5, 9, 12};
    const int64_t durations[] = {100, 200, 50};
    assert(rt_tilemap_set_import_tile_anim(tm, 5, 3, frames, durations) == 1);
    rt_tilemap_update_anims(tm, 150);
    assert(rt_tilemap_resolve_anim_tile(tm, 5) == 9);

    rt_string path = make_str("/tmp/test_tilemap_variable_anim.json");
    assert(rt_tilemap_save_to_file(tm, path) == 1);
    void *loaded = rt_tilemap_load_from_file(path);
    assert(loaded != NULL);
    assert(rt_tilemap_resolve_anim_tile(loaded, 5) == 9);
    rt_tilemap_update_anims(loaded, 149);
    assert(rt_tilemap_resolve_anim_tile(loaded, 5) == 9);
    rt_tilemap_update_anims(loaded, 1);
    assert(rt_tilemap_resolve_anim_tile(loaded, 5) == 12);
    PASS();
}

static void test_import_layout_round_trip(void) {
    TEST("Imported projection/source-frame/layer layout JSON round-trip");
    void *tm = rt_tilemap_new(2, 2, 16, 8);
    assert(tm != NULL);
    assert(rt_tilemap_configure_import_layout(tm,
                                              RT_TILEMAP_IMPORT_OBLIQUE,
                                              -3,
                                              5,
                                              24,
                                              16,
                                              -4,
                                              -7,
                                              RT_TILEMAP_IMPORT_LEFT_UP,
                                              0,
                                              1,
                                              0,
                                              1.25,
                                              -0.5,
                                              3.5,
                                              -2.25,
                                              2) == 1);
    int64_t overlay = rt_tilemap_add_layer(tm, make_str("overlay"));
    assert(overlay == 1);
    assert(rt_tilemap_configure_import_layer(tm, 0, 1.5, -1.25, 0.75, 0.5) == 1);
    assert(rt_tilemap_configure_import_layer(tm, overlay, -2.5, 4.75, 1.25, 0.25) == 1);

    void *atlas = rt_pixels_new(48, 16);
    assert(atlas != NULL);
    rt_tilemap_set_tileset(tm, atlas);
    assert(rt_tilemap_get_tile_count(tm) == 2);
    assert(rt_tilemap_set_import_tile_count(tm, 1) == 1);

    rt_string path = make_str("/tmp/test_tilemap_import_layout.json");
    assert(rt_tilemap_save_to_file(tm, path) == 1);
    void *loaded = rt_tilemap_load_from_file(path);
    assert(loaded != NULL);

    auto *impl = static_cast<rt_tilemap_impl *>(loaded);
    assert(impl->import_orientation == RT_TILEMAP_IMPORT_OBLIQUE);
    assert(impl->import_origin_tile_x == -3 && impl->import_origin_tile_y == 5);
    assert(impl->import_projection_height == 2);
    assert(impl->source_frame_width == 24 && impl->source_frame_height == 16);
    assert(impl->import_draw_offset_x == -4 && impl->import_draw_offset_y == -7);
    assert(impl->import_render_order == RT_TILEMAP_IMPORT_LEFT_UP);
    assert(impl->import_stagger_axis == 0 && impl->import_stagger_even == 1);
    assert(impl->import_hex_side_length == 0);
    assert(impl->import_skew_x == 1.25 && impl->import_skew_y == -0.5);
    assert(impl->import_parallax_origin_x == 3.5);
    assert(impl->import_parallax_origin_y == -2.25);
    assert(impl->layers[0].import_offset_x == 1.5);
    assert(impl->layers[0].import_offset_y == -1.25);
    assert(impl->layers[0].import_parallax_x == 0.75);
    assert(impl->layers[0].import_parallax_y == 0.5);
    assert(impl->layers[1].import_offset_x == -2.5);
    assert(impl->layers[1].import_offset_y == 4.75);
    assert(impl->layers[1].import_parallax_x == 1.25);
    assert(impl->layers[1].import_parallax_y == 0.25);
    assert(impl->tileset_cols == 2 && impl->tileset_rows == 1);
    assert(rt_tilemap_get_tile_count(loaded) == 1);
    PASS();
}

static void test_json_rejects_wrong_layer_tile_count(void) {
    TEST("JSON load rejects wrong layer tile count");
    const char *path = "/tmp/test_tilemap_wrong_tile_count.json";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "{"
            "\"version\":1,\"width\":2,\"height\":2,\"tileWidth\":16,\"tileHeight\":16,"
            "\"layers\":[{\"tiles\":[1,2,3],\"visible\":1,\"name\":\"base\"}],"
            "\"collision\":{\"layer\":0,\"types\":[]},"
            "\"tileProperties\":[],\"autotiles\":[],\"animations\":[]"
            "}");
    fclose(f);

    assert(rt_tilemap_load_from_file(make_str(path)) == NULL);
    PASS();
}

static void test_json_rejects_truncated_tileset_pixels(void) {
    TEST("JSON load rejects truncated tileset pixels");
    const char *path = "/tmp/test_tilemap_truncated_tileset.json";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "{"
            "\"version\":1,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,"
            "\"tileset\":{\"width\":2,\"height\":1,\"pixels\":[4278190335]},"
            "\"layers\":[{\"tiles\":[7],\"visible\":1,\"name\":\"base\"}],"
            "\"collision\":{\"layer\":0,\"types\":[]},"
            "\"tileProperties\":[],\"autotiles\":[],\"animations\":[]"
            "}");
    fclose(f);

    assert(rt_tilemap_load_from_file(make_str(path)) == NULL);
    PASS();
}

static void test_json_rejects_excess_layers(void) {
    TEST("JSON load rejects layers beyond maximum");
    const char *path = "/tmp/test_tilemap_excess_layers.json";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "{\"version\":1,\"width\":1,\"height\":1,\"tileWidth\":16,\"tileHeight\":16,");
    fprintf(f, "\"layers\":[");
    for (int i = 0; i < 20; i++) {
        if (i > 0)
            fprintf(f, ",");
        fprintf(f, "{\"tiles\":[%d],\"visible\":1,\"name\":\"l%d\"}", i, i);
    }
    fprintf(f,
            "],\"collision\":{\"layer\":0,\"types\":[]},"
            "\"tileProperties\":[],\"autotiles\":[],\"animations\":[]}");
    fclose(f);

    assert(rt_tilemap_load_from_file(make_str(path)) == NULL);
    PASS();
}

static void test_load_nonexistent(void) {
    TEST("Load nonexistent file returns NULL");
    assert(rt_tilemap_load_from_file(make_str("/tmp/nonexistent_tilemap.json")) == NULL);
    assert(rt_tilemap_load_csv(make_str("/tmp/nonexistent.csv"), 16, 16) == NULL);
    PASS();
}

static void test_clear_autotile(void) {
    TEST("Clear autotile rule");
    void *tm = rt_tilemap_new(3, 1, 16, 16);
    rt_tilemap_set_tile(tm, 0, 0, 1);
    rt_tilemap_set_tile(tm, 1, 0, 1);

    rt_tilemap_set_autotile_lo(tm, 1, 50, 51, 52, 53, 54, 55, 56, 57);
    rt_tilemap_set_autotile_hi(tm, 1, 58, 59, 60, 61, 62, 63, 64, 65);

    // Clear the rule
    rt_tilemap_clear_autotile(tm, 1);

    // Apply — should have no effect since rule is cleared
    rt_tilemap_apply_autotile(tm);
    assert(rt_tilemap_get_tile(tm, 0, 0) == 1); // unchanged
    PASS();
}

int main() {
    printf("test_rt_tilemap_io:\n");
    test_tile_properties();
    test_tile_property_update();
    test_tile_property_rejects_embedded_nul_alias();
    test_autotile_basic();
    test_autotile_isolated();
    test_autotile_partial_rule_falls_back_to_base();
    test_clear_autotile_reclaims_rule_capacity();
    test_json_save_load();
    test_json_save_load_preserves_extended_state();
    test_csv_import();
    test_csv_import_rejects_overflow_values();
    test_csv_import_rejects_overlong_line();
    test_csv_rejects_embedded_nul_and_invalid_tile_size();
    test_file_apis_reject_embedded_nul_paths();
    test_json_requires_supported_version();
    test_json_rejects_embedded_nul_and_oversized_layer_name();
    test_json_negative_anim_frame_normalizes();
    test_json_duplicate_animation_state_applies_to_replaced_base();
    test_tile_anim_sequential_frames_reject_overflow();
    test_tile_anim_duplicate_base_replaces();
    test_variable_tile_animation_round_trip();
    test_import_layout_round_trip();
    test_json_rejects_wrong_layer_tile_count();
    test_json_rejects_truncated_tileset_pixels();
    test_json_rejects_excess_layers();
    test_load_nonexistent();
    test_clear_autotile();

    printf("\n  %d/%d tests passed\n", tests_passed, tests_total);
    assert(tests_passed == tests_total);
    return 0;
}
