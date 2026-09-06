# ADR 0336: Independent shadow atlas resolution

Status: Implemented; native Metal/software verified, native OpenGL/D3D11 pending

## Context

The first four shadow slots are individual maps; subsequent slots use a shared
GPU atlas. Stadium primary maps use 4096 pixels, making the existing eight-tile
atlas expensive. CPU targets are already lazy, but share the same resolution
policy. Increasing slot capacity without separating atlas resolution compounds
memory pressure.

## Decision

Add Canvas3D.SetShadowAtlasResolution(Integer) and read-only
Canvas3D.ShadowAtlasResolution. Zero (default) inherits EnableShadows resolution;
negative inputs become zero, positive inputs clamp to 64..4096. Invalid canvas
handles are ignored/read as zero. No new diagnostic or feature toggle.

The setting controls slots >= VGFX3D_CSM_SLOTS only. Slots 0..3 retain the existing
primary resolution. All atlas slots share one size. CPU allocation validation
uses the per-slot desired size; it must not oscillate/reallocate because primary
and atlas sizes differ. Existing GPU shadow_begin dimensions and per-texture
sampling size drive rendering/filtering, without new shader conventions.
Changing the setting releases CPU shadow targets and invalidates shadow reuse;
GPU targets resize on next use. Same-value assignment preserves retained state.
Zero restores legacy sizing. Shadow selection, light power and slot capacity
are unchanged. This is a public runtime C ABI/registry addition.

## Verification

Test default/clamping, mixed target dimensions, repeated-frame target retention,
configuration invalidation and zero restoration. Run canonical graphics/surface
checks, native mixed-resolution stadium capture, game image/ball acceptance and
platform policy/host smoke. Native Windows/Linux/OpenGL/D3D11 acceptance remains
separate from macOS execution. This change enables, but does not complete, the
full-fixture capacity and commercial performance requirements.

## Results

581 GPU-path assertions and 163 canonical graphics tests pass. Native five-light
Metal/software probes demonstrate visible atlas sampling changes, stable repeated
mixed-size frames and pixel-exact default restoration. Full game livecheck passes
14 image bands and 42 ball samples. Balanced/cinematic uses 1024 secondary tiles;
closed-roof footprint falls 8.15→7.09 GB in a short diagnostic, with no CPU speedup
claim. Four ring shadow requests still drop at the unchanged 12-slot cap.
Evidence: baseball/analysis/plan96/shadow-atlas-15/README.md.
