---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0179: Add 2D Object Sprite Previews and Route Visualization Conventions

## Status

Accepted (2026-07-24)

## Context

The Studio 2D canvas renders every placed object as a generic disc-and-frame
marker. Tile layers show their real atlas frames, but an enemy spawn, pickup,
or trigger is an anonymous orange dot: authors must select each object to
learn what it is. Similarly, ordered waypoint data (patrol routes, moving
platform paths) can already be expressed with the object hierarchy, but the
canvas gives no visual connection between a route and its waypoints.

Both problems are presentation, not format. The scene schema already has
typed string properties, an organizational hierarchy with stable sibling
order (ADR 0164), and bounded external image budgets for layer atlases.
Adding editor-only conventions over existing data keeps every existing
runtime, save, and game untouched.

## Decision

### Sprite preview convention

An object may carry these ordinary typed properties:

- `editor.sprite` (string) — image path resolved exactly like a layer asset
  reference (scene-relative first, workspace roots second; PNG, JPEG, BMP,
  GIF).
- `editor.frame` (int, default 0) — frame index into the image using the
  scene's tile grid, identical to layer-atlas frame addressing.

When `editor.sprite` is absent, the canvas falls back to the `sprite`
property when it names a loadable image, then to the existing marker. The
resolved frame is drawn at the object's cell, scaled to the display cell
size; the existing selection disc/frame remains drawn on top so selection
feedback is unchanged. Missing, invalid, or over-budget images fall back to
the marker with the same placeholder discipline layer atlases use.

Preview images load through the existing bounded image machinery and count
against the existing per-source 16 MB, per-image decoded-pixel, and
aggregate decoded/cached scene-imagery budgets — sprite previews and layer
atlases share one budget, so a hostile scene cannot multiply retained
pixels. External image changes refresh quietly under the same metadata
polling as layer images. Preview resolution never mutates scene content,
history, revision, or dirty state.

The `editor.*` property namespace is documented as editor-facing convention:
games ignore it, and Studio never writes it implicitly — it appears only
when an author or tool (schema template, exporter) authors it.

### Route convention

A route is an ordinary object whose project component defines it as such
(by convention a component named `route`); its waypoints are its direct
children in existing sibling order, each an ordinary object. The canvas
draws a workspace-only polyline from the route object through each waypoint
in order, highlighted when the route or any waypoint is selected. Waypoint
editing is ordinary object editing — drag, nudge, reorder via the hierarchy,
duplicate, clipboard — with no new tools.

Rendering is bounded: at most 256 waypoints per route are drawn (the full
list remains editable through the hierarchy), and polylines clip to the
rendered canvas region. Objects with a `light` component receive the halo
marker defined in ADR 0177. All of these overlays are workspace-only
presentation and never serialize.

## Consequences

- A populated scene reads at a glance; authors stop selecting dots to learn
  what they placed.
- Zero format or runtime change: every convention rides on existing typed
  properties and hierarchy order, so old runtimes, old scenes, and games
  that never adopt the convention are unaffected.
- Exporters and schema templates can attach `editor.sprite` hints so
  generated scenes are immediately readable in Studio.
- The shared image budget means sprite-heavy scenes degrade to markers
  deterministically instead of growing memory.
- A Studio probe must pin: preview drawing and fallback order, budget
  sharing with layer atlases, marker fallback for missing/invalid frames,
  route polyline order following sibling order, the 256-waypoint render
  bound, and that preview/overlay work never dirties a scene.

## Alternatives Considered

- **A reserved schema field on objects (e.g. top-level `sprite` metadata).**
  Rejected: it would change the scene schema and force every consumer to
  learn an editor concern; typed properties already carry it.
- **Type-name → sprite mapping in a Studio settings file.** Rejected: the
  mapping belongs with the project content, travels with the scene, and may
  differ per object; a per-user mapping would make scenes render differently
  on different machines.
- **A dedicated route/path section in scene JSON.** Rejected: hierarchy plus
  sibling order already expresses ordered waypoints with full undo and
  clipboard support; a parallel structure would need its own editing tools
  and would drift from the object graph.
- **Embedding preview thumbnails in the scene file.** Rejected: scenes stay
  lean and diff-friendly; external references with budgets are the
  established pattern for layer imagery.
