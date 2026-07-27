---
status: active
audience: contributors
last-verified: 2026-07-27
---

# Studio Scene Backgrounds

`scene-region-01.png` through `scene-region-10.png` are deterministic
fixed-time captures of the live `GameCamera.drawBiomeBackground` paths. They
let Zanna Studio composite the same biome art behind authored tiles and objects
without loading or executing the game inside the editor.

Regenerate them from the `xenoscape-scenes` project directory with a display
available:

```sh
../../../build/src/tools/zia/zia tools/build_scene_preview_backgrounds.zia
```

The generator fixes the background timer at 2,400 ms and camera position at the
campaign's initial left-edge framing. Do not hand-edit the PNGs; change the
runtime renderer and rebake them together.
