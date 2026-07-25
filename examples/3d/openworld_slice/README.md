# Open World Slice

`openworld_slice/` is the Phase 12 vertical-slice smoke project.
Its world cells deliberately keep the legacy `.vscn` extension as the living
compatibility proof for ADR 0182 (`.scene3d` is canonical for new content). It combines a
manifest-driven >4 km² world stand-in, cell and terrain residency, async model
loading, KTX2 texture-asset residency, a first-person character, physics,
local-avoidance nav agents, a synthetic skinned glTF agent that crossfades from
`Idle` to `Wave` with a bound `IKSolver3D.LookAt`, committed GLB and WAV
package-asset fixture loads, a visible BC7 texture panel and terrain-foot IK
marker pair, final overlay capture,
`World3D` runtime counter checks, a world-scoped navmesh bake, deterministic
all-four-quadrant bounded-residency traversal, replay coverage, and a committed
software final-frame baseline comparison.

Run from this directory:

```sh
../../../build/src/tools/zanna/zanna run main.zia
ZANNA_3D_BACKEND=software ../../../build/src/tools/zanna/zanna run test.zia
ZANNA_3D_BACKEND=software ../../../build/src/tools/zanna/zanna run perf_probe.zia
../../../build/src/tools/zanna/zanna run streaming_hitch_probe.zia
ZANNA_3D_BACKEND=metal ZANNA_OPENWORLD_NATIVE_COMPRESSED_PROBE=1 ../../../build/src/tools/zanna/zanna run streaming_hitch_probe.zia
ZANNA_3D_BACKEND=d3d11 ZANNA_OPENWORLD_NATIVE_COMPRESSED_PROBE=1 ../../../build/src/tools/zanna/zanna run streaming_hitch_probe.zia
ZANNA_3D_BACKEND=software ../../../build/src/tools/zanna/zanna run visibility_dense_probe.zia
ZANNA_3D_BACKEND=metal ../../../build/src/tools/zanna/zanna run gpu_smoke.zia
ZANNA_3D_BACKEND=d3d11 ../../../build/src/tools/zanna/zanna run gpu_smoke.zia
../../../build/src/tools/zanna/zanna package . --target tarball --dry-run
```

Release perf baselines should be recorded from the Release build output, for
example `../../../build_release_perf/src/tools/zanna/zanna run perf_probe.zia`.
CTest also registers `g3d_openworld_slice_perf_harness`, which wraps the same
probe, validates the required `PERF:` counters, and emits a stable `HARNESS:`
summary for CI logs.

The current terrain stream instantiates and renders heightmapped `Terrain3D`
payloads from `assets/world/terrain.vscn` plus `assets/world/terrain/*.height`;
the runtime stitches full matching resident tile edges in world-height space
before terrain LOD meshes are drawn, and `test_rt_game3d` carries the
>4096-unit / >4 km2 proof with skirts disabled and adjacent tiles at different
terrain LOD thresholds. The stream manifests also support authored metadata for
materials, Game3D collision layers/masks, nav areas, traversal costs, and
optional binary sidecars exposed through `WorldStream3D` inspection hooks;
`assets/textures/` includes a tiny RGBA8 KTX2
material fallback and a BC7 KTX2 metadata/residency fixture;
`assets/models/skinned_agent.gltf` plus `skinned_agent.bin` are the committed
redistributable skinned animation fixture; `assets/models/triangle.glb` is the
committed binary glTF fixture; `assets/audio/jump.wav` is the committed audio
asset fixture loaded through `Sound3D.loadAsset`;
`assets/baselines/openworld_slice_software.png` is the software visual
baseline used by `test.zia`; `baselines/perf_macos_apple_m4_max.md` records the
current named macOS perf baseline for `perf_probe.zia`, while
`baselines/perf_windows_shakylaptop_ryzen7940hs.md` records the Windows
x64/MSVC software and D3D11 baseline. The `PERF:` line includes
`frame_gpu_us`, which is non-zero when the active backend exposes GPU timestamp
telemetry and otherwise remains `0`. The probe also builds a
small three-bone foot chain, samples the resident terrain tile height, renders
marker/leg entities near the streamed tile center, and asserts
`IKSolver3D.TwoBone` plants the foot on that terrain target. Per-tile
heightfield collider residency and terrain nav-bake inclusion are verified
through streamed terrain collider/source nodes. Scripted quadrant visits settle
the deterministic `WorldStream3D.update` load budget across a few ticks, and
the runtime unit tests assert `pendingRequestCount` while a terrain request is
deferred. `long_traversal.zia` churns all four streamed quadrants repeatedly,
emits `TRAVERSAL:` hitch/memory/seam telemetry, and replays the same route to
verify deterministic residency churn. The latest named local traversal proof is
recorded in `baselines/perf_macos_apple_m4_max.md` and the Windows D3D11 proof
is recorded in `baselines/perf_windows_shakylaptop_ryzen7940hs.md`.
`visibility_dense_probe.zia` builds a named dense city/forest visibility scene:
front city blocks and a reachable portal alley remain visible, while dense
forest/city zones behind an opaque blocker are culled by authored SceneGraph PVS.
It emits `VISIBILITY_DENSE:` draw-call and fill-proxy reduction metrics and
compares software final-frame pixels against the no-PVS baseline to prove no
visible geometry was removed. The current local reduction proof is recorded in
`baselines/perf_macos_apple_m4_max.md`.
`streaming_hitch_probe.zia` records blocking-vs-async model-template timing,
proves zero upload budget keeps positive-cost async commit work pending, then
checks the shared `Assets3D.GetResidentBytes` counter returns to zero after
clear. The same script also has an opt-in GPU lane used by
`g3d_openworld_slice_streaming_hitch_native_compressed_probe`; when
`ZANNA_OPENWORLD_NATIVE_COMPRESSED_PROBE=1` it binds a native compressed
`TextureAsset3D`, proves `Canvas3D.SetTextureUploadBudget(0)` keeps backend
upload bytes pending, then records the budgeted release upload bytes,
raw-vs-compressed RAM/VRAM reduction, and final-frame texture tolerance. The
local macOS/Metal proof currently reports ASTC with
`native_zero_pending_bytes=16`, `native_upload_bytes=16`,
`native_raw_rgba_bytes=64`, `native_compressed_bytes=16`,
`native_ram_reduction_pct=75`, `native_vram_reduction_pct=75`, and
`native_tolerance_checked=1`. The Windows/D3D11 proof reports native BC7 upload
with the same 16-byte pending/upload size and `native_tolerance_max_diff=0`.
`gpu_smoke.zia` also runs under CTest with the platform GPU
backend (`metal`, `d3d11`, or `opengl`) and skips cleanly if that backend is
unavailable. The smoke includes a small degenerate-normal/tangent normal-map
draw and a 24-light clustered/forward+ draw so GPU shader basis fallbacks and
many-light upload paths are exercised with the rest of the slice. It also
enables a 3-cascade primary directional CSM fixture with near/mid/far shadow
casters, reports `CSM_SHADOWS:` telemetry for the authored shadow path, and
prints `GPU_FRAME_TIME:` with backend GPU timing when the backend provides it.
