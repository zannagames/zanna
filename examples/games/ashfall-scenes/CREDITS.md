# ASHFALL — Credits & Licensing

## Code

All game code is original Zia, part of the Zanna project, under the GNU GPL v3.
See the repository `LICENSE`. No external code dependencies — every system
(movement, ballistics, AI, level tech, economy, UI, FX, audio mix) is written
from scratch on the Zanna runtime.

## Art & audio

The game follows an **asset-optional contract**: it is fully playable with the
procedural fallbacks (skybox, textures, composite-primitive models built in
`assets/`), and the downloaded models below swap in over the same registry when
present under `assets_src/`.

### Downloaded CC0 packs (`assets_src/`)

| Pack | Author | Used for | License | Source |
|------|--------|----------|---------|--------|
| Animated Mech Pack | Quaternius | Husk, Ranger, Marauder, Vanguard, WARDEN enemy rigs | CC0 1.0 | https://quaternius.com/packs/animatedmech.html |
| Ultimate Monsters | Quaternius | Sapper, Mite, Drone, Stalker, Wraith, Shrike, SHRIKE Prime rigs | CC0 1.0 | https://quaternius.com/packs/ultimatemonsters.html |
| Blaster Kit | Kenney | All 10 weapon viewmodels | CC0 1.0 | https://kenney.nl/assets/blaster-kit |

Per-pack license texts are kept alongside the files
(`assets_src/enemies/LICENSE_quaternius_*.txt`, `assets_src/weapons/LICENSE_kenney.txt`).
All three packs are Creative Commons Zero (public domain) — attribution is
appreciated but not required; we credit them gladly.

The Turret and HELIX Avatar remain fully procedural by design (pivot-head and
monolith constructions), and every downloaded model has a procedural composite
fallback so a checkout without `assets_src/` runs and tests identically.

## Engine

Built on the Zanna toolchain (IL-based compiler → VM / native codegen) and its
3D runtime (Graphics3D / Game3D / Physics3D / Sound). See the top-level project
documentation.
