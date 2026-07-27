# ASHFALL: SCENES

**The complete Ashfall campaign rebuilt from Zanna Studio scene files.** Every
campaign mission (1-9) and the hub load from authored
`assets/scenes/mission-NN.scene3d` / `hub.scene3d` files instead of code-side
level builders — geometry boxes are real mesh nodes with shared palette
materials and ADR 0185 collider metadata, lights are authored `SceneNode.Light`
components, and every spawn, pickup, prop, cover, flank, route marker,
landmark, encounter beat, and objective gate is a typed-metadata marker node
editable in Studio's 3D scene editor. Objectives, the water pool, ambient,
start pose, and the complete per-level environment row persist as scene root
metadata. The test arena (`--level 0`) intentionally stays code-built in
`world/arena.zia`.

The scenes were exported from the original game by
`examples/games/ashfall/tools/export_scenes.zia`, and
`examples/games/ashfall/tools/scene_parity_probe.zia` proves each one
reproduces the code-built level byte-for-byte through the canonical manifest
serializer (plus explicit water/objective/environment comparisons). The
original Ashfall remains untouched at `examples/games/ashfall`.

Launch straight into one authored scene:

```sh
examples/bin/ashfall-scenes -- --scene assets/scenes/mission-03.scene3d
```

`scene-components.json` ships the gameplay component palette (spawns, pickups,
covers, routes, gates, colliders...) for Studio's Add Missing workflow. Its 3D
preview profile maps the authored start pose and 90-degree gameplay lens,
zenith/horizon/ground sky palette, sky IBL, key/fill lights, distance and
height fog, and overlay defaults into Studio. Large missions therefore open at
player-eye scale with their runtime atmosphere and diagnostic mesh/collider
clutter hidden. It also maps every authored prop, enemy spawn, and pickup
marker to one of 43 real prefab previews under
`assets/editor-previews/`. Those self-contained previews are baked from the
same procedural mesh builders and palette choices as Ashfall's runtime
fallback art, so Studio shows recognizable gameplay silhouettes instead of
generic marker glyphs. They are read-only authoring visuals: preview loading
does not add nodes to the saved mission or change runtime instancing.
`materials.scene3d` seeds the project material library with the campaign's
surface-class palette. The generic contracts are documented by
[ADR 0198](../../../docs/adr/0198-project-owned-3d-scene-preview-profiles.md),
[ADR 0199](../../../docs/adr/0199-project-owned-3d-node-prefab-previews.md),
[ADR 0200](../../../docs/adr/0200-project-owned-3d-sky-and-light-preview-rigs.md),
and [ADR 0201](../../../docs/adr/0201-project-owned-3d-lens-and-atmosphere-previews.md).

Regenerate the preview prefabs deterministically after changing production
prop, enemy, or pickup art:

```sh
(
  cd examples/games/ashfall-scenes
  ../../../build/src/tools/zanna/zanna run tools/build_studio_previews.zia
)
```

`probes/assets_probe.zia` loads all 43 files as part of the portable asset
check, in addition to exercising optional art and the complete runtime fallback
path.

---


A single-player, combat-first sci-fi FPS campaign built entirely in Zia on the
Zanna engine. Salvager **Rook Ryder** fights through nine missions on the ash
world Erebos-4 against **HELIX**, a corrupted terraforming AI, its machine army,
and three distinct bosses.

Ashfall has no product dependencies and stays cross-platform across Metal,
D3D11, OpenGL, and the software backend. Optional GLTF/GLB enemy and weapon art
ships beside procedural PBR fallback meshes, so missing assets never make the
game unplayable.

The campaign was overhauled around direct action: three-to-five-wave encounters,
clear combat state, focused mission loadouts, enemy navigation and formation
logic, explosive hazards, real progression/checkpoints, spatial audio, and
stronger lighting/feedback. Required clue, console, and collectible objectives
were removed from all nine missions.

## Running

Ashfall's interactive workload is intended for a release-mode native binary.
The bytecode VM remains useful for bounded probes, but is not a real-time game
execution target; interactive VM launches exit with the native build command
instead of opening an unplayably slow window.

```sh
# macOS/Linux, from a built Zanna checkout:
mkdir -p examples/bin
build/src/tools/zanna/zanna build examples/games/ashfall-scenes \
  --build-profile release -o examples/bin/ashfall-scenes

./examples/bin/ashfall-scenes
./examples/bin/ashfall --windowed
./examples/bin/ashfall --windowed --level 1

# The bounded headless smoke may still run in the VM:
build/src/tools/zanna/zanna run examples/games/ashfall-scenes/main.zia -- --smoke
```

On Windows, use the built or installed `zanna.exe`, select an `.exe` output,
then launch it from PowerShell:

```powershell
New-Item -ItemType Directory -Force examples/bin | Out-Null
zanna build examples/games/ashfall --build-profile release -o examples/bin/ashfall.exe
.\examples\bin\ashfall.exe --windowed
```

Fullscreen is the default. `--windowed` forces a window, `--level 1` through
`--level 9` jumps to a campaign mission, and `--smoke` runs a bounded headless
self-check.

## Controls

| Action | Key / mouse |
|---|---|
| Move / look | WASD / mouse |
| Jump | Space |
| Sprint | Left Shift |
| Crouch / sprint-slide | Left Ctrl |
| Fire / aim down sights | LMB / RMB |
| Reload | R |
| Weapon slot | 1–9, 0 |
| Previous / next weapon | Mouse wheel or `[` / `]` |
| Throw / cycle grenade | G / Q |
| Melee | V |
| Flashlight | F |
| Interact | E |
| Diagnostics | F3 |
| Pause | Esc |

