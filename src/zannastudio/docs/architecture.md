# Zanna Studio Architecture

This document describes the current Zanna Studio source layout and ownership rules.
It is a contributor guide, not a roadmap.

For a file-by-file tour, read [source-map.md](source-map.md). For procedural
change guidance, read [maintenance.md](maintenance.md). This document stays at
the architecture and ownership level; the other two documents go deeper into
current modules and common change workflows.

## Runtime Shape

Zanna Studio is a frame-driven GUI application written in Zia. `src/main.zia`
constructs long-lived subsystem objects, creates the shell widgets, restores
settings/session state, then polls controllers and command triggers each frame.
Most GUI controls expose polling methods such as "was clicked", "was changed",
or "take input"; the app uses those instead of callback registration.

The implementation depends on runtime classes supplied by the C runtime and
registered through `src/il/runtime/runtime.def`. Important runtime families are:

- `Zanna.GUI.*` for windows, menus, widgets, `CodeEditor`, `OutputPane`,
  command palette, and test/virtual-list helpers.
- `Zanna.System.Process` for build, run, debug-adapter, and Git child processes.
- `Zanna.System.Pty` for the integrated terminal.
- `Zanna.Workspace.FileIndex` for project tree/search enumeration.
- `Zanna.Workspace.Edit` for transactional text replacement.
- `Zanna.Zia.*` and `Zanna.Basic.*` for language services.
- `Zanna.Game2D.SceneDocument` and `Zanna.Graphics3D.SceneGraph` for the
  built-in visual scene document models.

## Source Layout

```text
zannastudio/src/
    main.zia        Application bootstrap, frame loop, dispatch glue.
    app/            App-level helpers that do not own widgets.
    build/          Build/run jobs, diagnostic model, breakpoints, debugger.
    commands/       Command catalog, registry, and feature handlers.
    core/           Documents, projects, settings, sessions.
    editor/         Code editor adapter, semantic controllers, indexing.
    probes/         Focused IDE probe entry points.
    scm/            Git command wrapper and Source Control view model.
    services/       Leaf helpers for file kinds, locations, text, edits.
    terminal/       PTY session wrapper and integrated-terminal controller.
    tests/          Local GUI/runtime probe sources.
    ui/             Shell widgets, panels, activity bar, overlays.
    zia/            Pure Zia parsing, formatting, bind, and refactor helpers.
```

The larger feature areas are intentionally split into small teaching modules:

- `app/command_palette_controller.zia` owns command-palette mode transitions.
- `app/file_watch_controller.zia` owns active-file watch state and open-tab
  timestamp polling.
- `services/search_matcher.zia` owns pure literal/regex line matching.
- `services/search_paths.zia` owns search include/exclude/ignore path rules.
- `services/workspace_file_index.zia` owns shared FileIndex-backed workspace
  membership and enumeration policy.
- `commands/command_context.zia` names the long-lived dependencies used by the
  command dispatcher.
- `commands/search_commands.zia` owns the docked search panel flow and consumes
  normalized requests for direct, cached, and legacy search paths.
- `commands/quick_open_commands.zia` owns Quick Open palette rows, id encoding,
  and deterministic file scoring.
- `commands/source_transform_commands.zia` owns UI command orchestration for
  format/refactor source transforms.
- `commands/diagnostic_edit_commands.zia` owns warning suppression, diagnostic
  fix-it, and missing-bind source edit flows; its controller keeps compiler and
  project-wide discovery work out of command callbacks.
- `commands/zia_workspace_commands.zia` owns Zia definition, references, call
  hierarchy, and rename publication; project-index queries run on its owned
  latest-wins worker instead of menu or frame callbacks.
- `ui/notification_policy.zia` owns transient-popup eligibility so background
  results with durable panels do not interrupt later work.
- `ui/tool_panel_model.zia` names bottom-panel ids and tab indexes.
- `ui/output_cache.zia` owns pure build-output cache and wrapping policy.
- `editor/completion_items.zia` defines typed completion candidates while
  `editor/completion.zia` owns popup behavior.
- `editor/completion_workspace_source.zia` owns workspace source loading and
  detail formatting for completion's workspace symbol cache.
- `zia/function_scan.zia`, `trivia_scan.zia`, `call_scan.zia`,
  `delimiter_scan.zia`, and `occurrence_scan.zia` hold focused source scanners;
  `zia/source_scan.zia` remains a compatibility facade for older callers.

## Ownership Rules

Prefer this dependency direction:

```text
main -> app -> commands/ui/editor/core/build/services/zia
commands -> core/editor/services/zia/ui
editor -> core/services/zia
ui -> editor/core only when rendering state requires it
services/zia -> leaf helpers with no AppShell ownership
```

Rules:

- `main.zia` wires subsystems and dispatches events. Do not add parsing rules,
  filesystem mutation flows, formatting logic, or command metadata there.
- `commands/command_catalog.zia` owns command ids, labels, shortcuts,
  capability tags, and command-palette visibility.
