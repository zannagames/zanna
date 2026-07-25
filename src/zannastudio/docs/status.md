# Zanna Studio Current Status

Last reviewed against source: 2026-07-24.

This file is the current-state reference for Zanna Studio. It intentionally avoids
future-phase language and records limitations in the same place as shipped
behavior.

## Summary

Zanna Studio is usable as a code editor and project workbench for Zia projects, with
partial BASIC support, build/run integration, a VM-backed debugger path, an
integrated terminal, and lightweight Git operations.

It is not yet a polished product-complete IDE. The largest current gaps are:

- Bottom tool surfaces now have dockable responsive shells and bounded
  stable-row models. Problems, Output, References, Debug Console, Variables,
  and Call Stack have contextual in-panel controls, while the concrete result
  surfaces remain listbox/output-pane based rather than fully virtualized
  workbench views.
- The primary Explorer/Source Control/Run-and-Debug sidebar now moves as one
  live, persisted left/right dock with its activity rail, direct drag targets,
  mirrored width, collapse recovery, and Command Palette actions.
- Source Control covers status, staging, commit, paged history, per-commit
  diffs, queued jobs, in-app credential prompts, and a guided edit-then-Stage
  conflict path, but is still not a full Git client (no merge/rebase
  orchestration or ours/theirs recovery tools).
- BASIC semantic navigation and rename are implemented by the IDE-side scanner,
  not by the Zia project index or external BASIC server.
- Routine command text entry and rename review are non-modal. Native file
  pickers and destructive/external-state confirmations remain modal.
- Reinvoking project search or build/run while work is active reveals its
  Search or Output panel and Stop control without interrupting the operation.
- Run and Debug is a persisted activity-sidebar view with truthful session
  controls, direct inspection-panel links, and filterable breakpoint actions.
- The application source still has several oversized coordinator modules.

## Current Product Narrative

The most accurate way to describe Zanna Studio today is "a functional Zanna
workbench whose strongest path is Zia code editing." The editor can open real
projects, keep multiple files alive across sessions, run semantic services, and
drive the compiler/debugger toolchain. A Zia developer can use it for daily
editing in a small or medium project, especially when the workflow is edit,
search, build, run, diagnose, and debug one active program.

The roughness shows up when the workflow starts to look like a mature IDE. Some
secondary panels are still row-oriented instead of rich work surfaces.
Problems, Output, References, Debug Console, and Call Stack now expose filtering
and relevant actions in context; common short inputs, project search, settings,
Quick Open, and multi-file rename review also use integrated workbench surfaces.
The debug substrate is real: a state-aware Run and Debug activity view,
persistent watches, state-aware call frames, a clearable/filterable console,
and structured expansion of collections and class instances (field-by-field,
via the compile-time layout sidecar of ADR 0138).
BASIC support is intentionally honest but incomplete.
Source Control covers daily local Git plus commit history, queued operations,
live push/pull output, in-app credential prompts, and safe basic conflict
resolution guidance, but it does not have the merge/rebase workflow depth or
conflict recovery tools of a full client. Scene
documents mount built-in 2D and 3D visual editors with hierarchy/layer,
viewport, property, history, and import workflows, but those tools remain
earlier and narrower than a mature game-engine editor.

This distinction matters for documentation and release notes. Zanna Studio should
not be described as a complete scene editor, complete SCM client, or full
multi-language IDE. The terminal now emulates the sequences full-screen
programs (vim, less, htop) actually emit — alternate screen, scroll regions,
cursor modes, bracketed paste, and status replies — but VT coverage beyond
that pinned table is not claimed. It should be described as a growing IDE with
clear working slices and clearly documented gaps.

## What "Implemented" Means Here

In this document, "implemented" means that the feature has a user-visible path,
source ownership, and at least some regression coverage. It does not always mean
the feature is polished or complete by mature IDE standards.

"Partial" means that the feature has working pieces but should not be marketed
as complete. Partial features need explicit limitations in docs and UI.

"Not present" means that users may see related groundwork in code or runtime
APIs, but Zanna Studio does not provide the user-facing workflow yet.

## User Impact Summary

For Zia and BASIC developers, the biggest strengths are:

- The editor understands source well enough for completion, diagnostics, hover,
  signature help, symbols, semantic navigation, references, and rename.
- Project search, Quick Open, workspace symbols, and recent files make normal
  navigation practical.
- Build/run/debug are wired to the Zanna toolchain without leaving the IDE.
- Session restore and recovery reduce the risk of losing active work.