The options screen applies quality, FOV, sensitivity, invert Y, master volume,
screen shake, difficulty, flash intensity, and the high-contrast HUD live.

## Game systems

- **Combat:** ten genuinely different weapons, three grenades, melee, penetration,
  ricochets, swept projectiles, timed damage-over-time, locational damage,
  directional shields, occlusion-aware self-dangerous blasts, charge/heat,
  recoil, tracers, decals, soft particles, and dynamic light pulses.
- **Movement:** fixed-step `Character3D` acceleration, friction and air control;
  coyote time and jump buffering; physical crouching; sprint-sliding; stance-aware
  footsteps and hearing; camera sway, bob, landing, roll, shake, sprint FOV, and
  per-weapon ADS zoom.
- **Enemies:** eleven regular archetypes and three bosses with line-of-sight and
  hearing perception, NavMesh/NavAgent routing, local avoidance, deterministic
  sweep fallback, cover/flank selection, a squad token director, enemy projectile
  pools, and archetype-specific attack/defense behavior.
- **Campaign:** nine action missions, a hub, and a permanent test arena. Required
  objectives are clear, survive, or reach-after-clear. Every mission has authored
  waves, routes, landmarks, cover, flank pockets, encounter beats, pickups, and
  focused loadouts.
- **Progression:** Story/Soldier/Veteran combat scaling, a title/completion
  Armory where salvage upgrades change live weapon stats, per-level
  score/par/medals, campaign unlocks, run totals, and profile persistence.
- **Checkpoints:** Continue and Retry restore the level/wave, player transform and
  defenses, weapon ownership/ammo, grenades, objective state, score, shot stats,
  and deterministic RNG streams.
- **Presentation:** per-level skies, fog, clustered practical lighting,
  terrain/water and atmosphere; PBR materials, IBL, guarded cascaded shadows,
  SSAO/SSR and soft-particle intersections where supported, ACES tonemapping,
  a retained moving spot flashlight, instanced props, animated landmarks,
  spatially occluded procedural audio, reverb, adaptive music, and a
  resolution-aware combat HUD.
- **Performance fallbacks:** public capability checks guard GPU-only effects;
  Performance mode requests a safe reduced render scale; software/headless paths
  retain direct lighting, FXAA, fixed budgets, and deterministic behavior.

See [OVERHAUL_REPORT.md](OVERHAUL_REPORT.md) for the full source review,
54 implemented recommendations, level-by-level redesign, and public 3D API
assessment.

## Layout

```text
main.zia game.zia config.zia
core/       events, handles, RNG streams, saves, diagnostics
player/     actions, movement, camera, viewmodel, defenses
weapons/    definitions, controller, ballistics, projectile pools, targets
ai/         definitions, AI/director/navigation, hostile projectiles
world/      level specs/manager, objectives, props, pickups, hazards, arena
assets/     generated textures/meshes, optional-asset registry and fallbacks
fx/         particles/tracers/decals, lighting, sky, post-processing
audio/      spatial mixer, procedural cues, adaptive music
ui/         combat HUD, frontend, cinematics
meta/       difficulty, economy/upgrades, scoring/medals
probes/     deterministic Ashfall-specific validation programs
```

## Direct validation

The 14 portable probes can be run without ctest:

```sh
zanna check examples/games/ashfall --diagnostic-format=json

for probe in core movement perf stress_combat combat enemy level manifest meta render campaign menu assets smoke; do
  zanna run "examples/games/ashfall-scenes/probes/${probe}_probe.zia"
done
```

They cover compiler cleanliness, deterministic simulation, movement, budgets,
weapons/damage/audio mapping, AI/navigation/bosses, every level and hazard,
manifest round-trip, meta progression, rendering, the full nine-level campaign,
menus/accessibility/checkpoints, asset fallback, and bounded smoke execution.

The visual probe warms and captures the authored first campaign scene, then
gates center-frame luminance, lit-pixel coverage, and contrast. On macOS, the
registered CTest runs it specifically on Metal and rejects backend fallback:

```sh
ctest --test-dir build -R zia_visual_ashfall_metal --output-on-failure

# Direct run on the selected platform backend; the PNG is written to the OS temp directory.
ZANNA_3D_BACKEND=metal zanna run examples/games/ashfall-scenes/probes/visual_probe.zia
```

For real GPU timing, build the dedicated benchmark natively. Its default mode
sweeps all nine campaign levels. `--sustained` holds a fixed twelve-enemy combat
load for 3,600 frames and reports simulation/render maxima, managed-object
growth, broadphase rebuilds, and navigation retargets. Run from the game folder
so the benchmark also loads the loose authored assets. `--fullscreen --paced`
validates native-resolution delivery at the display's actual refresh rate:

```sh
zanna build examples/games/ashfall-scenes/probes/gpu_perf_probe.zia \
  --build-profile release -o /tmp/ashfall_gpu_perf
(cd examples/games/ashfall && /tmp/ashfall_gpu_perf)
(cd examples/games/ashfall && /tmp/ashfall_gpu_perf --sustained)
(cd examples/games/ashfall && \
  /tmp/ashfall_gpu_perf --sustained --fullscreen --paced)
```

See `misc/plans/fps/` for the original design material and [CREDITS.md](CREDITS.md)
for asset licensing.
