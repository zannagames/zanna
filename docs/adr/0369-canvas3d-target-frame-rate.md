---
status: accepted
audience: contributors
last-verified: 2026-09-17
---

# ADR 0369: Canvas3D.SetTargetFrameRate (Display-Snapped Frame Pacing)

## Status

Accepted. Adds `Zanna.Graphics3D.Canvas3D.SetTargetFrameRate(fps)` and the
`TargetFrameRate` property, `rt_sleep_us` in the runtime clock module, and
`vgfx_get_display_refresh_hz` in the window layer.

## Context

GPU-backed `Canvas3D` windows ran uncapped: `canvas3d_window_pacing_fps` disabled
the window limiter for every hardware backend, and presentation was paced only by
the swapchain (vsync). A frame that takes 12 to 27 ms of CPU on a 60 Hz panel
therefore lands on the first or second refresh after it finishes, and on a 120 Hz
ProMotion panel on the second or third. The presented cadence alternates between
refresh intervals from frame to frame, and `DeltaTime` (wall clock between
presents) jitters with it, so animation advances by uneven steps on a screen that
shows them at uneven times. Legacy Baseball's wide view, measured at a 24 ms median
on the M4 Max after the plan-127 CPU work, is exactly this case: it straddles the
16.7 ms budget and looks less smooth than a steady 30 or 60 would.

The 2D `Canvas` has always had `SetFps` through the vgfx limiter, but that limiter
sleeps to a millisecond deadline with `Sleep`/`nanosleep` alone (up to 15.6 ms of
oversleep on Windows) and knows nothing about the display's refresh period.

## Decision

- `Canvas3D.SetTargetFrameRate(fps)` sets a target in `[0, 1000]`; zero (the
  default) keeps today's behaviour. `TargetFrameRate` reads it back.
- With a positive target, every `Present`/`Flip` ends with a wait to a deadline
  that advances by a fixed interval. The interval is the reciprocal of the target
  unless the display refresh is known and the target divides it within ten
  percent, in which case it is a whole number of refresh periods (60 on 120 Hz
  waits two periods; 30 on 60 Hz waits two; 40 on 120 Hz waits three). The
  refresh rate is re-read every 128 frames so a window dragged to another
  monitor follows it.
- The wait sleeps in chunks of at most 4 ms until 0.8 ms before the deadline,
  then spins on the monotonic clock. `rt_sleep_us` backs the sleep: `nanosleep`
  on POSIX, a per-thread high-resolution waitable timer on Windows (`Sleep`
  granularity is unusable for pacing).
- A frame more than one interval late resyncs the deadline to "now" instead of
  running unpaced frames to catch up; the burst it would otherwise produce is
  the judder this feature removes.
- The wait runs after the event pump and before the live clock samples the
  frame, so `DeltaTime` reports the paced interval. Synthetic clocks (probes
  under `SetSyntheticClock`) never pace.
- When a target is set, the vgfx window limiter is disabled (software backend
  included) so only one pacer owns the cadence.
- `vgfx_get_display_refresh_hz(window, &hz)` reports the panel's rate: macOS from
  the window's screen (`maximumFramesPerSecond`, with the display mode as a
  fallback), Windows from the primary display's `VREFRESH`; X11, Wayland and the
  mock platform report unknown, and the pacer then uses the plain reciprocal.

## Consequences

- Legacy Baseball applies its `TARGETFPS` setting (shipped 60) as the cap of the
  live 3D canvas; the liveperf probe keeps it off unless `ZANNA_LIVEPERF_CAP=1` so
  its cadence keeps measuring CPU work.
- Two runtime functions, one method and one property are added; the graphics
  manifest hash, the Baseball generated-inventory pins and the graphics stub
  count are re-pinned in the same change. No IL, verifier or serialization change.
- The spin uses at most 0.8 ms of CPU per frame; on a machine that cannot hold
  the target the pacer never waits, so it cannot make a slow frame slower.
- `vgfx_get_display_refresh_hz` is a new cross-layer call from the 3D runtime into
  the window layer, next to `vgfx_set_fps`; no new platform dependency is taken
  (Windows uses `GetDeviceCaps`, already imported).