For a game developer, `.scene2d` and `.scene3d` documents (plus the legacy
`.scene`/`.level`/`.vscn` aliases) mount built-in
2D and 3D authoring surfaces. They cover a useful first hierarchy, viewport,
property, undo/redo, save, and import workflow. The 2D editor resolves layer
tileset images, renders their real frames in the canvas, and exposes a bounded
clickable palette. Its canvas supports replace, Shift-add, and
Control-or-Command-toggle point selection plus an inclusive authored-cell
marquee; selection feedback and canceled gestures remain workspace-only. Both
inspectors expose bounded, searchable project-asset choosers backed by the
existing multi-root workspace index. The 3D inspector authors compact PBR
material components with truthful mixed-value batch fields and embeds or
clears common albedo, normal, metallic/roughness, ambient-occlusion, and
emissive maps across a selection without mutating unselected users of shared
imported materials. The 3D viewport renders the live SceneGraph's authored
meshes and PBR materials through a windowless Canvas3D that requests the
platform GPU backend and falls back to the deterministic software rasterizer
truthfully (ADR 0191; probes always use the software path). Interactive
camera and gizmo drags render at reduced resolution and re-render at full
resolution on release, and the viewport now sizes up to the pane rather than
a fixed 1600x1000 cap. Shaded and
triangle-wireframe modes retain exact alignment with the editor grid,
hierarchy links, markers, selection, and transform gizmos; mode is per-scene
session state and never edits VSCN. The 3D viewport selects the nearest visible
transformed mesh bounds, falls back to bounded origin markers for meshless
nodes, supports replace/Shift-add/Control-or-Command-toggle/blank-clear
selection, and pans in the camera plane with Shift plus middle/right drag.
These interactions update workspace state only. Both hierarchies are
real expandable TreeViews with stable non-display row identities,
Ctrl/Command-click and Shift-click multi-selection, collapse retention, and
transactional above/onto/below row drops. `Ctrl`/`Cmd`+`F` reveals a
case-insensitive hierarchy query; Previous/Next wraps, expands ancestors, and
selects a represented match without filtering or scene mutation. The 2D
outliner moves complete object
subtrees while preserving absolute positions; the 3D outliner preserves
complete world transforms by default. Batch drag/transform, duplicate, and delete actions
commit as one undoable transaction. The 3D visibility checkbox shows a native
mixed state and resolves the complete node selection in one transaction. The
2D inspector can create, rename, update, and remove scene-wide typed metadata,
as well as set/remove one typed property across the object selection.
Scene-metadata selection is tab-local and survives session restore. The 3D
numeric inspector applies explicit relative position/rotation/scale batches.
The 2D Select tool also owns
focus-safe one-pixel or one-tile keyboard nudging plus primary-axis alignment
and stable distribution commands. The 3D Parent chooser reparents selected
top-level subtrees in one cycle-safe transaction, preserving complete world
transforms by default and rejecting singular/sheared conversions with exact
rollback. A preserve-local opt-out supports intentionally parent-relative
authoring, and selection is remapped after hierarchy order changes.
Earlier/Later moves one contiguous same-parent selection as a stable sibling
block with exact VSCN history and selection preservation; direct row drops use
the same runtime primitives, preserve complete world transforms by default,
and roll back the whole group on failure. Its Gameplay metadata group
creates, renames, updates, and removes bounded null, Boolean, integer, float,
and string values on one node. Exact scalar kinds round-trip through VSCN v6,
one accepted action is one history transaction, and the selected metadata row
remains local to its scene tab/session. Both editors load bounded project-root
`scene-components.json` templates and atomically add missing typed fields
across a selection without overwriting authored same-kind values. Their shared
structured schema form maintains every cross-target definition with validated
atomic writes, unknown-member preservation, external-conflict detection, and
independent 20-step file undo/redo without dirtying a scene. Project component
schemas accept version 2 with validated enum-choice and asset-reference
fields (authored as ordinary string values), the structured schema form
authors both kinds and writes the lowest required file version, and a saved
field rename/retype offers an explicit scan-review-confirm migration across
the workspace root's 2D and 3D scenes with per-file transactional refusals:
2D scenes convert through SceneDocument and 3D scenes through the canonical
VSCN loader (load, convert with the same representation-preserving rules,
save), each guarded by the scanned mtime and an exact match-count recheck;
grafted prefab instance content is excluded from counts and conversion since
it never serializes back. The 2D tile
palette owns a per-tile behavior inspector: collision kind (solid or
one-way-up), typed int/bool tile properties, per-frame tile animations, and
16-variant autotile rules author through the typed SceneDocument sections of
ADR 0176, each accepted edit is one canonical history transaction, and palette
frames carrying behavior show a corner badge. Objects with an `editor.sprite`
or loadable `sprite` string property render their authored atlas frame in the
canvas under the shared image budgets, falling back to the existing marker;
`route`-typed objects draw an ordered child-waypoint polyline and
`light`-component objects draw a halo ring, all workspace-only. Camera and
Lighting inspector groups author the complete scene-global sections of
ADR 0177 as single transactions with a canvas bounds overlay; the runtime
validates every documented field range. An optional per-root
`asset-library.json` (ADR 0180) annotates discovery with tags and native
frame grids: the project asset browser filters by tag or library membership,
badges tagged rows, edits entries through the same atomic
conflict-safe/undoable file transaction model as the component schema, and a
library grid that disagrees with the scene's tile size is reported without
resizing anything. Runtimes and games never read the library. A Preview toggle
plays authored per-frame tile animations on a bounded workspace-only clock
with camera-bounds and follow-deadzone overlays — canonical bytes, history,
and dirty state never change and every tool stays usable — and a Run Scene
command (toolbar and Command Palette) runs the owning project with
`-- --scene <path>` appended so the game's own entry point loads the scene
(ADR 0181). Persistent assigned-map thumbnails are present, but
automatic schema/scene-data migration, generalized runtime components,
material-library/thumbnail workflows, and advanced cubemap/lightmap authoring
remain well short of a mature game-engine editor.

The standard Edit menu and keybindings are surface-aware: Cut, Copy, Paste,
Select All, and Duplicate Selection operate on scene objects or hierarchy
nodes when a visual scene owns the active tab; scene-only Delete is available
as well. Duplicate/Delete keyboard dispatch requires hierarchy or viewport
focus, leaving inspector text editing native. Copy uses a typed text envelope
rather than guessing from scene syntax. Same-kind paste works across scene
tabs, preserves 2D typed properties
or complete selected 3D subtrees and resources, offsets the new content, and
commits exactly one undoable transaction. Wrong-kind, malformed, oversized, or
unreconstructable clipboard data leaves the destination byte-exact.

For a user expecting a polished workbench, the rough areas are mostly around
large-result panel density, advanced Source Control recovery workflows, deeper
scene authoring, and panel virtualization for very large result sets.

## Feature Matrix

