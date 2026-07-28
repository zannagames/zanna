---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0207: Add a Non-Destructive Scene Game View

## Status

Accepted (2026-07-27)

## Context

Project-owned 2D backgrounds, object sprites, draw stacks, 3D environment
prefabs, camera profiles, lighting rigs, atmosphere, and post-processing let a
Studio scene use the same visual inputs as its game. The ordinary authoring
view still deliberately paints editor information over those inputs.

The 2D canvas historically framed every object, including objects whose real
sprite had already rendered, then added fallback markers, light halos, routes,
camera bounds, the grid, rulers, guides, selection cells, handles, and tool
ghosts. Xenoscape therefore had the correct art and draw order but still looked
like a debug diagram. The 3D viewport similarly retained grids, node markers,
gizmos, selection brackets, collider/light/camera overlays, a camera inset, and
the orientation navigator over Ashfall's finalized frame. Its default editor
ambient floor could also differ from the project's runtime lighting.

Turning each overlay preference off manually is slow, destructive to a user's
workspace setup, incomplete (some chrome has no individual toggle), and easy to
forget before visual comparison. Reusing **Preview** would conflate 2D
animation playback with presentation. Reusing 3D **Gameplay** would conflate a
project camera reset with clean rendering. Reusing **Focus** would conflate
outer workbench layout with viewport pixels.

## Decision

Both scene editors expose an explicit **Game View** action in the view toolbar.
It is per-document, process-local workspace state. Entering or leaving it does
not change canonical scene bytes, document dirty state, revision, undo/redo,
selection, camera/scroll/zoom, tool choice, or any underlying overlay
preference. Switching tabs restores the mode belonging to that open document.
It is intentionally separate from Preview, Gameplay, Scene Layout, and Focus.

### 2D presentation and input

Game View keeps the project background, every authored-visible tile layer and
its opacity, and project/per-object sprite previews in the exact shared draw
stack. It temporarily ignores the editor-only Solo choice without rewriting
it. It masks:

- the tile grid, rulers, guides, camera/deadzone bounds, and routes;
- fallback object markers, light halos, every object frame, selection frame,
  sprite transform handles, and marquee/region frames;
- paint, erase, stamp, rectangle, line, ellipse, and autotile-neighbor ghosts.

Animation playback becomes effective while Game View is active without changing
the user's independent **Preview** toggle. Middle-drag pan, pointer-anchored
wheel zoom, toolbar zoom/Fit/1:1, and `F` to fit remain available. Canvas tool,
tile, grid, ruler, Preview, drop, and pointer-edit paths are disabled. The
canvas tooltip and accessible description name the reduced interaction model.
An already captured mutation or ruler gesture must finish or cancel before the
mode can be entered.

### 3D presentation and input

Game View keeps authored and project-preview geometry, materials, maps, scene
lights, project sky/lighting/atmosphere, and the finalized project post-FX
chain. It temporarily uses the shaded material pass even when the document's
underlying viewport preference is Wireframe or Shaded+Wire, and it suppresses
the editor ambient-light assist. It masks:

- the grid, hierarchy links, node/component markers, and transform gizmos;
- light, camera, collider, and route overlays;
- hover/selection brackets, marquees, the selected-camera inset, and the
  orientation navigator.

Orbit, pan, dolly, fly navigation, projection changes, framing, quick views,
and the project **Gameplay** camera reset remain available. Viewport selection,
gizmo, rename-shortcut, marquee, and drop paths are disabled. Entering during a
gizmo drag restores its exact origin before enabling the mode. The prior
shading mode, overlay options row, editor-light preference, selection, and
chrome return on exit.

### State and validation boundary

The mode is stored only on `Scene2DWorkspaceState` or
`Scene3DWorkspaceState`; it is not a scene-format member and is not written by
`SessionManager`. Renderers derive effective presentation from the flag rather
than rewriting the underlying preferences. Pointer hit helpers for masked 3D
chrome also return no target while active, so invisible controls cannot retain
input authority.

The focused cross-editor display probe pins exact authored 2D pixels, 3D
navigator removal, editing-control gating, real pointer click suppression,
per-tab isolation, preference/selection restoration, and byte-exact history
isolation. The real Xenoscape and Ashfall probes enter Game View before saving
their comparison PNGs, ensuring those artifacts remain free of editor chrome.

No scene format, project schema, runtime API, runtime C ABI, dependency, or
platform-specific branch changes.

## Consequences

- A scene can be compared with its running game in one click without dismantling
  the authoring workspace.
- Editor overlays remain rich and default-on in normal view; fidelity no longer
  requires weakening normal authoring affordances.
- Game View is safe to leave active on one tab while editing another.
- 2D animation and 3D runtime-like shading become truthful in the clean frame
  while their independent user preferences restore exactly.
- New editor-only viewport chrome must be gated by Game View and covered by the
  focused probe before it can ship.

## Alternatives Considered

- **Remove most overlays from the normal editor.** Rejected because selection,
  spatial handles, light/collider bounds, routes, and fallbacks are essential
  authoring information.
- **Save and clear every individual overlay toggle.** Rejected because it is
  incomplete, produces many state transitions, and risks overwriting choices
  when a new overlay is added.
- **Use Preview, Gameplay, or Focus as the toggle.** Rejected because each has
  an independent animation, camera, or layout contract.
- **Run the game inside the viewport.** Rejected for this milestone because it
  grants project code execution authority and introduces mutable game state;
  Game View is a deterministic authoring-render presentation.
