---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0202: Add Project-Owned 2D Scene Background Previews

## Status

Accepted (2026-07-27)

## Context

Studio can render a 2D scene's exact tile atlases and project-owned object
previews, but every scene still starts over the same dark editor clear color.
That is especially misleading for games whose runtime draws a large
screen-space sky, biome, or atmospheric backdrop before its tiles. The scene
has the correct geometry while looking unlike the game players see.

Studio cannot safely infer a project's scene-property vocabulary or execute its
renderer. Copying editor-only image paths into every scene would duplicate
project-wide presentation truth and produce noisy scene diffs. A collaborator
and CI also need the same deterministic view without per-user setup.

## Decision

### Component-schema version 8

`scene-components.json` version 8 adds one optional top-level
`scenePreview2D` object:

- `background` is an optional fallback image.
- `variantProperty` and `variantImages` are an optional pair. The property must
  be a portable scene integer key, and its value selects one of 1–64 images.
- `variantOffset` is an optional bounded integer added to the property value
  before indexing `variantImages`. It requires the variant pair.
- `fit` is optional and is `cover` (the default), `contain`, or `stretch`.
- `showGrid` is an optional first-open grid default.

The profile must contain a background mapping or `showGrid`. A fallback image
may accompany variants; a missing, wrong-typed, or out-of-range property uses
that fallback. Without a fallback, Studio retains its ordinary clear color.

Image references are forward-slash, project-root-relative PNG, JPEG, BMP, or
GIF paths of at most 1,024 characters. Absolute paths, drive/URI syntax,
backslashes, and parent traversal are rejected. Paths resolve beside the
project's `scene-components.json`, so moving a scene within its project does
not change its preview art. Wrong-version, malformed, unsafe, or oversized data
rejects the complete component schema without publishing partial rules.

### Read-only screen-space composition

Studio composites the selected image after its opaque editor clear and before
tiles, objects, guides, and selection overlays. The backdrop is screen-space:
scrolling the world does not pretend to reproduce game-specific parallax.
`cover` preserves aspect ratio and crops centrally, `contain` preserves aspect
ratio with the editor clear visible around it, and `stretch` fills the visible
render window exactly.

The existing bounded image policy applies: a source is at most 16 MB and
4,194,304 decoded pixels, all decoded scene imagery shares the 8,388,608-pixel
budget, and a scaled backdrop is at most 4,096 pixels per axis and 8,388,608
pixels. One decoded and one scaled backdrop are cached. Image failures fall
back to the normal clear color, and external metadata changes participate in
the existing bounded image-refresh poll.

`showGrid` applies only when a document has no saved 2D workspace state.
Subsequent tab/session restores and explicit user toggles win. Resolving or
rendering a profile never changes scene bytes, revision, dirty state, or
history.

Projects whose runtime art is procedural may bake deterministic ordinary
images by calling the same renderer at a fixed time. Studio consumes only the
resulting bounded files; it does not load project modules or run startup code.

## Consequences

- 2D scenes can resemble their game immediately while retaining Studio's full
  selection, grid, and editing overlays.
- Preview truth travels with the project and works identically on macOS,
  Windows, and Linux without dependencies or runtime ABI changes.
- Versions 1–7 and projects without `scenePreview2D` retain their prior
  rendering and first-open grid behavior.
- Probes must pin schema bounds/path rejection, variant/fallback selection,
  fit composition, first-open defaults, and the no-dirty invariant.

## Alternatives Considered

- **Execute the owning game renderer inside Studio.** Rejected because startup
  side effects, input loops, timing, saves, and arbitrary project code are not
  a safe deterministic editor contract.
- **Recognize Xenoscape's region names directly.** Rejected because project
  vocabulary and file naming must not leak into the general editor.
- **Store a background path in each scene.** Rejected because it duplicates
  project-wide authoring presentation and makes asset moves touch every scene.
- **Implement generic live parallax.** Deferred because a guessed parallax
  model would still diverge from game code; a truthful fixed runtime snapshot
  is preferable to simulated behavior with different semantics.