| Area | Status | Notes |
| --- | --- | --- |
| Text editing | Implemented | Multi-tab CodeEditor, undo/redo, selections, comments, formatting, folding, minimap option. |
| Split editor | Implemented (v1) | Two side-by-side panes ("Split Editor Right", "Focus Other Editor Pane", "Close Editor Split", click-to-focus). Each pane owns a distinct document and the focused pane drives the active tab, typing, IntelliSense, find, minimap, status, save, and recovery state. Opening a document already visible in the other pane focuses its existing owner, preventing divergent buffers and stale overwrites. Split-active state is restored when at least two documents are open. v1 limits: exactly two panes; open a second document before splitting; same-document multi-view awaits shared-buffer runtime support. |
| Workspace layout | Implemented with limits | The primary Explorer/Source Control/Run-and-Debug sidebar has a draggable header, stationary left/right targets, explicit arrows and Command Palette actions; its activity rail, logical width, visibility, active view, and position persist together. Problems/Output/Search/References/Variables/Call Stack/Debug/Terminal can form simultaneous left/bottom/right groups plus one movable/resizable in-window floating group: each header exposes direct selected-tool movement, the Command Palette exposes the same actions, and the primary group can drag/merge/float as a unit. Floating bounds recover across viewport/UI-scale changes and membership/order/attached sizes/floating geometry replay through migration-safe settings. Explicit cards and narrow edge strips make redocking deliberate, while Reset Workspace Layout restores coherent attached defaults. Native secondary-window and multi-monitor detachment are not implemented. |
| Zia IntelliSense | Implemented with limits | Completion, diagnostics, hover, signature help, symbols, definition, references, rename, workspace symbols. |
| BASIC IntelliSense | Implemented with limits | Completion, diagnostics, hover, document symbols, scanner-backed definition, references, rename, workspace symbols, call hierarchy, and signature help. |
| Plain text | Implemented | Opens unknown/text-like files as text without semantic features. |
| Scene files | Implemented with limits | `.scene2d` mounts the 2D editor and `.scene3d` mounts the 3D editor (legacy `.scene`/`.level`/`.vscn` remain accepted). Both retain per-document workspace/history state and provide real expandable multi-select hierarchies, transactional before/into/after row drops, group edits, typed gameplay data, searchable project assets, hierarchy-preserving clipboard transfer, undo/redo, and safe save/import flows. The 2D surface includes a runtime-backed organizational hierarchy with absolute positions, one-step root/child creation, explicit cycle-safe multi-root reparenting, stable subtree/sibling ordering, real bounded atlas rendering/palettes, viewport-windowed canvas rasterization (only visible cells render, so large maps stay fully scroll-reachable; 100% zoom equals the authored tile resolution, the wheel zooms around the pointer, and Fit/1:1/grid-toggle controls join the view toolbar), captured gap-free paint/erase with exact cancellation, inclusive rectangle paint, four-connected fill, active-layer tile picking, modifier-aware point and inclusive authored-cell marquee selection, object dragging, scene/object properties, nudging, alignment, and distribution. The 3D surface includes a runtime-backed shaded/triangle-wireframe viewport with exact editor-overlay alignment, exact preserve-world chooser/direct reparenting with preserve-local opt-out, stable sibling ordering, mixed-state batch visibility, switchable Local/World Move/Rotate/Scale with snapping and atomic exact-or-reject world conversion, filled Move-plane and crossed Scale-plane XY/XZ/YZ handles, projected X/Y/Z rotation rings with wrap-safe angular dragging, truthful mixed-value batch PBR materials, batch embedded texture maps, mixed-value batch authoring for every runtime light type with hierarchy/viewport feedback, authored camera nodes with look-through and a bounded preview inset, collider-convention authoring with wireframe overlays, and route polylines/badges for project gameplay components. Both load compatible definitions from bounded root-local `scene-components.json`; Add Missing preserves same-kind values, rejects any type conflict before mutation, and commits the complete selection once. A shared structured form maintains the complete cross-target schema through parser-validated atomic writes, external-conflict detection, and separate bounded file undo/redo. The 2D tile palette authors per-tile collision, typed int/bool properties, per-frame animations, and 16-variant autotile rules as one-transaction typed-section edits with palette behavior badges. Schema v2 enum/asset fields, the explicit 2D/3D migration assistant, the per-root asset library with tag filtering and import-grid surfacing, and the project material library are present; automatic unattended migration and generalized runtime components are not. |
| 3D node gameplay metadata | Implemented with limits | One selected `SceneNode` exposes deterministically ordered null, Boolean, integer, float, and string values for roles, IDs, spawn/trigger data, and component parameters. Create, rename, update, and remove validate bounds/no-ops before one canonical VSCN history transaction; values round-trip through VSCN v6 and row selection stays with its tab/session. Project schemas can batch-add missing metadata to multiple nodes, while arbitrary raw metadata editing remains single-node. |
| Scene clipboard | Implemented with limits | Standard Cut/Copy/Paste/Select All commands follow the active visual editor. A versioned, typed text envelope supports same-kind cross-tab transfer of up to 1,024 selected identities and 64 MB total, preserving typed 2D properties and internal parent links or serializable 3D subtrees. Cut and paste are one-step history transactions with exact rollback. Mixed 2D/3D paste and interchange with other editors are intentionally rejected. |
| Project explorer | Implemented with limits | Demand-loaded, scrollable tree; multi-root support; Quick Open cache; file actions; ignores. Rename/move preserve live editor buffers and undo state, while delete releases any removed split-pane owner. |
| Search | Implemented | Docked project/folder search panel with a compact-window minimum results viewport, runtime-paged file discovery, per-frame file/byte budgets, literal/regex, case/word filters, include/exclude filters, grouped results, and generation-scoped frame-sliced Replace All with bounded atomic closed-file writes. Search/Replace completion remains in the panel and status bar instead of interrupting later work with a popup. |
| Build/run | Implemented | Argument-vector jobs, project manifest overrides, streamed bounded output, JSON diagnostics, and durable completion in Output/Problems/status without duplicate background toasts. |
| Tool panels | Implemented with limits | Problems/Output/Search/References/Variables/Call Stack/Debug/Terminal retain their live controllers while moving among independent left/bottom/right groups or the in-window floating group, support group-local movable tab order, and use responsive controls. Problems has live text/severity filters, counts, durable navigation metadata, and selection-aware Quick Fix. Output has live filter/wrap/follow/copy/clear controls. References has grouped live filtering, visible/total counts, durable click-to-open locations, Copy All, Clear, and contextual empty states. Debug Console has filter/wrap/copy/clear controls and Call Stack has filter/copy controls plus live debugger-state empty text. In-panel Clear retains the visible Output, References, or Debug Console surface. Result widgets remain bounded ListBox/OutputPane surfaces rather than fully virtualized views. |
| Debugging | Implemented with UX gaps | External VM debug adapter, breakpoints, stepping, pause, async restart, run to cursor, locals, call stack, evaluate, conditions, and logpoints. The persisted Run and Debug activity view exposes state-valid Start/Continue/Pause/Restart/Stop/step actions, links to Variables/Call Stack/Console, and durable filterable breakpoint Open/Remove/Condition/Logpoint actions. The docked Variables surface has inline Add/Remove/Refresh/Clear watch controls, Call Stack preserves frame identity through filtering, and Debug Console separates clearable program output from retained session status. Lists/seqs/maps plus class-instance fields expand with value previews. |
| Terminal | Implemented with limits | PTY-backed shell in OutputPane terminal mode: alternate screen, DECSTBM scroll regions, IL/DL/ICH/DCH/ECH, tab stops, cursor visibility, bracketed paste, application cursor keys, DSR/DA replies, SGR 16/256/truecolor + reverse. Coverage is pinned to the vim/less/htop sequence table, not full VT. |
| Source Control | Implemented with limits | Responsive, selection-aware Git controls for async status, stage/unstage, commit, per-path diff, worker-computed/incrementally rendered side-by-side diffs, paged commit history with per-commit files and diffs, queued serialized jobs, PTY-backed push/pull with live output and focused in-app credential prompts. Real unmerged rows show edit-then-Stage guidance and block Commit until resolved. No merge/rebase orchestration or ours/theirs recovery tools. |
| Settings | Implemented | Platform config path, theme, editor behavior, auto-save, save-before-build, session options, settings search, rebindable keyboard shortcuts, and debounced primary-sidebar/tool-dock position and split sizing. The body is vertically scrollable with a fixed action footer; compact windows give Preferences the full workbench lane and stack descriptions above controls without horizontal overflow. |
| Session restore | Implemented | Project, tabs, cursor/scroll, recent files/projects, bounded recovery text, and painted caller-budgeted startup restoration. |
| File watching | Implemented with limits | Active file watcher, inactive document polling, missing/deleted/moved-file conflict state, capped recursive workspace watcher set with fallback scans, and quiet metadata-polled refresh of external 2D layer images. |
| Visual polish | Implemented with limits | Zanna-brand palettes (WCAG-gated), scalable vector icons across toolbar/tree/tabs/status, smooth scrolling, gamma-correct text with ligatures, and viewport-bounded welcome/About/Preferences/diff, command, and semantic popup surfaces. Focus-taking Settings, About, explorer, breakpoint, command-input, and diff surfaces are mutually exclusive with popup menus, preventing stacked panels and ambiguous Enter/Escape routing. Build/Search notifications follow a contextual policy: their durable background results stay quiet, while immediate failures may warn; routine success remains status text. Chrome text, floating overlays, wrapped output, and responsive tool tabs share one effective-scale coordinate space without applying user zoom twice. Long list rows—including compact Recent paths—use explicit ellipsis and expose their complete unmodified text on hover instead of ending at a hard clip. The native workbench minimum starts at 720 by 520 and grows with whole-UI zoom, contracting against a desktop-chrome safety margin when the display cannot fit that floor. A requested minimap is temporarily suppressed below a useful editor-lane width and restored automatically when the lane expands, without overwriting the user's preference. Remaining density work is tracked per panel. |
| Cross-platform | Intended | Runtime adapters exist for process, PTY, GUI; display/runtime behavior still needs regular platform smoke. |