- Command behavior belongs in the feature command modules:
  `file_commands.zia`, `edit_commands.zia`, `search_commands.zia`,
  `build_commands.zia`, `debug_commands.zia`, and `view_commands.zia`.
- Source-to-source transforms belong in `src/zia/`, not in UI or command code.
- Shared path, location, text, and workspace-edit rules belong in `src/services/`.
- Widget construction and persistent widget references belong in `src/ui/`.
- Process state belongs in `src/build/`, `src/terminal/`, or `src/scm/`
  depending on the feature.

Avoid cycles. When two modules need the same rule, move that rule down into
`services/` or `zia/`.

## Main Loop Responsibilities

`main.zia` currently coordinates:

- App shell creation and settings application.
- Document, tab, project, session, and recent-file state.
- Editor-to-document synchronization.
- File-watch and workspace-watch controllers.
- Command-palette controller, menu, toolbar, context menu, and shortcut dispatch.
- Editor controllers: completion, diagnostics, hover, signature help, symbols,
  inlay hints, semantic tokens, and project indexing.
- Build/run/debug/terminal/SCM panel updates.
- Close, save, project switch, and shutdown preflight.

This file is intentionally still too large. New behavior should usually be
introduced behind a subsystem object and called from the loop, not implemented
inline in the loop.

## Core Data Flow

### Documents

`core/document_manager.zia` owns the open document list and active index.
It deduplicates open files, tracks untitled documents, stores recently closed
paths, detects file kind and language by extension, and routes saves through
`Zanna.Workspace.Edit` when replacing existing files.

`core/document.zia` stores per-buffer state: file path, display name, language,
document kind, content, modified/new/read-only flags, disk metadata, cursor,
scroll, and external-change notification state.

Document kinds:

| Kind | Extensions | Current behavior |
| --- | --- | --- |
| Code | `.zia`, `.bas`, `.vb` | Full text editor; language-specific services. |
| Text | `.txt`, `.md`, `.json`, `.il`, unknown | Plain text editing or preview. |
| 2D scene | `.scene`, `.level` | Built-in hierarchy/viewport/properties authoring surface with per-document history. |
| 3D scene | `.vscn` | Built-in hierarchy/viewport/transform/material authoring surface with per-document history. |
| Binary unsupported | common image extensions | Read-only preview placeholder. |

### Projects

`core/project_manager.zia` owns the file tree, primary project root, additional
workspace roots, incremental tree population, Quick Open cache, and project
exclusion checks. Immediate children arrive through bounded `Zanna.IO.Dir.Page`
calls; natural sorting and tree-row publication are also frame-budgeted. It
delegates ignore matching to `Zanna.Workspace.FileIndex` and uses absolute paths
as tree-node data.

`core/project.zia` parses the small `zanna.project` manifest: project name,
language, entry, run/build overrides, working directory, problem matcher, and
ignore patterns.

### Sessions And Settings

`core/settings.zia` reads and writes platform-native settings paths and keeps
legacy `~/.zannastudio/settings.ini` compatibility. Settings are loaded once on
startup and updated by workbench commands. The shared settings/session file is
size-checked before parsing and replaced through a same-directory atomic staging
write; an oversized or interrupted state file therefore cannot monopolize
startup or destroy the previous complete snapshot.

`core/session.zia` stores the last project, open tabs, active tab, cursor/scroll
state, recent projects, recent files, and bounded base64 crash-recovery text for
modified editable buffers. Startup advances its resumable restore job one
workspace root or tab per painted frame, so large valid sessions preserve
progress feedback and native event polling while their files are loaded. The
writer mirrors the restore caps (256 tabs, 64 roots), bounds aggregate embedded
recovery text, and reserves a retained slot/payload budget for the active tab.

`core/recovery_store.zia` supplies the continuous crash-recovery layer. The UI
thread snapshots immutable buffer strings after the edit debounce; an owned
background request stages and atomically commits the filesystem payloads.
Requests coalesce, and save/close cancellation is serialized only around the
short same-directory rename so an old worker cannot recreate a retired swap.
Explicit document transitions synchronously prune stale names, then queue any
remaining dirty buffers instead of rewriting them on the command frame.
Recovery discovery itself is startup-paged, bounded by record count and total
retained bytes, and interleaved with native event polling and progress paints.

### Language Services

`editor/language_service.zia` routes capability checks by document language:

| Language | Completion | Diagnostics | Hover | Symbols | Definition/Refs/Rename | Signature |
| --- | --- | --- | --- | --- | --- | --- |
| Zia | yes | yes | yes | yes | yes | yes |
| BASIC | yes | yes | yes | yes | yes | yes |
| Text | no | no | no | no | no | no |
| Scene | no | no | no | no | no | no |

