# Zanna Studio Source Map

This document is a detailed tour of `zannastudio/src/`. It complements
[architecture.md](architecture.md) by naming the concrete files that own the
current behavior. Use it when deciding where a change belongs.

Zanna Studio is organized around long-lived state objects that are created by
`main.zia` and pumped every frame. The source tree is not a plugin system and it
is not yet a highly decoupled workbench framework. It is a practical IDE
application with clear subsystem boundaries and several large coordinator files
that still need pressure taken off them over time.

## Root Entry Point

### `main.zia`

`main.zia` is the application bootstrap and frame loop. It creates the shell,
document manager, project manager, settings manager, session manager, editor
engine, editor controllers, build system, debug session, terminal controller,
SCM view, and command registry.

It also performs the top-level polling work:

- Read editor input and synchronize editor text back to the active document.
- Pump semantic controllers.
- Pump file and workspace watchers.
- Pump command palette, menus, toolbar buttons, context menus, and shortcuts.
- Dispatch commands to `commands/`.
- Update build/run/debug/terminal/SCM state.
- Maintain status bar and panel state.
- Save session state and clean up processes on exit.

`main.zia` is already beyond the preferred file-size budget. New behavior should
usually be added to a focused module and called from `main.zia`, not implemented
inline. If a change requires adding another large block to the frame loop, first
look for a controller extraction point.

Common reasons to touch `main.zia`:

- Wiring a new subsystem object.
- Adding a new command dispatch branch after adding metadata to the command
  catalog.
- Adding a new per-frame controller pump.
- Adding startup/shutdown integration.

Avoid touching `main.zia` for:

- Source parsing rules.
- File mutation logic.
- UI widget construction details.
- Command metadata.
- Runtime binding policy.

## `app/`

The `app/` directory contains orchestration helpers that sit near the frame loop
but do not own the core document model or persistent widget tree.

### `app/build_info.zia`

Reads build metadata written beside the generated IDE binary. The Help/About
surface uses this so users can identify the build they are running. This file is
the right place for build-info parsing, not for general version policy.

### `app/context_menus.zia`

Builds and updates editor/explorer context menu state. It bridges the current
document, selection, language service, debug session, and explorer selection
into enabled/disabled menu entries.

Touch this file when adding right-click behavior. Do not put command
implementation here; dispatch should still route to `commands/` or a controller.

### `app/dispatch_helpers.zia`

Contains small helpers for command triggering and disabled-state checks shared
by the main dispatch loop. This keeps repetitive menu/toolbar/palette checks out
of individual command modules.

### `app/command_palette_controller.zia`

State machine for command-palette modes. It restores command mode, opens the
command palette, and tracks when the palette temporarily contains Quick Open
file rows.

### `app/explorer_action_runner.zia`

Coordinates explorer actions that need multiple subsystems: document manager,
tabs, project manager, session manager, and AppShell. It is a deliberate bridge
between UI action requests and safe document/project mutations.

This is the right place to keep explorer workflows visible and auditable. File
creation, rename, duplicate, delete, and "open and record" behavior should stay
centralized here instead of being spread across the frame loop.

### `app/file_watch_controller.zia`

Owns active-file watcher state and timestamp polling for inactive open tabs.
`main.zia` calls its `Pump` method instead of carrying watcher variables inline.

### `app/language_tool_frame.zia`

Coordinates one frame of language-service capability, completion, diagnostics,
hover, signature, inlay, symbol, index, and breakpoint work. It publishes a
language status only while the active document actually uses the text editor;
visual scenes publish their own status later in the frame, so the two surfaces
cannot invalidate the retained status bar by overwriting one another every
idle frame.

### `app/settings_applier.zia`

Applies persisted settings to the shell and editor. Settings parsing belongs in
`core/settings.zia`; settings application belongs here when it requires widgets
or editor state.

### `app/workspace_watcher.zia`

Tracks workspace-level change notifications. It complements document-level file
watching in the main loop and should remain focused on detecting project-tree
changes, not mutating documents directly.

## `build/`

The `build/` directory owns process-backed build/run/debug state and diagnostic
models.

### `build/build_system.zia`

Owns build and run child processes. It resolves the `zanna` executable, starts
`Zanna.System.Process` jobs with argv sequences, streams stdout/stderr, bounds
retained output, parses structured JSON diagnostics, and falls back to legacy
diagnostic text parsing.

Important behavior:

- The IDE never runs build/run commands through a shell.
- `ZANNA_BINARY`, colocated binaries, the build tree, `ZANNA_BUILD_DIR`, and
  `PATH` are searched for `zanna`.
- Output is bounded to avoid uncontrolled memory growth.
- Diagnostics are kept as structured `Diagnostic` objects for Problems/output
  navigation.

Touch this file for process lifecycle, compiler resolution, output retention, or
diagnostic parsing. Do not put UI row formatting here; that belongs in command
or shell code.

### `build/run_config.zia`

Builds a `RunConfig` from the active document and project manager. It parses
`zanna.project` build/run overrides into argv tokens and expands `{target}`.

This file is the authority for:

- Current-file versus project-entry build/run.
- Custom build/run program and args.
- Working-directory selection.
- Problem matcher selection.

### `build/diagnostic.zia`

Small diagnostic data model shared by build, Problems, and output navigation.
Use this instead of display strings when a feature needs severity, path, line,
column, code, and message.

### `build/breakpoints.zia`

Stores persisted breakpoint records: path, line, condition, and log message.
Also owns breakpoint gutter marker constants and exact source-location removal,
which never toggles a missing record back on. Debug command code and the debug
session both depend on this store.

### `build/debug_session.zia`

Controls the external VM debug adapter process. It launches:

```text
zanna run --debug-adapter <file>
```

The IDE writes newline JSON commands to stdin and reads sentinel-prefixed JSON
events from stderr. Program stdout is kept as program output. The session tracks
stopped/running/terminated state, current stop path and line, locals, call
stack, evaluation results, persistent watches, program output, and debug console
text. Clear removes only session-owned stdout/stderr; debugger control status
remains available for truthful console presentation. Restart requests are
queued and launched only after the old adapter process terminates.

Touch this file for debug protocol transport and session state. UI presentation
belongs in `commands/debug_commands.zia`, `ui/app_shell.zia`, or debug-specific
overlays.

## `commands/`

The `commands/` directory is where user-triggered behavior lives.

### `commands/command_catalog.zia`

Declarative command metadata: id, label, description, shortcut, capability, and
palette visibility. Add command ids here first. Command ids are stable API
inside the app; changing them can break shortcuts, probes, and dispatch.

### `commands/command_registry.zia`

Builds runtime command entries from the catalog, installs shortcuts, populates
the command palette, detects duplicate shortcuts, and computes language-service
availability. It does not implement feature behavior.

### `commands/file_commands.zia`

File and tab workflows: new file, open file, open folder, add folder, save,
save as, save all, close, reload, recent files, reopen closed files, and
project/file selection flows.

Use this file for user-facing file commands. Keep low-level document save logic
in `core/document_manager.zia`.

### `commands/edit_commands.zia`

Editor and semantic editing commands: navigation, references, call hierarchy,
rename, diagnostics navigation, code actions, multi-cursor, comments, duplicate
line, and move line.

Transform-heavy edit flows delegate to `source_transform_commands.zia` so the
basic editor command surface does not keep growing.

### `commands/basic_workspace_commands.zia`

UI-side owner for project-wide BASIC definition, references, incoming/outgoing
calls, and rename. It snapshots roots and open BASIC buffers, schedules the
owned background job, rejects stale editor/workspace anchors, navigates or
opens complete rename results in the shared non-modal review surface, and
publishes large result sets in bounded per-frame batches. Keep scanner/file work
in the worker module below.

### `commands/zia_workspace_commands.zia`

UI-side owner for Zia definition, references, incoming/outgoing calls, and
rename. It waits for cooperative project indexing without blocking the event
loop, submits immutable editor/index anchors to the owned worker, rejects stale
or incomplete results, navigates on the UI thread, opens complete rename results
in shared review UI, and publishes at most 100 result items per frame. Keep project parsing in
`editor/zia_workspace_query_job.zia` and GUI/document mutation here.

### `commands/workspace_edit_preview_commands.zia`

Consumes Apply/Cancel actions from the rename review surface. Apply revalidates
the complete edit set immediately before mutation, refreshes the visible editor
and index, and reopens stale failures inline; Cancel restores editor focus
without changing the workspace.

### `commands/source_transform_commands.zia`

User-facing orchestration for source transforms: organize binds, inline local,
extract local, extract function, trim trailing whitespace, format document, and
format selection. It collects document/editor context, calls pure transforms in
`zia/`, replaces the buffer, marks documents modified, and reports status.