## Language Support

### Zia

Zia is the primary supported language. Current Zia features include:

- Syntax highlighting and semantic tokens.
- Completion with popup filtering, commit behavior, snippets, docs, runtime
  metadata, workspace-symbol completions, and stale-result rejection.
- Hover, signature help, and overload navigation.
- Live diagnostics and explicit "Run Check Now".
- Problems panel integration with diagnostic navigation.
- Non-blocking, revision/caret-gated fix-it application for supported
  structured diagnostics.
- Create Missing Bind for known runtime aliases and unambiguous project-file
  binds. Project discovery is bounded and asynchronous, and refuses ambiguous,
  incomplete, or changed candidate snapshots.
- Non-blocking Suppress Warning insertion for supported warnings.
- Definition, references, incoming calls, outgoing calls, and rename through
  `Zanna.Zia.ProjectIndex`. Project queries run on an owned background worker;
  delayed results require the same tab, revision, caret, workspace, and index
  generation, and large result sets render over multiple frames.
- Organize Binds.
- Extract Local Variable, Extract Function, and Inline Local Variable for
  deliberately conservative cases.
- Document formatting and selection formatting.

Known Zia limits:

- Workspace indexing is lazy and cooperative. A semantic command waits without
  blocking while the index warms up, then refuses the query if any source was
  unreadable/oversized or the 20,000-file/64 MB workspace ceilings were reached.
- Reference and call publication retains at most 2,000 results; rename refuses
  to apply when the reference ceiling is exceeded. Closed-file edits carry the
  expected symbol text so delayed content changes cancel the refactor.
- Refactors are intentionally conservative and reject many legal programs.
- Some UI panels use string display rows even when the underlying location data
  is structured.

### BASIC

BASIC support is implemented through a mixed runtime/IDE path:

- Completion.
- Diagnostics.
- Hover.
- Document symbols.
- Scanner-backed Go to Definition.
- Scanner-backed Find References.
- Scanner-backed Rename Symbol.
- Scanner-backed Workspace Symbols.
- Scanner-backed Signature Help.
- Scanner-backed incoming/outgoing call hierarchy.
- Project-wide definition, references, call hierarchy, and rename scans run on
  an owned background worker; unsaved open BASIC buffers override disk.
- Delayed results are rejected after a tab, caret, revision, or workspace-root
  change, and large References rows are painted over multiple frames.
- Formatting for supported line forms.
- Build/run through the same `zanna` toolchain path.

BASIC still has important limits:

- Semantic results come from `src/basic/semantic_scan.zia`, a lightweight
  scanner, rather than the compiler's full semantic model.
- Workspace scans are asynchronous and bounded, but are not backed by the Zia
  project index data structure. File/source/declaration ceilings can limit a
  navigation result; reference/call results cap at 1,000 rows.
- Rename refuses to apply when any workspace or reference limit was reached,
  and validates the scanned token text plus closed-file mtime before mutation.
- Ambiguous BASIC syntax and dynamic dispatch can still produce conservative or
  incomplete navigation results.

The command registry marks unavailable commands with language-specific reasons.

### Text And IL

Plain text, Markdown, JSON, and IL open as text buffers. The IDE provides core
editing, search, save, session, and file-watcher behavior, but no semantic
language service for these file kinds.

### Scene Files

`.scene2d` files (plus legacy `.scene`/`.level`) open in the built-in 2D
scene editor; `.scene3d` (plus legacy `.vscn`) files
open in the built-in 3D scene editor. Each open scene owns its selection,
viewport/camera, inspector, and undo/redo workspace state independently, while
document dirty/save/session behavior remains integrated with ordinary tabs.

The visual editors provide hierarchy/layer navigation, viewport selection and
camera controls, property editing, object creation/deletion/duplication,
history, and import/export-oriented file workflows. A 2D object drag and a 3D
transform drag each become one undo entry.

The 2D tile toolbox has captured gap-free Paint and Erase strokes, inclusive
forward/reverse Rectangle painting with a non-destructive preview,
four-connected Fill, and active-layer Pick. Escape rolls back a freehand stroke
or discards a rectangle preview, completed mutations serialize once, and exact
no-ops do not add history. Tile ID zero clears. Tool mode and selected tile
follow the owning tab and bounded session state.

The 2D canvas provides the same replace, Shift-add, and
Control-or-Command-toggle selection vocabulary as the hierarchy. Pressing an
already-selected object without a modifier preserves the full selection for a
group drag. Press-dragging from blank space draws an inclusive authored-cell
marquee: plain replaces, Shift unions, and Control/Command toggles. A plain
empty marquee clears while a modified empty marquee preserves the current
selection. Escape cancels an active captured marquee. The test is against each
object's authored point cell, not sprite or collider bounds, and every point or
marquee selection result remains workspace-only with no scene-content,
revision, history, or dirty-state change.

The object and node TreeViews support Ctrl/Command-click for additive selection
and Shift-click for visible ranges. Selection is recovered from byte-exact node
data rather than display labels, so duplicate or renamed labels do not redirect
a batch operation. Standard Find searches 2D object ID/type or 3D node name
across bounded represented rows, wraps in either direction, and restores a
hidden inspector/query and collapsed ancestors without changing canonical
bytes, history, dirty state, or camera state. In 2D, row drops move complete selected subtrees before,
into, or after a target as one cycle/depth/no-op-safe history transaction.
**Add Root** and **Add Child** create the intended structure directly in one
history step. The adjacent Parent chooser offers Scene root and bounded
cycle/depth-safe object destinations, reparents all selected top-level
subtrees, and is usable without a precision pointer drop. Object positions
remain absolute, and internal hierarchy links survive duplicate and
cross-scene paste. Dragging any selected object on the canvas moves the group
while preserving offsets; duplicate and remove apply to the complete selection
and preserve typed properties. While the Select button owns keyboard focus,
Arrow keys move
the selection by one pixel and Shift+Arrow moves it by one tile; inspector and
hierarchy controls keep their native arrow behavior. The layout row aligns X
or Y to the primary object and distributes three or more objects by stable
coordinate order, with fixed extrema and deterministic integer rounding.
In 3D, Move/Rotate/Scale apply the same axis delta to every
selected node's local transform, Frame Selected bounds the group, and
duplicate/delete collapse selected descendants beneath a selected ancestor so
a subtree is processed exactly once. Each accepted group action is one history
entry with exact rollback. The Parent row applies the same selected-root
collapse rule when moving existing nodes, filters cycle- and depth-invalid
destinations, preserves each moved root's local transform, and remaps the
complete selection after preorder indices change. Reserved 2D object identity
and 3D name/raw gameplay-metadata controls remain single-selection; visibility,
material, property, transform, and parent controls state their batch semantics
explicitly.

