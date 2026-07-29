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
| `zia_zannastudio_scene_canvas_selection` | Public 2D point and marquee selection policy, reverse inclusive cell queries, real captured blank-space drag/release, visible overlay, Escape cancelation, exact canonical/history isolation, click-granted canvas surface focus, the workspace-only focused-canvas Escape selection clear (never on a gesture-cancel key edge), selection-scoped inspector fields (object fields need selected objects; scene groups stay with the scene), and live single-object X/Y editing (echo values commit nothing; differing values commit once and undo byte-exactly). | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_tile_tools` | Inclusive forward/reverse Rectangle transactions, four-connected Fill counts and undo, Pick isolation, captured non-destructive preview/release, Escape cancelation, and freehand rollback. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_editor_3d` | Responsive VSCN hierarchy/viewport behavior, real expandable TreeView structure/collapse persistence/stable multi-selection, case-insensitive node-name hierarchy Find with wrapping/ancestor expansion/hidden-inspector reveal and exact no-history/camera mutation, transactional BEFORE/INTO/AFTER row drops with preserve-world round trips/cycle rejection/exact undo, typed node gameplay-metadata validation/VSCN v6/history/tab isolation, atomic multi-node project-component application/conflicts/history, complete cross-target structured schema maintenance with scene isolation, ordered failed-direct-model to procedural-scene preview fallback with successful-rule transform retention, cycle/no-op-safe exact preserve-world chooser reparenting, singular/shear rollback, explicit preserve-local mode, selection remapping, stable contiguous sibling-block ordering with boundary/gap/mixed-parent no-ops and exact VSCN undo/redo, native mixed-state batch visibility with exact one-step history, exact one-history group transform/subtree duplicate/delete/cut/cross-document paste, wrong-kind clipboard rejection, focus-safe W/E/R routing (including material/map controls), parent-aware Move/Rotate/Scale math and snapping, conditioned XY/XZ/YZ plane picking, real snapped two-axis pointer input, Local/World plane group history and ancestor ordering, clone-safe bounded PBR scalar/image-map editing, embedded-map VSCN round trips, node identity, import, history, and save safety. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_light_authoring` | All seven `Light3D` types, public spot-cone readback, independent replacement, exact no-op suppression, one-step add/apply/remove history, VSCN round trips, hierarchy/viewport markers, and single-selection gating. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_rotation_ring` | Projected ellipse conditioning, wrap-safe angular math, real ring hover/down/move/up, Local/World group history, exact undo, and edge-on rejection. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_scale_plane` | Mode-aware conditioned plane math, real snapped XY Scale pointer input without viewport reflow, Local and exact World group scaling, untouched zero-scale preservation, hierarchy ordering, byte-exact undo, and atomic shear rejection. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_surface_drop` | Viewport-focused End routing, multi-root visual-bottom placement, selected-mesh self-ray exclusion, nearest authored/project-environment surfaces, live X-axis Surface Move pointer input, bounded terrain-rise probing, twist-preserving slope-normal alignment for live Move and Drop, transient prefab bounds/following, deferred one-step history, exact undo/redo, workspace state, grounded/missing-surface no-ops, and complete terrain-action containment at medium, laptop, and compact scene widths. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_vertex_snap` | Authoritative runtime mesh-vertex readback, exact source/target classification, persistent Placement-menu state, real vertex-to-vertex, pivot-to-vertex, and vertex-to-surface pointer gestures, live canonical isolation, one-step release history, exact Escape/undo rollback, and toolbar containment. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_terrain_authoring` | Canonical grid topology and typed-metadata protection, pure deterministic brush math, real Terrain creation and precise viewport sculpt input, live no-history mesh feedback, single release history, exact Escape/undo/redo restoration, flatten/regenerate transactions, VSCN round trips, and responsive inspector containment. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_material_batch` | Runtime Spinner mixed-state use, truthful common/mixed PBR presentation, sparse per-field group patches, missing-component defaults, staged clone sharing, complete-selection retention, batch removal/map assignment/map clearing, no-op history, and byte-exact undo/redo. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_shaded_viewport` | Default per-scene shaded mode, windowless Canvas3D/RenderTarget retention, exact orthographic camera-to-overlay projection, authored mesh submission and pixel output, real-pointer triangle-wireframe switching, accessible mode state, zero VSCN/history mutation, idle full-resolution target invariant, the ADR 0191 acceleration request rebuilding a working (software-fallback) renderer without content changes, click-granted viewport surface focus, focused-surface W/E/R tool routing, and the workspace-only Escape selection clear. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_orientation_navigator` | Exact responsive corner geometry, six camera faces, active/hover pixels, real face and projection-chip clicks, panel input priority, per-tab workspace restoration, and zero VSCN/revision/history mutation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_hierarchy_affordances` | Real group Show/Hide input and exact undo, mixed-selection action truth, tab-local pick locks and row cues, live reorder identity remapping, deleted-lock pruning, canonical-history isolation, and wide/narrow action-strip containment. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_gameplay_preview` | Real Ashfall project loading, exact gameplay eye/FOV across responsive docks, schema-v18 Wave defaults/All switching/filtered counts/canonical isolation, schema-v17 typed direct enemy-rig paths/transforms/counts, schema-v16 typed surface matches with exact production albedo/normal/PBR/emissive/environment state and immediate canonical-material restoration, transient terrain plus schema-v15 runtime `Water3D` dimensions/production images/two waves/bounded animation, one-frame canonical-preview-water draw accounting, schema-v10 retained five-pass post-FX, schema-v13 direct spawn creation with exact metadata/immediate prefab preview/one-step byte-exact undo-redo, schema-v12 1600x900 target framing/matte/camera restoration, finalized runtime-lit luminance, actual PNG captures, Gameplay View recovery, and canonical content isolation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_gameplay_preview_2d` | Real Xenoscape project loading, native-scale player-start framing, project biome/object pixels, schema-v11 game-category draw order despite different canonical array order, schema-v13 direct enemy creation with exact type/position/preview/one-step byte-exact undo-redo, schema-v12 1280x720 framing/matte/camera restoration, actual PNG capture, and canonical content isolation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_asset_thumbnails` | Bounded image and VSCN thumbnail rendering, responsive visible-card grid scheduling, grid-to-list selection identity, real card click routing, selected-model detail preview, and fixture isolation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_viewport_picking` | Off-origin closest-depth visible mesh-bounds picking, shaded/wireframe parity, meshless marker fallback, replace/add/toggle/blank selection policy, public Control/Command key surface, exact camera-plane pan, workspace persistence, zero VSCN/history mutation, and selection-scoped inspector groups (node groups need a selection, single-selection groups exactly one node, component groups presence on the selection, scene groups the Scene tab) including the Add-component row creating a default light as one undoable, exactly-restoring transaction, and live single-node transform editing (echo values commit nothing; differing values commit once and undo byte-exactly). | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_command_bar` | Real 2D Scene/Tools/View and 3D Scene/Create/Placement/View popup clicks, active/checkable state refresh, bounded command-bar and canvas/viewport height, and stable progressive disclosure at medium and wide widths. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_inspector_topics` | One-topic-at-a-time 2D/3D Scene-inspector visibility, real dropdown revision dispatch, sibling-group isolation, canonical-byte isolation, and per-document topic restoration. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene_workspace_state` | Bounded per-scene canvas/camera/inspector/typed-property-selection/preview-state/complete-2D-tool-range/transform-tool/shaded-wireframe session persistence, hostile-field clamping, and dirty untitled-scene recovery. | `zia;zannastudio;scene;persistence` |
| `zia_zannastudio_scene_component_schema` | Fail-closed project component parsing through schema v18, exact explicit/implicit typed defaults, bounded exact-typed node-preview states, preview-group completeness and bounds including typed direct-model matches/transforms/paths and typed material maps/PBR/emissive overlays, additive environment layers, runtime water inputs, object draw stacks, and paired game-output dimensions, target-compatible creation recipes, target filtering, duplicate rejection, and empty-schema behavior. | `zia;zannastudio;scene` |
| `zia_zannastudio_scene_component_authoring` | Pure structured component/field edits, stable ordering, typed defaults, preview-profile preservation through schema-v18 node-preview states, schema-v17 direct models, schema-v16 material overlays, schema-v15 runtime water layers, and schema-v14 additive environment layers, schema-v13 starter/edit/target-normalized creation recipes, atomic create/replace, external-conflict rejection, and bounded exact file undo/redo. | `zia;zannastudio;scene` |
| `zia_zannastudio_scene_history` | Shared ADR 0190 undo-snapshot trimming: oldest-first entry and byte-budget drops, guaranteed newest-snapshot retention, degenerate-limit clamping, and untouched within-budget stacks. | `zia;zannastudio;scene` |
| `zia_zannastudio_scene2d_history_palette` | Real labeled 2D History rows, row-selection jumps with complete redo retention, tab-owned labels, 50–300% palette geometry, numeric tile search/reveal/bounds, zoom-aware pointer selection, and exact scene/history isolation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene2d_large_map` | Windowed 2D canvas contract on a 512x512 scene: viewport-bounded rasterization, native-resolution 100% zoom, window-center zoom anchoring, far-corner scroll reachability, and a real-pointer paint on the last cell. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene2d_rulers_guides` | Exact scroll/zoom/padding ruler geometry, real click and captured create/move input on both axes, live preview, drag-out removal, collision/Escape rollback, visibility reflow, tab ownership, 64-per-axis bounds, captured pixels, and zero scene/revision/history mutation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene2d_layer_navigator` | Real Add/Move/Remove and retained-row drag input, one-history reorder/byte-exact undo, Enter visibility and exact undo, row cues, tab-local Lock/Solo state, live identity remapping/pruning, boundary-aware actions, and wide/narrow containment. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene2d_brush_preview` | Real-atlas Paint/Fill/stamp/captured-Rectangle ghost pixels, immediate tool-switch repaint, stamp clearing, exact Line/Ellipse tab ownership, capture cancellation, and zero scene/revision/history/dirty mutation. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene2d_autotile_preview` | Runtime-exact N/E/S/W masks and first-64-rule precedence, base-layer scope, real-atlas resolved pixels, prospective Paint-neighbor variants, per-tab toggle restoration, raw-cell preservation, runtime parity after paint, and autotile-before-animation composition. | `zia;zannastudio;scene;requires_display` |
| `zia_zannastudio_scene2d_object_draw_stack` | Project tile-layer interleaving, stable signed priorities, wrong-kind override fallback, real overlapping sprite pixels, visual-topmost canvas picking, direct inspector Apply/no-op/Use Project transactions, and exact project-default byte restoration. | `zia;zannastudio;scene;requires_display` |
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
| `console_search_probe.zia` | Output/console, contextual notifications, status presentation, dynamic surface Edit/Find command state, idempotent idle visual-surface publication, docked Search, and Quick Open helpers. |
| `tool_panel_toolbar_probe.zia` | Problems/Output/References toolbar state, grouped filtering, incremental publication, pointer actions, durable locations, empty states, and responsive side-dock layout. |
| `debug_tool_surfaces_probe.zia` | Call Stack and Debug Console toolbar state, durable filtered-frame identity, session-owned output clearing, pointer actions, and responsive side-dock layout. |
| `run_debug_view_probe.zia` | Run/Debug activity routing, full debugger-state mapping, real controls, versioned/filterable breakpoint identity and removal, persistence, and compact scrolling. |
| `scene_editor_2d_probe.zia` | 2D canvas/inspector responsiveness, true retained parent/child rows, collapse retention, stable retained-row multi-selection, ID/type hierarchy Find wrapping/ancestor expansion/standard-command focus/hidden-inspector reveal with unchanged scene bytes and revision, transactional subtree-aware BEFORE/INTO/AFTER drops with cycle/no-op rejection and exact undo/redo, hierarchy-preserving duplicate/cross-scene paste, inspector-safe Arrow routing, exact pixel nudge and tile-step ownership, primary-axis alignment, deterministic distribution/no-op history, scene-wide typed metadata and tab-local selection, root-owned project-component defaults/preservation/conflict rollback/no-op/undo/redo/raw-field drafts, structured schema edit/file undo/file redo with unchanged scene bytes/revision, typed clipboard envelope and malformed-data rejection, exact group move/delete/cut rollback, portable project-asset matching/search/preview, relative real-PNG tileset decode/source-over render/quiet refresh, palette hit/selection behavior, no-op and rejected assignment safety, asset/paint/object transactions, properties, import, canonical round trips, history, and save safety. |
| `scene_canvas_selection_probe.zia` | Point replace/add/toggle/group preservation, reverse inclusive cell queries, marquee replace/union/toggle/empty rules, real captured blank drag/release, visible overlay, Escape cancelation, and exact content/revision/history/dirty isolation. |
| `scene_tile_tools_probe.zia` | Inclusive rectangle/no-op/history behavior, four-connected Fill counts and exact undo, active-layer Pick isolation, real captured preview/release, Escape cancelation, and freehand snapshot rollback. |
| `scene2d_history_palette_probe.zia` | Labeled oldest-first history rows, click-equivalent jumps with retained redo, per-tab label restoration, bounded palette zoom/search/reveal, real scaled-pointer selection, and canonical-content/revision/history isolation. |
| `scene2d_rulers_guides_probe.zia` | Scroll/zoom/padding ruler ticks, real guide toggle plus captured create/move input, both-axis live previews, drag-out removal, collision/Escape rollback, visibility reflow, per-tab restoration, capacity bounds, rendered pixel captures, and canonical-history isolation. |
| `scene2d_layer_navigator_probe.zia` | Layer visibility activation/undo, real retained-row drag ordering and exact undo, visibility/lock/solo/opacity cues, per-tab workspace isolation, structural state remapping/pruning, boundary action state, and responsive button containment. |
| `scene2d_brush_preview_probe.zia` | Real two-frame atlas Paint/Fill/stamp/captured-Rectangle ghosts, immediate stale-ghost removal, consistent stamp clearing, Line/Ellipse tab identity, Escape capture release, and canonical-state isolation. |
| `scene2d_autotile_preview_probe.zia` | Exact runtime masks and registration precedence, base-layer scope, real-atlas resolved pixels, neighbor-aware single-cell hover, tab-local toggle restoration, raw-cell/history isolation, runtime parity after paint, and autotile-before-animation composition. |
| `scene2d_object_draw_stack_probe.zia` | Stable project priority/layer stacks, wrong-kind per-object fallback, real tile/sprite interleaving and overlap pixels, matching topmost pointer picks, direct inspector transactions/no-op suppression, and exact project-default restoration. |
| `scene_editor_3d_probe.zia` | 3D hierarchy/viewport responsiveness, true retained parent/child rows, collapse persistence, stable retained-row multi-selection, node-name hierarchy Find wrapping/ancestor expansion/standard-command focus/hidden-inspector reveal with unchanged VSCN bytes, revision, history, and camera, transactional BEFORE/INTO/AFTER row drops with preserve-world round trips/cycle rejection/exact undo, exact typed node gameplay-metadata kinds, root-owned multi-node project-component defaults/preservation/conflict rollback/no-op/undo/redo/raw-field drafts, unfiltered cross-target schema maintenance/order availability with unchanged VSCN bytes/revision, exact-typed project preview-state/All filtering with canonical isolation, ordered failed-direct-model to procedural-scene preview fallback and successful-rule transform retention, invalid/no-op rejection, VSCN v6 round trip, one-step history and tab-local selection, cycle/no-op-safe exact preserve-world chooser reparenting, singular/shear rollback, preserve-local opt-out, multi-root descendant collapse, stable contiguous sibling-block Earlier/Later moves with boundary/gap/mixed-parent no-ops, native mixed-state batch visibility and exact undo, post-move selection remapping, exact group transform/subtree duplicate/delete/cut/cross-document paste rollback, wrong-kind rejection, focus-scoped tool shortcuts, transform projection/snapping, consecutive node-identity-safe edits, shared-material copy-on-edit, decoded-map limits, PBR scalar and real PNG map load/clear/history, embedded-map VSCN round trips, import, and save safety. |
| `scene_light_authoring_probe.zia` | Seven-type construction/reconstruction, public spot-cone values, shadow eligibility, independent type conversion, no-op history, add/remove undo/redo, hierarchy and viewport presentation, VSCN reopen, and disabled multi-selection actions. |
| `scene_material_batch_probe.zia` | Spinner retained-seed mixed state, common/mixed scalar/color/enum/Boolean presentation, sparse alpha-only patching without collateral PBR changes, missing-material creation, selection retention, no-op detection, one-step removal and map assignment/clear, and byte-exact undo/redo. |
| `scene_shaded_viewport_probe.zia` | Windowless shaded and triangle-wireframe rendering, exact camera/overlay projection, retained target dimensions, authored pixel differences, accessible real-pointer mode switching, per-document state, and canonical-history isolation. |
| `scene_orientation_navigator_probe.zia` | Production face geometry, six axis camera poses, per-tab restoration, real hover/click/projection routing, protected panel chrome, captured pixels, and canonical-history isolation. |
| `scene_game_view_probe.zia` | Exact clean 2D authored pixels, 3D navigator/gizmo/editor-light masking, temporary shaded presentation, disabled edit/drop controls, real click suppression, per-tab isolation, exact preference/selection restoration, and canonical-history isolation. |
| `scene_surface_drop_probe.zia` | End-driven multi-root placement plus real live Surface Move pointer input on canonical/project-only surfaces, selected canonical/prefab self-ray exclusion, transient prefab bounds/synchronization, bounded raised-ramp acquisition, exact twist-preserving normal alignment through both live Move and Drop, deferred exact history, workspace state, undo/redo, no-op boundaries, and medium/laptop/compact terrain-toolbar containment. |
| `scene_vertex_snap_probe.zia` | Exact runtime source/target candidates and real vertex-to-vertex, pivot-to-vertex, and vertex-to-precise-surface drags, persistent Placement-menu state, live no-history transforms, one-step release commits, exact Escape/undo restoration, and toolbar containment. |
| `scene_terrain_authoring_probe.zia` | Exact centered grid topology, pure Raise/Lower/Smooth/Flatten behavior, typed-convention rejection, real Create-menu Terrain dispatch and viewport stroke capture, live mesh/history isolation, one-step commit, exact Escape/undo/redo, flatten/regenerate no-ops and transactions, VSCN round trips, and responsive inspector containment. |
| `scene_hierarchy_affordances_probe.zia` | Real pointer Show/Hide, exact group visibility undo, mixed-state controls, tab-owned pick locks, retained-row cues, reorder identity remapping, delete pruning, and responsive action containment. |
| `scene_gameplay_preview_probe.zia` | Real Ashfall mission loading, project-owned camera/terrain/post-FX resolution, exact schema-v18 Wave 1 default plus Wave 2/All switching and active/direct/filtered counts, exact schema-v17 typed direct enemy-rig paths/scales/yaw/counts, exact schema-v16 production surface maps/PBR/emissive/environment overlays with restored canonical material pointers, exact schema-v15 runtime `Water3D` center/full dimensions/production images/resolution/two waves, bounded animated pixel changes, one-frame canonical-preview-water draw accounting, finalized runtime-lit luminance, schema-v13 direct spawn creation with exact metadata/immediate prefab preview/one-step exact history, Gameplay View recovery, schema-v12 1600x900 render-target framing/matte/camera lock and restoration, chrome-free PNG captures, and canonical isolation. |
| `scene_gameplay_preview_2d_probe.zia` | Real Xenoscape region loading, native-scale player framing, project biome/object decoding, schema-v11 gameplay category order despite different canonical grouping, schema-v13 direct enemy creation with exact position/type/preview/one-step exact history, schema-v12 1280x720 framing/matte/navigation lock and restoration, chrome-free PNG capture, and canonical isolation. |
| `scene_asset_thumbnail_probe.zia` | Real PNG and VSCN square previews, visible-card one-per-frame scheduling, responsive grid geometry, canonical list selection behind card clicks, list/grid switching, typed detail rendering, and a captured model thumbnail. |
| `scene_viewport_picking_probe.zia` | Visible mesh-bounds closest-hit picking away from origins, shaded/wireframe parity, meshless fallback, selection modifier policy, exact camera-plane pan, real compact Create-menu Box/Cylinder actions, Super-key constants, workspace persistence, and canonical-history isolation. |
| `scene_command_bar_probe.zia` | Real 2D Scene/Tools/View and 3D Scene/Create/Placement/View menu clicks, checked-state refresh, medium-lane bar/canvas/viewport bounds, and wide progressive-disclosure stability. |
| `scene_inspector_topics_probe.zia` | One-topic-at-a-time 2D/3D Scene-inspector visibility, real dropdown revision dispatch, sibling-group isolation, canonical-byte isolation, and per-document topic restoration. |
| `scene_component_schema_probe.zia` | Bounded version-1 through version-18 schema parsing, target lookup, canonical explicit/implicit defaults, exact-typed node-preview state labels/values/defaults, complete/ranged preview profiles including typed direct-model matches/transforms/paths and typed material maps/PBR/emissive overlays, additive environment layers, runtime water inputs, object draw stacks, and paired game-output dimensions, target-compatible creation recipes, and fail-closed invalid/duplicate behavior. |
| `scene_component_authoring_probe.zia` | Pure component/field create/update/delete/reorder, exact scalar defaults, preview-profile/version preservation through schema-v18 node-preview states, schema-v17 direct models, schema-v16 material overlays, schema-v15 runtime water, and schema-v14 additive environment layers, schema-v13 starter/edit/target-normalized creation recipes, duplicate/last-field rejection, atomic create/replace, external conflicts/reload, and bounded exact file undo/redo. |
| `scene_workspace_state_probe.zia` | Bounded visual-scene, complete 2D tool-range, typed-property-selection, preview-state, and Scene-inspector-topic persistence, hostile-field clamping, and untitled scene recovery. |
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
  Surface placement has focused viewport-End routing, selected-geometry
  exclusion, authored/project-preview nearest-hit selection, multi-root exact
  history, and no-op coverage in `scene_surface_drop_probe`; the same fixture
  drives a real X-axis Surface Move gesture, verifies live grounding and
  transient-prefab following before serialization, climbs onto a raised
  30-degree ramp, aligns canonical and prefab up axes while preserving twist,
  exercises the same normal-aware explicit Drop path, then pins one-step
  release history and exact undo/redo. It also realizes the expanded terrain
  placement/view menus in 1000-by-700 dock-heavy, 1320-by-760 Scene Layout,
  and 560-by-420 compact windows, and proves every persistent action remains
  inside its command bar without re-expanding secondary controls. Exact
  geometry placement has separate
  `scene_vertex_snap_probe` coverage for authoritative runtime vertex
  positions, persistent Placement-menu state, real vertex/pivot-to-vertex gestures,
  precise-surface fallback, live canonical/history isolation, single release
  transactions, and exact Escape/undo restoration.
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