### `commands/diagnostic_edit_commands.zia`

Diagnostic-driven source edits: suppress warning, apply diagnostic fix-it, and
create missing runtime/project binds. `DiagnosticActionController` starts the
compiler query without editing, validates the captured path/revision/caret on
later frames, and delegates project-wide candidate discovery to the bounded
worker in `editor/project_bind_query_job.zia`. Disk candidates are mtime-checked
and unsaved candidates are content-checked immediately before insertion.
`edit_commands.zia` keeps synchronous compatibility wrappers for probes and
older callers; the product dispatcher uses the controller.

### `commands/editor_document_edit.zia`

Low-level editor replacement primitive shared by command modules that replace a
whole buffer. Use this instead of rebuilding the same full-document selection
range in multiple commands.

### `commands/search_commands.zia`

Project search, folder search, workspace symbols, and related result
navigation. This module coordinates runtime-paged file discovery, search
options, result rows, and location ids.

Search result display strings should never be the only source of navigation
truth. Use `services/locations.zia` for stable location ids.

The interactive path is the docked Search panel owned by `ui/app_shell.zia` and
started from `SearchController`. Search commands collect text and options there;
blocking message-box prompts are intentionally not part of the Studio workflow.
Direct and panel paths converge on `services/search_request.zia`.

Text search is bounded by both file count and aggregate source bytes per frame.
The controller records the first/last `LocationStore` ids published by each run;
Replace All must use that interval rather than every historical `KIND_SEARCH`
record in the shared location store. Search and Replace completion is durable
panel/status feedback and must not emit a second transient notification.
Immediate command failures route through `ui/notification_policy.zia`.

### `commands/replace_commands.zia`

Pure line replacement helpers plus `ProjectReplaceController`, the cooperative
owner for Search-panel Replace All. It groups the current search generation in
one linear pass, performs no file I/O in the button callback, and applies no
more than two file plans per later frame. Closed-file reads are bounded and
revalidated before atomic writes; open buffers are modified in memory with an
inactive-tab workspace undo snapshot. Dense lines that exceed the match ceiling
cancel the entire file rather than writing a partial transform.

`commands/quick_open_commands.zia` owns Quick Open palette rows, command id
encoding, deterministic file scoring, and opening the selected file. Keep Quick
Open behavior there so text search remains focused on matching and navigation.

### `commands/quick_open_commands.zia`

Quick Open workflow for project files. It warms the project file cache, scores
relative paths against the query, populates command-palette rows, translates
palette ids back into paths, and opens selected files through the normal file
command path.

This module intentionally shares project file enumeration with search through
`core/project_manager.zia`, but it should not grow text-search options or result
formatting. Search and Quick Open are separate user workflows even though they
both consume workspace paths.

### `commands/build_commands.zia`

User-facing build/run command behavior. It performs save preflight, starts
build/run configs, updates running jobs, populates output rows, and publishes
diagnostics to the shell.

Process mechanics belong in `build/build_system.zia`; command workflow and
status messages belong here. Completed results remain in Output, Problems, and
the status bar without a toast, even when the job fails; only an immediate start
failure routes through `ui/notification_policy.zia`.

### `services/diff_engine.zia`

Pure line alignment and summary counting for side-by-side diffs. The Myers
search has explicit combined-line, edit-depth, and trace-cell ceilings;
`BuildRowsCapped` counts the complete aligned result while allocating only a
caller-sized row prefix. Keep GUI widgets and async ownership out of this
module.

### `services/diff_job.zia`

Latest-wins owned worker for bounded diff computation. Requests contain only
immutable title/text values, reject sources above 4 MB each or 20,000 combined
lines, retain at most 4,000 aligned rows, and support a bounded shutdown drain.

### `ui/diff_view.zia`

Reusable side-by-side overlay shared by Compare with Saved and Source Control.
It queues `DiffJob` in `Open`, shows pending state immediately, and turns worker
rows into at most 100 colored list items per frame. `Shutdown` is mandatory
before application teardown so no promise outlives the runtime.

### `commands/debug_commands.zia`

User-facing debug commands: start, continue, step, pause, stop, restart, run to
cursor, breakpoint toggles, breakpoint metadata, evaluation, and UI publishing
from `DebugSession` state. It also pumps the inline Variables-panel watch
toolbar and maps structured watch-row identities to Add/Remove/Refresh/Clear
session operations. Call Stack publication attaches the original adapter frame
index to each row, and Debug Console publication sends program output and
session status to separate shell inputs. The same module publishes primitive
state and versioned breakpoint snapshots to `RunDebugView`, then resolves its
action intent through existing commands and exact source records.

### `commands/view_commands.zia`

Workbench display commands: sidebar visibility/left-right placement, status
bar/theme/fullscreen/minimap, font zoom, UI zoom, settings, keybindings,
tool-row copy, output copy/clear, output filter/wrap/autoscroll, recent files,
and About.

## `core/`

The `core/` directory owns persistent IDE domain state.

### `core/document.zia`

The per-document record. It stores file path, display name, language, document
kind, text content, modified/new/read-only flags, disk metadata, cursor/scroll
state, and external disk-state/conflict fields.

Do not add editor-widget references here. `Document` is model state, not UI.

### `core/document_manager.zia`

Owns the open document list and active document index. Responsibilities:

- Create untitled documents.
- Open files with deduplication.
- Detect language and document kind.
- Load text or read-only preview content.
- Save active documents.
- Save As.
- Save All.
- Close documents and maintain active index.
- Track recently closed paths.
- Classify external disk changes, including changed, deleted, renamed, and
  missing files.
- Use `Zanna.Workspace.Edit` for safe existing-file replacement.

If a feature needs to mutate open documents, prefer going through this module or
a service that clearly documents how document state is updated.

### `core/project.zia`

Parses `zanna.project` manifest data: project name, language, entry, build/run
programs, build/run args, working directory, problem matcher, and ignore rules.

Keep manifest parsing here. Runtime execution of those values belongs in
`build/run_config.zia`.

### `core/project_manager.zia`

Owns the primary project, additional workspace roots, explorer tree, tree-node
path data, Quick Open cache, incremental directory loading, ignore checks, and
project file cache refresh.

It uses `Zanna.Workspace.FileIndex` for exclusion rules and paged project-file
cache refresh. Immediate Explorer children use `Zanna.IO.Dir.Page`; collection,
natural sorting, and node publication remain separate cooperative stages. Do not
duplicate ignore logic or recursive enumeration elsewhere unless the runtime
surface cannot answer the question.

### `core/project_file_ops.zia`

Small filesystem policy helpers for project/explorer flows: safe child names,
project-root containment checks, `.zia` file classification, and reversible
delete trash destinations.

### `core/settings.zia`

Persistent settings manager. It handles defaults, platform-native settings
paths, legacy settings-path compatibility, validation on load, and read-modify-
write persistence so unrelated INI sections survive. Shared state reads are
size-bounded and successful writes use the atomic persistence helper in
`services/safe_io.zia`.

Settings are parsed here but applied elsewhere. Keep widget calls out of this
file except for theme routing that is explicitly part of settings behavior.

### `core/session.zia`

Session and recent history persistence. It stores project root, open files,
active file, active index, cursor/scroll, bounded recovery text for modified
text buffers, recent projects, and recent files. `BeginRestore` / `PumpRestore`
provide caller-budgeted root and tab restoration; main polls and repaints the
startup card between entries instead of opening an entire large session on one
unresponsive UI frame. `Restore` remains the blocking compatibility wrapper.
Save mirrors the restore record limits, caps aggregate embedded recovery, and
prioritizes the active document when a large tab set must be truncated.

Recovery text is intentionally capped and base64 encoded. This module should
remain conservative because it runs during startup/shutdown and protects user
work.

### `services/safe_io.zia`

Non-trapping filesystem boundary helpers. Shared Studio INI state has a global
size ceiling and is committed through a same-directory staging file plus atomic
replace, so settings, sessions, and breakpoints share one corruption-resistant
policy.

### `core/recovery_store.zia`

Continuous per-document crash swaps and the unclean-session lock. Interactive
requests contain immutable path/text snapshots and run through one owned async
worker plus one coalesced latest request. Large writes use ignored staging names;
only cancellation-serialized atomic renames publish canonical `.path`/`.swp`
pairs. Save, reload, close, rename, and delete reconciliation cancels older
commits, prunes stale pairs, and queues remaining dirty buffers.

