# ADR 0334: Additive lens-flare composition

Status: Accepted; implemented. Date: 2026-09-06.

## Problem and evidence

A low-tint flare over gray reduced a native screenshot's center from 153 to 90
on Metal and software. Ordinary alpha-blended sprites can obscure the scene.
Switching to additive RGB alone was insufficient with post-processing: Metal
still reduced a tone-mapped background from 212 to 111. A fixed-camera stadium
capture omitting flares removed the corresponding dark spots.

The overlay accumulation also acquired source alpha from additive draws, making
light look like opaque coverage during final composition. Metal's final shader
then multiplied its already-premultiplied RGB by alpha a second time.

## Contract

- LensFlare sprites add RGB using texture radial alpha and smoothed visibility.
  Visibility also retains the existing size response; zero visibility draws
  nothing. Projection, depth occlusion and retained texture lifetimes remain.
- Additive blending preserves destination alpha: light adds radiance, not opaque
  coverage. Metal and D3D11 use zero/one alpha blend factors. OpenGL masks alpha
  writes for additive draws and restores ordinary writes on subsequent draws.
  Software preserves destination alpha for additive fragments. This applies to
  the existing additive capability, including particles and sprites.
- Metal composites its premultiplied overlay as `overlay.rgb + scene *
  (1 - clamp(overlay.a, 0, 1))`, matching the premultiplied source-over contract
  already used by D3D11. This also corrects translucent image/text edges.
- An internal screen-image helper accepts additive mode and opacity. Its existing
  wrapper preserves ordinary image draw behavior. No public runtime API, ABI
  registration, opcode, dependency or new platform special case is introduced.

## Verification

The permanent native flare probe passes with and without post-processing on
Metal and software. Plain gray 153 becomes 170 on both. Tone-mapped gray 212
becomes 229 on Metal and 216 on software; these are backend measurements, not
an assertion of identical post-processing. Ordinary images, disabled lights
and half-transparent final overlays retain the expected coverage behavior.
Command-level tests verify additive selection and visibility opacity.

The canonical graphics build passes 163 tests plus audit/smoke. Platform policy
and host smoke pass. Native OpenGL/D3D11 and Windows/Linux execution remain
unverified. Legacy Baseball's final native livecheck passes 14 image bands and
42 temporal ball checks; ring captures verify removal of dark ghosts. Game
flare tint is scaled to bank power with lower displaced-ghost energy, separately
from this engine blend contract. The broader commercial roadmap remains open.
