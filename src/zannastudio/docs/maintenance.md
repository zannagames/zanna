# Zanna Studio Maintenance Guide

This guide explains how to change Zanna Studio without making the app harder to
reason about. It is intentionally procedural: use the section that matches the
kind of change you are making.

## Maintenance Principles

Zanna Studio is a real application, but it is still young enough that accidental
architecture drift is easy. The safest changes have these traits:

- They keep model state out of UI widgets.
- They route user commands through the command catalog and command modules.
- They use structured locations, diagnostics, and edit records rather than
  parsing displayed strings.
- They preserve document safety: dirty state, close prompts, external-change
  checks, session restore, and recovery.
- They keep expensive work off the typing hot path.
- They document partial support honestly.
- They add focused probes close to the behavior being changed.

The quickest way to make Zanna Studio feel broken is to add visible commands that
do not participate in these shared systems. A feature is not complete just
because a menu item can call a function.

## Before Changing Code

Read these docs in order:

1. [status.md](status.md), to understand whether the feature area is complete,
   partial, or absent.
2. [architecture.md](architecture.md), to understand layering and ownership.
3. [source-map.md](source-map.md), to find the concrete files.
4. [runtime-integration.md](runtime-integration.md), if the change crosses the
   runtime boundary.
5. [testing.md](testing.md), to pick verification.

Check the working tree before editing. The repository often contains unrelated
local changes. Do not revert or reformat files outside the scope of your work.

## Adding A User Command

Use this path for anything triggered by a menu item, toolbar button, shortcut,
command palette item, context menu item, or activity bar action.

1. Add a `CommandSpec` row in `services/command_catalog.zia`.
2. Choose a stable command id. Prefer lower-case words without punctuation.
3. Add a label and description that explain the action, not implementation.
4. Add a shortcut only if it is important and does not conflict.
5. Set the language-service capability when the command only makes sense for a
   subset of languages.
6. Wire menu/toolbar/context UI in `ui/app_shell.zia` or the relevant UI module.
7. Dispatch the command from `main.zia` to a command handler.
8. Implement behavior in the appropriate command module.
9. Add disabled-state behavior if the command can be visible but unavailable.
10. Add or update a probe.
11. Update docs when the command is user-visible.

Do not implement command behavior inside the command catalog. Do not hide
capability checks inside random command handlers when the command registry can
communicate availability consistently.

## Adding User Feedback

Treat a popup as an interruption, not as the default place for command status.

1. Put progress and routine completion in `AppShell.SetStatusLeft`.
2. Keep background results and failures on their durable owning panel when one
   exists; do not duplicate Build, Search, or Replace completion in a toast.
3. Route an immediate warning/error caused by the current command through
   `ui/notification_policy.zia`.
4. Show external-state warnings only when they affect the active resource.
5. Do not add routine `Info` or `Success` toasts. Recovery and application
   activation are the only reviewed exceptions.
6. Run `zannastudio_notification_policy` and the affected interaction probe.

The notification-policy lint intentionally forbids direct warning/error toast
calls from background-capable Build and Search controllers. Extend the policy
boundary and its regression cases instead of adding a local exception.

## Adding A Language Feature

Use this path for completion, diagnostics, hover, signature help, semantic
tokens, symbols, definition, references, rename, formatting, or code actions.

1. Decide which languages support the feature.
2. Update `editor/language_service.zia` if the capability changes.
3. Keep Zia-specific implementation in the relevant `editor/` controller or
   `zia/` helper.
4. Keep BASIC-specific behavior explicit. Do not let BASIC silently call Zia
   project-index APIs.
5. Use structured runtime APIs when available.
6. Wrap partial buffers through `editor/zia_query.zia` when needed.
7. Schedule work with revision checks and debounce.
8. Reject stale results when the active path, revision, or cursor context
   changes.
9. Keep large-file caps visible and documented.
10. Add tests for supported and unsupported languages.
11. Update the language matrix in [status.md](status.md).

For typing-adjacent features, treat responsiveness as part of correctness. A
feature that gives the right answer while making large-file typing stall is not
finished.

## Adding A Document Kind