Startup discovery uses `BeginRecoveryScan` / `PumpRecoveryScan` to page the flat
recovery directory, remove abandoned staging files, and retain recovered text
under fixed record and aggregate-byte caps. Main polls and paints between scan
entries and between recovered tabs.

## `editor/`

The `editor/` directory adapts `Zanna.GUI.CodeEditor` and language services into
IDE behavior.

### `editor/editor_engine.zia`

The adapter between `Document` and `CodeEditor`. It loads documents into the
editor, saves editor state back to documents, sets language/display options,
manages current file path/language, and caches full-text snapshots by editor
revision.

Use `GetTextSnapshot()` instead of reading full editor text repeatedly.

### `editor/editor_tabs.zia`

Owns tab widget behavior and tab metadata such as modified markers, tooltips,
active tab, and close signals. Document content belongs in `DocumentManager`;
visual tab state belongs here.

### `editor/language_service.zia`

Capability matrix for Zia, BASIC, text, and scene documents. The command
registry and semantic controllers use it to decide what should run.

This file is the first place to update when adding a language capability.

### `editor/scheduler.zia`

Priority and debounce model for editor background work. It names semantic job
kinds, default delays, and scheduling state. Use it to keep typing responsive
when adding more background work.

### `editor/basic_query_job.zia`

Serial latest-wins worker used by compiler-backed BASIC completion,
diagnostics, hover, symbols/folding, and signature queries. Requests retain an
immutable active-buffer snapshot and cancelled generations never publish.

### `editor/basic_workspace_query_job.zia`

Bounded project-wide BASIC semantic worker. It pages every captured workspace
root, lets captured open-buffer text override disk, limits files/per-file and
aggregate source/declarations/results, checks cancellation between units, and
returns plain navigation or rename records. It never borrows GUI,
`ProjectManager`, `DocumentManager`, or editor objects from its worker thread.

### `editor/project_bind_query_job.zia`

Serial latest-wins worker for Create Missing Bind. It pages every captured
workspace root, lets captured open Zia buffers shadow disk, and requires a
complete scan with exactly one defining file before publishing. File-count,
per-file size, line-count, and aggregate-source ceilings turn uncertain
uniqueness into an explicit refusal; the request retains the candidate mtime or
open-source snapshot for final UI-thread stale validation.

### `editor/zia_workspace_query_job.zia`

Serial latest-wins worker for Zia project-index navigation and rename. Requests
retain the index handle, active-source snapshot, caret, document revision,
workspace signature, and index content generation. Definition, references,
incoming calls, and rename use the runtime index snapshot; outgoing calls scan
a bounded token set and resolve each target on the worker. Results cap at 2,000
references, superseded generations cannot publish, and shutdown drains the
owned future before controller teardown.

### `editor/completion.zia`

Completion controller. It handles automatic and explicit triggers, query
snapshots, async result tracking, popup population, filtering, acceptance,
commit characters, snippet cursor placement, and workspace-symbol completion
cache updates. Workspace-symbol discovery consumes the project manager's
runtime-paged file cache incrementally; do not reset the symbol cache merely
because the known file count grew.

The file is large. Pure filtering helpers have already been split into
`completion_filter.zia`, typed rows into `completion_items.zia`, and workspace
source/detail policy into `completion_workspace_source.zia`. New reusable
completion-display logic should move into helpers instead of growing the
controller.

### `editor/completion_items.zia`

Typed completion candidate model and constructors. Use this when adding fields
that must travel together with a completion row, such as insert text, replacement
range, cursor offset, or commit characters.

### `editor/completion_filter.zia`

Pure completion filtering and display helpers. This module is small and easy to
probe. Prefer adding deterministic string/ranking helpers here rather than
inside `completion.zia`.

### `editor/completion_workspace_source.zia`

Leaf helpers for completion's workspace-symbol cache: project-open checks,
cache-reset decisions, open-document-first source loading, file-size caps, and
workspace detail text.

### `editor/diagnostics.zia`

Live diagnostics controller. It schedules structured toolchain checks, rejects
stale results, updates Problems, tracks active-file inline highlights, and
publishes diagnostic actions.

### `editor/quick_fixes.zia`

Helpers for interpreting diagnostic records into quick-fix actions, missing
runtime bind names, undefined identifier extraction, and known runtime bind
lines. This is the right place for deterministic diagnostic-to-action mapping.

### `editor/hover.zia`

Dwell-based hover controller. It schedules hover lookups, rejects stale results,
formats hover display text, and positions tooltips.

### `editor/signature.zia`

Signature-help controller. It detects call-site context, schedules structured
signature lookups, formats active overloads, and implements overload navigation.

### `editor/symbols.zia`

Document symbol outline and semantic fold-region support. It updates the
outline panel and derives fold regions for Zia symbols when appropriate.

### `editor/project_index.zia`

Lazy Zia project index integration. It owns the `Zanna.Zia.ProjectIndex` handle,
syncs open documents, indexes workspace files cooperatively, enforces size and
per-frame byte limits, deduplicates overlapping roots, and exposes an explicit
complete/incomplete state. Workspace queries are refused after unreadable or
oversized input, more than 20,000 Zia files, more than 64 MB of source, or a
truncated runtime file enumeration. The runtime handle supports concurrent
updates and immutable query snapshots.

Changes here can affect responsiveness and semantic correctness. Keep file-size
caps, stale-data handling, and dirty-open-document behavior explicit.

### `editor/semantic_tokens.zia`

Semantic highlighting controller. It maps runtime token kinds to CodeEditor
highlight categories and applies semantic spans after debounce.

### `editor/inlay_hints.zia`

Conservative inlay hints. The current implementation uses IDE-side heuristics
for parameter names and inferred local types. It deliberately caps scanned lines
and hint count.

### `editor/zia_query.zia`

Helpers for wrapping source snippets before sending them to Zia runtime queries.
Use this when a semantic feature needs a parseable module wrapper around partial
editor text.

### `editor/perf_monitor.zia`

Optional perf logging for dogfood and probes. It records frame/controller/editor
counters when enabled by environment configuration.

### `editor/workbench_state.zia`

Small active-workbench-state model. Use it when command enablement or context
needs a structured snapshot rather than reading widgets ad hoc.

## `services/`

Services are leaf modules with shared rules and no ownership of AppShell.

### `services/file_utils.zia`

Extension-based language and document-kind detection plus open/save dialog
filters. This is the source of truth for `.zia`, `.bas`, `.vb`, `.scene`,
`.level`, text, IL, and unsupported binary detection.

### `services/locations.zia`

Structured location id storage for search, diagnostics, build output,
definitions, references, scene locations, and symbols. This prevents UI rows
from becoming the navigation contract.

When adding a clickable result type, add a structured location kind here rather
than parsing display text.

### `services/text_utils.zia`

Small deterministic text helpers: trailing CR stripping, leading whitespace,
right-trim, string-list joining, and string-list membership.

### `services/search_matcher.zia`

Pure project-search line matching. It owns literal matching, whole-word checks,
and the compact editor-search regex subset used by Search in Project.

### `services/search_paths.zia`

Search path normalization and include/exclude/ignore matching. Use it when a
feature needs to decide whether a project path belongs in search results.

### `services/workspace_file_index.zia`

Shared `Zanna.Workspace.FileIndex` boundary. Use it for workspace containment,
default excludes, ignore checks, and file enumeration instead of duplicating
recursive directory or map-unpacking rules in command/core modules.

### `services/workspace_edits.zia`

Validation, conflict detection, transactional application, legacy text-summary
formatting, and diagnostics formatting for workspace edit sequences. Rename and
refactor commands depend on this to avoid partial or conflicting application.
Scanner-backed edits may attach the source text and disk mtime observed during
the background scan; validation rejects changed buffers/files before applying
any edit.

## `basic/`

### `basic/semantic_scan.zia`

Lightweight IDE-only BASIC declaration/reference/call/signature scanner. The
capped declaration, reference, and call-token entry points are the worker-safe
boundary for large projects. This scanner is deliberately not presented as the
compiler's full semantic model.

## `terminal/`

### `terminal/terminal_session.zia`

Thin wrapper around `Zanna.System.Pty.PtySession`. It starts a child, drains
bounded output through `ReadResult()`, writes input, resizes, tracks exit,
stores the latest startup error, reports runtime/frame truncation in-band, and
destroys the PTY on stop.

### `terminal/terminal_controller.zia`

UI-side integrated terminal controller. It wires the shell-owned OutputPane,
sets terminal mode, lazily starts a shell when visible, resolves the platform
shell, derives rows/columns from OutputPane cell metrics, appends output,
forwards captured input, drains already-running hidden sessions into a bounded
replay buffer, handles Stop and Restart, and updates the working directory for
future sessions.