Edit > Cut, Copy, Paste, and Select All and their standard keybindings target
the active scene instead of the hidden text editor. The scene clipboard is an
explicit `2d` or `3d` envelope capped at 64 MB and 1,024 selected identities.
Paste accepts only a matching kind. A 2D paste reconstructs null, Boolean,
integer, floating-point, and string object properties with a unique ID and
one-tile offset. A 3D paste reconstructs each selected top-level subtree as a
sibling of the primary destination node, preserves its serializable components
and shared resources, assigns unique root names, and offsets local X by one
unit. Cut and paste each create one history entry; Copy and Select All do not.
Every rejected paste preserves the prior canonical content and selection.

Each 2D layer can reference a PNG, JPEG, BMP, or GIF atlas. Relative references
resolve beside the saved scene, the first 512 frames appear in a clickable
palette, and visible layers render their real atlas frames in the canvas.
Replacing or clearing a reference is one undoable transaction; unchanged or
rejected references do not alter history. Studio quietly checks one referenced
image per polling interval and refreshes changed pixels without dirtying the
scene; Reload Image remains available for an explicit reread. References remain
external rather than being embedded in the scene document. The layer inspector and Tiled
import section can search supported files already inside any open workspace
root, with at most 512 realized matches and bounded image previews. Studio
bounds each source to 16 MB, each
decoded atlas to 4,194,304 pixels, and aggregate decoded/cached scene imagery to
8,388,608 pixels. Missing, invalid, over-budget, or out-of-range frames retain a
deterministic placeholder and contextual inspector status.

The 3D viewport is projection-switchable through one retained camera:
perspective is the default and orthographic is one toggle away. Every overlay
and pick — markers, hierarchy links, gizmo axes, rotation rings, plane
handles, and the grid — projects through the same camera that renders the
shaded frame, so exact render/overlay/pick alignment holds in both modes, and
the derived perspective eye distance keeps pixels-per-unit at the view target
equal to the ortho scale so zoom, pan, and framing keep their meaning across
the toggle. Behind-eye projections are neither drawn nor pickable. Holding
right-mouse flies: mouse-look turns about the fixed eye, WASD moves in the
camera basis with Q/E world down/up, Shift is a fast multiplier, and the wheel
retunes a bounded speed while flying; release or Escape ends the capture, and
the W/E/R tool shortcuts stay suppressed during it. Otherwise the wheel
dollies about the pointer, middle-drag orbits, Shift+drag pans, and F frames
the selection or the whole scene. A Persp/Ortho toggle and a View... options
row (grid, marker, and light-overlay visibility, a live visible/culled stats
readout, and the three snap increments) live in the view toolbar; the
perspective grid distance-fades while the ortho grid is unchanged. All of this
state — projection, overlay toggles, stats, and snap increments — is
per-scene workspace state that follows the owning tab and session and never
touches VSCN content or history (ADR 0183).

Camera nodes are first-class authored components (ADR 0184). **+ Camera**
creates a node carrying an independent `Camera3D`; the Camera component
inspector authors projection (perspective fov or orthographic size) and clip
planes as one-transaction VSCN edits with no-op refusal and exact undo, inside
the runtime's sanitized clip envelope so inspector values always equal
effective renderer values. Camera nodes draw a pickable marker and an authored
frustum wireframe (near/far rectangles and edge lines) derived from the node
world transform and projected through the shared viewport camera, honoring
the camera-overlay toggle. **Look Through** drives the editor viewport from
the authored camera without touching the editor pose, navigation stays
suppressed while active, exiting restores the prior pose exactly, and
removing the camera or switching documents exits automatically. A bounded
picture-in-picture inset (256×144, refreshed at most every eight editor
frames during damaged frames only) renders the selected camera's actual view
and can be toggled off; look-through and the inset never change canonical
bytes, history, or dirty state. Editing a shared imported camera constructs a
replacement before assignment, so shared instances are never mutated in
place.

Each workspace root may carry a project material library:
`materials.scene3d`, an ordinary VSCN scene whose top-level nodes each hold
one named material (ADR 0189). The Material library group lists entries
fail-closed (byte budgets, parse failures, and naming violations publish a
truthful error instead of a partial list), **Save to Library** stores an
independent clone of the selected node's material through a staged,
conflict-guarded atomic replacement (overwrites need an explicit second
click), and **Apply from Library** clones the chosen material per selected
node as one canonical undoable transaction — scenes never reference the
library file, so they stay self-contained and games need no library
awareness. External library changes reload on a bounded polling cadence,
and library operations never touch scene content, history, or dirty state.

Baked global illumination is a one-panel workflow over the runtime's
deterministic CPU path tracer (ADR 0188). Nodes opt into bakes with the new
batch **Static (baked)** checkbox; the Bake panel authors texels-per-unit,
samples, bounces, and sky color (persisted as root `bake.*` metadata, which
VSCN now serializes at the document level), refuses shared meshes with the
offending node list, and offers **Make Meshes Unique for Baking** as one
undoable deep-copy transaction. The bake itself runs chunked inside the
editor pump with live progress; edits during an active bake are refused with
a truthful message, Cancel restores the exact prior document, and completion
commits chart UVs, per-node atlas materials, and settings as one canonical
transaction whose lightmaps survive save/reload with no rebake. The Light
probe grid group authors the `probes.*` node convention and **Bake Probes**
writes the `.vlpg` sidecar beside the scene without dirtying the document.
The Environment group writes the root `env.skybox`/`env.iblEnabled`/
`env.iblIntensity` convention in one transaction; the editor viewport (and
the camera preview inset) applies it live as workspace-only render state,
and games consume the same metadata through
`Canvas3D.SetSkybox`/`IblEnabled`/`IblIntensity`.

Prefab instancing places scenes by reference (VSCN v7, ADR 0187). **Import
Instance** creates a node whose file stores only the reference (a portable
relative path) plus the node's overrides — transform, name, visibility, and
typed metadata; the canonical loader grafts the referenced scene's content on
every load path (SceneGraph.Load, SceneAsset, async handles, streaming) with
cycle, depth (8), and fan-out (4096) guards that resolve to
reference-retaining placeholders instead of failures. Grafted content is
locked: hierarchy rows carry prefab/instance badges, selection resolves to
the owning instance, and authored children inside instances are refused with
a truthful message. The Prefab instance inspector offers Open Source (opens
the referenced file as its own document), Reload (re-grafts every instance,
preserving overrides), Unpack (one undoable transaction converting the
instance to plain editable nodes), and Re-link. Scenes without prefab nodes
keep serializing at v6 or lower, and legacy files load unchanged.
`SceneNode.PrefabPath`/`IsInstanceContent` expose instance identity to games
without JSON parsing.

The Light component group accepts multi-node selections with the material
inspector's exact semantics (ADR 0186): every retained light field presents a
native mixed state when selected lights disagree (kind itself may be mixed),
Apply resolves only determined fields while mixed fields preserve each
node's value, and choosing a concrete kind on a mixed selection rebuilds each
light keeping kind-compatible fields and defaulting the rest. Replacement
lights are constructed independently per node before any assignment, selected
sharing groups reuse one staged replacement so they stay shared, and
unselected users of a shared light are untouched. With no light-bearing nodes
selected, Apply creates independent lights on the whole selection; with
partial coverage it patches the light-bearing nodes and says so; Remove
clears every selected light-bearing node. Each accepted add, apply, or
remove is one canonical VSCN transaction with exact rollback and no-op
refusal.

