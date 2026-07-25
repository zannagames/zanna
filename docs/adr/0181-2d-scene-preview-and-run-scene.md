---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0181: Add 2D Scene Preview and Run Scene

## Status

Accepted (2026-07-24)

## Context

The Studio 2D canvas is a static editing surface: authored tile animations
never play, camera framing is invisible, and seeing a scene in motion means
leaving the editor, building the game, and navigating to the scene by hand.
With tile behavior (ADR 0176) and camera/lighting settings (ADR 0177) now
authored data, the editor holds everything needed to show motion and framing
directly — and the existing build/run system already launches project run
configurations with argument vectors, which is the honest way to "play" a
scene: through the game that owns it, not through an editor emulation of
gameplay.

## Decision

### In-editor preview

The 2D editor gains a **Preview** toggle beside the existing tile tools:

- While enabled, tiles whose base ID has an authored animation render their
  current frame from the typed animations state on a bounded shared clock;
  resolution reuses the per-frame durations exactly as `Tilemap` resolves
  them at runtime, so editor playback and game playback agree.
- The camera section, when present, draws its bounds rectangle and, in
  follow mode, a deadzone rectangle centered on the player-start marker when
  one exists. The lighting section, when present, may dim the canvas by its
  authored darkness as a toggleable visualization.
- Preview is per-document workspace state, following the owning tab and
  bounded session state like tool mode. It repaints only cells whose
  resolved frame changed on animation ticks, and suspends while the
  document is not the active tab.
- Preview never changes canonical content, revision, history, dirty state,
  or selection, and every editing tool remains available while it runs;
  edits behave identically with preview on or off.

### Run Scene

A **Run Scene** command (menu, command palette, and 2D editor toolbar)
launches the owning project's existing run configuration with the active
scene's path appended as the final program argument, using the same
argument-vector job, streamed output, and status behavior as ordinary
Run. The contract is deliberately thin:

- The scene must be saved and inside an open workspace root that owns a
  project run configuration; otherwise the command is disabled with a
  language-service-style reason.
- A dirty scene triggers the existing save-before-build preflight.
- Studio passes the path verbatim; the project's entry point decides what a
  trailing scene path means (the convention documented for game templates is
  a `--scene <path>` pair; the recreated Xenoscape adopts it). Studio makes
  no attempt to verify the game actually loaded the scene.

## Consequences

- Animation timing and camera framing become visible while authoring,
  closing the largest remaining feedback gap in the 2D editor.
- "Play the scene" resolves to the game's own binary and loader, so preview
  fidelity claims stay honest: Studio previews data, games execute it.
- The shared animation clock and damage-scoped repaint keep preview within
  the editor's existing frame budgets; a pathological animation set degrades
  to slower frame advancement, never to unbounded work per paint.
- A Studio probe must pin: frame resolution equivalence with `Tilemap`
  playback rules, workspace-only guarantees (bytes, history, dirty state,
  selection), per-tab clock suspension, overlay correctness, Run Scene
  argument passing, disabled-state reasons, and dirty-save preflight.

## Alternatives Considered

- **Embedding a live game loop in the editor ("play in editor").** Rejected:
  Zanna games own their loops, input maps, and systems; an editor-hosted
  loop would preview a second, diverging implementation of every game.
- **Launching a generic runtime scene viewer.** Rejected: without the owning
  game's tile behavior consumers, entities, and lighting, the viewer would
  show something no player ever sees.
- **Auto-detecting scene support by scanning game sources for `--scene`.**
  Rejected: fragile heuristics over arbitrary game code; a disabled-state
  reason plus a documented convention is truthful and cheap.
- **Animating via a live `BuildTilemap()` copy.** Rejected: building a full
  render/collision copy per edit tick duplicates document state; the typed
  animations table is already in the document and is the exact source the
  runtime consumes.