Do not treat this as a full terminal emulator. It is a PTY-backed shell surface
for line-oriented interactive work, not a full-screen TUI host.

## `scm/`

### `scm/scm_git.zia`

Async Git command layer over `Zanna.System.Process`. It resolves `git`, starts
argv-sequence jobs in the repository root, captures stdout/stderr/exit code,
parses porcelain v2 status (including the real three-object-ID unmerged-row
shape), stages/unstages files, commits, diffs, pushes, pulls, lists branches,
and switches branches.

Known constraints are documented in [status.md](status.md) and
[runtime-integration.md](runtime-integration.md). Blocking wrapper methods are
kept for probes and older call sites, but UI code should start and pump
`GitJob` objects.

### `scm/scm_view.zia`

Source Control view model and UI action state. It owns the current Git snapshot,
selected path, diff text, commit message, active job, and refresh/operation
behavior needed by AppShell. Wrapped action rows reflow with the sidebar;
selection/index/conflict/message state drives truthful enablement; focused Enter
submits commits and credential responses; and unmerged rows expose the safe
edit-then-Stage path without destructive resolution shortcuts.

### `scm/scm_gutter_controller.zia`

Owns the editor's asynchronous Git diff job and change-bar projection. Requests
coalesce across edits/tab switches, resolve the repository from the document's
owning workspace root, reject stale completions, time out slow children, and
bound marker publication.

## `ui/`

The `ui/` directory owns persistent widgets, overlays, and display helpers.

### `ui/notification_policy.zia`

Stateless eligibility rules for transient warning/error popups. Background
results that already have a durable owning surface stay quiet, immediate action
failures are eligible, and external-state warnings require active-resource
ownership. Build and Search controllers are linted against bypassing this
boundary with direct warning/error toast calls.

### `ui/app_shell.zia`

Constructs the workbench: window, menu bar, toolbar, sidebar, explorer,
activity bar, editor container, tab bar, breadcrumb, find bar, bottom panels,
debug panels, terminal, Source Control and Run/Debug hosts, preferences,
overlays, status bar, and helper methods for showing/hiding/updating those
widgets. It arbitrates the single focus-taking transient-surface slot used by
Settings, About, explorer actions, breakpoint input, command input, rename
preview, and diff views. Frame rendering also reconciles the requested minimap
with the measured editor-lane width so compact windows reclaim its space without
losing the persisted preference.

This file is also too large. Add new UI state here only when it truly belongs to
the persistent shell. For complex new surfaces, prefer a dedicated controller or
view module that AppShell wires.

### `ui/menu_chrome.zia`

Owns persistent menu, toolbar, and context-menu widgets plus their command
availability state. Text-editor, visual-surface edit, undo/redo, and execution
states are transition-cached: callers may publish current truth every frame,
but identical inputs must not mutate retained widgets or trigger repaint.

### `ui/status_shell.zia`

Owns status-bar presentation and visibility policy. It caches project,
language, index, job, task, diagnostics, and left-status values before touching
widgets; surface orchestration must still publish only the active surface's
identity rather than toggling through an invisible intermediate value.

### `ui/workbench_shell.zia`

Builds the stable nested splitter topology for the activity rail, primary
sidebar, editor, Preferences lane, visual-scene hosts, and left/bottom/right
tool-strip hosts. It delegates direct primary-sidebar movement and persistence
edges to `PrimarySidebarDock` and tool content/docking to `ToolPanelShell`.

### `ui/primary_sidebar_dock.zia`

Owns the primary sidebar's drag header, explicit arrows, typed stationary
left/right targets, mirrored logical-width math, collapse recovery, focus-safe
live-tree reparenting, and one-shot position/size persistence edges. It moves
Explorer, Source Control, and Run and Debug plus their activity rail without
reparenting the editor or tool-panel trees.

### `ui/workspace_dock.zia`

Pure portable location IDs, typed drag payloads, bounded edge-target geometry,
narrow floating-window snap geometry, and concise feedback text shared by
primary-sidebar and tool-strip docking.

### `ui/activity_bar.zia`

Activity bar state and persisted view ids for Explorer, Source Control, and Run
and Debug sidebar surfaces.

### `ui/debug_breakpoint_overlay.zia`

Overlay for breakpoint metadata entry: conditions and logpoints. It owns the
temporary input UI and action result consumed by debug command code.

### `ui/run_debug_view.zia`

State-aware, vertically scrollable Run and Debug activity view. It consumes
primitive session snapshots, retains copied breakpoint path/line/store identity,
filters without parsing row text, realizes at most 1,000 matching rows, and
emits action constants only. Process ownership, document navigation, breakpoint
persistence, and tool-panel selection stay in command code.

### `ui/run_debug_support.zia`

Small presentation-support leaf for stable Run/Debug action ids, copied
breakpoint records, row filtering/formatting, count text, and responsive
button/scroll sizing. It has no build-system or debug-session dependency.

### `ui/scene_editor_2d.zia`

Document-backed 2D layer/tile/object authoring surface. It owns responsive
canvas coordination, including stable progressive Scene/Tools/View menus at
every width so wider lanes grow the canvas instead of restoring secondary
controls. Its per-document Scene-inspector topic keeps Setup & Import,
Properties, Camera & Lighting, Layers, Tile Behavior, and History complete but
mutually disclosed, so unrelated scene-wide forms do not compete for the
visible inspector. It owns layer
selection and asset assignment, gap-free
captured paint/erase strokes with exact Escape rollback, inclusive rectangle
painting, integer-walk lines, ellipse outlines, runtime-backed four-connected
fill, and active-layer atlas-true hover/captured-shape/stamp previews that never
enter canonical state. Palette/tool changes invalidate those disposable pixels
immediately, stamps clear consistently on every tool path, and all nine tool
modes follow their owning scene tab. A bounded cached autotile lookup mirrors
the runtime's first-64-rule, N/E/S/W base-layer resolution. It substitutes only
rendered tile IDs, composes animation afterward, and previews the affected
neighbors for prospective single-cell Paint/Erase input while canonical base
cells and history remain untouched. The controller also owns
active-layer tile picking, modifier-aware point selection and inclusive
authored-cell marquees with pointer capture/Escape cancelation, a bounded retained object
hierarchy with preserved expansion, stable multi-object selection,
non-destructive bounded hierarchy Find, transactional before/into/after
subtree drops, one-step root/child creation, bounded cycle-safe explicit parent
selection for subtree groups, hierarchy-preserving duplicate/paste, group
drag/delete, focus-scoped pixel/tile nudging, primary-axis alignment,
deterministic distribution, typed scene-wide and object properties, atomic
multi-object component application, selection-free schema-v13 gameplay-object
creation with typed defaults staged before one commit, Tiled import,
external-image reload, and
process-local canonical history with an oldest-first labeled Scene-tab
timeline that jumps through normal undo and retains redo. Timeline labels
follow the owning document across tab switches. The layer navigator presents
authored visibility/opacity plus tab-owned Lock/Solo state, toggles visibility
through one canonical transaction, remaps workspace state across live layer
structure edits, and keeps wrapped boundary-aware actions truthful. Its
application-directed ListBox reorder requests, Alt+Arrow requests, and
explicit buttons converge on one canonical transaction (ADR 0205). Canonical
reloads clear numeric layer workspace identities. Canvas selection state and
feedback never enter canonical scene history. The surrounding ruler surfaces
derive exact ticks from window origin, zoom, and centered padding; their
bounded vertical/horizontal guides are tab-local presentation layered over the
canvas. Captured ruler gestures provide live create, move, drag-out removal,
collision refusal, and Escape rollback without leaking transient coordinates
into document state. Per-document Game View masks all editor overlays and
canvas editing while preserving selection, tools, preferences, bytes, and
history; legacy profiles retain free pan/zoom, while schema-v12 output profiles
fit the authored player start into a centered project-aspect frame, lock
navigation, and restore the exact editor view on exit. Game View also makes
animation playback effective without rewriting the independent Preview toggle.
Project-owned 2D background profiles resolve typed scene variants and composite
bounded fixed-screen imagery behind the editable layers without executing
project code. Real atlas decoding/rendering, project-asset discovery, selection
normalization, precision-layout rules, hierarchy matching, and palette
presentation live in smaller leaf modules.

### `ui/scene_panels_2d.zia`

Constructs the complete retained 2D scene widget tree, including stable
Scene/Tools/View menu-item handles and the compact Scene-inspector topic
selector. `SceneEditor2D` owns their responsive labels, checked/enabled state,
topic visibility, and dispatch through existing commands.

### `ui/scene_property_inspector_2d.zia`