Colliders are authored as the documented typed-metadata convention
(ADR 0185): `collider.kind` (`box`, `sphere`, `capsule`, `mesh-bounds`),
kind-specific node-local dimensions, and `collider.trigger`. The Collider
inspector writes only kind-applicable keys and removes stale ones as one
undoable transaction with no-op refusal; Remove deletes the complete key set
while keeping the node. Games derive physics at spawn from the metadata plus
node world transforms and mesh bounds — the editor never instantiates
physics. The viewport draws collider wireframes (box edges, sphere
three-circle wire, capsule profile; trigger colliders dash), route polylines
through the ordered direct children of nodes carrying the project `route`
component (at most 256 drawn waypoints), hierarchy badges naming each node's
first recognized project component, and a deterministic per-component marker
tint — all workspace-only, projected through the shared camera, and gated by
the Cameras/Colliders/Routes visibility toggles in the View options row.
Asset-kind schema fields now open the bounded project asset browser filtered
by the field's declared asset kinds in both scene editors; picking stages a
portable reference into the raw metadata/property draft and the canonical
write still flows through the normal one-transaction path (closing the ADR
0178 deferral).

The 3D editor has distinct Move, Rotate, and Scale handles, mode-aware snapping,
pointer capture, Escape cancelation, per-scene tool persistence, group framing,
and W/E/R tool shortcuts that only activate while the viewport/tool owns
keyboard focus. Its Local/World control also follows the owning scene tab and
bounded session state. Local mode projects the parent-relative basis used by
the local TRS fields; World mode aligns the handles to the absolute axes and
applies the resulting delta around each selected node's own pivot. The runtime
must reproduce each
requested world matrix as exact parent-relative TRS. A singular, degenerate, or
shear-producing conversion restores every captured local transform and creates
no history entry. Accepted command or pointer edits serialize the group once,
and Escape restores the full pre-drag group.
Move and Scale also present XY/XZ/YZ plane squares; Scale crosses each square
so mode does not depend on color alone. Plane picking wins inside the visible
square, overlapping candidates prefer the most face-on projection, and nearly
edge-on planes are neither drawn nor pickable. Pointer motion is solved against
both projected basis vectors together. Move retains scene-unit handle lengths;
Scale maps one complete handle width to one scale unit on each axis. Snapping
uses the immutable primary origin on both axes, additive group deltas are
retained, and Local/World acceptance, rollback, history, and Escape rules match
axis dragging.
The Parent chooser moves selected top-level roots under Scene Root or one valid
existing node. Destinations inside any moved subtree and destinations that
would exceed the scene depth limit are omitted. Keep world transform is enabled
by default and uses the runtime's exact conversion: new local TRS must
recompose to the complete prior world matrix, while singular or
shear-producing requests restore the prior bytes and selection. Clearing the
option preserves local TRS instead. Accepted moves serialize once and restore
the same node selection at its new preorder rows. Earlier/Later moves one
contiguous same-parent selection as a stable sibling block, preserving internal
order, local transforms, and selection in one undoable VSCN transaction.
Mixed-parent, gapped, and boundary requests are no-ops. The retained TreeView
also accepts direct row drops: the top and bottom regions place the complete
selected root block before or after the target, while the middle reparents it
into the target. Every path uses the same cycle/depth checks, exact
preserve-world default, selection remap, canonical rollback, and one-step
history.
The Gameplay metadata group edits one selected node independently of its
display name and render components. Keys are deterministic and bounded to 128
bytes; each node may contain 256 values, and string values are capped at 64
KiB. The editor preserves explicit null, Boolean, integer, float, and string
kinds, including integral-looking floats. Create, rename, update, and remove
validate before mutation, reject duplicates and no-ops, and each commit exactly
one canonical VSCN history snapshot. A metadata-bearing scene is VSCN v6; load,
undo/redo, cross-document subtree operations, and ordinary Save use the same
runtime representation. The selected metadata row follows the owning scene tab
and bounded session state.
The shared Project components group loads compatible typed templates from the
active scene's owning workspace-root `scene-components.json`. Add Missing
preflights every selected object/node and field, preserves existing same-kind
values, rejects the complete batch on any type conflict, then creates one
canonical history entry. Already-complete components are no-ops and failed
writes restore the prior scene. Edit Field copies a template field into the
raw editor; 3D raw editing remains single-node even though component
application is multi-node. Unsaved scenes use a schema only when one workspace
root is open, avoiding an arbitrary choice in multi-root workspaces. The format
and limits are documented in [scene-components.md](scene-components.md).
Edit Schema opens the same complete, unfiltered definition list from either
editor. Component and field create/update/delete/reorder operations preserve
unknown version-1 members, revalidate the complete result, and commit one atomic
project-file transition. Schema Undo/Redo owns 20 exact file snapshots, rejects
external conflicts, and never changes scene content, revision, dirty state, or
scene history. Definition renames and removals deliberately leave existing
scene values untouched; automatic data migration is not implemented.
Its Material component edits base RGB, alpha, metallic, roughness, ambient
occlusion, opaque/mask/blend mode, double-sided, and unlit state. Multiple
selected nodes expose native mixed numeric, color, enum, and Boolean states;
applying resolves only concrete fields and preserves every unresolved field per
node. All clones are staged before assignment and selected sharing groups reuse
one staged clone, so unselected users of an imported shared material remain
unchanged and unexposed maps/custom data are retained. The map
controls assign or clear albedo, normal, metallic/roughness, ambient-occlusion,
and emissive slots from PNG, JPEG, BMP, GIF, or strictly validated KTX2 sources
up to 16 MB. Decoded raster maps are capped at 16,777,216 pixels. The hierarchy
import and texture-map sections can search supported files from the same
bounded multi-root project index and preview supported images before use.
Map replacement also clones shared materials, and the image data is embedded in
VSCN. The selected assigned slot shows a bounded thumbnail from the canonical
live material, so load, history, import, clone, and round trips cannot leave a
stale picker-path preview behind. Scalar apply/remove and map replace/clear
operate across the complete selection, retain it, and each create one history
transaction that round-trips through VSCN.

The Light component authors directional, point, ambient, spot,
rectangle-area, sphere-area, and volume lights with every applicable retained
field, including light-local position/direction, falloff, dimensions, radius,
range, and spot cone. Studio constructs an independent replacement before
assignment so a shared imported light is never edited in place.
Add/apply/remove are exact no-op-aware one-step VSCN transactions. Hierarchy
badges and viewport color, direction, offset, and range markers keep meshless
emitters visible and pickable. Multi-node selections edit with truthful
native mixed-value fields (ADR 0186): mixed fields preserve each node's
value, concrete kinds rebuild each light keeping compatible fields, selected
sharing groups reuse one staged replacement, and unselected sharers stay
untouched.

