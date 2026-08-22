# macOS Metal headless scene evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus the reviewed working-tree changes for this recommendation set.
- Host: macOS 26.6 (25G72), arm64.
- GPU: Apple M4 Max, 32 cores, Metal 4.
- Focused build: `cmake --build build --target test_rt_canvas3d -j2`.
- Focused run: `ctest --test-dir build -R '^test_rt_canvas3d$' --output-on-failure`.
- Result: passed (0.66 seconds).

The accelerated offscreen constructor created no platform window, selected the
native Metal backend without reporting software fallback, bound the explicit
96×64 render target, rendered a frame, and read the requested clear color back
within the test's ±2-per-channel GPU tolerance. The same test also exercises
the deterministic software reference path and the broader Canvas3D target,
scene, material, mesh, and readback contracts.