A document kind is more than a file extension. Before advertising a new kind as
supported, wire it through the full document lifecycle:

1. Add language and kind detection in `services/file_utils.zia`.
2. Update open/save dialog filters.
3. Decide whether the document is editable text, read-only preview, or a
   specialized surface.
4. Update `core/document_manager.zia` open/save behavior.
5. Update `core/session.zia` restore and recovery behavior if needed.
6. Update `editor/language_service.zia` capabilities.
7. Update `ui/app_shell.zia` surface selection only when a real surface exists.
8. Ensure close prompts and dirty state work.
9. Ensure Save, Save As, Save All, Reload, and external-change detection are
   correct.
10. Add probes.
11. Document the current behavior in [status.md](status.md).

Scene files are the cautionary example. `.scene`/`.level` and `.vscn` now have
specialized 2D/3D surfaces, so changes must preserve their per-document
selection/camera/history state and scene-specific save/reload behavior rather
than routing them through the hidden text editor. The 2D layer-image path is
serialized document state, while decoded atlas pixels and scaled thumbnails are
bounded preview state; an explicit image reload must not dirty the document.
Scene-wide typed-property selection is also document/session state even though
the shared inspector widgets are presentation state. The same rule applies to
3D node gameplay-metadata selection: the inspector is presentation-only, while
the owning scene document stores its bounded selected row.

## Changing Project Scene Component Schemas

`ui/scene_component_schema.zia` is the sole parser for
`scene-components.json`; both scene editors must consume its validated records
rather than parsing JSON or inventing target/default rules locally.

When changing the format or authoring behavior:

1. Update [ADR 0160](../../../docs/adr/0160-project-scene-component-schemas.md)
   when compatibility, ownership, limits, or transaction semantics change.
2. Keep parsing bounded and fail-closed. Never publish a valid prefix or retain
   stale definitions after a failed reload.
3. Keep identities portable and scalar defaults canonical before widgets or
   runtime mutation see them.
4. Resolve a saved scene through its owning longest-match workspace root. Do
   not guess a root for an unsaved scene in a multi-root workspace.
5. Keep `scene_component_palette.zia` presentation-only. Put pure structured
   JSON mutations and exact file snapshots in `scene_component_authoring.zia`;
   route widget intent, external polling, and publication through
   `scene_component_authoring_controller.zia`.
6. Preserve unknown version-1 members on retained JSON objects and reparse every
   structured candidate through the sole schema parser before disk publication.
7. Keep schema-file history independent of scene history and dirty state.
   Existing files require exact-content plus expected-metadata replacement;
   missing files require a no-overwrite atomic create. An external conflict must
   reject save/undo/redo until reload.
8. Preflight every selected item before writing scene data. Preserve same-kind
   values and abort the complete selection on one type conflict.
9. Verify every runtime write and commit the complete scene operation once. A no-op
   must not add history.
10. Update [scene-components.md](scene-components.md), the parser and authoring
    probes, and both scene-editor probes.

Adding nested values, enums, asset references, schema migrations, generated
game adapters, or true runtime component membership is a format/architecture
change, not a palette-only enhancement.

## Adding A Tool Panel

Tool panels are bottom workbench views such as Problems, Output, Search,
References, Debug Console, Variables, Call Stack, Debug, and Terminal.
They can form simultaneous left/bottom/right groups plus one in-window floating
group, with group-local tab order and live per-tool movement. The primary
Explorer/Source Control/Run-and-Debug sidebar is a separate left/right dock
owned by `ui/primary_sidebar_dock.zia`; do not conflate their typed drag
payloads or persistence keys.

When adding or replacing a panel:

1. Decide whether the panel is a row list, text output, form, or specialized
   widget.
2. Add persistent widgets to `ui/app_shell.zia` or a focused UI module.
3. Add a stable panel/tab ID and route live selection by that ID; construction
   indexes are not durable after users reorder tabs.
4. Keep backing state structured. Display rows are presentation.
5. Use `services/locations.zia` for clickable rows.
6. Add copy/clear/filter/wrap/autoscroll behavior only when it makes sense.
7. Decide how the panel participates in active-tool-panel state.
8. Add keyboard access and command-palette entries when appropriate.
9. Add probes for showing, hiding, selecting, filtering, action enablement,
   navigating rows, and narrow side-dock containment.