Commands with missing capabilities remain visible in the command palette when
that is useful, but are marked unavailable and report a status/toast reason.
BASIC semantic commands are scanner-backed rather than project-index-backed, so
definition, references, call hierarchy, and rename run through one owned
latest-wins background worker. Requests contain immutable roots, open-document
snapshots, and an editor anchor; completions apply only if the path, revision,
caret, and workspace roots still match. File/source/declaration/result ceilings
bound the scan, large reference sets render across multiple frames, and rename
refuses any partial result set.

Zia definition, references, call hierarchy, and rename use a separate owned
latest-wins worker over `Zanna.Zia.ProjectIndex`. The runtime takes a shallow,
immutable source snapshot under a short handle lock, then performs all parsing
after releasing that lock, so typing, file watches, and workspace teardown can
continue concurrently. The UI controller waits for cooperative indexing,
captures path/revision/caret/workspace/index generations, rejects stale or
incomplete results, and paints at most 100 result items per frame. Workspace
indexing refuses partial queries after a 20,000-file, 200 KB per-file, or 64 MB
aggregate-source ceiling; navigation retains at most 2,000 references and
rename refuses a larger edit set.

### Editor Controllers

`editor/editor_engine.zia` adapts `Zanna.GUI.CodeEditor` to IDE document state.
It caches full-text snapshots by editor revision so semantic jobs do not copy
the full buffer every frame.

Controllers under `editor/` own focused features:

- `completion.zia`: completion popup, filtering, commit behavior, workspace
  symbol completion cache.
- `diagnostics.zia`: live diagnostics, Problems rows, minimap/inline highlights.
- `hover.zia`: dwell-triggered hover requests.
- `signature.zia`: signature help and overload navigation.
- `symbols.zia`: document symbol outline.
- `basic_query_job.zia`: serial compiler-backed BASIC editor queries.
- `basic_workspace_query_job.zia`: bounded project-wide BASIC navigation and
  refactor scans over immutable multi-root/open-document snapshots.
- `zia_workspace_query_job.zia`: serial project-index definition/reference/call
  and rename queries over immutable editor anchors.
- `project_bind_query_job.zia`: bounded latest-wins discovery of one unique Zia
  file for Create Missing Bind.
- `project_index.zia`: lazy, multi-root Zia workspace indexing with explicit
  completeness, file-count, per-file, aggregate-source, and frame budgets.
- `semantic_tokens.zia`: semantic highlighting.
- `inlay_hints.zia`: conservative Zia hints.
- `scheduler.zia`: priority model for editor background work.
- `perf_monitor.zia`: optional frame/controller/editor counters.

### Visual Scene Documents

`ui/scene_editor_2d.zia` and `ui/scene_editor_3d.zia` are document controllers,
not alternate persistence systems. They parse the active visual document into
a bounded runtime scene model and immediately serialize every accepted
transaction back into `Document.content`. DocumentManager therefore remains
the sole owner of dirty state, save paths, session tabs, and recovery.

Scene workspace state lives on `core/document.zia` records. Camera/canvas
position, the 2D tile tool and selected tile, the process-local selection set,
inspector layout, 3D transform mode, Local/World space, and snap preference
follow their scene tab;
`core/session.zia` persists only bounded scalar view state, including the
primary selected row. Undo/redo
snapshots remain process-local and are valid only while their recorded
canonical content still matches the document.

`ui/scene_selection.zia` is the stable retained-row selection boundary shared by
both scene controllers. Each retained TreeView hierarchy row stores its current
bounded model index in node data. The helper consumes the byte-exact
`GetSelectedData()` sequence defined by ADR 0163, discards invalid/duplicate
indices, and preserves an explicit primary row. Display labels never determine
which scene objects a batch action mutates. Group transform, hierarchy drop,
duplicate, and delete operations serialize once, roll back atomically on
failure, and create one history entry. ADR 0156 remains the equivalent
ListBox-row identity contract for non-hierarchy consumers.

`ui/scene_hierarchy_search.zia` is the shared pure Find boundary. It normalizes
case-insensitive queries, matches one or two row identities, and computes
deterministic wrapping cursors without owning widgets or scenes. Each scene
controller builds match indices only from its bounded represented rows, keeps
the retained hierarchy intact, expands ancestors, restores selection through
stable row identity, and scrolls the target into view. Standard Find routing
reveals the inspector and calls the runtime's live-descendant
`ScrollView.ScrollTo`; ADR 0165 retains that request through settled layout
when a split pane was previously collapsed. No search path enters canonical
content, scene history, dirty state, or camera mutation.

`SceneEditor2D` also owns point and marquee selection on the canvas. It reads
the runtime's public Shift, Control, and Super key states, captures blank-space
drags, and retains the inclusive start/current authored-cell rectangle only as
transient controller state. Point, union, and toggle results are normalized in
canonical object order with an explicit primary index. Cancelation, selection,
and marquee rendering cannot enter `SceneDocument`, `Document.content`,
revision, history, or dirty state; only a later accepted object move uses the
selected identities in a canonical transaction. The current hit model is the
authored object point cell rather than visual sprite or collider bounds.