Presentation-only scene-wide metadata inspector. It realizes at most 512
deterministically ordered typed properties, preserves exact scalar kinds, and
returns selection and Set/Remove intent to `SceneEditor2D`. The controller owns
validation, canonical mutation, history, dirty state, and per-document
selection persistence.

### `ui/scene_layout_2d.zia`

Stateless helper for 2D alignment/distribution operation identity, selection
requirements, stable coordinate ordering, and deterministically rounded
distribution targets. It has no GUI, document, or Scene2D ownership.

### `ui/scene_object_draw_stack_2d.zia`

Stateless bounded compositor ordering for 2D object previews. It resolves
exact integer `editor.afterLayer`/`editor.drawOrder` overrides over validated
project rules, maps them to tile-layer boundaries, and builds one stable
bottom-to-top identity stack with fixed buckets. The 2D editor shares that
stack between sprite rendering, marquee ordering, and topmost canvas picking;
the helper owns no GUI, pixels, document mutation, or history.

### `ui/scene_clipboard.zia`

Shared document-independent clipboard envelope for both visual editors. It
encodes a versioned scene kind, primary identity, bounded selection identities,
and byte-exact canonical source content into the platform text clipboard. Its
decoder rejects malformed, duplicate, oversized, or unsupported identities
before either controller can reconstruct selected 2D objects or 3D subtrees.

### `ui/scene_component_schema.zia`

Document-independent, fail-closed loader for project-root
`scene-components.json`. It validates versions 1 through 16, target,
identifiers, limits, scalar kinds, exact defaults, asset/object/node preview
conventions, 3D gameplay-view profiles, scene environments and portable
post-processing, 2D scene backgrounds, object draw-stack rules, and paired
project output dimensions plus target-compatible component creation recipes
and independently matched metadata-transformed 3D environment layers plus
runtime-water construction inputs and typed metadata-selected PBR material
overlays into value-only records. It does not own widgets, scenes, project
state, or document mutation.

### `ui/scene_component_authoring.zia`

Pure structured component/field mutations plus one bounded, conflict-aware
project-file session. Known-member changes operate on the raw JSON tree,
preserve unknown version-1 members, reparse every candidate through
`scene_component_schema`, and use atomic create/rooted replace operations.
Twenty exact file-presence/text snapshots provide schema-only undo/redo.

### `ui/scene_component_authoring_controller.zia`

Shared coordinator between either scene editor and the schema form. It consumes
one authoring intent per pump, owns external-change polling and reload, publishes
the complete cross-target schema, and keeps project-file history separate from
scene content, revision, dirty state, and history.

### `ui/scene_component_palette.zia`

Shared presentation-only component picker and structured schema form for both
scene inspectors. Its Add Missing picker filters definitions by
2D-object/3D-node target, while its independent authoring dropdown exposes the
complete schema. It renders bounded fields, coverage, typed drafts, ordering,
and file-history controls, then returns intent without mutating disk or scenes.
`SceneEditor2D` and `SceneEditor3D` own root resolution and scene transactions;
`scene_component_authoring_controller` owns project-file transactions.

### `ui/scene_selection.zia`

Shared document-independent selection boundary for retained scene TreeViews
and other data-bearing row controls. It converts byte-exact
`GetSelectedData()` values into bounded, deduplicated model indices, preserves
an explicit primary row, and never parses display labels. TreeView behavior is
recorded by ADR 0163; ADR 0156 covers the ListBox counterpart.

### `ui/scene_hierarchy_search.zia`

Pure shared case-insensitive hierarchy Find semantics. It trims and normalizes
queries, matches a primary and optional secondary row identity, and advances a
previous/next cursor with deterministic wrapping. Scene controllers own bounded
match lists, retained TreeView selection/reveal, hierarchy-master focus, and all scene
state; this helper cannot filter widgets or mutate documents.

### `ui/scene_history.zia`

Pure bounded-history trimming shared by both scene editors' undo/redo snapshot
stacks (ADR 0190). Entry count and aggregate bytes are both bounded, oldest
snapshots drop first, and the newest snapshot is always retained. Callers own
the lists; the helper owns no widgets, documents, or scene state.

### `ui/scene_asset_browser.zia`

Shared presentation-only project asset chooser for both scene editors. It
cooperatively warms `ProjectManager`'s multi-root file cache, applies exact
extension and case-insensitive text filters, realizes at most 512 list rows and
48 responsive thumbnail cards, retains one absolute selection identity across
both views, schedules only visible thumbnails at one per frame, and reports
selection/typed-drag intent without mutating a document. It also owns the
conservative saved-scene-relative path spelling used by 2D layer assets.

### `ui/scene_asset_thumbnails.zia`

Read-only bounded preview renderer for the shared browser. Common raster
formats decode into contained neutral squares. Supported 3D scene/model formats
load through `SceneAsset`, frame complete subtree bounds, and render under a
deterministic neutral camera/light rig through windowless Canvas3D. Source
bytes, decoded pixels, output edges, and scene-node counts reject before the
browser uploads a widget-owned copy.

### `ui/scene_tileset_2d.zia`

Document-independent 2D tileset preview library. It resolves authored paths,
enforces source/decoded/cache budgets, decodes PNG/JPEG/BMP/GIF images, maps
tile IDs to atlas frames, polls one external reference's metadata per interval,
and renders the bounded palette and canvas images. Missing or invalid references
remain deterministic fallback state.

### `ui/scene_tileset_inspector_2d.zia`

Presentation-only 2D layer-image status and scrollable palette. It owns
accessible Reload/Clear controls, 50–300% nearest-neighbor presentation zoom,
numeric tile-ID search/reveal, cached thumbnails, selection outlines, and
zoom-aware pointer intent while `SceneEditor2D` owns document mutation and
history.

### `ui/scene_tile_behavior_2d.zia`

Presentation-only per-tile collision, typed-property, animation, and 16-variant
autotile editor. It returns validated-draft intent to `SceneEditor2D` and owns
the scene-wide live-autotile-preview checkbox; canonical mutation, lookup-cache
refresh, history, and dirty state stay in the editor controller.

### `ui/scene_editor_3d.zia`

Document-backed VSCN hierarchy and runtime-backed shaded/triangle-wireframe
viewport. It retains one windowless Canvas3D, RenderTarget3D, and exactly
matched projection-switchable camera, then draws deterministic editor overlays
on the readback. The same camera unprojects pointer rays for
closest-visible-mesh bounds picking before a meshless origin-marker fallback.
Its responsive chrome keeps active transforms and Game View direct while
stable Scene/Create/Placement/View menus own secondary commands at every width;
wider lanes enlarge the viewport instead of repopulating the chrome.
Its per-document Scene-inspector topic discloses exactly one complete World,
Lighting & Bake, Assets & Materials, or History surface at a time.
The responsive corner orientation navigator owns six axis views, projection
switching, active/hover presentation, and priority pointer routing entirely as
per-document workspace state. It owns
replace/add/toggle/blank selection policy, camera-plane pan, import and
primitive creation, camera navigation,
responsive hierarchy/inspector state,
true retained parent/child rows with preserved expansion, stable multi-node
selection, non-destructive bounded hierarchy Find, wrapped group-aware
Show/Hide/pick-lock actions, tab-owned lock state with identity-safe structural
remapping and retained-row cues, transactional
before/into/after hierarchy drops, parent-aware group Move/Rotate/Scale handles,
conditioned XY/XZ/YZ Move/Scale-plane picking and transactional two-axis
dragging,
bottom-bound Drop-to-Surface placement across canonical and project-preview
geometry with selected-subtree exclusion and one-step exact rollback,
bounded live X/Z/XZ Surface Move conformance with transient prefab
bounds/following and twist-preserving precise-normal alignment,
runtime-backed vertex/pivot-to-vertex snapping with precise-surface fallback,
bounded source/target scans, live selected-root transforms, and exact
release/Escape transactions,
canonical centered mesh-heightfield terrain creation, typed-contract
validation, responsive inspector drafts, conforming brush feedback, bounded
Raise/Lower/Smooth/Flatten strokes, and exact release/Escape history,
subtree-aware batch duplicate/delete, pointer capture and Escape rollback,
focus-scoped W/E/R/V/End and Duplicate/Delete selection commands, live
single-node transforms, relative multi-node numeric transform batches, compact PBR
material component coordination, cycle-safe exact preserve-world reparenting
with preserve-local opt-out, mixed-state batch visibility, bounded native
texture selection, stable contiguous sibling-block ordering, typed
gameplay-metadata transactions, exact responsive gameplay-eye anchoring,
transient project node/environment preview composition, retained project
post-FX construction with finalized shaded readback, schema-v14 additive
environment layers that transform ordinary project prefabs from exact typed
root metadata, schema-v15 bounded runtime `Water3D` construction with exact
typed dimensions/images/waves and 30 Hz animation, one explicit composition
frame shared by canonical geometry, project prefabs, and procedural water,
schema-v16 typed node-metadata material overlays with bounded project maps,
PBR/emissive/environment state, synchronous clone substitution, and immediate
canonical-material restoration, schema-v17 exact typed node-preview matching,
direct project model loading, bounded fixed transforms, and ordered
successful-load fallback with exact successful-rule retention,
atomic multi-node
component application, selection-free schema-v13 gameplay-node creation at the
viewport target with all metadata staged before one commit, and a per-document
Game View that derives a clean
temporary shaded pass, masks editor lighting/overlays/editing, retains free
camera navigation for legacy profiles, locks schema-v12 output profiles to
their project gameplay camera and centered aspect frame, restores every
underlying camera/preference/selection exactly, and canonical one-step edit
history.

