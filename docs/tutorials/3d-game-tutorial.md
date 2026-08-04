---
status: active
audience: public
last-verified: 2026-07-30
---

# Your First 3D Game

This tutorial takes you from a fresh checkout to a running, scene-driven
3D game: scaffold a project in Zanna Studio, edit its scene while the
game hot-reloads, and package the result. It uses only shipped tooling —
no placeholder steps.

Estimated time: 20 minutes.

## 1. Build the toolchain

```sh
./scripts/build_zanna_unix.sh        # macOS / Linux
# or, on Windows (PowerShell):
# .\scripts\build_zanna_win.ps1
```

This produces the `zanna` CLI (under `build/src/tools/zanna/`) used by
every later step. Add it to your `PATH` or spell out the path.

## 2. Launch Zanna Studio

```sh
zanna run src/zannastudio/
```

## 3. Scaffold a 3D game project

Open the command palette and run **New Project: 3D Game**
(`newproject3dgame`), then pick a name and location. The scaffold
publishes atomically and creates:

- `zanna.project` — the manifest, with `run-profile native` so Run and
  Play build a native binary.
- `src/main.zia` — a complete scene-driven entry point. It accepts
  `--scene <path>` (which scene to load), `--scene-watch` (hot reload on
  file change), and `--smoke` (a deterministic 240-frame self-check).
- `assets/scenes/level-01.scene3d` — the starter scene: a ground slab,
  two pickup spheres, and a goal box, each tagged with `game.kind`
  metadata the main loop reads.
- `scene-components.json` — a schema (v19) declaring the `pickup`
  component and a typed **Scene Settings** form for the player start.
- `asset-library.json` — an empty tagged asset library for the asset
  browser.

## 4. Tour the scene

Open `assets/scenes/level-01.scene3d`. You are in the 3D scene editor:

- **Navigate:** middle/right-drag orbits, Shift adds panning, the wheel
  dollies toward the cursor, and holding the right button flies with
  WASD. `F` frames the selection; Frame All fits the whole scene.
- **Select:** click meshes directly — picking is triangle-accurate.
  Shift adds, Ctrl/Cmd toggles.
- **Transform:** `W`/`E`/`R` switch Move/Rotate/Scale gizmos; every
  accepted gesture is exactly one undo entry.
- **Scene Settings:** the Scene tab shows the typed form declared by
  `scene-components.json` — change *Player start* and the value lands in
  root metadata.

Make a visible edit: create a primitive (Create menu → Box), move it,
and save.

## 5. Play with hot reload

Press **Play** on the scene toolbar. Studio builds the project (first
run takes a moment), launches it with
`--scene assets/scenes/level-01.scene3d --scene-watch`, and presents the
game's frames inside the viewport. Walk into the spheres to collect
them, then reach the goal box.

Leave the game running and edit the scene — move the goal, add another
pickup, save. The running game reloads the scene on save: that is
`--scene-watch`, implemented in your project's own `src/main.zia`, so
you can tailor it.

Press **Stop** to return the pane to editing.

## 6. Run from the command line

Everything Studio does is plain CLI underneath:

```sh
cd <your-project>
zanna run . -- --scene assets/scenes/level-01.scene3d
zanna run . -- --smoke        # deterministic 240-frame self-check
```

## 7. Understand the main loop

Open `src/main.zia`. The scaffold is deliberately small and readable:

- `SceneGraph.Load` (or the diagnostic-carrying
  `SceneGraph.LoadResult`) loads the scene file.
- Root metadata (`game.name`, `game.startX`, `game.startZ`) positions
  the player; node metadata (`game.kind` = `pickup`/`goal`) drives the
  collect-and-finish rules.
- `World3D` owns the frame loop and camera; `world.DrawScene()` renders
  the loaded graph.

This is the pattern to grow from: gameplay reads the scene's typed
metadata, and everything an artist edits in Studio is data, not code.

## 8. Package

```sh
zanna package .
```

produces a distributable build for the host platform (`.app`/`.dmg`,
`.deb`/`.rpm`/`.run`, or `.exe`), or run **Package Project** from
Studio's command palette for the same result as a streamed job.

## Where to go next

- The Studio manual's *Scene Authoring* and *Scene-Driven Game
  Workflows* sections (`src/zannastudio/docs/workflows.md`) cover
  terrain sculpting, prefab instances, the material library, and
  lightmap/probe/navmesh baking.
- `docs/graphics3d-guide.md` is the full `Zanna.Graphics3D` API guide;
  `docs/zannalib/graphics/game3d.md` covers the `Zanna.Game3D` gameplay
  layer (characters, cameras, combat, dialogue, streaming).
- `docs/specs/vscn-scene-format.md` specifies the `.scene3d` format.
- The shipped demos scale the same pattern up: `zannademos/games/ashfall-scenes`
  is a nine-mission campaign authored entirely as `.scene3d` files.