The same controller owns active-layer tile gestures. Paint and Erase mutate a
captured pre-stroke model but do not serialize until release; Escape restores
the captured canonical snapshot. Rectangle retains only an inclusive transient
outline until release, then delegates its one rectangular write to
`SceneDocument.FillTiles`. Fill delegates four-connected traversal to the
bounded runtime `SceneDocument.FloodFill` primitive. Pick changes only the
document's workspace tile selection and returns to Paint. Changed gestures
serialize once; preview, sampling, cancelation, and exact no-ops never enter
canonical history.

`ui/scene_layout_2d.zia` is the stateless precision-layout boundary. It defines
the supported align/distribute operations, minimum selection sizes, stable
coordinate ordering, and deterministic rounded distribution targets.
`SceneEditor2D` retains keyboard-focus ownership, model mutation, canonical
no-op detection, rollback, and the single history commit. Arrow nudging is
accepted only while the Select surface owns focus, so hierarchy and inspector
controls retain their native keys.

`ui/scene_clipboard.zia` is the document-independent scene interchange
boundary. It writes an explicit version, scene kind, primary identity, bounded
selection identities, and byte-exact canonical source content into the shared
text clipboard. Controllers reconstruct only the selected 2D objects or
top-level 3D subtrees through typed runtime APIs. The helper never infers scene
data from arbitrary text; wrong-kind and structurally invalid envelopes cannot
reach document mutation.

`ui/scene_asset_browser.zia` is the shared non-modal asset-discovery boundary.
It borrows `ProjectManager`'s cooperative multi-root file cache, incrementally
filters supported extensions and text, retains absolute row identity, realizes
at most 512 rows, and decodes at most one bounded common-image preview. It
returns selection intent only; scene controllers still validate and commit the
chosen asset. The same module derives a portable saved-scene-relative spelling
for external 2D references. Native pickers remain controller-owned for assets
outside the workspace.

Project scene components keep three distinct boundaries.
`ui/scene_component_schema.zia` is the only fail-closed parser and publishes
complete value records. `ui/scene_component_palette.zia` owns both the
target-filtered application picker and the unfiltered structured authoring
widgets, but returns intent only. `ui/scene_component_authoring.zia` mutates raw
version-1 JSON while retaining unknown members and owns a bounded exact
project-file history; `ui/scene_component_authoring_controller.zia` performs
atomic, expected-state writes and external reloads for either scene editor.
Schema-file transitions never enter `Document.content`, scene revision, dirty
state, or scene undo/redo. The scene controllers remain responsible for
preflighting and committing Add Missing through their typed runtime APIs.

`ui/scene_tileset_2d.zia` is the 2D image-preview boundary. It resolves relative
references beside a saved scene, safely decodes PNG/JPEG/BMP/GIF sources under
per-source, per-image, aggregate-scene, and scaled-cache limits, and renders
atlas frames for both the palette and scene canvas. Cached opaque frames use a
fast copy; cached translucent frames composite source-over so upper layers
cannot erase lower-layer artwork. A library revision and selection-aware
palette cache prevent unrelated inspector refreshes from rebuilding hundreds of
thumbnails. A rotating metadata poll stats at most one referenced image per
eligible interval; detected changes rebuild disposable previews without
touching canonical content or history. Missing, malformed, over-budget, and
out-of-range assets are
ordinary preview states with deterministic fallback rendering; they never
rewrite scene JSON.

`ui/scene_tileset_inspector_2d.zia` owns only the palette widgets, inline asset
status, and pointer/button intent. It cannot mutate a `Document` or
`SceneDocument`. `SceneEditor2D` retains layer selection, native image
selection, project-browser intent, asset assignment/clearing, automatic and
explicit external-image reload, and canonical history ownership. The JSON
stores only the authored external path, not decoded image pixels. Assignment
and clearing produce at most one history transaction, while reload affects
preview pixels only. The controller also owns typed multi-object property
set/remove transactions: every selected row is verified before one canonical
commit, and any rejection reloads the complete pre-edit snapshot.

`SceneEditor2D` presents a three-pane workbench: a left Objects pane
(search, full-height tree, parent chooser, and the hierarchy-owned
creation/duplication/removal/ordering rows) beside the canvas/inspector
split, with the tile palette in a vertical strip under the canvas. All three
divider fractions persist per document; hosts narrower than the
hierarchy-pane threshold collapse the Objects pane automatically. The
inspector carries explicit Object | Scene tabs that auto-follow
selection-presence changes while manual choices hold between them; object
fields and component groups live on the Object tab and scene-scoped groups
on the Scene tab through `UpdateInspectorScope`.