### `ui/scene_panels_3d.zia`

Constructs the complete retained 3D scene widget tree, including stable
Scene/Create/Placement/View menu-item handles and the compact Scene-inspector
topic selector. `SceneEditor3D` owns responsive labels, truthful check/enable
state, topic visibility, and transactional dispatch.

### `ui/scene_metadata_inspector_3d.zia`

Presentation-only bounded Gameplay metadata inspector. It realizes at most 256
deterministically ordered `SceneNode` values, preserves null/Boolean/integer/
float/string kinds, and returns selection plus Set/Remove intent to
`SceneEditor3D`. The controller owns canonical VSCN mutation, validation,
history, rollback, dirty state, and document-local selection persistence.

### `ui/scene_hierarchy_3d.zia`

Stateless helper for existing-node hierarchy containment, destination
validation, unchanged-parent detection, and common-parent lookup. It has no
GUI, document, or SceneGraph ownership.

### `ui/scene_transform_3d.zia`

Stateless helper for transform-mode labels, mode-aware snap increments, target
quantization, safe scale bounds, single-axis projection, conditioned two-axis
Move/Scale plane solves, projected rotation-ring inversion, wrap-safe angular
deltas, and projected pointer-drag math. It has no GUI, document, or SceneGraph
ownership.

### `ui/scene_material_3d.zia`

Material value/file-loading helper for compact PBR draft normalization, sparse
group-patch no-op matching/application, ColorPicker conversion, editable
map-slot selection, common-image and strict KTX2 decoding, retained-map
summaries, canonical bounded thumbnail creation, and clone-safe material
creation. It preserves hidden imported material state and every unresolved
per-node PBR field and has no GUI, document, or SceneGraph ownership.

### `ui/scene_material_inspector_3d.zia`

Presentation-only compact PBR and texture-map inspector. It owns accessible
common/mixed widgets, sparse-patch intent, selected-slot coverage summaries, a
cached 128-pixel canonical map thumbnail, and truthful group-action enablement,
while `SceneEditor3D` retains selection, mutation, rollback, and history
ownership.

### `ui/scene_camera_3d.zia`

Clone-safe Camera3D drafts: normalization inside the runtime's sanitized clip
envelope, reconstruction, and equality for the camera inspector (ADR 0184).

### `ui/scene_camera_inspector_3d.zia`

Presentation-only camera component inspector with projection-aware fields,
Look Through, and the preview-inset toggle.

### `ui/scene_collider_3d.zia`

The ADR 0185 collider metadata vocabulary: kind tokens, kind-aware
dimensions, normalized drafts, and node read/write/remove helpers.

### `ui/scene_collider_inspector_3d.zia`

Presentation-only collider convention inspector with kind-aware rows.

### `ui/scene_bake_3d.zia`

Bake settings (`bake.*`), probe-grid (`probes.*`), and environment (`env.*`)
metadata conventions plus shared-mesh preflight scanning and world-space
light staging for the LightBaker3D workflow (ADR 0188).

### `services/material_library_3d.zia`

Fail-closed load and atomic conflict-guarded persistence of the per-root
`materials.scene3d` project material library (ADR 0189).

### `ui/scene_light_3d.zia`

Document-independent normalized authoring state for all seven `Light3D` types.
It converts live components to bounded field values, constructs complete
independent replacement lights, compares observable state for no-op detection,
and centralizes type applicability without owning GUI or scene state.

### `ui/scene_light_inspector_3d.zia`

Presentation-only light inspector with mixed-value batch fields. It owns type selection,
accessible common/type-specific controls, conditional visibility and
enablement, and normalized draft intent. `SceneEditor3D` retains typed
`SceneNode.Light` mutation, canonical serialization, rollback, history, and
hierarchy/viewport marker presentation.

### `ui/explorer_actions.zia`

Overlay for explorer create/rename/duplicate/delete workflows. It collects user
input and reports action intent; actual filesystem/document mutation happens in
`app/explorer_action_runner.zia`.

### `ui/command_input.zia`

Reusable non-modal single-value command input overlay. It is used by Go To Line,
output filtering, Workspace Symbols, Rename Symbol, Extract Local, Extract
Function, and New Project; command modules consume the completed value and
perform validation/effects.

### `ui/workspace_edit_preview.zia`

Responsive non-modal review surface for multi-file rename edits. It retains the
exact edit/root payload until Apply or Cancel, renders at most 50 row widgets
per frame and 250 preview rows total, and participates in the shell's exclusive
transient-focus ownership.

### `ui/ide_overlays.zia`

Shared overlay helpers used by command palette or workbench popups.

### `ui/tool_panel_text.zia`

Formatting constants and helpers for Problems/Output/Search/References rows:
colors, column widths, severity labels, truncation, diagnostics summary, and
language display names.

### `ui/tool_panel_model.zia`

Stable panel/tab ids, default tab indexes, durable Problems records, and bounded
row state for the dockable tool-panel strip. Use stable IDs rather than visual
indices after tabs can move. Problems records retain severity, source,
location, code, message, action, and location-store data while filtered
ListBox rows are rebuilt. Generic tool rows retain optional location data plus
References group/header identity so filtered result items can be recreated
without breaking click-to-open navigation. Their generic data field also carries
durable producer identity such as the original adapter frame index for a
filtered Call Stack row.

### `ui/tool_panel_groups.zia`

Pure migration-safe membership and ordering for simultaneous left, bottom,
right, and in-window floating tool groups. It validates complete canonical
layouts atomically, migrates legacy three-group/single-dock settings, moves one
stable panel ID, and merges a complete primary group into an occupied
destination without depending on GUI handles.

### `ui/tool_panel_shell.zia`

Owns live tool widgets, simultaneous left/bottom/right group roots, one
movable/resizable in-window floating root, direct selected-tool movement,
primary-group drag/merge/float, geometry and membership persistence edges,
stable bounded row projection, and responsive in-panel controls. Moving a tool
reparents its existing content widget and recreates only its owning Tab, so
terminal/search/filter focus and producer state survive. Problems, Output,
References, Debug Console, Variables, and Call Stack keep their control state
here while producer-owned records remain outside concrete ListBox items. Call
Stack rebuilds filtered rows with original frame IDs; Debug Console presents
separately owned program output and debugger status, and emits a clear request
for the session owner rather than clearing presentation rows optimistically.

### `ui/output_cache.zia`

Pure output-cache policy for AppShell's build output: retained character
budget, truncation marker, raw-pane versus row-list decision, and wrap width
heuristic.

## `zia/`

Pure Zia source-analysis and rewrite helpers live here. These modules should
not own GUI widgets or document-manager state.

### `zia/identifier_utils.zia`

Identifier and keyword rules used by refactors and source scanning. Unicode
identifier support should start here if added later.

### `zia/source_scan.zia`

Compatibility facade for source scanners. Existing callers can keep importing
`source_scan`, while new code should prefer the focused modules below.

### `zia/function_scan.zia`

Function lookup helpers: source-line access, function-name parsing, enclosing
function detection, and function end-line matching.

### `zia/trivia_scan.zia`

Trivia-aware line scanning for comment depth, brace/paren depth, continuation
indent, whitespace skipping, and identifier replacement outside strings/comments.

### `zia/call_scan.zia`

Outgoing call-token collection for simple call hierarchy support.

### `zia/delimiter_scan.zia`

Inline delimiter and block brace matching used by selection expansion.

### `zia/occurrence_scan.zia`

Whole-word next-occurrence search for multi-cursor commands.

### `zia/formatters.zia`

Zia and BASIC document/range formatting, whitespace trimming, indentation rules,
and line formatting helpers.

