# XENOSCAPE: Scenes

This is the **scene-driven recreation of Xenoscape**. It plays identically to
[`examples/games/xenoscape`](../xenoscape/README.md) — same ten regions, same
abilities, bosses, saves, and probes — but every campaign region is an
authored `assets/scenes/region-NN.scene` file instead of code. The original
game's ~3,500 lines of `buildDescent()`-style level builders are replaced by a
~120-line loader (`level.zia`), and everything those builders produced now
lives in canonical scene JSON that Zanna Studio's 2D scene editor opens
directly.

## What the scenes carry

- **Tiles**: each region's full tile grid on one `terrain` layer, rendered in
  Studio through `assets/tiles/scene-tile-atlas.png` (frame `N-1` shows tile
  id `N`).
- **Tile behavior** (ADR 0176 typed sections): the complete collision list,
  hazard/surface/quicksand/conveyor/steam/save/lore/crumble/grapple tile
  properties, and the lava animation. `SceneDocument.BuildTilemap()` applies
  them, so the game has no code-side tile registry at all.
- **Objects**: every enemy spawn, pickup, interaction (switches, doors, save
  stations, shrines, teleporters, lore terminals, region gates), the boss,
  the player start, the checkpoint, and structural `marker` anchors
  (landmark/tutorial/mastery/setpiece/secret) with typed properties defined
  by `scene-components.json` (schema v2 with an enum marker kind).
- **Scene properties**: theme and player-start coordinates.

`asset-library.json` tags the canonical tileset with its 64x64 grid for the
Studio asset browser.

Post-load `enrichEnemies()` (newer enemy types and gem placement) still runs
in code, exactly as it does in the original, so the two games remain
behaviorally identical.

## Provenance and parity

The scenes were exported from the original game's own production
`loadLevel()` path by `../xenoscape/tools/export_scenes.zia`, and
`../xenoscape/tools/scene_parity_probe.zia` proves every region matches the
code-built level **exactly**: tile-for-tile, spawn-for-spawn,
interaction-for-interaction, including built-tilemap collision, tile
properties, and animation resolution. From here on the scene files are the
source of truth — edit them in Zanna Studio, not in code.

## Run from source

```sh
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes

# Jump straight to one region (also what Zanna Studio's Run Scene sends):
./build/src/tools/zanna/zanna run examples/games/xenoscape-scenes -- \
  --scene assets/scenes/region-03.scene
```

Open `examples/games/xenoscape-scenes` as a workspace folder in Zanna Studio
and open any `assets/scenes/region-NN.scene` to edit tiles, tile behavior,
spawns, and markers visually; the **Run Scene** toolbar button launches the
game at that region.

## Validation

The original probe suite is carried over unchanged and passes against
scene-loaded levels:

```sh
cd examples/games/xenoscape-scenes
for probe in smoke world campaign mechanics progression meta settings \
             cadence soak ui_flow performance render playthrough \
             level_validation; do
  zanna run ${probe}_probe.zia
done
```

XENOSCAPE and its bundled first-party assets are distributed under GNU GPL v3
as part of the Zanna project.
