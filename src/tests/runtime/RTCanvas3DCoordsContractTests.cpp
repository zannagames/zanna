//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTCanvas3DCoordsContractTests.cpp
// Purpose: Pin the Canvas3D coordinate-space invariant (ADR 0242): the
//   physical->public mouse conversion, the public extent, and the size
//   scaling helpers all derive from ONE scale, so a click on the last
//   physical pixel lands on `Width - 1` / `Height - 1` in every window
//   shape — windowed HiDPI, fullscreen on a 16:10 display, fullscreen on a
//   21:9 display — and never in a space the overlay is not drawn in.
//
// Key invariants:
//   - physical_to_public(physical_extent - 1) == public_extent - 1 on both axes.
//   - The public scale is uniform and equals the ratio vgfx_get_size() applies.
//   - A public extent that does not carry the framebuffer aspect (the
//     2026-09-03 lender-extent regression) is detectably wrong.
//   - A coordinate-scale change with no physical resize (the 2026-09-04
//     lender-presentation-scale regression) is detected as extent drift.
//
// Ownership/Lifetime:
//   - Fake windows are stack fixtures; no runtime objects are allocated.
//
// Links: src/runtime/graphics/3d/render/rt_canvas3d_coords.inc,
//        src/runtime/graphics/3d/render/rt_canvas3d.c
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "vgfx.h"
}

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

/// @brief Minimal stand-in for the vgfx window: a physical framebuffer plus the
///        coordinate scale the production window would apply in vgfx_get_size().
struct FakeWindow {
    int32_t physical_w;
    int32_t physical_h;
    float coord_scale;
};

int32_t scale_down(int32_t physical, float scale) {
    return (int32_t)std::floor((double)physical / (double)scale + 0.5);
}

} // namespace

extern "C" int vgfx_get_size(vgfx_window_t window, int32_t *out_width, int32_t *out_height) {
    const FakeWindow *win = reinterpret_cast<const FakeWindow *>(window);
    if (!win)
        return 0;
    if (out_width)
        *out_width = scale_down(win->physical_w, win->coord_scale);
    if (out_height)
        *out_height = scale_down(win->physical_h, win->coord_scale);
    return 1;
}

extern "C" int32_t vgfx_window_get_width(vgfx_window_t window) {
    const FakeWindow *win = reinterpret_cast<const FakeWindow *>(window);
    return win ? win->physical_w : 0;
}

extern "C" int32_t vgfx_window_get_height(vgfx_window_t window) {
    const FakeWindow *win = reinterpret_cast<const FakeWindow *>(window);
    return win ? win->physical_h : 0;
}

extern "C" {
#include "../../runtime/graphics/3d/render/rt_canvas3d_coords.inc"
}

