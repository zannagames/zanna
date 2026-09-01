---
status: active
audience: contributors
last-verified: 2026-09-01
---

# ADR 0309: Render-Target Shadow Inheritance

## Status

Accepted (2026-09-01)

## Context

`canvas3d_render_shadow_pass` runs on every canvas frame, render-target frames
included. A game that renders a secondary in-world camera each frame — Legacy
Baseball's picture-in-picture runner inset is the motivating case — therefore
pays the whole shadow rig twice per displayed frame: with a 4-cascade 4096
map and a night stadium's shadowed point lights (six atlas cube faces each),
that is the dominant frame cost on every platform.

The per-slot signature cache (`shadow_slot_signature`) cannot help across two
cameras. A slot's signature hashes the light view-projection and the frame's
culled caster list; the inset's narrow lens and the broadcast camera's wide
lens produce disjoint culled sets, so the two passes never match and each
overwrites the signatures the other needs — the cache thrashes instead of
saving work. Two alternatives were examined and rejected:

- **Per-target signature sets** are unsound with shared storage: the
  signature describes what is physically in the shared depth texture
  `shadow_rts[slot]` right now, so keying it per target would let one pass
  "reuse" depth another pass just overwrote. Sound per-target keying needs a
  second full shadow-map allocation per target — memory-prohibitive.
- **Skipping the RT shadow pass entirely** (sampling the window frame's maps)
  breaks cascaded sun shadows: cascade selection is by view depth from the
  *current* camera against splits fitted to the camera that built them, and
  an out-of-bounds light-space sample resolves fully lit. Point-cube and spot
  selection, by contrast, is world-space and camera-independent.

The change adds a backend vtable hook, canvas registry members, and a
render-pass behavior change, which require an ADR under repository policy.

## Decision

1. **Opt-in flag** `Canvas3D.SetRenderTargetShadowInherit(canvas, enabled)`
   (default off; `rt_shadow_inherit` on the canvas). When enabled, a
   render-target frame re-renders **only the primary cascaded directional
   light's slots** — fitted to the RT camera, so its sun shadows are correct
   and sharp — and **inherits every other slot** the last full window-backed
   pass rendered, by re-arming its persisted depth. Cube-face and spot
   selection being world-space, inherited slots are exact modulo one frame of
   staleness.
2. **Full-pass cache.** A full window-backed shadow pass snapshots its packed
   light entries, per-slot light view-projections (frame Begin zeroes the
   live array), granted-slot count, and the leading slot count consumed by
   the cascaded primary (`shadow_cache_*` on the canvas;
   `shadow_cache_lights` is lazily allocated because
   `vgfx3d_light_params_t` is not a complete type in the internal header).
   Render-target frames never write the cache. The cache invalidates on
   shadow-target release (resolution change, disable, destroy) and on
   cascade-count or budget changes.
3. **Backend hook** `shadow_inherit(ctx, slot, depth_buf, w, h, light_vp)` —
   like `shadow_reuse`, but atlas slots (>= `VGFX3D_CSM_SLOTS`) may also
   re-arm. That is legal precisely because an inheriting render-target frame
   renders **no** atlas slot of its own: the shared atlas is whole-cleared
   only on its first tile pass of a frame, so a frame with no tile pass
   leaves the previous full pass's tiles intact. The engine guarantees this
   invariant by only inheriting the region *after* the cascaded primary and
   rendering nothing else. Metal and D3D11 implement the hook (their
   `shadow_reuse` must decline atlas slots; the inherit variant validates the
   atlas texture instead); OpenGL (no atlas) and software (canvas-owned
   per-slot CPU depth) delegate to their existing `shadow_reuse`. A NULL hook
   makes the opt-in a no-op on that backend.
4. **Fallback.** Any re-arm failure — no cache, slot layout mismatch, freed
   or resized storage, budget cap below the cached count — abandons
   inheritance for that frame and takes the existing full pass. A failed
   attempt publishes nothing; slots it already armed are overwritten by the
   fallback's normal begin/draw/end.
5. **Diagnostics.** `Canvas3D.ShadowSlotsCached` exposes the existing
   `last_shadow_slots_cached` counter (signature reuse + inheritance), so the
   win is observable per frame instead of inferred.

## Consequences

- With the flag on, a render-target frame's shadow cost drops from the full
  rig to the cascade region alone; the night-stadium atlas tiles (six faces
  per shadowed point light) stop re-rendering entirely on those frames.
- Inherited slots are one frame stale with respect to moving casters —
  imperceptible at inset scale, and strictly better than the pre-existing
  cross-camera cache thrash.
- A render-target frame still overwrites cascade-slot signatures, so a scene
  with fully static cascade content loses cascade signature reuse on the
  window frame while an RT camera is active. Skinned casters zero cascade
  signatures anyway, so scenes with animated actors (the motivating case)
  lose nothing.
- Cached light entries are matched to the RT frame's queued lights by
  parameter equality; a light whose parameters change between the window
  frame and the RT frame simply loses its shadow for that frame (it re-renders
  on the next full pass).
- `test_canvas3d_rt_shadow_inherit.zia` pins the no-cache fallback, the
  full-inherit path (distinguished from signature reuse by a moving caster),
  the flag-off behavior, and the cascades-re-render/cubes-inherit split, on
  the software and Metal lanes.
