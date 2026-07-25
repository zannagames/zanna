# Zanna Studio Testing And Verification

This document lists the current automated probes, useful CTest invocations, and
manual checks for Zanna Studio.

## Build First

For repository changes, use the repository build scripts rather than raw CMake
full-build commands:

```sh
./scripts/build_zanna_mac.sh
./scripts/build_zanna_linux.sh
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1
```

For IDE-only native binary checks:

```sh
./scripts/build_ide.sh
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_ide_win.ps1
```

Targeted `ctest` commands assume the `build/` tree exists.

## Main CTest Entries

Current Zanna Studio-related CTest entries are registered in
`src/tests/CMakeLists.txt`.

| Test | Purpose | Labels |
| --- | --- | --- |
| `zia_smoke_zannastudio` | Compile/run smoke for `src/probes/smoke_probe.zia`. | `zia;zannastudio;smoke;requires_display` |
| `zia_smoke_zannastudio_project_compile` | Build the `zannastudio/` project to IL. | `zia;zannastudio` |
| `zannastudio_notification_policy` | Source audit forbidding routine success/info popups and direct Build/Search warning/error toasts. | `zannastudio;shell;lint` |
| `zia_zannastudio_phase0_phase1` | Core document/project/command/location/session/language-service regression coverage, including text-versus-scene standard Edit/Find command gating and validated primary-sidebar placement persistence. | `zia;zannastudio;phase0;phase1` |
| `zia_zannastudio_phase2_phase3` | Build/run jobs, project entry execution, breakpoints, debug boundary, scene data contracts. | `zia;zannastudio;phase2;phase3` |
| `zia_zannastudio_editor_hot_path` | Editor revision/performance hot-path guard. | `zia;zannastudio;editor;requires_display;perf` |
| `zia_zannastudio_intellisense` | Completion, diagnostics, hover, signature, and IntelliSense UI behavior. | `zia;zannastudio;intellisense;requires_display` |
| `zia_zannastudio_file_tree` | Explorer interactions and file-tree workflows. | `zia;zannastudio;file-tree;requires_display` |
| `zia_zannastudio_activity_bar` | Activity bar, workbench visibility, and coherent left-sidebar layout reset behavior. | `zia;zannastudio;activity-bar;requires_display` |
| `zia_zannastudio_bottom_panel` | Real pointer docking for the primary sidebar and primary tool group, simultaneous left/bottom/right/floating tool groups, floating move/resize/edge-redock and bounds persistence, direct per-tool movement, focus/collapse safety, mirrored size persistence, movable tabs, compact target containment, and reset hygiene. | `zia;zannastudio;shell;requires_display` |
| `zia_zannastudio_multi_root` | Multi-root workspace behavior. | `zia;zannastudio;multi-root;requires_display` |
| `zia_zannastudio_scm` | Git Source Control command layer, async job pump, and spaces/rename/real-unmerged-row parsing. | `zia;zannastudio;scm` |
| `zia_zannastudio_scm_history` | Paged history, per-commit files/diffs, and credential-prompt classification. | `zia;zannastudio;scm` |
| `zia_zannastudio_scm_view` | Responsive live action state, real pointer staging, focused-Enter commits, and a real conflict edit/Stage/commit workflow. | `zia;zannastudio;scm;shell;requires_display` |
| `zia_zannastudio_tool_panel_toolbar` | Live Problems/Output/References controls, structured and grouped filtering, durable navigation data, quick-fix request routing, real pointer actions, and zoomed side-dock containment. | `zia;zannastudio;shell;console;diagnostics;requires_display` |
| `zia_zannastudio_debug_tool_surfaces` | State-aware Call Stack and Debug Console controls, durable filtered-frame navigation, clearable program output, real pointer actions, and zoomed side-dock containment. | `zia;zannastudio;debug;shell;console;requires_display` |
| `zia_zannastudio_run_debug_view` | Persisted Run/Debug activity routing, session-state controls, durable filtered breakpoint actions/persistence, real pointer input, and high-zoom scroll reachability. | `zia;zannastudio;activity-bar;debug;shell;requires_display` |
| `zia_zannastudio_scene_editor_2d` | Responsive 2D hierarchy/canvas/inspector behavior, real expandable TreeView/collapse retention/stable multi-selection, case-insensitive ID/type hierarchy Find with wrapping/ancestor expansion/hidden-inspector reveal and exact no-history mutation, one-step child creation, bounded explicit single/multi-root parent selection, transactional subtree-aware BEFORE/INTO/AFTER drops with cycle/no-op rejection and exact undo/redo, hierarchy-preserving duplicate/cross-scene paste, focus-safe pixel/tile nudging, deterministic primary-axis alignment/distribution, scene-wide typed metadata with tab-local selection, atomic project-component application/conflicts/history, structured schema file editing with exact file undo/redo and scene isolation, typed clipboard validation, exact one-history group move/delete/cut, relative real-PNG tileset decode/source-over render, project-index asset search/preview, quiet external refresh, palette selection, transactional asset/paint/object edits, typed properties, import, canonical round trips, invalid-candidate rejection, history, and save safety. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_canvas_selection` | Public 2D point and marquee selection policy, reverse inclusive cell queries, real captured blank-space drag/release, visible overlay, Escape cancelation, and exact canonical/history isolation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_tile_tools` | Inclusive forward/reverse Rectangle transactions, four-connected Fill counts and undo, Pick isolation, captured non-destructive preview/release, Escape cancelation, and freehand rollback. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_editor_3d` | Responsive VSCN hierarchy/viewport behavior, real expandable TreeView structure/collapse persistence/stable multi-selection, case-insensitive node-name hierarchy Find with wrapping/ancestor expansion/hidden-inspector reveal and exact no-history/camera mutation, transactional BEFORE/INTO/AFTER row drops with preserve-world round trips/cycle rejection/exact undo, typed node gameplay-metadata validation/VSCN v6/history/tab isolation, atomic multi-node project-component application/conflicts/history, complete cross-target structured schema maintenance with scene isolation, cycle/no-op-safe exact preserve-world chooser reparenting, singular/shear rollback, explicit preserve-local mode, selection remapping, stable contiguous sibling-block ordering with boundary/gap/mixed-parent no-ops and exact VSCN undo/redo, native mixed-state batch visibility with exact one-step history, exact one-history group transform/subtree duplicate/delete/cut/cross-document paste, wrong-kind clipboard rejection, focus-safe W/E/R routing (including material/map controls), parent-aware Move/Rotate/Scale math and snapping, conditioned XY/XZ/YZ plane picking, real snapped two-axis pointer input, Local/World plane group history and ancestor ordering, clone-safe bounded PBR scalar/image-map editing, embedded-map VSCN round trips, node identity, import, history, and save safety. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_light_authoring` | All seven `Light3D` types, public spot-cone readback, independent replacement, exact no-op suppression, one-step add/apply/remove history, VSCN round trips, hierarchy/viewport markers, and single-selection gating. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_rotation_ring` | Projected ellipse conditioning, wrap-safe angular math, real ring hover/down/move/up, Local/World group history, exact undo, and edge-on rejection. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_scale_plane` | Mode-aware conditioned plane math, real snapped XY Scale pointer input without viewport reflow, Local and exact World group scaling, untouched zero-scale preservation, hierarchy ordering, byte-exact undo, and atomic shear rejection. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_material_batch` | Runtime Spinner mixed-state use, truthful common/mixed PBR presentation, sparse per-field group patches, missing-component defaults, staged clone sharing, complete-selection retention, batch removal/map assignment/map clearing, no-op history, and byte-exact undo/redo. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_shaded_viewport` | Default per-scene shaded mode, windowless Canvas3D/RenderTarget retention, exact orthographic camera-to-overlay projection, authored mesh submission and pixel output, real-pointer triangle-wireframe switching, accessible mode state, zero VSCN/history mutation, idle full-resolution target invariant, and the ADR 0191 acceleration request rebuilding a working (software-fallback) renderer without content changes. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_viewport_picking` | Off-origin closest-depth visible mesh-bounds picking, shaded/wireframe parity, meshless marker fallback, replace/add/toggle/blank selection policy, public Control/Command key surface, exact camera-plane pan, workspace persistence, and zero VSCN/history mutation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_workspace_state` | Bounded per-scene canvas/camera/inspector/typed-property-selection/transform-tool/shaded-wireframe session persistence and dirty untitled-scene recovery. | `zia;zannastudio;scene;persistence` |
| `zia_zannastudio_scene_component_schema` | Fail-closed project component parsing, exact explicit/implicit typed defaults, target filtering, duplicate rejection, and empty-schema behavior. | `zia;zannastudio;scene` |
| `zia_zannastudio_scene_component_authoring` | Pure structured component/field edits, stable ordering, typed defaults, unknown-member preservation, atomic create/replace, external-conflict rejection, and bounded exact file undo/redo. | `zia;zannastudio;scene` |
| `zia_zannastudio_scene_history` | Shared ADR 0190 undo-snapshot trimming: oldest-first entry and byte-budget drops, guaranteed newest-snapshot retention, degenerate-limit clamping, and untouched within-budget stacks. | `zia;zannastudio;scene` |
| `zia_zannastudio_scene2d_large_map` | Windowed 2D canvas contract on a 512x512 scene: viewport-bounded rasterization, native-resolution 100% zoom, window-center zoom anchoring, far-corner scroll reachability, and a real-pointer paint on the last cell. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_diagnostic_actions` | Asynchronous suppression/fix-it/missing-bind edits, including opening and starting from a selected Problems record. | `zia;zannastudio;semantic;threads;requires_display` |
| `zia_zannastudio_terminal` | Terminal session/controller non-display behavior. | `zia;zannastudio;terminal` |
| `zia_zannastudio_terminal_open` | Terminal panel open/start behavior. | `zia;zannastudio;terminal;requires_display` |
| `zia_zannastudio_terminal_hidden_start` | Terminal hidden/open lifecycle and hidden-output draining behavior. | `zia;zannastudio;terminal;requires_display` |
| `zia_zannastudio_terminal_render` | Terminal rendering path. | `zia;zannastudio;terminal;requires_display` |
| `zia_zannastudio_context_menu` | Context menu routing and enabled state. | `zia;zannastudio;context-menu;requires_display` |
| `zia_zannastudio_syntax_render` | Syntax rendering path. | `zia;zannastudio;syntax;requires_display` |
| `zia_zannastudio_formatting` | Formatting commands and helpers. | `zia;zannastudio;format` |
| `zia_zannastudio_debug` | VM-backed debug adapter integration. | `zia;zannastudio;debug` |
| `zia_zannastudio_semantic_tokens` | Semantic token rendering behavior. | `zia;zannastudio;semantic;requires_display` |
| `zia_zannastudio_console_search` | Output panel helpers, contextual notification eligibility, dynamic surface Edit-menu state, docked search panel, workspace-symbol discovery, and Quick Open ranking. | `zia;zannastudio;console;search;requires_display` |
| `native_smoke_zannastudio_completion_arm64` | Native completion link/e2e smoke on arm64 when enabled. | native/e2e labels from CMake |
| `native_smoke_zannastudio_completion_x64` | Native completion link/e2e smoke on x64 when enabled. | native/e2e labels from CMake |

Some labels and availability depend on the configured platform, graphics
backend, and native-link settings.

## Useful Test Commands

Compile the Zanna Studio project:

```sh
ctest --test-dir build -R zia_smoke_zannastudio_project_compile --output-on-failure
```

Run all Zanna Studio-labelled tests:

```sh
ctest --test-dir build -L zannastudio --output-on-failure
```

Run non-display Zanna Studio tests in a headless environment:

```sh
ctest --test-dir build -L zannastudio -LE requires_display --output-on-failure
```

Run display-dependent editor and UI probes:

```sh
ctest --test-dir build -R 'zia_zannastudio_(editor_hot_path|intellisense|file_tree|activity_bar|run_debug_view|console_search)' --output-on-failure
```

Run terminal-specific probes:

```sh
ctest --test-dir build -R 'zia_zannastudio_terminal' --output-on-failure
```

Run debugger probe:

```sh
ctest --test-dir build -R zia_zannastudio_debug --output-on-failure
```

Run Source Control probe:

```sh
ctest --test-dir build -R zia_zannastudio_scm --output-on-failure
```

Run OutputPane low-level regressions:

```sh
ctest --test-dir build -R test_vg_audit_fixes --output-on-failure
```

## Probe Source Map

Zanna Studio probes live in `zannastudio/src/probes/`.

| Probe file | Area |
| --- | --- |
| `smoke_probe.zia` | Basic app compile/runtime smoke. |
| `phase0_phase1_probe.zia` | Documents, commands, sessions, language gates, surface-aware standard Edit command gates, workspace edits. |
| `phase2_phase3_probe.zia` | Build/run/debug boundary and scene data contracts. |
| `editor_hot_path_probe.zia` | Editor copy/layout/index performance hot paths. |
| `intellisense_probe.zia` | Completion and language-service UI behavior. |
| `file_tree_probe.zia` | Explorer behavior. |
| `activity_bar_probe.zia` | Activity bar/workbench view toggles and primary-sidebar reset. |
| `bottom_panel_probe.zia` | Pointer-driven primary-sidebar/tool-strip docking, floating move/resize/edge-redock and persisted bounds, focus/collapse safety, mirrored persisted sizing, tool-tab order, compact targets, and reset hygiene. |
| `multi_root_file_tree_probe.zia` | Multi-root workspace behavior. |
| `scm_probe.zia` | Git command layer, async status jobs, paths with spaces, staged renames, and realistic porcelain-v2 unmerged rows. |
| `scm_history_probe.zia` | Paged history, commit files/revisions, and credential-prompt classification. |
| `scm_view_probe.zia` | Real-Git responsive controls, action enablement, pointer staging, Enter commits, and conflict recovery guidance. |
| `terminal_probe.zia` | Terminal session/controller core behavior. |
| `terminal_open_probe.zia` | Terminal panel start/open behavior. |
| `terminal_hidden_start_probe.zia` | Hidden terminal lifecycle and output replay. |
| `terminal_render_probe.zia` | Terminal render behavior. |
| `context_menu_probe.zia` | Context menu state and dispatch. |
| `syntax_render_probe.zia` | Syntax rendering path. |
| `formatting_probe.zia` | Formatting helpers and commands. |
| `debug_probe.zia` | VM debug adapter session integration. |
| `semantic_tokens_probe.zia` | Semantic tokens. |
| `console_search_probe.zia` | Output/console/contextual-notification/dynamic surface Edit/Find menu state/docked-search/Quick Open helpers. |
| `tool_panel_toolbar_probe.zia` | Problems/Output/References toolbar state, grouped filtering, incremental publication, pointer actions, durable locations, empty states, and responsive side-dock layout. |
| `debug_tool_surfaces_probe.zia` | Call Stack and Debug Console toolbar state, durable filtered-frame identity, session-owned output clearing, pointer actions, and responsive side-dock layout. |
| `run_debug_view_probe.zia` | Run/Debug activity routing, full debugger-state mapping, real controls, versioned/filterable breakpoint identity and removal, persistence, and compact scrolling. |
| `scene_editor_2d_probe.zia` | 2D canvas/inspector responsiveness, true retained parent/child rows, collapse retention, stable retained-row multi-selection, ID/type hierarchy Find wrapping/ancestor expansion/standard-command focus/hidden-inspector reveal with unchanged scene bytes and revision, transactional subtree-aware BEFORE/INTO/AFTER drops with cycle/no-op rejection and exact undo/redo, hierarchy-preserving duplicate/cross-scene paste, inspector-safe Arrow routing, exact pixel nudge and tile-step ownership, primary-axis alignment, deterministic distribution/no-op history, scene-wide typed metadata and tab-local selection, root-owned project-component defaults/preservation/conflict rollback/no-op/undo/redo/raw-field drafts, structured schema edit/file undo/file redo with unchanged scene bytes/revision, typed clipboard envelope and malformed-data rejection, exact group move/delete/cut rollback, portable project-asset matching/search/preview, relative real-PNG tileset decode/source-over render/quiet refresh, palette hit/selection behavior, no-op and rejected assignment safety, asset/paint/object transactions, properties, import, canonical round trips, history, and save safety. |
| `scene_canvas_selection_probe.zia` | Point replace/add/toggle/group preservation, reverse inclusive cell queries, marquee replace/union/toggle/empty rules, real captured blank drag/release, visible overlay, Escape cancelation, and exact content/revision/history/dirty isolation. |
| `scene_tile_tools_probe.zia` | Inclusive rectangle/no-op/history behavior, four-connected Fill counts and exact undo, active-layer Pick isolation, real captured preview/release, Escape cancelation, and freehand snapshot rollback. |
| `scene_editor_3d_probe.zia` | 3D hierarchy/viewport responsiveness, true retained parent/child rows, collapse persistence, stable retained-row multi-selection, node-name hierarchy Find wrapping/ancestor expansion/standard-command focus/hidden-inspector reveal with unchanged VSCN bytes, revision, history, and camera, transactional BEFORE/INTO/AFTER row drops with preserve-world round trips/cycle rejection/exact undo, exact typed node gameplay-metadata kinds, root-owned multi-node project-component defaults/preservation/conflict rollback/no-op/undo/redo/raw-field drafts, unfiltered cross-target schema maintenance/order availability with unchanged VSCN bytes/revision, invalid/no-op rejection, VSCN v6 round trip, one-step history and tab-local selection, cycle/no-op-safe exact preserve-world chooser reparenting, singular/shear rollback, preserve-local opt-out, multi-root descendant collapse, stable contiguous sibling-block Earlier/Later moves with boundary/gap/mixed-parent no-ops, native mixed-state batch visibility and exact undo, post-move selection remapping, exact group transform/subtree duplicate/delete/cut/cross-document paste rollback, wrong-kind rejection, focus-scoped tool shortcuts, transform projection/snapping, consecutive node-identity-safe edits, shared-material copy-on-edit, decoded-map limits, PBR scalar and real PNG map load/clear/history, embedded-map VSCN round trips, import, and save safety. |
| `scene_light_authoring_probe.zia` | Seven-type construction/reconstruction, public spot-cone values, shadow eligibility, independent type conversion, no-op history, add/remove undo/redo, hierarchy and viewport presentation, VSCN reopen, and disabled multi-selection actions. |
| `scene_material_batch_probe.zia` | Spinner retained-seed mixed state, common/mixed scalar/color/enum/Boolean presentation, sparse alpha-only patching without collateral PBR changes, missing-material creation, selection retention, no-op detection, one-step removal and map assignment/clear, and byte-exact undo/redo. |
| `scene_shaded_viewport_probe.zia` | Windowless shaded and triangle-wireframe rendering, exact camera/overlay projection, retained target dimensions, authored pixel differences, accessible real-pointer mode switching, per-document state, and canonical-history isolation. |
| `scene_viewport_picking_probe.zia` | Visible mesh-bounds closest-hit picking away from origins, shaded/wireframe parity, meshless fallback, selection modifier policy, exact camera-plane pan, Super-key constants, workspace persistence, and canonical-history isolation. |
| `scene_component_schema_probe.zia` | Bounded version-1 schema parsing, target lookup, canonical explicit/implicit defaults, and fail-closed invalid/duplicate behavior. |
| `scene_component_authoring_probe.zia` | Pure component/field create/update/delete/reorder, exact scalar defaults, unknown-member preservation, duplicate/last-field rejection, atomic create/replace, external conflicts/reload, and bounded exact file undo/redo. |
| `scene_workspace_state_probe.zia` | Bounded visual-scene and typed-property-selection persistence, hostile-field clamping, and untitled scene recovery. |
| `diagnostic_action_probe.zia` | Stale-safe asynchronous diagnostic edits and selected-Problems action routing. |

## Performance Logging

Set `ZANNASTUDIO_PERF_LOG` before launching the IDE to write frame/controller and
editor performance counters:

```sh
ZANNASTUDIO_PERF_LOG=/tmp/zannastudio-perf.log ./src/zannastudio/bin/zannastudio
```

Perf output is intended for dogfood sessions and hot-path regressions. It is
especially useful for checking:

- Full-text copy counts and bytes.
- Layout scan counts.
- Project index updates and bytes.
- Controller timing spikes.
- Worst-frame windows during large-file editing.

## Manual Verification Checklist

Use this when changing user-facing IDE behavior:

- Launch with a temporary settings directory or isolated profile.
- Open a real project with at least one large Zia file.
- Restore session and verify active tab/cursor/scroll.
- Type in a large file and verify no obvious frame stalls.
- Trigger completion and accept an item.
- Trigger diagnostics, filter by severity/text, navigate from Problems, and run
  an available Quick Fix.
- Filter, wrap, pause following, copy, and clear Output from its toolbar.
- Use hover and signature help.
- Use Quick Open and workspace search.
- Rename or create a file from the explorer.
- Save, Save As, Save All, and close modified tabs.
- Build and run the project.
- Start debugging, hit a breakpoint, step, inspect locals/call stack, evaluate,
  and stop.
- Open the integrated terminal, type a shell command, resize, stop, restart.
- Use Source Control status, stage/unstage, diff, and commit in a throwaway repo.
- Switch dark/light theme and check contrast.
- Restart the IDE and verify session/recovery behavior.

## Coverage Gaps

Known areas needing stronger tests:

- Primary-sidebar left/right movement, activity-rail placement, real typed
  target drops, tree-focus recovery, mirrored width changes, hidden restoration,
  settings validation, and reset behavior are display-probe covered. Tool-panel
  coverage includes primary-group pointer drops, direct selected-tool header
  controls, simultaneous left/bottom/right/floating groups, migration-safe
  membership and floating-bounds persistence, real floating move/resize/edge
  docking, tab order, all three attached split states, terminal focus, and
  compact target containment. Native secondary-window/multi-monitor detachment
  remains outside the current in-window model.
- The 2D/3D scene editors have focused hierarchy, viewport, history,
  per-document-state, save/import, stable multi-selection, transactional batch
  duplicate/delete/cut/cross-document paste, typed clipboard rejection, and
  deterministic group transform-tool and focus-ownership probes.
  The 2D probe pins real PNG atlas source-over rendering, relative-path resolution,
  project-index image discovery/preview, palette selection, nondirty automatic
  external-image refresh, assignment/clear history, canonical round trips,
  retained hierarchy expansion, subtree-aware before/into/after drops,
  cycle/no-op rejection, exact hierarchy undo/redo, hierarchy-preserving typed
  duplication and cross-scene paste, group move/delete/cut, exact paste undo,
  transactional batch property set/remove, focus-safe selection commands and
  Arrow routing, exact pixel nudging, primary-axis alignment, stable rounded
  distribution and no-op history, plus root-owned component application with
  exact defaults, preserved overrides, type-conflict rollback, no-op history,
  raw-field transfer, and rejected-candidate safety.
  The dedicated component-authoring probe pins structured mutations,
  unknown-member retention, disk conflict handling, and exact file history.
  The 3D probe pins selected-ancestor subtree handling, group transforms,
  exact typed gameplay metadata, invalid/no-op rejection, VSCN v6 persistence,
  one-action history, per-tab/session metadata selection, and multi-node
  component application with exact defaults, preserved overrides,
  type-conflict rollback, no-op history, and raw-field transfer,
  material focus routing, shared-material copy-on-edit, apply/remove history,
  relative numeric multi-node inspector transforms,
  real PNG map assignment/clearing, the decoded-raster ceiling, and scalar plus
  embedded-map VSCN round trips, persistent canonical map thumbnails across
  assignment/clear/undo/reload, subtree clipboard transfer, exact paste undo,
  existing-node cycle/no-op rejection, exact preserve-world and preserve-local
  multi-root reparenting, singular/shear rollback, selection remapping, stable
  Local/World Move differences, exact world Rotate/Scale pivots, persisted
  transform space, one-step world history, reversed ancestor/descendant
  selection order, lossy world-edit rollback, conditioned plane picking/solve,
  a real snapped XY pointer drag, Local plane movement, and exact
  parent-before-child World XZ plane history plus singular-plane rejection,
  contiguous sibling-block ordering, retained hierarchy expansion, and
  before/into/after row-drop transactions with
  boundary/gap/mixed-parent no-ops, and wrong-kind rejection. Native picker
  behavior on each desktop backend,
  coarse-timestamp same-size external rewrites, and large/malformed source
  stress still need manual coverage. Strict KTX2 decoding is runtime-covered.
  Rich tagged/import-configured asset pipelines, cubemap/lightmap authoring,
  automatic schema/scene-data migrations and generalized runtime component
  composition,
  advanced Tiled atlas/image-collection editing, tile
  animation/collision/metadata editing need broader coverage. The dedicated
  `scene_material_batch_probe` pins mixed-state presentation, sparse patching,
  staged group map/remove transactions, no-op history, and exact undo/redo.
  `scene_shaded_viewport_probe` pins production SceneGraph shading, triangle
  wireframes, exact runtime-camera overlay alignment, retained target sizing,
  accessible mode switching, and content/history isolation.
  `scene_viewport_picking_probe` pins off-origin nearest-depth mesh-bounds
  selection, mode parity, meshless fallback, additive/toggle/blank policy,
  camera-plane pan, Super-key availability, and content/history isolation.
  `scene_canvas_selection_probe` pins 2D point modifiers, reverse inclusive
  cell rectangles, marquee replace/union/toggle/empty semantics, real pointer
  capture/release, visible feedback, Escape cancelation, and complete
  content/revision/history/dirty isolation.
  `scene_tile_tools_probe` pins inclusive forward/reverse rectangle commits,
  contiguous-fill counts and no-ops, active-layer tile sampling, captured
  preview/release, Escape cancelation, and exact freehand rollback.
  `scene_light_authoring_probe` pins all seven light types, spot-cone
  inspection, independent replacement, exact transaction counts, VSCN reopen,
  marker presentation, and truthful single-selection scope.
  Projected rotation rings have
  focused pure geometry plus real hover/down/move/up and exact-history coverage
  in `scene_rotation_ring_probe`. Planar Scale has focused mode-aware geometry,
  real snapped pointer input, Local/World group history, exact undo, ancestor
  ordering, and shear-rejection coverage in `scene_scale_plane_probe`.
- Source Control status, staging, commit, paged history, per-commit diffs,
  credential-prompt detection, narrow layout, and a real content-conflict
  recovery are probe-covered (`scm_probe`, `scm_history_probe`,
  `scm_view_probe`); a real credentialed push plus rename/multi-file conflict
  recovery still need manual passes.
- Terminal emulation is pinned by `test_vg_outputpane_term.c` (grid-state
  sequence table) and `terminal_altscreen_probe.zia` (alt screen, scroll
  regions, edits, replies through the runtime surface); running vim/less/htop
  in the built IDE remains a manual per-platform gate.
- Cross-platform PTY/ConPTY behavior needs regular Windows/macOS/Linux smoke.
- Tool-panel virtualization and huge-output behavior need stronger UI stress
  coverage.
- Debugger class-field expansion is covered end-to-end by
  `zia_zannastudio_debug_fields`; struct-payload expansion and a dedicated
  watch-management panel are not present.
- Accessibility and keyboard-focus behavior need more systematic checks.