The 2D object outliner is a bounded retained `TreeView`, not a flat draw-order
ListBox. `SceneDocument.ObjectParent` provides the structural model while
absolute `x`/`y` positions remain unchanged. Refresh snapshots expanded object
IDs and structural paths, rebuilds actual parent/child rows, reveals ancestors
of the primary selection, and realizes at most 4,096 rows and depth 256 without
discarding hidden scene data. Row-aware before/into/after drops classify
selected top-level roots, move their complete subtrees as one stable draw-order
block, update formal parents, remap selection, serialize once, and reload the
canonical pre-edit snapshot on any rejection. Duplicate and cross-document
paste restore parent links whose endpoints are both selected; external
clipboard parents become roots. **Add Child** establishes its parent before
the first serialization, so history never exposes an intermediate root.
The explicit Parent chooser computes one depth/ownership snapshot, offers at
most the same 4,096-object horizon as the outliner, omits selected subtrees and
depth-invalid destinations, and routes accepted single- or multi-root requests
through the same drop transaction. Reparenting to Scene root appends after the
last remaining root; every path preserves absolute object coordinates.

`ui/scene_transform_3d.zia` is the stateless transform-math boundary. It maps
projected pointer motion into Move, Rotate, or Scale amounts and snaps the final
axis target, performs one conditioned two-axis solve for Move/Scale planes, and
validates persisted Local/World state. Scale-plane projection deliberately maps
one complete handle width to one scale unit on each axis, independent of the
local lengths needed to keep parent-aware handles pixel-stable.
`SceneEditor3D` owns pointer capture, live SceneNode mutation, Escape rollback,
inspector synchronization, and the single serialization commit performed when
a drag finishes. Local mode applies the established parent-relative TRS path.
World mode captures immutable local TRS and world matrices, composes an
absolute-axis delta around each selected node's own pivot, and calls the
runtime's exact `SceneNode.TrySetWorldMatrix` boundary. It derives a
parent-before-child work list from the hierarchy preorder so a selected
descendant cannot inherit the same ancestor delta twice even if restored
selection state arrived child-first. Any rejected member restores the full
local group before return; accepted command and drag paths serialize once.
Transform-only edits do not rebuild the hierarchy, preserving node identity and
selection while the viewport moves.
The 3D viewport is a retained runtime render, not an editor-side mesh
approximation. ADR 0168 gives `SceneEditor3D` a windowless software
`Canvas3D` bound to an explicit `RenderTarget3D`. A matching orthographic
`Camera3D` maps one world unit to the same `viewScale` pixels used by retained
markers and gizmos. Shaded and triangle-wireframe modes both traverse the live
`SceneGraph`; Studio adds grid, hierarchy, selection, and manipulation overlays
to the readback copy. The target is recreated only when viewport dimensions
change, rendering occurs only when dirty, and mode preference belongs to the
document workspace rather than canonical VSCN.
ADR 0169 uses that same configured camera for selection: Studio unprojects the
pointer with `ScreenToRayOrigin`/`ScreenToRay`, asks `SceneGraph.RaycastNodes`
for the closest visible transformed mesh bounds, and only then tests bounded
origin markers for meshless nodes. Gizmo handles retain first priority.
Replace, Shift-add, Control/Command-toggle, and blank-clear selection all remain
workspace-only. Shift plus middle/right drag moves the camera target along its
exact right/up basis, so pan follows the pointer at every orbit angle without
entering canonical VSCN history.
The viewport Image is a real focusable widget: clicking it takes keyboard
focus through the GUI's standard click-to-focus path and paints the shared
theme focus ring. W/E/R, F framing, and the workspace-only Escape
selection-clear are accepted while the viewport surface (or, for keyboard
toolbar workflows, a transform tool button) owns native focus, so scene
shortcuts cannot consume text intended for another workbench surface. Single-node numeric transform edits commit live: spinner changes mutate the
live node immediately (matching the gizmo-preview model), serialize exactly
once when the transform spinners lose keyboard focus, and Escape restores
the captured values without history. Only values differing beyond display
epsilon count as edits, and only differing groups re-apply — re-applying an
untouched rotation would perturb the quaternion through the lossy euler
round trip and fold drift into the next unrelated commit. Programmatic
refresh edges are drained at pump end, where no user input can exist.
Numeric multi-selection inspector edits remain explicitly relative:
position/rotation fields add local deltas and scale fields multiply each
selected local scale before one rollback-safe commit. Duplicate Selection and
Delete use the same hierarchy/viewport focus boundary; command-palette
invocation remains explicit after the palette moves focus.
The node visibility checkbox uses the GUI's native indeterminate state for a
mixed selection. Resolving it writes and verifies every selected live node,
serializes once, and reloads the canonical pre-edit scene on failure.

`ui/scene_hierarchy_3d.zia` is the stateless existing-node reparent validation
boundary. It rejects destinations inside a moved subtree, detects unchanged
common parents, and exposes the containment checks needed by the Parent
chooser. `SceneEditor3D` adds bounded depth validation, collapses descendants
beneath selected roots, moves each root once, resolves the post-move preorder
selection by node identity, and performs one canonical commit. The default
**Keep world transform** path calls the runtime's exact
`SceneNode.TryAddChildPreserveWorld` primitive. Singular or shear-producing
conversions fail before that node changes; if an earlier root in the group
already moved, the controller reloads the canonical pre-edit VSCN and restores
the complete selection. Clearing the option uses `TryAddChild` and deliberately
keeps local TRS instead. The adjacent Earlier/Later controls use the runtime's
allocation-free `SceneNode.TryMoveChild` primitive to move one contiguous
same-parent selection as a stable block, then remap selection and commit once.
Mixed-parent, gapped, and boundary selections are no-ops.