### `zia/bind_utils.zia`

Bind normalization and rewrite helpers. Explorer rename and missing-bind
commands use this kind of logic to keep source updates deterministic.

### `zia/refactors.zia`

Pure refactor transformations for inline local, extract local, and extract
function. The transforms are deliberately conservative and return unchanged
source when unsafe or unsupported.

## `probes/`

Probe files are focused executable checks. They are not examples for end-user
application structure; they are regression harnesses for IDE behavior.

Important probe groups:

- `smoke_probe.zia`: basic app smoke.
- `phase0_phase1_probe.zia`: core document/project/command/session behavior.
- `phase2_phase3_probe.zia`: build/run/debug boundary and scene data contracts.
- `editor_hot_path_probe.zia`: editor performance and copy/layout/index guards.
- `intellisense_probe.zia`: completion, diagnostics, hover, signature behavior.
- `console_search_probe.zia`: output/search/Quick Open helpers.
- `bottom_panel_probe.zia`: real pointer docking for the primary sidebar and
  primary tool group; simultaneous left/bottom/right/floating groups; real
  floating move/resize/edge-redock and geometry persistence; direct selected
  tool controls; migration-safe group persistence; mirrored size persistence;
  collapse/focus safety; movable tabs; compact targets; and layout reset.
- `tool_panel_toolbar_probe.zia`: live Problems/Output/References filters and
  actions, durable diagnostic/reference metadata, incremental grouped results,
  real pointer activation, and zoomed side-dock containment.
- `debug_probe.zia`: VM debug adapter integration.
- `debug_tool_surfaces_probe.zia`: state-aware Call Stack and Debug Console
  filtering, copy/clear behavior, durable frame identity, real pointer actions,
  and zoomed side-dock containment.
- `run_debug_view_probe.zia`: persisted activity routing, complete debugger
  state labels, real pointer controls, versioned breakpoint publication,
  filtered exact removal/persistence, and high-zoom scroll reachability.
- `scene_editor_2d_probe.zia` and `scene_editor_3d_probe.zia`: stable hierarchy
  multi-selection; case-insensitive wrapping hierarchy Find, standard-command
  routing, hidden-inspector descendant reveal, and exact no-history/camera
  mutation; exact one-transaction group transform, duplicate, delete,
  cut, same-kind cross-document paste, selection restoration, malformed or
  wrong-kind rejection, focus-safe pixel/tile nudging, deterministic 2D
  alignment/distribution, typed scene-wide and 3D node gameplay metadata with
  tab-local selection,
  cycle/no-op-safe exact preserve-world 3D reparenting with singular/shear
  rollback and preserve-local opt-out, stable contiguous sibling-block
  ordering, truthful mixed-state batch visibility, post-move selection
  remapping, and rollback behavior.
- `scene_command_bar_probe.zia`: real Scene/Tools/Create/Placement/View menu
  clicks, checked-state refresh, bounded bar/canvas/viewport height, and
  stable progressive disclosure across compact, medium, and wide editors.
- `scene_inspector_topics_probe.zia`: one-topic-at-a-time 2D/3D Scene
  inspector visibility, real dropdown dispatch, canonical isolation, and
  per-document topic restoration.
- `scene_canvas_selection_probe.zia`: public 2D point replace/add/toggle/group
  preservation policy, reverse inclusive cell queries, marquee
  replace/union/toggle/empty behavior, real captured blank-space dragging,
  visible overlay, exactly-once release, Escape cancelation, and canonical
  content/history isolation.
- `scene_tile_tools_probe.zia`: forward/reverse inclusive rectangles,
  four-connected exact-count fill, active-layer picker isolation, real captured
  rectangle preview and release, Escape cancelation, and freehand snapshot
  rollback.
- `scene2d_history_palette_probe.zia`: labeled oldest-first 2D history rows,
  click-equivalent row jumps with retained redo, per-document label ownership,
  bounded palette zoom geometry, numeric tile search/reveal, exact scaled
  pointer selection, and canonical-content/history isolation.
- `scene2d_rulers_guides_probe.zia`: scroll/zoom ruler geometry, real
  click-to-toggle guides, captured create/move previews on both axes,
  drag-out removal, collision/Escape rollback, visibility reflow, per-tab
  state, bounded guide capacity, captured ruler/canvas pixels, and
  canonical-content/history isolation.
- `scene2d_layer_navigator_probe.zia`: retained layer state cues, Enter
  visibility with exact undo, real retained-row drag ordering, tab-local
  Lock/Solo isolation, live add/move/remove identity remapping, boundary-aware
  actions, and responsive containment.
- `scene2d_object_draw_stack_probe.zia`: stable project priority/layer
  ordering, invalid override fallback, real tile/sprite interleaving and
  overlap pixels, matching topmost pointer selection, direct inspector
  Apply/no-op/Use Project transactions, and byte-exact default restoration.
- `scene_game_view_probe.zia`: exact clean 2D authored pixels, 3D
  navigator/gizmo/editor-light masking, temporary shaded presentation,
  disabled viewport editing, real click suppression, tab-local restoration,
  and byte-exact content/history isolation.
- `scene_light_authoring_probe.zia`: every runtime light constructor,
  normalized public cone readback, independent replacement, exact no-op
  suppression, add/apply/remove history, VSCN round trips, hierarchy and
  viewport markers, and truthful multi-selection gating.
- `scene_rotation_ring_probe.zia`: conditioned X/Y/Z projected-ring picking,
  seam-safe angular math, real hover/down/move/up input, stable viewport
  geometry during status changes, one-step snapped rotation history, and exact
  undo.
- `scene_surface_drop_probe.zia`: viewport-focused End placement, independent
  multi-root world-bottom casts, selected canonical/prefab self-ray exclusion,
  canonical/project-environment nearest hits, real X-axis Surface Move input,
  bounded acquisition of a raised 30-degree ramp, twist-preserving normal
  alignment through live Move and explicit Drop, live transient-prefab
  alignment before deferred serialization, workspace state, exact undo/redo,
  grounded or missing-surface history no-ops, and complete terrain-action
  containment across medium dock-heavy, laptop Scene Layout, and compact
  viewport widths.
- `scene_vertex_snap_probe.zia`: authoritative runtime mesh-vertex source and
  target queries, persistent control state, real vertex-to-vertex,
  pivot-to-vertex, and vertex-to-precise-surface pointer gestures, live
  canonical/history isolation, one-step release history, exact Escape and undo
  restoration, and transform-toolbar containment.
- `scene_terrain_authoring_probe.zia`: canonical grid topology and typed
  convention protection, pure deterministic brush math, real direct creation
  and precise viewport sculpt input, live no-history feedback, one-step
  release, exact Escape/undo/redo, flatten/regenerate transactions, VSCN round
  trips, and responsive inspector containment.
- `scene_orientation_navigator_probe.zia`: production navigator geometry,
  six axis-aligned camera poses, per-tab camera restoration, real hover/click
  and projection-chip routing, overlay input priority, captured pixels, and
  canonical-content/history isolation.
- `scene_hierarchy_affordances_probe.zia`: real Show/Hide action input,
  exact group visibility history/undo, tab-local multi-node pick locks,
  retained-row cues, live reorder identity remapping, deleted-lock pruning,
  and wide/narrow action-strip containment.
- `scene_shaded_viewport_probe.zia`: retained windowless Canvas3D/RenderTarget
  identity, exact runtime-camera/editor-overlay projection, authored shaded and
  triangle-wireframe pixel differences, accessible real-pointer mode
  switching, per-document preference, and canonical-history isolation.
- `scene_asset_thumbnail_probe.zia`: real PNG and VSCN preview rendering,
  visible-card one-per-frame scheduling, responsive wrapping geometry,
  grid-click/list-model selection identity, mode-switch retention, selected
  model detail rendering, and captured thumbnail pixels.
- `scene_gameplay_preview_2d_probe.zia`: real Xenoscape project loading,
  native-scale player-start framing, schema-v11 biome/object art and exact game
  category ordering despite a differently grouped canonical array, exact
  150-by-17/530-tile/82-object fixture retention, schema-v13 direct enemy
  creation at a chosen cell with project preview and byte-exact one-step
  undo/redo, schema-v12 1280-by-720 output framing/matte/navigation lock and
  restoration, clean Game View capture, and canonical content/history isolation
  across native control realization.