namespace {

int g_failures = 0;

#define EXPECT_EQ(actual, expected)                                                                \
    do {                                                                                           \
        auto a_ = (actual);                                                                        \
        auto e_ = (expected);                                                                      \
        if (!(a_ == e_)) {                                                                         \
            std::fprintf(stderr,                                                                   \
                         "%s:%d: EXPECT_EQ(%s, %s) actual=%lld expected=%lld\n",                   \
                         __FILE__,                                                                 \
                         __LINE__,                                                                 \
                         #actual,                                                                  \
                         #expected,                                                                \
                         (long long)a_,                                                            \
                         (long long)e_);                                                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(actual, expected, tol)                                                         \
    do {                                                                                           \
        double a_ = (double)(actual);                                                              \
        double e_ = (double)(expected);                                                            \
        if (std::fabs(a_ - e_) > (tol)) {                                                          \
            std::fprintf(stderr,                                                                   \
                         "%s:%d: EXPECT_NEAR(%s, %s) actual=%f expected=%f\n",                     \
                         __FILE__,                                                                 \
                         __LINE__,                                                                 \
                         #actual,                                                                  \
                         #expected,                                                                \
                         a_,                                                                       \
                         e_);                                                                      \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

/// @brief The invariant every window shape must satisfy: the last physical pixel maps to the
///        last public unit on both axes, and the scale is the one vgfx_get_size() applies.
void checkLastPixelLandsOnExtent(FakeWindow &win, const char *label) {
    int32_t public_w = 0;
    int32_t public_h = 0;
    int32_t mx = -1;
    int32_t my = -1;
    vgfx_window_t handle = reinterpret_cast<vgfx_window_t>(&win);

    vgfx_get_size(handle, &public_w, &public_h);
    canvas3d_coords_physical_to_public(handle, win.physical_w - 1, win.physical_h - 1, &mx, &my);
    if (mx != public_w - 1 || my != public_h - 1) {
        std::fprintf(stderr,
                     "%s: last pixel -> (%d,%d), public extent %dx%d\n",
                     label,
                     mx,
                     my,
                     public_w,
                     public_h);
        ++g_failures;
    }
    canvas3d_coords_physical_to_public(handle, 0, 0, &mx, &my);
    EXPECT_EQ(mx, 0);
    EXPECT_EQ(my, 0);
    EXPECT_NEAR(canvas3d_coords_public_scale(handle), win.coord_scale, 0.01);
    // Public extent carries the framebuffer aspect (cross-multiplied, one unit of slack per
    // axis for integer rounding).
    long long lhs = (long long)public_w * win.physical_h;
    long long rhs = (long long)public_h * win.physical_w;
    long long slack = (long long)win.physical_w + win.physical_h;
    if (lhs - rhs > slack || rhs - lhs > slack) {
        std::fprintf(stderr,
                     "%s: public extent %dx%d does not carry framebuffer aspect %dx%d\n",
                     label,
                     public_w,
                     public_h,
                     win.physical_w,
                     win.physical_h);
        ++g_failures;
    }
}

void testWindowedIdentity() {
    FakeWindow win{1280, 720, 1.0f};
    checkLastPixelLandsOnExtent(win, "windowed 1x");
    vgfx_window_t handle = reinterpret_cast<vgfx_window_t>(&win);
    EXPECT_EQ(canvas3d_coords_scale_logical_size(handle, 1280), 1280);
    EXPECT_EQ(canvas3d_coords_unscale_physical_size(handle, 720), 720);
}

void testWindowedHiDpi() {
    FakeWindow win{2560, 1440, 2.0f};
    checkLastPixelLandsOnExtent(win, "windowed 2x");
    vgfx_window_t handle = reinterpret_cast<vgfx_window_t>(&win);
    EXPECT_EQ(canvas3d_coords_scale_logical_size(handle, 1280), 2560);
    EXPECT_EQ(canvas3d_coords_unscale_physical_size(handle, 1440), 720);
}

/// @brief Fullscreen on a 16:10 Retina laptop: a 1280x720 game adopted onto a 3024x1964
///        framebuffer. The public extent is the monitor in backing-scale units (1512x982), so
///        a bottom-right click lands on (1511, 981) — exactly `Width - 1`, `Height - 1`.
void testFullscreenSixteenTen() {
    FakeWindow win{3024, 1964, 2.0f};
    checkLastPixelLandsOnExtent(win, "fullscreen 16:10");
    vgfx_window_t handle = reinterpret_cast<vgfx_window_t>(&win);
    int32_t public_w = 0;
    int32_t public_h = 0;
    vgfx_get_size(handle, &public_w, &public_h);
    EXPECT_EQ(public_w, 1512);
    EXPECT_EQ(public_h, 982);
}

void testFullscreenUltrawide() {
    FakeWindow win{3440, 1440, 1.0f};
    checkLastPixelLandsOnExtent(win, "fullscreen 21:9");
    FakeWindow hidpi{6880, 2880, 2.0f};
    checkLastPixelLandsOnExtent(hidpi, "fullscreen 21:9 2x");
}

/// @brief Regression for the 2026-09-03 lender-extent contract: if the public extent were
///        pinned to the 2D lender's designed 1280x720 while the framebuffer is 16:10, the
///        derived scale (min of the two axis ratios) puts the bottom-right click at y = 831,
///        past `Height - 1`. The invariant check above must flag that shape as broken, which
///        is what keeps the two spaces from drifting apart again.
void testLenderExtentShapeIsRejected() {
    // A window claiming a 1280x720 public extent over a 3024x1964 framebuffer has no single
    // coord_scale; emulate what vgfx_get_size() would have to report for that claim.
    FakeWindow win{3024, 1964, 3024.0f / 1280.0f}; // x fits; y does not (1964/2.3625 = 831)
    vgfx_window_t handle = reinterpret_cast<vgfx_window_t>(&win);
    int32_t mx = 0;
    int32_t my = 0;
    canvas3d_coords_physical_to_public(handle, 3023, 1963, &mx, &my);
    EXPECT_EQ(mx, 1279);
    // The y extent that a uniform scale yields is 831, not 719: mouse and a 720-tall
    // overlay cannot agree. The contract requires the public extent to be 1280x831 here,
    // never 1280x720.
    EXPECT_EQ(my, 830);
    int32_t public_h = 0;
    vgfx_get_size(handle, nullptr, &public_h);
    EXPECT_EQ(public_h, 831);
}

/// @brief Borrower self-heal (ADR 0242): the public extent can change with NO physical resize —
///        any vgfx_set_coord_scale rewrites the scale vgfx_get_size() and the mouse apply. The
///        2026-09-04 Legacy Baseball skew was exactly this: the 2D lender pushed its fullscreen
///        presentation scale (1.5 on 1080p, 2.3625 on the 16:10 Retina) under an adopted Canvas3D
///        whose cached extent stayed at the backing-scale size. The drift helper must flag it.
void testExtentDriftAfterScaleChangeWithoutResize() {
    // 1080p at 1x: adopted extent 1920x1080; lender pushes 1.5 -> public 1280x720.
    FakeWindow win{1920, 1080, 1.0f};
    vgfx_window_t handle = reinterpret_cast<vgfx_window_t>(&win);
    int32_t w = 0, h = 0, pw = 0, ph = 0;
    EXPECT_EQ(canvas3d_coords_extent_drifted(handle, 1920, 1080, 1920, 1080, &w, &h, &pw, &ph), 0);
    win.coord_scale = 1.5f;
    EXPECT_EQ(canvas3d_coords_extent_drifted(handle, 1920, 1080, 1920, 1080, &w, &h, &pw, &ph), 1);
    EXPECT_EQ(w, 1280);
    EXPECT_EQ(h, 720);
    EXPECT_EQ(pw, 1920);
    EXPECT_EQ(ph, 1080);
    // Once the cache adopts the live extent there is no drift, and the mouse lands on Width-1.
    EXPECT_EQ(canvas3d_coords_extent_drifted(handle, w, h, pw, ph, &w, &h, &pw, &ph), 0);
    checkLastPixelLandsOnExtent(win, "1080p after 1.5 presentation scale");

    // 16:10 Retina: adopted 1512x982 at 2x; lender pushes min(3024/1280, 1964/720) = 2.3625.
    FakeWindow retina{3024, 1964, 2.0f};
    vgfx_window_t rh = reinterpret_cast<vgfx_window_t>(&retina);
    EXPECT_EQ(canvas3d_coords_extent_drifted(rh, 1512, 982, 3024, 1964, &w, &h, &pw, &ph), 0);
    retina.coord_scale = 3024.0f / 1280.0f;
    EXPECT_EQ(canvas3d_coords_extent_drifted(rh, 1512, 982, 3024, 1964, &w, &h, &pw, &ph), 1);
    EXPECT_EQ(w, 1280);
    EXPECT_EQ(h, 831);
    checkLastPixelLandsOnExtent(retina, "retina after 2.3625 presentation scale");

    // A physical resize with an unchanged scale is drift too (the RESIZE path also lands here).
    FakeWindow grown{2560, 1440, 2.0f};
    vgfx_window_t gh = reinterpret_cast<vgfx_window_t>(&grown);
    EXPECT_EQ(canvas3d_coords_extent_drifted(gh, 1280, 720, 2560, 1440, &w, &h, &pw, &ph), 0);
    grown.physical_w = 3024;
    grown.physical_h = 1964;
    EXPECT_EQ(canvas3d_coords_extent_drifted(gh, 1280, 720, 2560, 1440, &w, &h, &pw, &ph), 1);
    EXPECT_EQ(w, 1512);
    EXPECT_EQ(h, 982);
    EXPECT_EQ(pw, 3024);
    EXPECT_EQ(ph, 1964);

    // Degenerate windows never report drift and never write the outputs.
    FakeWindow zero{0, 0, 1.0f};
    w = h = pw = ph = -1;
    EXPECT_EQ(canvas3d_coords_extent_drifted(
                  reinterpret_cast<vgfx_window_t>(&zero), 1, 1, 1, 1, &w, &h, &pw, &ph),
              0);
    EXPECT_EQ(w, -1);
    EXPECT_EQ(canvas3d_coords_extent_drifted(nullptr, 1, 1, 1, 1, &w, &h, &pw, &ph), 0);
    EXPECT_EQ(canvas3d_coords_extent_drifted(handle, 1, 1, 1, 1, nullptr, &h, &pw, &ph), 0);
}

void testDegenerateWindows() {
    vgfx_window_t null_handle = nullptr;
    int32_t mx = 5;
    int32_t my = 7;
    canvas3d_coords_physical_to_public(null_handle, 100, 200, &mx, &my);
    EXPECT_EQ(mx, 100);
    EXPECT_EQ(my, 200);
    EXPECT_NEAR(canvas3d_coords_public_scale(null_handle), 1.0, 0.0001);
    EXPECT_EQ(canvas3d_coords_scale_logical_size(null_handle, 0), 0);
    EXPECT_EQ(canvas3d_coords_unscale_physical_size(null_handle, -4), -4);

    FakeWindow zero{0, 0, 2.0f};
    vgfx_window_t zero_handle = reinterpret_cast<vgfx_window_t>(&zero);
    EXPECT_NEAR(canvas3d_coords_public_scale(zero_handle), 1.0, 0.0001);

    // Scales below 1 are clamped to 1; above 16 saturate.
    FakeWindow tiny{10, 10, 0.5f};
    vgfx_window_t tiny_handle = reinterpret_cast<vgfx_window_t>(&tiny);
    EXPECT_NEAR(canvas3d_coords_public_scale(tiny_handle), 1.0, 0.0001);
    FakeWindow huge{3200, 3200, 32.0f};
    vgfx_window_t huge_handle = reinterpret_cast<vgfx_window_t>(&huge);
    EXPECT_NEAR(canvas3d_coords_public_scale(huge_handle), 16.0, 0.0001);
}

} // namespace

int main() {
    testWindowedIdentity();
    testWindowedHiDpi();
    testFullscreenSixteenTen();
    testFullscreenUltrawide();
    testLenderExtentShapeIsRejected();
    testExtentDriftAfterScaleChangeWithoutResize();
    testDegenerateWindows();
    if (g_failures != 0) {
        std::fprintf(stderr, "RTCanvas3DCoordsContractTests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("RTCanvas3DCoordsContractTests: all passed\n");
    return 0;
}