`SceneEditor3D` presents a three-pane workbench: a left Hierarchy pane
(search, full-height tree, parent and sibling-order controls) beside the
viewport/inspector split, with both divider fractions persisted per document.
Hosts narrower than the hierarchy-pane threshold collapse the pane
automatically. The inspector carries explicit Object | Scene tabs that
auto-follow selection-presence changes while manual choices hold between
them: `UpdateInspectorScope` shows node groups on the Object tab only for
selected nodes (single-selection groups for exactly one node) and
scene-scoped groups on the Scene tab, then re-baselines the Visible/Static
checkbox revision trackers so programmatic visibility toggles can never
masquerade as a user resolving those states.

The visible 3D hierarchy is a retained `TreeView`, not a text-indented flat list.
Every scene parent owns its actual child rows; expansion survives live refresh
by node identity and canonical reload by structural path. Runtime
multi-selection returns stable flattened indices from node data. Row-aware
drag/drop classifies before/into/after without mutating the control:
`SceneEditor3D` applies the latched intent to selected top-level roots, combines
exact reparenting with stable destination ordering, serializes once, and
restores canonical bytes plus selection if any stage fails.

`ui/scene_material_3d.zia` is the material value and file-decoding boundary. It
clamps compact-inspector PBR values, normalizes scalar and editable-map slots,
loads common images under a decoded-pixel ceiling or strictly validated KTX2
assets, recognizes full and sparse no-op drafts, and clones an existing
`Material3D` before applying resolved fields or replacing a map. Sparse patches
preserve each unresolved field independently. Selected nodes sharing one source
material reuse one staged edited clone, while unselected users retain the
original. That copy-on-edit rule
prevents one imported node from mutating siblings that share the same material
identity while preserving maps, shading extensions, and custom parameters the
inspector does not expose. Its bounded thumbnail helper reads the selected
slot's canonical decoded `Material3D` pixels through ADR 0157, so previews
follow load, history, paste, import, and VSCN round trips without retaining an
editor-only source path.

`ui/scene_material_inspector_3d.zia` owns only the material widgets,
common/mixed-state calculation, sparse-patch intent, summaries, enablement
rules, and one disposable 128-pixel map thumbnail; it never mutates a
`SceneNode` or `Document`. Spinner mixed state comes from ADR 0167; checkbox
indeterminate state and a Dropdown placeholder cover Boolean and enum fields.
`SceneEditor3D` retains selection, the 16 MB source-size boundary, native and
project-browser selection, node assignment, and canonical history ownership.
Sparse scalar apply, material removal, and map replace/clear operate across the
complete selection, preallocate before mutation, preserve selection, and each
produce at most one history transaction. Material-only refreshes avoid
rebuilding hierarchy identity.

`ui/scene_light_3d.zia` is the document-independent light-state boundary from
ADR 0172. It normalizes all fields shared by the seven runtime light types,
reconstructs complete independent `Light3D` objects, converts live components
back into inspector state, and compares normalized observable values for exact
no-op detection. It has no widget, `Document`, or `SceneGraph` ownership.

`ui/scene_light_inspector_3d.zia` owns only the light widgets and their
mixed-value presentation,
type-specific visibility and enablement, accessible labels, and normalized
draft intent. It explicitly disables mutation for zero or multiple selected
nodes rather than inventing an incomplete mixed-state patch.
`SceneEditor3D` owns the typed `SceneNode.Light` assignment, canonical VSCN
serialization, rollback, selection, and history. It stages a replacement
before touching the live node so imported shared lights cannot leak mutations
to another instance. Add, apply/type conversion, and remove each produce at
most one history transaction. Hierarchy badges and light-only viewport
position, direction, offset, color, enabled-state, and finite-range overlays
are presentation derived from the live component and never enter VSCN.

### Build, Run, And Debug

`build/build_system.zia` starts argument-vector jobs through
`Zanna.System.Process`, streams stdout/stderr, bounds retained output, parses
structured JSON diagnostics when available, and falls back to legacy diagnostic
line parsing.

`build/run_config.zia` builds the command argv from the active document or
project manifest. It never invokes a shell.

`build/debug_session.zia` launches `zanna run --debug-adapter <file>` as an
external process, sends newline JSON commands on stdin, consumes sentinel-tagged
newline JSON debug events on stderr, and owns program stdout/stderr separately
from debugger control status. Program output can therefore be cleared without
discarding the truthful running, stopped, or terminated state.
Restart is an asynchronous state transition: the active adapter is asked to
terminate, the frame pump waits until the process has actually exited, and only
then is the replacement adapter launched with the current breakpoint set.

