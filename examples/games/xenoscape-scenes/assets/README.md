# XENOSCAPE first-party assets

All files in this directory are project-bound runtime or QA-reference assets for XENOSCAPE and
are distributed under the Zanna project's GNU GPL v3 license unless a nested
notice states otherwise. No runtime download or external product dependency is
required; `zanna.project` bundles this directory into packaged builds.

## Art provenance

- `art/title-key-art.png` — replaced on 2026-07-10 using OpenAI's image-
  generation service and an original project brief. The runtime edit keeps
  the crash-site composition while using the same crisp 16-bit pixel language,
  navy/cyan/magenta/amber palette, and silhouette priorities as gameplay. It has
  no embedded text, third-party logo, or watermark.
- `art/aria-portrait.png` — replaced on 2026-07-10 in the same deliberate pixel-
  art language. It depicts the original ARIA ship-AI avatar with readable cyan
  hologram planes and amber circuit nodes at the shipped 70px presentation size.
  It contains no text, third-party logo, or watermark.

## Deterministic production atlases

The following QA reference PNGs are baked by `tools/build_release_assets.zia` from the
in-tree first-party SpriteFactory recipes and fixed XENOSCAPE palette. They have
no external source material and are reproducible without a network connection:

- `sprites/player-release-sheet.png` — all 41 stable right-facing release poses
  in an 8x6 grid; the game mirrors left-facing frames after decode.
- `tiles/biome-tile-atlas.png` — stable tile ids 0-56 in an 8x8 grid.
- `icons/hud-icon-atlas.png` — pickup, map, objective, radio, and Simulator UI
  pictograms in eight 64px cells.
- `ui/title-menu-panel.png` — translucent circuit-frame texture behind the live
  title menu.

The player sheet is consumed at runtime. The tile/icon/panel atlases are visual
regression references for the code-native renderer and are not falsely counted
as runtime features. SpriteFactory and Canvas drawing remain permanent fallbacks.
Rebuild the deterministic atlases from the game directory with:

```sh
mkdir -p assets/sprites assets/tiles assets/icons assets/ui
../../../build/src/tools/zia/zia tools/build_release_assets.zia
```