10. Document the panel in [status.md](status.md) and [workflows.md](workflows.md).

For high-volume panels, avoid ListBox-only designs. The runtime has VirtualList
helpers, but a panel is not truly virtualized until row realization, selection,
copy, filtering, and navigation all work against a virtual model.
If filtering rebuilds concrete rows, retain grouping, action, and location data
in the backing model and provide one metadata setter that also handles a
currently filtered-out row. Never make a ListBox item the only navigation
record.
If Clear affects producer-owned state, route a request back to that owner and
mutate the durable source. Clearing only realized rows creates output that
reappears on the next producer update.

## Adding Runtime API Used By Zanna Studio

Runtime API work is higher risk because it crosses Zia, IL registry, C runtime,
native builds, VM behavior, and documentation.

Follow the repository rules first:

- Check the normative IL guide when changing IL/runtime contracts.
- Use an ADR when the change requires one.
- Keep platform decisions in approved adapter layers.
- Do not add external dependencies.

Implementation checklist:

1. Add or update C runtime implementation and header.
2. Add or update `src/il/runtime/runtime.def`.
3. Add class/member registration when exposing object methods.
4. Add graphics-off stubs if the API is GUI/graphics related.
5. Update runtime completeness checks.
6. Add runtime tests.
7. Add Zanna Studio probes when the IDE depends on the behavior.
8. Update [runtime-integration.md](runtime-integration.md).

For process-like APIs, document buffering, truncation, blocking behavior,
stdout/stderr handling, exit-code behavior, cancellation, and platform support.

## Changing Build Or Run Behavior

Build/run changes are user-visible and can cause data loss if they bypass save
preflight.

Use this split:

- `build/run_config.zia`: target selection and argv construction.
- `build/build_system.zia`: process launch, output, diagnostics, stop.
- `commands/build_commands.zia`: user command flow and shell updates.
- `ui/app_shell.zia`: display state and controls.

Keep these invariants:

- Do not invoke a shell for normal build/run commands.
- Preserve argument vector behavior.
- Respect project manifest overrides.
- Resolve `zanna` predictably.
- Save according to user settings before launching.
- Keep output bounded.
- Prefer structured JSON diagnostics.
- Make Stop work for active jobs.

## Changing Debugger Behavior

Debug transport belongs in `build/debug_session.zia`; command behavior belongs
in `commands/debug_commands.zia`; persistent debug tool-panel display state
belongs in `ui/tool_panel_shell.zia`; Run and Debug activity presentation belongs
in `ui/run_debug_view.zia`, with stable host/activity integration in
`ui/workbench_shell.zia` and `ui/activity_bar.zia`.

When adding debugger features:

1. Extend the debug adapter protocol deliberately.
2. Keep JSON messages structured and version-tolerant where possible.
3. Preserve async command behavior: commands write now, events arrive later.
4. Keep program output separate from debug control output.
5. Update breakpoint persistence if metadata changes.
6. Update gutter markers and session state.
7. Add transport/session coverage in `debug_probe.zia`, docked-panel coverage in
   `debug_tool_surfaces_probe.zia`, and activity-view coverage in
   `run_debug_view_probe.zia`.
8. Update status and workflow docs.

Do not present a debugger feature as complete until it has UI affordances,
protocol support, session-state behavior, and tests.

Keep `RunDebugView` presentation-only. Publish primitive state and copied,
versioned breakpoint records from debug command code, and return action intent
to that boundary. Never parse a displayed breakpoint row or use its filtered
index as store identity. Use exact removal for destructive breakpoint actions;
toggle semantics are only appropriate for an explicit gutter toggle.

## Changing Terminal Behavior

The terminal is split between:

- `terminal/terminal_session.zia`: PTY wrapper.
- `terminal/terminal_controller.zia`: UI and shell behavior.
- `Zanna.System.Pty`: runtime PTY implementation.
- `Zanna.GUI.OutputPane`: terminal-mode rendering/input capture.

When changing terminal behavior:

1. Decide whether the change belongs in runtime PTY, OutputPane, session, or
   controller.
2. Keep platform shell resolution explicit.
3. Preserve clear startup errors through `Pty.LastError()`.
4. Be careful with hidden-panel pumping and PTY buffer drain behavior.
5. Keep output deltas bounded.
6. Add terminal probes for session, open, hidden-start, and rendering behavior.
7. Update terminal scope docs when behavior changes.

Do not document full terminal-emulator support unless alternate screen,
cursor-addressing, terminal modes, resize semantics, and rendering have actually
been implemented and tested.

## Changing Source Control

Current Source Control is intentionally small and async. When modifying it:

1. Keep Git command construction in `scm/scm_git.zia`.
2. Keep view state in `scm/scm_view.zia`.
3. Prefer argument vectors and repository-root working directories over shell
   strings.
4. Start `GitJob` objects from UI code and pump them from the view; do not block
   the frame loop for network or large-diff operations.
5. Treat path parsing carefully. Use porcelain v2 status and add probes for
   spaces, renames, staged/unstaged combinations, and conflicts.
6. Capture and display stderr when possible.
7. Add tests in `scm_probe.zia`.
8. Update docs when behavior becomes more complete.

If adding serious Git workflows, add progress, cancellation, conflict handling,
and credential/error recovery deliberately instead of layering more buttons on
the current one-job view.

## Changing Settings

Settings changes affect startup, persistence, and user expectations.

1. Add default fields in `core/settings.zia`.
2. Load with validation and range checks.
3. Save using read-modify-write so unknown sections survive.
4. Apply widget/editor behavior through `app/settings_applier.zia` or a command.
5. Add UI in Preferences or commands.
6. Preserve legacy values when reasonable.
7. Update [workflows.md](workflows.md).
8. Add probe coverage for persistence when practical.

Settings should be recoverable by editing `settings.ini`. Avoid opaque formats
for simple settings.

## Changing Session Or Recovery

Session restore protects user workflow and unsaved work. Be conservative.

1. Keep session persistence in `core/session.zia`.
2. Keep recovery bounded.
3. Do not store huge buffers in settings.
4. Store enough metadata to restore cursor and scroll safely.
5. Do not restore files that no longer exist unless recovery text exists for an
   untitled draft.
6. Preserve recent projects and recent files ordering.
7. Add probes for new session fields.

Any new document surface should decide how session restore works before it is
presented as supported.

## Changing Workspace Edits Or Refactors

Workspace edits can touch multiple files. The safety bar is higher than for a
single-buffer text edit.

1. Keep pure rewrite logic in `zia/` or runtime language services.
2. Use structured edit records.
3. Preview the edit set for user-visible rename/refactor commands.
4. Validate paths are inside the expected root.
5. Detect overlapping edits.
6. Apply open-document edits in memory and closed-file edits transactionally.
7. Keep undo expectations explicit.
8. Report diagnostics from failed edit application.
9. Add tests for open docs, closed files, conflicts, and missing files.

Avoid string patching based on displayed search rows.

## Documentation Maintenance

The docs are part of the product. Update them in the same change when behavior
changes.

Use this rule:

- README: high-level overview, build/run, doc index.
- `status.md`: feature status, partial support, known gaps.
- `workflows.md`: user-facing workflows and troubleshooting.
- `architecture.md`: ownership and layering.
- `source-map.md`: concrete file/module map.
- `runtime-integration.md`: C runtime and Zia/runtime boundary.
- `testing.md`: probes, commands, manual checks, gaps.
- `maintenance.md`: change procedures and safety rules.

Do not add new plan files. If a feature is planned but not implemented, document
it as a known gap in `status.md` only when it helps users understand current
behavior.

## Verification Checklist By Change Type

For docs-only changes:

- Check links.
- Scan for stale plan references.
- Scan for overclaims about incomplete features.
- Do not run full builds unless docs include generated artifacts or examples
  that need validation.

For IDE source changes:

- Run the focused probe for the changed subsystem.
- Run `zia_smoke_zannastudio_project_compile`.
- Run display probes when UI behavior changed and a display is available.
- Run `./scripts/build_ide.sh` before reporting native app behavior.