`ui/run_debug_view.zia` deliberately has no build or debugger dependency. It
owns the scrollable activity-sidebar widgets, copied breakpoint presentation
records, filtering, and responsive control state. `commands/debug_commands.zia`
publishes primitive session/breakpoint snapshots into that view and consumes
integer action intent to invoke the existing debugger commands. Breakpoint rows
carry their original `BreakpointStore` index and source path/line; filtering
never makes visible row position authoritative. The store also exposes exact
removal, so a stale action cannot toggle a missing breakpoint back on.

### Terminal

`terminal/terminal_session.zia` wraps `Zanna.System.Pty.PtySession`.
`terminal/terminal_controller.zia` owns the UI side: lazy start, shell
resolution, terminal mode on `OutputPane`, raw key forwarding, output append,
cell-metric-based resize, bounded hidden-session draining, Stop, Restart, and
shutdown cleanup.

The terminal is intended for interactive shells and simple commands. OutputPane
terminal mode handles common cursor addressing and clear-screen redraws, but
full-screen TUI programs requiring complete alternate-screen and terminal-mode
semantics are out of scope.

### Source Control

`scm/scm_git.zia` is an async `Zanna.System.Process` wrapper around Git argv
sequences. It resolves `git`, captures stdout/stderr/exit code, parses
porcelain v2 status, and keeps blocking compatibility wrappers for probes and
older call sites. `scm_view.zia` pumps one active Git job at a time and
maintains the Source Control UI state. Responsive wrapped controls derive their
enabled states from the latest structured selection/index snapshot; porcelain
v2 unmerged rows block commits and explain the edit-then-Stage recovery path.
Active Git jobs expose cancellation, and stdout/stderr capture uses the process
read-result API so excessive output is reported as truncated rather than
trapping the workbench.

`scm/scm_gutter_controller.zia` separately owns one coalescing read-only diff
job for editor change bars. The main loop drains it every frame, rejects output
when the active document or owning multi-root repository changed, kills a job
after five seconds, and caps marker publication. Gutter diffs explicitly disable
external diff and text-conversion drivers because passive editor decoration must
never execute arbitrary configured helpers or wait for interactive input.

The Source Control view is intentionally lightweight. Push and pull are
long-running operations with streamed progress and heuristic in-app credential
prompts. Basic content conflicts have a safe guided path, but merge/rebase
orchestration, ours/theirs review, and advanced recovery are absent. Treat it as
useful local Git integration, not a complete Git client.

## UI Ownership

`ui/app_shell.zia` creates persistent widgets for the menu bar, toolbar,
activity bar, editor area, bottom tool panels, preferences, overlays, status bar,
debug panels, terminal, and Source Control view. Other subsystems receive
references to widgets owned by `AppShell`.

`WorkbenchShell` creates stable Explorer, Source Control, and Run and Debug
activity hosts. `ActivityBar` selects one by persisted string id. `main.zia`
owns the `RunDebugView` controller and pumps it through the debug-command
snapshot/action boundary; the view can therefore remain open for a standalone
saved source file without pretending that Explorer or Source Control has a
workspace.

`ui/primary_sidebar_dock.zia` owns the direct left/right placement boundary for
those activity hosts. `WorkbenchShell` provides nested stable splitters and
reparents only the live sidebar view tree plus its activity rail; the editor
and tool-panel trees never move as part of sidebar docking. The controller mirrors one logical width
between left and right splitter coordinates, retains collapsed state, restores
safe sidebar focus, and accepts only its typed payload on stationary edge
targets. User moves publish one-shot settings edges; startup/reset placement
does not. `ui/workspace_dock.zia` supplies the shared portable location IDs,
bounded target geometry, and distinct sidebar/tool-panel payload types.

`ui/tool_panel_groups.zia` is the complete, GUI-independent membership/order
model for Problems, Output, Search, References, Variables, Call Stack, Debug
Console, and Terminal. `ToolPanelShell` keeps one stable content widget per tool,
recreates only Tab presentations when a widget crosses groups, and mounts
simultaneous group roots in WorkbenchShell's stable left/bottom/right hosts plus
one `GUI.FloatingPanel` overlay. Whole-primary-group docking or floating merges
occupied destinations without duplicating widgets. The floating overlay has
bounded move/resize geometry, explicit target-card and narrow-edge docking, and
viewport/UI-scale recovery. Per-group selection and collapse are independent;
canonical membership, global stable-ID order, primary host, attached split
sizes, and floating bounds are replayed from Settings. Focus restoration targets
the exact terminal/search/filter control that owned keyboard input before
reparenting.

`ui/notification_policy.zia` is the shared eligibility boundary introduced for
background-capable command controllers. Build and Search use it so immediate
failures caused by the current command are eligible, while external state is
eligible only when it affects the active resource. Their background Build,
Search, and Replace completion stays in the status bar and its durable owning
panel, regardless of which tool group is currently active.