The 2D tile palette's behavior inspector authors the typed scene sections of
ADR 0176 for the selected palette tile: a collision dropdown (none, solid,
one-way-up), bounded typed int/bool tile properties with the same draft/Set/
Remove vocabulary as scene properties, comma-separated per-frame animation
frames and positive millisecond durations, and exactly-16-variant autotile
rules. The editor validates every draft before mutation, commits each accepted
edit as one canonical history transaction with exact rollback, treats exact
no-ops as history-free, and re-renders palette badges for tiles carrying any
authored behavior. `BuildTilemap()` applies the authored sections at runtime,
so games no longer need code-side collision/animation registries for authored
scenes.

The scene editors are intentionally v1: a full tagged asset
library/import-settings workflow,
automatic component-schema/scene-data migration, generalized runtime component
composition, advanced Tiled atlas
metadata/image-collection editing, batch light editing, and
cubemap/lightmap authoring still need depth.

## Workbench Status

### Explorer

The explorer supports:

- Open folder.
- Add folder to workspace.
- Demand-loaded folder expansion with bounded directory pages, incremental
  natural sorting, and bounded tree-row publication.
- Open selected files.
- Create file/folder.
- Rename.
- Delete/move-to-trash flow.
- Duplicate.
- Copy path and relative path.
- Search in folder.
- Run file.
- Set project entry.
- Refresh.
- Project-specific and runtime workspace ignore rules.

Known limits:

- Tree operations use integrated overlays for create/rename/delete/duplicate;
  confirmation-heavy workflows still use dialogs.
- Ignore behavior is whatever `Zanna.Workspace.FileIndex` supports; do not
  assume full Git ignore semantics beyond what the runtime implements.
- Very large workspaces depend on cooperative tree/cache/index pumping; slow
  network filesystems can still delay an individual native directory operation.
- Quick Open and completion file discovery use runtime `FileIndex.Page` instead
  of treating the visible tree as a complete workspace snapshot.

### Bottom Panels

Current bottom panel tabs:

- Problems.
- Output.
- Search.
- References.
- Debug Console.
- Variables.
- Call Stack.
- Debug.
- Terminal.

Output rows are bounded by an OutputPane ring buffer plus a bounded row model.
Problems, Search, References, Debug Console, Variables, and Call Stack share a
bounded stable-row model. This prevents runaway UI memory in normal cases and
gives panel rows stable identities, but it is not the same as a fully
virtualized dockable workbench surface for very large logs/search results.

Problems retains structured severity, source, code, action, and location data
independently of its realized rows. Its toolbar filters by text and severity,
reports visible/total counts, and enables Quick Fix only for an actionable
selection. Filtering and side-dock movement do not discard navigation or
action metadata. Diagnostic source edits currently use the Zia action
controller; unsupported language/action combinations report that limit without
editing.

Output exposes its text filter, wrapping, follow state, complete-text copy, and
clear action directly above the log. The controls wrap in side docks. Clearing
from the toolbar empties the log without collapsing the panel, and generated
reports replace stale captured-output copy state.

References retains group, row text, color, and location identity independently
of realized ListBox items. Its live filter includes every child when a group
heading matches, or only matching children with their heading otherwise.
Incremental Zia and BASIC workspace results honor an active filter without
repainting the whole result set. Copy All uses the complete unfiltered grouped
model, while Clear leaves an explicit empty References surface open.

Call Stack reports idle, paused, running, pausing, stopping, restarting, and
terminated session states instead of presenting stale frames or a generic empty
list. Its live filter rebuilds visible rows from durable frames while retaining
each adapter frame index, so selecting a filtered row still opens the correct
frame. Copy Stack copies the complete unfiltered stack.

Debug Console keeps program output separate from debugger status text. Its
responsive toolbar provides live filtering, wrapping, complete-text copy, and
Clear. Clear removes session-owned stdout/stderr so output does not reappear on
the next debugger event, but retains the current running, stopped, or terminated
status. Filtering and wrapping affect presentation only.

### Debugger

The debugger uses the external VM debug adapter:

```text
zanna run --debug-adapter <file>
```

Supported behavior:

- Launch active file.
- Open a persisted Run and Debug activity-sidebar view even without a project.
- Present Idle, Running, Paused, Pausing, Stopping, Restarting, and Terminated
  state with only valid primary, restart, stop, and step controls enabled.
- Open Variables, Call Stack, or Debug Console directly from the activity view.
- Set and persist breakpoints.
- Conditional breakpoints and logpoints.
- Filter breakpoints by file, path, line, condition, or log text while retaining
  exact store identity; Open, Remove, Condition, and Logpoint act on the selected
  source record. Concrete rows are capped at 1,000 while counts remain complete.
- Continue, pause, step over, step in, step out.
- Run to cursor.
- Stop and restart.
- Restart waits for the previous adapter process to terminate before launching
  a replacement.
- Current-line gutter marker.
- Locals and call stack at stop points.
- State-aware Call Stack filtering and complete-stack copy. Filtered rows retain
  their original adapter frame index for correct source navigation.
- Expression evaluation while stopped.
- Persistent watch expressions, shown in the Variables panel above locals.
- Inline watch entry plus Add, Remove selected, Refresh, and Clear controls in
  the docked Variables panel; Enter submits without leaving the workbench.
- Command-palette Add Watch routes to and focuses that same inline field.
- Variables panel rows are grouped through a `VirtualTree` model for Watches and
  Locals before being rendered into the current ListBox UI.
- Composite locals (lists, seqs, maps) are expandable: clicking a `▸` row loads
  its children asynchronously through the adapter's `variables` request, shows
  an immediate loading row, and publishes the reply on a later frame; nested
  containers expand one level at a time. Timed-out rows remain retryable.
  Expansion state is kept by variable name-path, so stepping re-opens the same
  nodes automatically.
- Class instances expand field-by-field with `{field=value}` previews on the
  locals row. Field layouts come from the module's own compile (the ADR 0138
  class-layout sidecar), so display types are the semantic Zia types; objects
  nest with collections in both directions.
- Debug console output with live filtering, wrapping, complete-text copy, and
  program-output Clear that retains debugger status.

Known debugger UX gaps:

- Boxed struct-typed fields display as typed leaves (struct payload expansion
  is a recorded follow-up); direct-IL and BASIC debug sessions have no layout
  sidecar and keep `<TypeName>` leaves for objects.
- Breakpoint condition/logpoint editing still uses a focused single-field
  overlay rather than a richer breakpoint-details inspector.

### Terminal

The integrated terminal starts a platform shell in a PTY when the Terminal panel
is shown. It supports prompt output, raw typed input, line editing delegated to
the shell, resize, Stop, Restart, and workspace-root working directory selection
for new sessions.

Emulation coverage (pinned to the vim/less/htop sequence table, exercised by
`test_vg_outputpane_term.c` and `terminal_altscreen_probe.zia`):

- Cursor addressing, save/restore, line/display erase, insert/delete lines and
  characters, tab stops (HT/HTS/TBC).