For runtime changes:

- Run the relevant runtime unit/e2e tests.
- Run runtime completeness checks.
- Run affected Zanna Studio probes.
- Run platform policy lint for cross-platform-sensitive work.
- Use the full repository build scripts before proposing the change.

## Common Failure Modes

### Feature Exists In UI But Not In State

Example: a button toggles a widget but the document/session/command state does
not know the feature exists. Fix by adding a real model owner and wiring state
through the command registry or controller.

### Display Text Becomes Data

Example: a Problems row displays `path:line` and later navigation parses that
string. Fix by storing a structured location id in `LocationStore`.

### Background Work Ignores Revisions

Example: completion returns for an old buffer and populates the popup after the
user typed more text. Fix by recording path, revision, cursor, and query context
when launching work and rejecting stale results.

### Runtime API Is Too Stringly

Example: returning tab-separated records from a runtime API. Fix by returning
maps, sequences, stable handles, or structured classes where the runtime surface
supports them.

### Large Output Freezes The UI

Example: appending thousands of rows directly to a ListBox every frame. Fix by
bounding retained output, batching updates, and moving toward virtualized row
models for high-volume surfaces.

### Language Feature Runs On The Wrong Language

Example: a Zia semantic command runs on BASIC or scene files. Fix the capability
matrix and command availability checks before adding special cases in handlers.

### Scene Support Is Overclaimed

Example: docs imply that recognizing an extension alone constitutes scene
support. Fix by naming the actual v1 2D/3D surfaces and their asset, component,
advanced tileset/animation/collision/metadata, integrated asset-library,
shaded-material-preview/advanced-map, and gizmo limits.

## Release Notes And User Communication

When summarizing Zanna Studio changes, be specific:

- Say "BASIC completion and diagnostics" rather than "BASIC IntelliSense" if
  navigation/rename are not supported.
- Say "integrated PTY shell" rather than "full terminal emulator".
- Say "Git Source Control view" rather than "complete SCM support".
- Say "direct persisted workspace docking for the primary sidebar,
  simultaneous left/bottom/right tool groups, and one movable/resizable
  in-window floating group." State separately that native secondary-window and
  multi-monitor detachment are not implemented.
- Say "built-in v1 2D/3D scene editors with a visual 2D atlas palette and real
  layer rendering, a runtime-backed shaded/triangle-wireframe 3D viewport,
  stable multi-selection, modifier-aware 2D point and inclusive authored-cell
  marquee selection, transactional group transform/duplicate/delete,
  focus-safe 2D pixel/tile nudging, deterministic primary-axis
  alignment/distribution, typed 2D scene-wide metadata authoring and batch
  object-property set/remove, a runtime-backed absolute-position 2D object
  hierarchy with one-step child creation, an explicit bounded multi-root
  parent chooser, subtree row drops, and hierarchy-preserving clipboard,
  typed per-node 3D gameplay metadata with exact VSCN scalar kinds,
  relative 3D numeric batch transforms, cycle-safe exact preserve-world 3D
  reparenting with a preserve-local opt-out, switchable Local/World 3D
  Move/Rotate/Scale with exact-or-reject group transactions plus conditioned
  XY/XZ/YZ Move-plane and Scale-plane handles, true expandable
  hierarchies with
  before/into/after transactional row drops, stable sibling-block ordering,
  focus-safe
  Duplicate/Delete, scene-aware
  Cut/Copy/Paste/Select All with bounded same-kind cross-document transfer,
  Move/Rotate/Scale axis tools, an integrated asset browser, clone-safe compact
  PBR material editing with truthful mixed-state batch fields, and batch
  embedded common texture maps" and separately list
  their advanced tileset
  animation/collision/metadata, shaded material preview, cubemap/lightmap, richer
  runtime-component, and asset-pipeline gaps.
- Say "debug adapter supports stepping/breakpoints/evaluate/inline watches and
  collection/class expansion" and separately list boxed struct payloads plus
  direct-IL/BASIC layout-sidecar limits.

This avoids repeating the documentation drift that made the old plan files
misleading.