`AppShell` also owns one transient-surface token. Settings, About, explorer
actions, breakpoint editing, command input, and the side-by-side diff claim that
token before taking focus. A claim hides the previous surface and popup menus;
the superseded controller observes the lost token on its next pump and clears
its logical state. Releases are owner-checked so a late close from an older
surface cannot dismiss the newer one.

`WorkbenchShell` separates the saved minimap preference from effective chrome
visibility. `AppShell.Render` reconciles that state after layout: editor lanes
below 620 effective UI units reclaim the minimap's width, while a later resize
restores it automatically when the user preference remains enabled.

`ui/command_input.zia` owns reusable non-modal single-value command input for
Go To Line, output filtering, Workspace Symbols, Rename Symbol, source-extract,
and New Project names. Command modules still perform validation and effects.

`ui/workspace_edit_preview.zia` owns bounded non-modal review for multi-file
rename results. The language controllers supply immutable edit/root payloads;
`commands/workspace_edit_preview_commands.zia` revalidates and applies them only
after an explicit Apply action, then restores editor focus.

Current tool panels include Problems, Output, Search, References, Debug Console,
Variables, Call Stack, Debug, and Terminal. Problems, Search, References, Debug
Console, Variables, and Call Stack share a bounded stable-row model; Output has
its own bounded row/raw-pane model. Problems additionally retains durable
diagnostic records outside its realized ListBox rows so text/severity filtering
cannot discard location or quick-fix metadata. Problems and Output own wrapped,
responsive in-panel toolbars with selection/content-aware enabled states.
References retains grouped rows and location IDs outside its realized ListBox
items; its responsive toolbar can filter, copy, and clear while incremental
workspace-result publication continues against the durable model.
Variables similarly wraps an inline watch expression field and
Add/Remove/Refresh/Clear actions around its structured rows. Call Stack retains
adapter frame indices independently of filtered ListBox rows and presents live
session state when no frames should be visible. Debug Console retains separate
program-output and debugger-status sources; filtering and wrapping rebuild only
presentation rows, while Clear mutates the session-owned program output. Both
own wrapped responsive toolbars with complete-source copy. The tools can form
simultaneous left/bottom/right groups plus one in-window floating group, with
persisted membership/order/bounds and independent collapse, but the concrete
content widgets remain ListBox/OutputPane surfaces rather than fully virtualized
views.

Search also owns a cooperative replacement state machine. The Search button
callback records an exact location-id generation; Replace All copies only that
generation into file/line plans and returns without filesystem work. Later
frames process at most two plans, with bounded/stable reads before atomic disk
writes. This keeps the event loop paintable and prevents old global location
records from broadening a replacement transaction.

The shared side-by-side diff overlay follows the same ownership rule. `Open`
shows a pending surface and queues immutable texts to `services/diff_job.zia`;
the worker applies byte, line, Myers-depth, trace-cell, and retained-row limits.
`DiffView.Pump` publishes at most 100 formatted rows per frame, and application
teardown drains the owned future before destroying GUI/runtime state.

Diagnostic edit commands also use staged ownership. Their click callback
captures the active path/revision/caret and queues a semantic job. Suppression,
fix-it, and runtime-bind edits apply only after that anchor is revalidated.
Project binds add a bounded `project_bind_query_job.zia` scan across immutable
root and open-buffer snapshots; the scan must finish completely and find exactly
one definition. Disk mtimes and open-buffer contents are checked again before
the UI thread inserts the bind.

## File Size Budget

Use these as review triggers:

- 300 lines: check whether helpers should move out.
- 500 lines: require a clear reason the module is cohesive.
- 1000 lines: split before adding more behavior unless the file is generated or
  an intentionally exhaustive fixture.

Several current files exceed the budget. When touching them, prefer extracting
cohesive behavior rather than adding more inline logic.

## Comment Style

New Zia modules should start with a short module header explaining purpose,
ownership, and where the module fits. Public or nontrivial functions should use
`///` comments with `@brief`, parameters, return values where useful, and
`@details` for policy or safety assumptions.

Comments should explain intent and boundaries. Avoid restating the next line of
code.

## Adding A Feature

1. Add command metadata to `commands/command_catalog.zia` if the feature is
   user-triggered.
2. Put behavior in the appropriate command or subsystem module.
3. Keep shared parsing/path/edit logic in `services/` or `zia/`.
4. Route UI through `AppShell` or a focused controller instead of constructing
   widgets ad hoc in `main.zia`.
5. Add or extend a probe in `src/probes/`.
6. Register the probe in `src/tests/CMakeLists.txt` when it should be part of
   CTest.
7. Update the docs in this directory when the visible behavior or subsystem
   ownership changes.

For detailed recipes covering commands, language features, document kinds,
panels, runtime APIs, settings, session recovery, debugger, terminal, and Source
Control changes, use [maintenance.md](maintenance.md).