- `scene_gameplay_preview_probe.zia`: real Ashfall project loading, exact
  gameplay eye/FOV retention across responsive dock changes, schema-v9 terrain
  composition, schema-v16 typed surface selection with exact production
  maps/PBR/emissive/environment state and canonical-material restoration,
  schema-v17 direct enemy-rig loading with exact typed archetypes,
  project paths/transforms, and truthful direct/fallback counts,
  schema-v15 exact runtime `Water3D` dimensions, production image inputs, two
  wave records, bounded animation pixel changes, and one-frame
  canonical/preview/water draw accounting, schema-v10 retained five-pass
  post-FX and finalized runtime-lit high-key luminance, schema-v13 direct spawn creation at the viewport target
  with exact runtime metadata, immediate prefab preview, and byte-exact
  one-step undo/redo, schema-v12 1600-by-900 render-target
  framing/matte/camera lock and restoration, explicit Gameplay View recovery,
  clean Game View capture, and canonical content/history isolation.
- `scene_viewport_picking_probe.zia`: off-origin nearest-depth mesh-bounds
  selection, shaded/wireframe parity, meshless marker fallback, additive and
  primary-modifier selection policy, blank clear/preserve behavior, exact
  camera-plane pan, real compact Create-menu Box/Cylinder dispatch, public
  Super key constants, and VSCN/history isolation.
- `terminal_*`: PTY terminal behavior and rendering.
- `scm_probe.zia`: Git Source Control behavior.
- `scm_view_probe.zia`: real-Git responsive panel, pointer staging,
  focused-Enter commits, and content-conflict recovery.
- `scm_gutter_probe.zia`: bounded rendering plus asynchronous/stale-safe gutter
  diffs in a real temporary multi-root Git workspace.

When adding a feature, add or extend a focused probe near the smallest surface
that can verify it. Do not rely only on a full IDE smoke test.

## `tests/`

`zannastudio/src/tests/` contains local GUI/runtime probe sources and small
experiments. CTest registration for IDE probes lives in the repository-level
`src/tests/CMakeLists.txt`.

## Where To Put New Code

Use this practical decision table:

| Change | Preferred location |
| --- | --- |
| New command id or shortcut | `commands/command_catalog.zia` |
| Command availability | `commands/command_registry.zia`, `editor/language_service.zia` |
| File open/save behavior | `core/document_manager.zia` |
| New document metadata | `core/document.zia` |
| Project manifest parsing | `core/project.zia` |
| Explorer tree/cache behavior | `core/project_manager.zia` |
| Editor widget synchronization | `editor/editor_engine.zia` |
| Zia semantic query scheduling | appropriate `editor/*` controller |
| Zia project navigation/refactor commands | `commands/zia_workspace_commands.zia` + `editor/zia_workspace_query_job.zia` |
| BASIC project semantic commands | `commands/basic_workspace_commands.zia` + `editor/basic_workspace_query_job.zia` |
| Pure source scanning | focused `zia/*_scan.zia` module; keep `zia/source_scan.zia` as facade |
| Search matching/path/file discovery rules | `services/search_matcher.zia`, `services/search_paths.zia`, `services/workspace_file_index.zia` |
| Quick Open palette/scoring | `commands/quick_open_commands.zia` |
| Completion row data | `editor/completion_items.zia` |
| Completion workspace source loading | `editor/completion_workspace_source.zia` |
| Pure source rewrite | `zia/refactors.zia` or `zia/bind_utils.zia` |
| Source-transform command flow | `commands/source_transform_commands.zia` |
| Diagnostic quick-fix command flow | `commands/diagnostic_edit_commands.zia` |
| Text formatting | `zia/formatters.zia` |
| Build/run process mechanics | `build/build_system.zia` |
| Build/run user command flow | `commands/build_commands.zia` |
| Transient notification eligibility | `ui/notification_policy.zia` |
| Debug transport/session state | `build/debug_session.zia` |
| Debug command behavior | `commands/debug_commands.zia` |
| Run and Debug activity presentation | `ui/run_debug_view.zia` |
| Shared scene project-asset search/selection | `ui/scene_asset_browser.zia` |
| Shared image/model asset-thumbnail rendering | `ui/scene_asset_thumbnails.zia` |
| Shared typed scene clipboard envelope | `ui/scene_clipboard.zia` |
| Shared project scene-component schema | `ui/scene_component_schema.zia` |
| Shared project scene-component structured edits/file history | `ui/scene_component_authoring.zia` |
| Shared project scene-component authoring coordination | `ui/scene_component_authoring_controller.zia` |
| Shared project scene-component presentation | `ui/scene_component_palette.zia` |
| Shared retained-row scene selection | `ui/scene_selection.zia` |
| Shared scene hierarchy Find semantics | `ui/scene_hierarchy_search.zia` |
| Shared scene undo-history byte/entry bounds | `ui/scene_history.zia` |
| 2D visual scene authoring | `ui/scene_editor_2d.zia` |
| 2D scene-wide property presentation | `ui/scene_property_inspector_2d.zia` |
| 2D precision layout rules | `ui/scene_layout_2d.zia` |
| 2D tileset decode/palette rendering | `ui/scene_tileset_2d.zia` |
| 2D tileset inspector presentation | `ui/scene_tileset_inspector_2d.zia` |
| 2D tile-behavior inspector presentation | `ui/scene_tile_behavior_2d.zia` |
| 3D visual scene authoring | `ui/scene_editor_3d.zia` |
| 3D surface-placement query/history workflow | `ui/scene_editor_3d.zia` |
| 3D canonical terrain mesh/brush rules (ADR 0211) | `ui/scene_terrain_3d.zia` |
| 3D terrain inspector presentation | `ui/scene_terrain_inspector_3d.zia` |
| 3D node gameplay-metadata presentation | `ui/scene_metadata_inspector_3d.zia` |
| 3D hierarchy reparent rules | `ui/scene_hierarchy_3d.zia` |
| 3D transform mode/space and gizmo math | `ui/scene_transform_3d.zia` |
| 3D material sparse-patch/copy-on-edit rules | `ui/scene_material_3d.zia` |
| 3D material common/mixed inspector presentation | `ui/scene_material_inspector_3d.zia` |
| 3D normalized light reconstruction/no-op rules | `ui/scene_light_3d.zia` |
| 3D mixed-value light inspector presentation | `ui/scene_light_inspector_3d.zia` |
| 3D camera component drafts (clip-envelope normalization) | `ui/scene_camera_3d.zia` |
| 3D camera inspector, look-through, preview intents | `ui/scene_camera_inspector_3d.zia` |
| 3D collider metadata convention (ADR 0185) | `ui/scene_collider_3d.zia` |
| 3D collider inspector presentation | `ui/scene_collider_inspector_3d.zia` |
| 3D bake/probe/environment conventions (ADR 0188) | `ui/scene_bake_3d.zia` |
| Project material library storage (ADR 0189) | `services/material_library_3d.zia` |
| Terminal PTY wrapper | `terminal/terminal_session.zia` |
| Terminal UI behavior | `terminal/terminal_controller.zia` |
| Git command execution | `scm/scm_git.zia` |
| Source Control view state | `scm/scm_view.zia` |
| Persistent shell widgets | `ui/app_shell.zia` |
| Workbench splitter topology | `ui/workbench_shell.zia` |
| Primary sidebar docking | `ui/primary_sidebar_dock.zia` |
| Shared workspace dock geometry/payloads | `ui/workspace_dock.zia` |
| Independent tool-group membership/order | `ui/tool_panel_groups.zia` |
| Build-output cache/wrap policy | `ui/output_cache.zia` |
| Shared display row formatting | `ui/tool_panel_text.zia` |
| Bottom panel ids/tab indexes | `ui/tool_panel_model.zia` |
| Structured clickable locations | `services/locations.zia` |
| Transactional workspace edits | `services/workspace_edits.zia` |
| Runtime API change | C runtime + `runtime.def` + docs/probes |

## Current Pressure Points

The source map should also make current weak spots explicit:

- `main.zia` and `ui/app_shell.zia` are oversized coordinator files.
- `commands/edit_commands.zia`, `commands/search_commands.zia`, and
  `editor/completion.zia` are also large and should be split when adding
  cohesive new behavior.
- Tool panels support independent attached groups and one in-window floating
  group, but not native secondary-window/multi-monitor detachment or true
  virtualized workbench surfaces.
- Built-in 2D/3D scene editors exist, but their asset/component/gizmo depth is
  still substantially below mature game-engine authoring tools.
- Source Control has a safe basic conflict path but no merge/rebase
  orchestration or ours/theirs recovery surface.
- Terminal behavior depends on OutputPane terminal mode, not a complete terminal
  emulator.
- BASIC support is intentionally narrower than Zia.

New code should reduce these pressure points when it naturally can, but should
not perform unrelated refactors that make feature verification harder.
