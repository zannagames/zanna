---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0208: Add Project-Authored Game Output Frames

## Status

Accepted (2026-07-27)

## Context

ADR 0207 removed editor chrome from scene Game View, but deliberately retained
the current editor camera, zoom, scroll, and viewport shape. That made the mode
safe and useful for inspection, yet it did not define the frame a running game
would actually present. A wide editor lane could expose more of a 2D world than
the game's camera, crop a different vertical range, or drive a 3D perspective
camera with a different aspect ratio. Resizing docks could therefore change the
composition being reviewed.

Xenoscape renders a 1280-by-720 game frame and Ashfall renders a 1600-by-900
frame. Both projects already provide declarative scene-preview data, but Studio
had no project-owned output-size contract. Authors had to approximate the game
camera manually and could still capture an editor-aspect image.

The output contract belongs to the project preview schema rather than a scene
file: every scene in a game normally shares its presentation size, and adding
editor metadata to canonical scene content would make visual inspection dirty
the asset.

## Decision

Schema version 12 adds an optional paired `outputWidth` and `outputHeight` to
both `scenePreview2D` and `scenePreview3D`.

- Both members are required together.
- Each is an exact integer from 64 through 8,192.
- Either member in a schema older than version 12 rejects the whole schema.
- A version-12 `scenePreview2D` may consist only of the output pair; a
  background mapping or grid default is no longer required in that case.
- Structured component edits preserve the pair and keep the file at version 12
  while either member remains.

The values are editor-only project presentation. Studio does not write them to
2D scene JSON, VSCN, session data, runtime state, or game configuration.

### Shared Game View frame

When the active profile declares an output size, Game View finds the largest
centered rectangle with that aspect ratio inside the current scene viewport.
Unused pixels are filled with a neutral near-black matte. The toolbar and
accessible description name the declared output size, and navigation controls
are disabled because changing the camera would stop the frame from representing
the project contract.

Projects without the version-12 pair retain ADR 0207 behavior: Game View still
masks editor chrome but keeps free pan, zoom, orbit, dolly, fly, projection,
framing, and quick-view navigation.

### 2D framing

The 2D editor scales authored pixels so the declared output extent fits the
centered frame, then centers the camera on the conventional gameplay start:
typed `playerStartX`/`playerStartY`, a `player-start` object, or the scene
center, in that order. Project background `cover`, `contain`, and `stretch`
scaling use the content rectangle rather than the surrounding matte. Tiles,
object draw stacks, animation, and all ADR 0207 overlay masking remain
unchanged.

Wheel, middle-drag, toolbar zoom, Fit/1:1, and `F` framing are inert while this
output-framed mode is active. Canvas focus and drop refusal remain available so
the mode is keyboard- and accessibility-truthful.

### 3D framing

The 3D editor creates its retained `RenderTarget3D` at the content rectangle's
dimensions, not the outer editor viewport's dimensions. Camera aspect,
perspective eye distance, orthographic extent, projection, and post-processing
therefore all use the declared game aspect. Finalized authored pixels are copied
into a full-viewport matte buffer only after the project post-FX chain runs.
Projection helpers add the centered content offset so diagnostic probes and any
future content-space overlays agree with the rendered camera.

Entering output-framed Game View reapplies the project gameplay camera when the
editor is not already looking through an authored camera node. An active
authored-camera look-through remains authoritative. Orbit, pan, dolly, fly,
projection, quick views, framing, bookmarks, and the project Gameplay reset are
inert until the user exits.

### Reversible workspace state

Each document workspace stores a `gameViewOutputFramed` latch beside the
existing Game View flag. Before entry, Studio snapshots the exact editor camera:
2D zoom and logical scroll; or 3D yaw, pitch, scale, target, gameplay-eye anchor,
projection, and overlay preferences. Repeated frame pumps and tab switches do
not overwrite that snapshot with the temporary project camera. Exiting restores
the snapshot exactly and then resumes ordinary workspace capture.

The latch is process-local and is not persisted by `SessionManager`. If an
active tab is switched away and back, Studio reconstructs the temporary output
camera from the still-valid project profile while retaining the underlying
editor snapshot.

## Validation

The component-schema probe covers valid 2D and 3D pairs plus old-version,
missing-partner, fractional, undersized, and oversized rejection. The structured
authoring probe proves ordinary component edits preserve both version-12
profiles.

The real Xenoscape and Ashfall preview probes assert declared output sizes,
centered aspect geometry, matte behavior, disabled navigation, content-sized 3D
render targets, byte/history isolation, and exact editor-camera restoration.
Their captured PNGs are produced from the output-framed Game View.

No runtime API, runtime C ABI, IL, external dependency, or platform-specific
branch changes are required. The implementation uses the existing portable
`Pixels`, `RenderTarget3D`, `Canvas3D`, and camera surfaces.

## Consequences

- Game View now answers “what does the game camera show?” instead of only
  “what remains after editor chrome is hidden?”
- Dock and window shapes no longer alter the aspect or composition under
  review; matte makes unused editor space explicit.
- Normal editor navigation remains flexible and returns exactly on exit.
- Projects opt in declaratively and older schemas keep their established
  behavior.
- Projects that change their shipping resolution must update the schema pair
  deliberately; Studio does not infer output size from a transient window.

## Alternatives Considered

- **Use the current editor viewport aspect.** Rejected because it is the source
  of the visual mismatch and changes with dock layout.
- **Store output size in every scene.** Rejected because it duplicates
  project-wide presentation data and would contaminate canonical game assets
  with an editor concern.
- **Render at the full declared pixel dimensions.** Rejected because an
  8,192-by-8,192 offscreen allocation is unnecessary for an embedded preview;
  rendering at the largest on-screen rectangle preserves aspect and bounded
  work while the schema values still describe the project output.
- **Allow navigation inside the declared frame.** Rejected for the opt-in mode
  because it would immediately stop representing the project's gameplay
  camera. Legacy profiles retain the free-navigation inspection mode.
