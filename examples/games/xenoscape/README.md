# XENOSCAPE: The Descent

XENOSCAPE is an active-development ten-region science-fiction action
Metroidvania and a substantial Zanna/Zia game example.

> **Scene-driven recreation:** every campaign region also exists as an
> authored `.scene2d` file in
> [`examples/games/xenoscape-scenes`](../xenoscape-scenes/README.md), editable
> in Zanna Studio's 2D scene editor. `tools/export_scenes.zia` regenerates
> those scenes from this game's builders and
> `tools/scene_parity_probe.zia` proves the two stay exactly identical. The current project
version is **0.3.0 development preview**. It is not 1.0 and this repository does
not claim final platform certification or commercial-release status.

The preview includes a nonlinear physical world graph, six traversal abilities,
persistent profiles, an evolving shipwreck hub, ten regional bosses,
accessibility assists, contextual tutorials, performance ranks, Time Trials,
Boss Rush, New Game+, authored key art, adaptive procedural music, and permanent
deterministic art/audio fallbacks.

## Run from source

Build Zanna with the platform script from the repository root, then run:

```sh
./build/src/tools/zanna/zanna run examples/games/xenoscape
```

To build the standalone project:

```sh
./build/src/tools/zanna/zanna build examples/games/xenoscape
```

The game targets 1280x720 and is cadence-checked at 30, 60, and 120 Hz.
Keyboard and gamepad actions share the
runtime action-binding source, so prompts reflect the bindings that are
actually active.

## Current development features

- Three explicit profile slots with New/Load/Delete/Overwrite confirmation,
  schema-v4 checksummed saves, migration, corruption backup, checkpoints, and
  clean-exit autosave.
- Ten regions, eighty named rooms, twelve bidirectional routes, stable lore and
  interaction ids, physical endpoint gateways, two secrets per region,
  landmarks, mastery routes, and movement-aware reachability validation.
- Baseline Wall Jump and Double Jump, followed by Dash, Charge Shot, Ground
  Pound, and Grapple, plus
  ice, quicksand, crumble, conveyor, steam, destructible, one-way, switch,
  keyed-door, shrine, save-station, teleporter, and lore interactions.
- Shipwreck Map, Workbench, Archive, Simulator, and Launch stations; four
  permanent three-tier upgrade branches; Explorer/Standard/Veteran profiles.
- Per-region D/C/B/A/S ranks, unlocked Time Trials, four-boss Boss Rush, and
  New Game+ with 135% enemy HP and 150% encounter density.
- Distinct Beacon Warden, Spore Mother, Crystal Wyrm, Relay Custodian, Tide
  Leviathan, Magma Core, Verdant Colossus, Cryo Custodian, Null Harvester, and
  Architect encounters with readable phase tells and regional attack signatures.
- Coordinated modal/top/bottom UI lanes, expiring and replayable tutorials,
  checkpoint recovery, HUD-detail and aim-assist choices, and large-text render
  coverage.
- Authored title and ARIA art with region-specific code-native environments and
  music; missing-asset fallbacks remain tested.

## Package the development preview

From the game directory, inspect and build the portable archive with the Zanna
binary already built for the host:

```sh
../../../build/src/tools/zanna/zanna package . --target tarball --dry-run --json
../../../build/src/tools/zanna/zanna package . --target tarball \
  --output /tmp/xenoscape-0.3.0-preview.tar.gz
```

The package smoke mode is `xenoscape --zanna-package-smoke`. It checks packaged
runtime tuning and an audio asset without opening the game window.

## Documentation

- [Player and controls guide](docs/player-guide.md)
- [Accessibility and difficulty](docs/accessibility.md)
- [Profiles, saves, and recovery](docs/saves-and-profiles.md)
- [Developer architecture](docs/internals/architecture.md)
- [Testing and release validation](docs/internals/testing.md)
- [Development release notes](docs/release-notes.md)
- [Credits and licenses](docs/credits.md)
- [Asset provenance](assets/README.md)

XENOSCAPE and its bundled first-party assets are distributed under GNU GPL v3
as part of the Zanna project.
