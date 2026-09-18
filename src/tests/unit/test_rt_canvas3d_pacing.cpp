//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_canvas3d_pacing.cpp
// Purpose: Verify ADR 0369 frame pacing: the display-snapped interval math, the
//          target clamp, the uncapped default and the microsecond sleep helper.
// Key invariants:
//   - A target that divides the refresh rate within ten percent snaps to whole
//     refresh periods; otherwise the interval is the plain reciprocal.
//   - Targets clamp to [0, 1000]; zero means uncapped.
//   - rt_sleep_us never returns early for a positive interval.
// Ownership/Lifetime:
//   - Stack canvas fixtures only; nothing is retained across cases.
// Links: docs/adr/0369-canvas3d-target-frame-rate.md,
//        src/runtime/graphics/3d/render/rt_canvas3d.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_time.h"

#include "../TestHarness.hpp"

#include <cstring>

TEST(Canvas3DPacing, pace_interval_snaps_to_whole_refresh_periods) {
    EXPECT_EQ(canvas3d_pace_interval_us(60, 120.0), (int64_t)16667);
    EXPECT_EQ(canvas3d_pace_interval_us(60, 60.0), (int64_t)16667);
    EXPECT_EQ(canvas3d_pace_interval_us(30, 60.0), (int64_t)33333);
    EXPECT_EQ(canvas3d_pace_interval_us(40, 120.0), (int64_t)25000);
    EXPECT_EQ(canvas3d_pace_interval_us(120, 120.0), (int64_t)8333);
}

TEST(Canvas3DPacing, pace_interval_falls_back_to_the_reciprocal) {
    /* 144 Hz: two periods are 13.9 ms, three are 20.8 ms; neither is within ten
       percent of 16.7 ms, so the target keeps its own interval. */
    EXPECT_EQ(canvas3d_pace_interval_us(60, 144.0), (int64_t)16667);
    /* Unknown refresh. */
    EXPECT_EQ(canvas3d_pace_interval_us(60, 0.0), (int64_t)16667);
    EXPECT_EQ(canvas3d_pace_interval_us(50, -1.0), (int64_t)20000);
    /* Off. */
    EXPECT_EQ(canvas3d_pace_interval_us(0, 60.0), (int64_t)0);
    EXPECT_EQ(canvas3d_pace_interval_us(-5, 60.0), (int64_t)0);
}

TEST(Canvas3DPacing, target_frame_rate_clamps_and_defaults_to_uncapped) {
    rt_canvas3d canvas;
    std::memset(&canvas, 0, sizeof(canvas));
    EXPECT_EQ(rt_canvas3d_get_target_frame_rate(&canvas), (int64_t)0);
    rt_canvas3d_set_target_frame_rate(&canvas, 60);
    EXPECT_EQ(rt_canvas3d_get_target_frame_rate(&canvas), (int64_t)60);
    EXPECT_EQ(canvas.pace_interval_us, (int64_t)16667);
    EXPECT_EQ(canvas.pace_deadline_us, (int64_t)0);
    rt_canvas3d_set_target_frame_rate(&canvas, 5000);
    EXPECT_EQ(rt_canvas3d_get_target_frame_rate(&canvas), (int64_t)1000);
    rt_canvas3d_set_target_frame_rate(&canvas, -3);
    EXPECT_EQ(rt_canvas3d_get_target_frame_rate(&canvas), (int64_t)0);
    EXPECT_EQ(canvas.pace_interval_us, (int64_t)0);
    EXPECT_EQ(rt_canvas3d_get_target_frame_rate(nullptr), (int64_t)0);
}

TEST(Canvas3DPacing, sleep_us_waits_at_least_the_interval) {
    int64_t start = rt_clock_ticks_us();
    rt_sleep_us(2000);
    int64_t elapsed = rt_clock_ticks_us() - start;
    EXPECT_GE(elapsed, (int64_t)2000);
    rt_sleep_us(0);
    rt_sleep_us(-10);
}

int main() {
    return zanna_test::run_all_tests();
}
