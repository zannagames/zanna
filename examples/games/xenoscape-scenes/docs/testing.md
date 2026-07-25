# Xenoscape validation

Xenoscape has project-local probes that can run through an already-built Zanna
binary. They do not require rebuilding Zanna. From the repository root:

```sh
./build/src/tools/zanna/zanna check examples/games/xenoscape \
  --diagnostic-format=json
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/playthrough_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/level_validation_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/ui_flow_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/progression_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/mechanics_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/world_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/meta_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/settings_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/cadence_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/campaign_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/performance_probe.zia
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes/soak_probe.zia
```

The display and package probes resolve assets relative to the game directory:

```sh
cd examples/games/xenoscape-scenes
../../../build/src/tools/zanna/zanna run smoke_probe.zia
../../../build/src/tools/zanna/zanna run render_probe.zia
../../../build/src/tools/zanna/zanna run package_probe.zia
../../../build/src/tools/zanna/zanna package . --target tarball --dry-run --json
```

Every gate prints `RESULT: ok` on success.

## Probe contracts

| Probe / registered CTest | Contract |
|---|---|
| `zia_smoke_xenoscape` | Canvas/system construction and one-frame world render |
| `zia_xenoscape_playthrough` | Clean-profile Crash Site/Fungal slice, physical gateways, Spore reward, next-route gate |
| `zia_xenoscape_level_validation` | All ten production levels, topology, content anchors, and movement-aware critical reachability |
| `zia_xenoscape_ui_flow` | Modal/top/lower-third priority, expiry, dismissal, acknowledgement, replay |
| `zia_xenoscape_progression` | Schema-v4 round trip, schema-3 migration, checksum tamper detection, selective recovery, corrupt backup |
| `zia_xenoscape_mechanics` | Ability gates, combat traits, surfaces, camera, collision grace, hit stop, animation contracts |
| `zia_xenoscape_world` | Exact graph, gates, endpoint rooms, route choices, discovery, fast travel |
| `zia_xenoscape_meta` | Economy, difficulty, recovery, tutorials, ranks, radio, postgame rules |
| `zia_xenoscape_settings` | Twenty settings, defaults, normalization, persistence, HUD detail, aim assist |
| `zia_xenoscape_cadence` | Equal-elapsed safety and timer convergence at 32/16/8 ms (about 30/60/120 Hz) |
| `zia_xenoscape_campaign` | Clean-profile route to normal/true endings and New Game+ |
| `zia_xenoscape_render` | Authored/fallback title plus 150% profile, pause, ability, debrief, HUD, and tutorial surfaces |
| `zia_xenoscape_package` | Runtime asset decoding, QA-reference dimensions, feature defaults, procedural fallback inventory |
| `zia_xenoscape_performance` | Two deterministic Veteran NG+ full-campaign stress passes under fixed pool caps |
| `zia_xenoscape_soak` | 3,600-frame Core/boss/dynamic-tile/effect soak under fixed pool caps |
| `xenoscape_package_dry_run` | Portable archive inventory and metadata plan |

The tests are registered in `src/tests/CMakeLists.txt`. The render lanes use the
`zanna_display` resource lock; pure probes are suitable for VM/native parity
lanes. The performance tests use generous wall-clock timeouts and assert fixed
pool bounds and determinism instead of brittle platform-specific milliseconds.

## Package smoke

Build a host archive, extract it outside the source tree, and run the embedded
smoke mode:

```sh
../../../build/src/tools/zanna/zanna package . --target tarball \
  --output /tmp/xenoscape-0.3.0-preview.tar.gz
./xenoscape --zanna-package-smoke
```

The smoke executable verifies packaged runtime tuning and audio resolution and
exits without opening a window.

## Platform sign-off policy

A future 1.0 declaration requires clean macOS, Windows, and Linux build-script
and Xenoscape-test evidence, keyboard and gamepad exploratory passes, install /
uninstall checks for each intended format, and signed/notarized artifacts where
distribution policy requires them. Passing a single host's probes is not a
substitute for that matrix.

On 2026-07-10, the 0.3.0 development preview passed the project type-check,
every direct probe above, tarball creation on macOS arm64, archive inventory,
and extracted package smoke. Per request, Zanna itself was not rebuilt and the
repository CTest suite was not run during this remediation.