- Alternate screen (47/1047/1049) with primary scrollback preserved.
- DECSTBM scroll regions with region-aware LF/IND/RI and SU/SD, so status
  rows stay pinned during full-screen redraws.
- DEC private modes 25 (cursor visibility), 2004 (bracketed paste), 1
  (application cursor keys); DSR and DA replies on the input stream.
- SGR 16/256/truecolor plus bold and reverse video.
- Clipboard chords: Cmd+V / Ctrl+Shift+V paste (bracketed when armed),
  Cmd+C / Ctrl+Shift+C copy a selection; plain Ctrl+C/Ctrl+V still reach the
  child process.

Current limitations:

- VT features outside the pinned table (e.g. underline rendering, Sixel,
  mouse reporting, OSC beyond swallowing) are not claimed.
- Terminal dimensions come from OutputPane cell metrics via `ColumnsForWidth()`
  and `RowsForHeight()`.
- Hidden panels do not auto-start shells, but already-running sessions are pumped
  into a bounded replay buffer so PTY output does not back up while another panel
  is selected.

### Source Control

The Source Control view is a Git integration, not a general SCM abstraction.
It supports:

- Detecting whether the project root is a Git repository.
- Current branch display.
- Status entries.
- Stage one file.
- Unstage one file.
- Stage all.
- Commit staged changes with a message.
- Diff selected path (unified in the panel, or side-by-side via the diff view).
- Responsive wrapped action rows whose enabled states follow the repository,
  selected path, staged index, authored message, active job, and conflict state.
- Focused Enter submission for commit messages and Git credential responses.
- Basic conflict recovery: real porcelain-v2 unmerged rows are highlighted,
  explain "edit conflict markers, then Stage", and keep Commit disabled until
  every conflict is staged as resolved.
- Editor gutter change bars are produced by cancellable, frame-pumped Git jobs.
  Tab switches coalesce to the newest path, secondary workspace folders use
  their owning repository, configured external diff/textconv commands are
  disabled for this passive decoration, and a five-second/4,096-marker safety
  budget prevents a slow child or pathological hunk from freezing the editor.
- Commit history: lazily paged log, per-commit file lists, and side-by-side
  parent-vs-commit diffs for any file in a commit.
- Push and pull on a PTY with live output streaming into the panel; detected
  Username/Password/passphrase/host-key prompts surface an in-app credential
  row (masked input for secrets) — no external askpass helper.
- Queued operations: actions requested while a job runs wait in a bounded,
  visible queue and run in order; Cancel clears the active job and the queue.
- Switch branch basics.

Known limits:

- Operations remain serialized by design (git mutates shared repository
  state); the queue makes waiting visible rather than adding parallelism.
- Credential prompt detection is a heuristic over PTY output and fails open:
  unrecognized prompts simply stream into the panel.
- Status parsing uses porcelain v2 and handles common spaces, renames, and real
  unmerged rows, but exotic path bytes and multi-file/rename conflict recovery
  still need more coverage.
- No merge, rebase, stash, ours/theirs selection, or merge-abort workflow.

## Data Safety

Implemented protections:

- Modified tabs get close prompts through the document close flow.
- Save All skips untitled and read-only preview buffers.
- Existing-file saves use `Zanna.Workspace.Edit.ApplyInRoot`.
- Save As uses same-directory temporary writes for new files.
- File watchers detect external changes, deletions, and missing/moved files;
  saves after a missing-file conflict require confirmation before recreating the
  original path.
- Session restore persists unsaved small text buffers as bounded base64 recovery
  data. Session/settings reads reject oversized state before parsing; writers
  cap tabs, roots, breakpoints, and aggregate embedded recovery, retain the
  active tab when truncating, and atomically replace the shared INI file.
- Continuous crash swaps snapshot modified editable buffers after a two-second
  debounce and perform large writes on a coalescing background worker. Atomic
  staged commits are cancellation-safe across save, close, reload, rename, and
  delete transitions, so an old worker cannot resurrect discarded text.
- Explorer path-only rename and drag-move operations retain the live editor
  buffer, including undo, selection, folds, and scroll. Moving an open path to
  project trash closes its documents and releases either split-pane owner before
  the surviving tab is activated.
- Build/debug preflight can save all modified files before launching.

Known data-safety gaps:

- Recovery applies only to editable text buffers; a hard crash can still lose
  edits made after the most recent two-second debounce snapshot.
- Source Control write operations depend on Git command success and basic
  stderr reporting.

## Product Polish Gaps

These gaps are current documentation, not a plan commitment:

- Extend named vector icons when new workbench actions are added.
- Continue replacing routine confirmation-style workflows with contextual
  workbench surfaces while retaining explicit destructive safeguards.
- Bottom-group panels share one vertical splitter, while independent left and
  right groups and one in-window floating group can remain open simultaneously.
  Each attached boundary is drag-resizable; the floating group moves/resizes and
  redocks through explicit cards or narrow edge strips. Search temporarily
  reserves a useful compact-window result viewport without overwriting the
  bottom preference, side widths stay mirrored, and empty groups collapse
  independently. Group membership/order, floating bounds, and the primary
  draggable group's host persist alongside the independently movable primary
  sidebar. Remaining: native secondary-window/multi-monitor detachment and
  fully virtualized panel content beyond the bounded stable-row model.
- Add boxed struct-payload expansion in the debugger.
- Deepen Source Control with merge/rebase orchestration, ours/theirs review,
  merge-abort, and multi-file conflict recovery.
- Deepen the scene editors' remaining explicit deferrals: prefab per-field
  descendant overrides (VSCN v7 instances override transform, name,
  visibility, and metadata only), multiple simultaneous 3D viewports,
  GPU-accelerated lightmap baking (the CPU path tracer is the only baker),
  visual terrain/water/navmesh authoring (these stay metadata-driven
  conventions games consume), automatic unattended schema migration (the
  assistant remains explicitly confirmed per run), and generalized runtime
  components beyond the typed-metadata conventions.
- The Ashfall recreation's test arena intentionally stays code-built
  (`ashfall-scenes/world/arena.zia`); every campaign mission is scene-driven.
- Split oversized coordinator modules.
- Expand platform and display test coverage.

## Documentation Honesty Rules

Use these phrasing rules when updating user-facing docs:

- Say "Zia and BASIC semantic navigation" only for definition, references,
  rename, call hierarchy, workspace symbols, and signature help.
- Say "BASIC compiler-backed completion, diagnostics, hover, and symbols" when
  discussing the `Zanna.Basic.LanguageService` runtime bridge specifically.
- Say "integrated PTY terminal covering the vim/less/htop sequence table"
  instead of "full terminal emulator".
- Say "Git Source Control view" instead of "SCM platform".
- Say "built-in v1 2D/3D scene editors" while naming their
  asset-library/material-preview/advanced-map, component, and gizmo limits.
- Say "debug adapter supports stepping, breakpoints, locals, call stack,
  evaluate, inline watch management, and structured expansion of collections
  and class-instance fields" while still mentioning the struct-payload leaf
  and direct-IL/BASIC layout-sidecar gaps.

The goal is to make the app feel more trustworthy by making the docs less
optimistic than the code.
