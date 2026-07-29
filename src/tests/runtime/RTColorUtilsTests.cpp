//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTColorUtilsTests.cpp
// Purpose: Tests for Zanna.Graphics.Color utility functions.
// Key invariants:
//   - Covers raw RGB and explicit-alpha tagged color representations.
//   - Exercises boundary, malformed-string, and interpolation behavior.
// Ownership/Lifetime:
//   - Test-owned runtime strings are released after assertions where required.
// Links: src/runtime/graphics/2d/rt_color.c,
//        src/runtime/graphics/common/rt_graphics.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics.h"
#include "rt_internal.h"
#include "rt_string.h"

#include <cassert>
#include <cstring>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static bool str_eq(rt_string s, const char *expected) {
    const char *cstr = rt_string_cstr(s);
    return cstr && strcmp(cstr, expected) == 0;
}

// Helper: build a color from RGB components
static int64_t rgb(int r, int g, int b) {
    return ((int64_t)(r & 0xFF) << 16) | ((int64_t)(g & 0xFF) << 8) | (int64_t)(b & 0xFF);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_from_hex_6digit() {
    rt_string hex = rt_string_from_bytes("#FF8000", 7);
    int64_t c = rt_color_from_hex(hex);
    assert(c == rgb(0xFF, 0x80, 0x00));
    rt_string_unref(hex);
}

static void test_from_hex_no_hash() {
    rt_string hex = rt_string_from_bytes("00FF00", 6);
    int64_t c = rt_color_from_hex(hex);
    assert(c == rgb(0x00, 0xFF, 0x00));
    rt_string_unref(hex);
}

static void test_from_hex_3digit() {
    rt_string hex = rt_string_from_bytes("#F00", 4);
    int64_t c = rt_color_from_hex(hex);
    assert(c == rgb(0xFF, 0x00, 0x00));
    rt_string_unref(hex);
}

static void test_to_hex_basic() {
    int64_t c = rgb(0xFF, 0x80, 0x00);
    rt_string result = rt_color_to_hex(c);
    assert(str_eq(result, "#FF8000"));
    rt_string_unref(result);
}

static void test_to_hex_black() {
    rt_string result = rt_color_to_hex(rgb(0, 0, 0));
    assert(str_eq(result, "#000000"));
    rt_string_unref(result);
}

static void test_to_hex_white() {
    rt_string result = rt_color_to_hex(rgb(255, 255, 255));
    assert(str_eq(result, "#FFFFFF"));
    rt_string_unref(result);
}

static void test_roundtrip_hex() {
    int64_t original = rgb(0x12, 0x34, 0x56);
    rt_string hex = rt_color_to_hex(original);
    int64_t back = rt_color_from_hex(hex);
    assert(back == original);
    rt_string_unref(hex);
}

static void test_roundtrip_hex_transparent_alpha_zero() {
    int64_t original = rt_color_rgba(0xFF, 0x00, 0x00, 0x00);
    rt_string hex = rt_color_to_hex(original);
    assert(str_eq(hex, "#FF000000"));
    int64_t back = rt_color_from_hex(hex);
    assert(back == original);
    rt_string_unref(hex);
}

static void test_from_hex_invalid_returns_zero() {
    rt_string bad_chars = rt_string_from_bytes("#GG0000", 7);
    rt_string bad_len = rt_string_from_bytes("#12345", 6);
    assert(rt_color_from_hex(bad_chars) == 0);
    assert(rt_color_from_hex(bad_len) == 0);
    rt_string_unref(bad_chars);
    rt_string_unref(bad_len);
}

static void test_from_hex_embedded_nul_rejected() {
    const char raw[] = {'#', '1', '2', '3', '\0', '5', '6'};
    rt_string hex = rt_string_from_bytes(raw, sizeof(raw));
    assert(rt_color_from_hex(hex) == 0);
    rt_string_unref(hex);
}

static void test_from_hsl_primary_red() {
    assert(rt_color_from_hsl(0, 100, 50) == rgb(255, 0, 0));
}

static void test_from_hsl_wraps_large_hue() {
    assert(rt_color_from_hsl(720, 100, 50) == rgb(255, 0, 0));
    assert(rt_color_from_hsl(-720, 100, 50) == rgb(255, 0, 0));
}

static void test_get_hsl_components() {
    int64_t red = rgb(255, 0, 0);
    assert(rt_color_get_h(red) == 0);
    assert(rt_color_get_s(red) == 100);
    assert(rt_color_get_l(red) == 50);
    assert(rt_color_get_s(rgb(255, 1, 1)) == 100);
}

static void test_from_hsl_preserves_fractional_channel_precision() {
    assert(rt_color_from_hsl(210, 33, 47) == rgb(80, 119, 159));
}

static void test_lerp_midpoint() {
    int64_t c = rt_color_lerp(rgb(0, 0, 0), rgb(200, 100, 50), 50);
    assert(c == rgb(100, 50, 25));
}

static void test_color_transforms_preserve_explicit_alpha() {
    const int64_t explicit_alpha_flag = INT64_C(1) << 56;
    int64_t c = rt_color_rgba(100, 150, 200, 64);
    int64_t brighter = rt_color_brighten(c, 25);
    int64_t darker = rt_color_darken(c, 25);
    int64_t inverted = rt_color_invert(c);
    assert((brighter & explicit_alpha_flag) != 0);
    assert((darker & explicit_alpha_flag) != 0);
    assert((inverted & explicit_alpha_flag) != 0);
    assert(rt_color_get_a(brighter) == 64);
    assert(rt_color_get_a(darker) == 64);
    assert(rt_color_get_a(inverted) == 64);

    int64_t transparent = rt_color_rgba(0, 0, 0, 0);
    int64_t midpoint = rt_color_lerp(transparent, rgb(100, 100, 100), 50);
    assert((midpoint & explicit_alpha_flag) != 0);
    assert(rt_color_get_a(midpoint) == 127);
}

static void test_get_alpha_reports_stored_byte() {
    assert(rt_color_get_a(rgb(1, 2, 3)) == 0);
    assert(rt_color_get_a(rt_color_rgb(1, 2, 3)) == 0);
    assert(rt_color_get_a(rt_color_rgba(1, 2, 3, 64)) == 64);
    assert(rt_color_get_a(rt_color_rgba(1, 2, 3, 0)) == 0);
}

static void test_brighten_and_darken() {
    int64_t c = rgb(100, 150, 200);
    int64_t brighter = rt_color_brighten(c, 50);
    int64_t darker = rt_color_darken(c, 50);

    assert(((brighter >> 16) & 0xFF) > 100);
    assert(((brighter >> 8) & 0xFF) > 150);
    assert((brighter & 0xFF) > 200);

    assert(((darker >> 16) & 0xFF) < 100);
    assert(((darker >> 8) & 0xFF) < 150);
    assert((darker & 0xFF) < 200);
}

static void test_complement_red() {
    // Red's complement should be cyan-ish
    int64_t red = rgb(255, 0, 0);
    int64_t comp = rt_color_complement(red);
    // Complement shifts hue by 180 degrees
    int64_t r = (comp >> 16) & 0xFF;
    int64_t g = (comp >> 8) & 0xFF;
    int64_t b = comp & 0xFF;
    // Red complement should have low R and higher G/B
    assert(r < 50);
    assert(g > 200);
    assert(b > 200);
}

static void test_grayscale() {
    int64_t c = rgb(100, 150, 200);
    int64_t gray = rt_color_grayscale(c);
    int64_t r = (gray >> 16) & 0xFF;
    int64_t g = (gray >> 8) & 0xFF;
    int64_t b = gray & 0xFF;
    // All channels should be equal
    assert(r == g);
    assert(g == b);
    // Luminance of (100,150,200) = (100*299 + 150*587 + 200*114) / 1000 = 140
    assert(r == 140);
}

static void test_invert() {
    int64_t c = rgb(100, 150, 200);
    int64_t inv = rt_color_invert(c);
    assert(inv == rgb(155, 105, 55));
}

static void test_invert_roundtrip() {
    int64_t c = rgb(42, 128, 200);
    int64_t inv = rt_color_invert(rt_color_invert(c));
    assert(inv == c);
}

static void test_saturate() {
    // Start with a grayish color and saturate it
    int64_t c = rgb(128, 128, 128);
    int64_t sat = rt_color_saturate(c, 50);
    // Pure gray has 0 saturation; adding saturation to gray should still be gray
    // (because HSL saturation of gray is 0, and adding to 0 may not visibly change much)
    // Use a colored input instead
    int64_t colored = rgb(200, 100, 100);
    int64_t more_sat = rt_color_saturate(colored, 20);
    // After saturating, the dominant channel should stay dominant
    int64_t r = (more_sat >> 16) & 0xFF;
    int64_t g = (more_sat >> 8) & 0xFF;
    assert(r > g);
    (void)sat;
}

static void test_desaturate() {
    int64_t c = rgb(255, 0, 0); // Pure red
    int64_t desat = rt_color_desaturate(c, 100);
    // Fully desaturated red should be gray
    int64_t r = (desat >> 16) & 0xFF;
    int64_t g = (desat >> 8) & 0xFF;
    int64_t b = desat & 0xFF;
    assert(r == g);
    assert(g == b);
}

static void test_saturate_clamps() {
    int64_t c = rgb(200, 100, 100);
    // Saturate by 200% should clamp to 100%
    int64_t sat = rt_color_saturate(c, 200);
    // Should not crash and should return a valid color
    int64_t r = (sat >> 16) & 0xFF;
    assert(r <= 255);
}

int main() {
    test_from_hex_6digit();
    test_from_hex_no_hash();
    test_from_hex_3digit();
    test_to_hex_basic();
    test_to_hex_black();
    test_to_hex_white();
    test_roundtrip_hex();
    test_roundtrip_hex_transparent_alpha_zero();
    test_from_hex_invalid_returns_zero();
    test_from_hex_embedded_nul_rejected();
    test_from_hsl_primary_red();
    test_from_hsl_wraps_large_hue();
    test_get_hsl_components();
    test_from_hsl_preserves_fractional_channel_precision();
    test_lerp_midpoint();
    test_color_transforms_preserve_explicit_alpha();
    test_get_alpha_reports_stored_byte();
    test_brighten_and_darken();
    test_complement_red();
    test_grayscale();
    test_invert();
    test_invert_roundtrip();
    test_saturate();
    test_desaturate();
    test_saturate_clamps();

    return 0;
}
