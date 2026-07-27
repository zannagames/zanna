# Zanna Studio Workflows

This document covers how to use and develop Zanna Studio as it exists today.

Zanna Studio is frame-driven and project-oriented. Most workflows begin with a
project folder, a set of open documents, and the active editor. Commands are
available through menus, toolbar buttons, the command palette, shortcuts, and
context menus. When a command is not valid for the active language or current
state, the IDE should either disable it or report an unavailable reason.

The workflows below are written from the user's point of view, but they also
name the implementation boundaries that matter when debugging a broken flow.

## Launching

From the repository root:

```sh
zanna run src/zannastudio/
```

From inside `src/zannastudio/`:

```sh
zanna run .
```

After building the native IDE binary:

```sh
./scripts/build_ide.sh
./src/zannastudio/bin/zannastudio
```

On Windows:

```powershell
.\scripts\build_ide_win.ps1
src\zannastudio\bin\zannastudio.exe
```

## Opening Projects

Use "Open Folder" to choose a project root. Use "Add Folder to Workspace" for
additional roots. The first opened folder is the primary project root used by
build/run defaults, session restore, Source Control, and workspace indexing.

The project explorer populates one directory level at a time. Quick Open and
workspace search build their full file cache cooperatively so opening a large
workspace does not require a full recursive tree build up front.

When a project opens, Zanna Studio does not immediately index every source file for
every semantic feature. It establishes the tree and caches enough state for
interactive work, then semantic indexing and Quick Open/search caches progress
cooperatively. This is why a freshly opened large workspace can show a tree
quickly while deeper semantic commands still warm up.

If the project has a `zanna.project` file, Zanna Studio reads project name, entry,
language, build/run overrides, working directory, problem matcher, and ignore
patterns. If there is no manifest, the folder still opens and defaults are used.

## Opening Files

Open files through:

- File > Open.
- Explorer selection.
- Quick Open.
- Search result navigation.
- Diagnostics/output/reference result navigation.
- Recent Files.
- Reopen Closed File.

Opening the same file twice activates the existing tab instead of creating a
duplicate document.

Zanna Studio tracks one `Document` object per open path. The editor widget displays
the active document, but the document manager owns the buffer state. Switching
tabs writes cursor/scroll/text state back to the outgoing document and loads the
incoming document into the editor. This is why file commands should go through
the document manager rather than setting editor text directly.

## File Kinds

| Extensions | IDE kind | Behavior |
| --- | --- | --- |
| `.zia` | code | Zia editor services. |
| `.bas`, `.vb` | code | BASIC editor services. |
| `.txt`, `.md`, `.json`, `.il` | text | Plain text editing. |
| `.scene`, `.level` | 2D scene | Built-in 2D hierarchy, viewport, properties, history, and import tools. |
| `.vscn` | 3D scene | Built-in 3D hierarchy, viewport, transform/material, history, and import tools. |
| image extensions | binary unsupported | Read-only preview placeholder. |
| unknown | text | Plain text editing. |

## Editing

Core editing behavior includes:

- Undo and redo.
- Cut, copy, paste, and select all for the active text or visual-scene surface.
- Multi-cursor commands.
- Add next occurrence, skip occurrence, select all occurrences.
- Clear extra cursors.
- Expand and shrink selection.
- Toggle line comment.
- Toggle block comment where the language supports it.
- Duplicate line.
- Move line up/down.
- Find/replace in the active file.
- Word wrap toggle.
- Line numbers toggle.
- Minimap toggle.
- Format document and format selection where a formatter exists.
- Trim trailing whitespace command and optional save-time trimming.

The editor keeps a revision number. Semantic controllers use that revision to
avoid applying results to the wrong buffer after the user types. If completion,
diagnostics, hover, or signature help appears stale, the likely bug is missing
path/revision/cursor validation around an async or delayed result.

## Zia Coding Workflow

Common Zia commands:

- Trigger Completion: `Ctrl+Space`.
- Trigger Signature Help: `Ctrl+Shift+Space`.
- Go to Definition: `F12`.
- Find References: `Shift+F12`.
- Rename Symbol: `F2`.
- Workspace Symbols: `Ctrl+T`.
- Symbol Outline: `Ctrl+Shift+O`.
- Run Check Now: `Ctrl+Alt+C`.
- Apply Diagnostic Fix-It: `Ctrl+.`.
- Organize Binds: `Shift+Alt+O`.

Zia semantic results are revision-gated. Stale completion, diagnostics, hover,
signature, and symbol work is rejected when the buffer changes before a result
lands.

Suppress Warning, Apply Diagnostic Fix-It, and Create Missing Bind also return
to the event loop before compiler work finishes. Keep the originating tab and
caret stable while they run. Project-bind discovery scans captured workspace
roots on a bounded worker and inserts only when exactly one candidate remains;
ambiguous, unreadable, oversized, or subsequently changed candidates are left
untouched with a status explanation.

Zia workspace navigation and rename depend on `Zanna.Zia.ProjectIndex`, but the
commands return to the event loop before project parsing begins. While the lazy
index warms up, the status bar shows queued progress; the command starts only
after a complete index exists. Moving the caret, editing, switching tabs or
roots, or changing indexed content cancels the delayed result. References and
call rows render in batches of at most 100 per frame.

The Zia index accepts at most 20,000 files, 200 KB per file, and 64 MB of source.
Unreadable/oversized files or truncated discovery make it explicitly incomplete,
and project navigation/refactor commands refuse instead of returning partial
answers. Reference results cap at 2,000; rename refuses above that limit and
validates expected symbol text again when edits are applied.

The References toolbar filters by file/group heading, language, location,
marker, or preview text while Zia and BASIC results continue to arrive. A
matching heading shows all of that group; a matching row shows its heading and
that row. Filtered rows keep their source locations, so selecting one still
opens the original range. Copy All copies the complete unfiltered grouped
result, and Clear keeps References open with an explicit empty state.

BASIC workspace navigation uses Zanna Studio's in-process semantic scanner on
an owned background worker. It pages every workspace root, lets unsaved open
BASIC buffers override disk, and rejects results when the active path, revision,
caret, or root set changes before completion. File/source/declaration ceilings
bound the scan; reference and call results are limited to 1,000 rows and render
incrementally. Rename is canceled rather than applying a partial result set.

## BASIC Coding Workflow

BASIC files support completion, diagnostics, hover, document symbols, and
build/run. Semantic navigation commands such as definition, references, rename,
workspace symbols, call hierarchy, and signature help are available for BASIC.

## Search

Use project search for workspace-wide text queries. The command opens a docked
Search panel so the query and filters can stay visible while results update.
Supported options include:

- Literal or regex matching.
- Case-sensitive matching.
- Whole-word matching.
- Include path filter.
- Exclude path filter.
- Folder-scoped search from the explorer.

Search results are grouped by file and navigate through structured location ids
instead of parsing displayed `path:line` strings. Search processes at most 16
files and 2 MB of source per frame; the same 2 MB per-file ceiling applies to
unsaved open buffers as well as disk files.

Replace All is tied to the exact completed text-search location-id range and
the query/options that produced it. It is disabled while search is running,
after cancellation, when discovery/results were truncated, or after panel
options change without a new search. Starting replacement only snapshots file
paths and line numbers; subsequent frames apply at most two files each. Closed
files are reread under the search size ceiling and atomically replaced only if
their content and modification time remain stable. Open buffers remain unsaved,
receive a workspace-undo snapshot, and never overwrite their disk copies.
Stopping a running replacement preserves the explicitly reported files already
changed and leaves every later file untouched. Results are cleared afterward so
a stale row cannot be applied twice; run Search again to refresh them.

## Compare And Diff

Compare with Saved and Source Control history/worktree comparisons share one
side-by-side overlay. Opening it is non-blocking: a latest-wins worker computes
the alignment under 4 MB-per-side, 20,000-combined-line, edit-depth, and trace
allocation ceilings. The overlay then adds at most 100 of its maximum 4,000
retained rows per frame. Closing the overlay cancels unpublished work; opening a
new comparison supersedes the old request. Very large inputs show a concise
limit message instead of attempting a potentially unbounded diff.

## Workspace Layout

The primary sidebar contains Explorer, Source Control, and Run and Debug.
Drag its `Sidebar - drag` header to the stationary Left or Right target, or use
the adjacent arrow buttons. `Move Primary Side Bar Left/Right` provide the same
actions through the Command Palette. The complete live view tree and activity
rail move together; selection and the logical sidebar width are retained, and
the mirrored splitter position is persisted across restarts. Collapsing the
sidebar still leaves its activity rail reachable on the configured edge.

Problems/Output/Search/References/Variables/Call Stack/Debug Console/Terminal
can occupy simultaneous left, bottom, and right groups plus one in-window
floating group. Each attached group header moves its selected tool directly,
including into the floating group; Command Palette actions provide the same
destinations. Use `Float Primary Panel Group` or drag the primary group onto
the central Float target to move the complete group without rebuilding its
tools.

The floating header moves the window and shows the same stationary targets.
Release over a target card or narrow workbench edge to dock the whole group;
ordinary movement elsewhere simply places it. The lower-right handle resizes
the window. Its bounded position and size recover after viewport/UI-scale
changes and persist across restarts. Arrow buttons move only the selected
floating tool back to an attached group.

Tabs reorder within their group, and membership, order, floating bounds, active
split sizes, and the primary host are durable. Typed drop targets ensure that a
target for the other surface does not alter layout. Focused sidebar trees and
terminal/search/filter inputs are restored after a move rather than handing
typing back to the hidden editor.

`Reset Workspace Layout` restores the primary sidebar to the left, merges every
tool into the default bottom group, restores split sizes/tab order and one
editor pane, and does not clear panel content or active documents. Tool groups
return to the attached bottom workbench. The floating group is an in-window
workspace surface; native secondary OS windows and multi-monitor detachment are
not yet available.

## Build And Run

Build and run use `Zanna.System.Process` and argument vectors.

Default project behavior:

```text
zanna build --diagnostic-format=json <project-root>
zanna run --diagnostic-format=json <project-root>
```

Default single-file behavior:

```text
zanna build --diagnostic-format=json <active-file>
zanna run --diagnostic-format=json <active-file>
```

The IDE resolves the `zanna` binary in this order:

1. `ZANNA_BINARY`.
2. Binary next to the IDE executable.
3. Developer build tree relative to the IDE executable.
4. `ZANNA_BUILD_DIR`.
5. `PATH`.

Build/run output is streamed to the Output panel and retained in a bounded
buffer. Structured JSON diagnostics are preferred. Legacy text diagnostics are
parsed as a fallback.

The Output toolbar keeps its common operations in the panel:

- Filter updates visible rows as text is entered.
- Wrap reflows long lines to the effective dock width.
- Follow controls whether incoming output scrolls to the bottom.
- Copy uses the complete unfiltered captured output when available and the
  current generated report rows otherwise.
- Clear empties the output but leaves the panel open when invoked there.

The Problems toolbar provides a message/file/source/code filter plus
Errors/Warnings/Info toggles and visible/total counts. Selecting a diagnostic
navigates to its source. When that row advertises a safe action, Quick Fix opens
the same durable location and starts the existing asynchronous Zia diagnostic
action. Filtering rebuilds presentation rows without losing navigation or
action metadata. Unsupported non-Zia edits are reported rather than applied.
Both toolbars wrap and compact when the shared panel is docked at a narrow side.

Before launching build/debug flows, Zanna Studio can save modified files according
to settings. This is intentional: the compiler and debugger run on disk state,
while the editor may have unsaved memory state. If a build appears not to see an
edit, first check `saveAllBeforeBuild`, active document modified state, and
whether the file is untitled or a read-only preview.

## Project Manifest Overrides

Inside `zanna.project`, use:

```text
build-program zanna
build-args build "{target}"
run-program zanna
run-args run "{target}"
working-directory .
problem-matcher zanna
ignore build-output/
ignore *.tmp
```

Rules:

- `build-program` and `run-program` select the executable.
- `build-args` and `run-args` are parsed into argv tokens.
- Single and double quotes group spans.
- Backslash escapes are supported.
- `{target}` expands to the project root or active file.
- `working-directory` is resolved relative to the project root.
- Commands are not shell-expanded.

## Debugging

Start Debugging launches:

```text
zanna run --debug-adapter <active-file>
```

Supported commands:

- Start Debugging.
- Continue.
- Pause.
- Step Over.
- Step Into.
- Step Out.
- Run to Cursor.
- Restart Debugging.
- Stop Debugging.
- Toggle Breakpoint.
- Conditional Breakpoint.
- Add Logpoint.
- Evaluate expression while stopped.
- Add the evaluated expression to the watch section.

The activity rail includes a persisted **Run and Debug** view. It remains
available for a standalone saved Zia source file when no project is open.
Its primary action follows the live session state: Start while idle, Pause while
running, Continue while paused, and a disabled progress label while pausing,
stopping, or restarting. Restart, Stop, and step controls are enabled only when
the adapter can accept them. Variables, Call Stack, and Console buttons open the
corresponding dockable tool surface without changing debugger state.

The same view lists persisted source breakpoints. The live filter matches file,
path, line, condition, and logpoint text. Visible rows retain their original
store identity, so Open, Remove, Condition, and Logpoint never infer a source
location from formatted row text or filtered position. The concrete list
realizes at most 1,000 matches and reports the complete count. At narrow widths
or high UI zoom, wrapped action rows live in a vertical scroll viewport.

Debug output is split between:

- Program output.
- Debug console/control messages.
- Variables panel.
- Grouped watch and local rows in the Variables panel.
- Call Stack panel.
- Current-line gutter marker.

Call Stack reports the current idle, paused, running, pausing, stopping,
restarting, or terminated state when frames are unavailable. Its toolbar
filters frames and copies the complete unfiltered stack. Filtering preserves
each frame's original adapter index, so opening a visible result navigates to
the correct source frame rather than the row's filtered position.

Debug Console exposes live text filtering, wrap control, complete-source copy,
and program-output Clear. Clear removes retained stdout/stderr from the debug
session and keeps the current debugger status visible, so cleared output does
not reappear when the next adapter event arrives. Filtering and wrapping do not
mutate session output.

Limitations:

- Lists, sequences, maps, and class instances expand asynchronously; boxed
  struct payloads and direct-IL/BASIC objects without a layout sidecar remain
  typed leaves.
- The Variables panel provides inline watch entry plus Add, Remove, Refresh,
  and Clear actions. Add Watch from the command palette focuses the same field.
- Breakpoint condition/logpoint values are edited in a focused single-field
  overlay rather than a full details inspector.

## Terminal

Toggle Terminal starts the integrated terminal panel. The first visible pump
starts the configured shell:

- Windows: `%COMSPEC%` when valid, otherwise `cmd.exe`.
- POSIX: `$SHELL` when valid, otherwise `/bin/sh`.

POSIX shells receive `-i`. The terminal working directory is the active project
root for newly started sessions. If the workspace changes while a terminal is
already running, the terminal keeps its current child process and shows a note
that restart is needed to use the new directory.

Supported terminal operations:

- Type directly in the terminal pane.
- Stop.
- Restart.
- Resize with the panel.
- Clear-screen shell redraws that use common `CSI J` sequences.
- Cursor-position shell redraws that use common `CSI H/f` sequences.

Limitations:

- Not a full terminal emulator.
- Full-screen TUIs that require complete alternate-screen or terminal-mode
  semantics are out of scope.
- Width/height are estimated from widget pixels, not font metrics.

## Source Control

The Source Control view operates on the primary project root when it is a Git
repository.

Supported operations:

- Refresh status.
- View current branch.
- Stage or unstage the selected file; Stage All only advertises itself while
  worktree changes remain.
- Commit staged changes after entering a message; focused Enter submits.
- View diff for selected file.
- View paged commit history, changed files, and parent-to-commit comparisons.
- Push.
- Pull.
- Switch branch.
- Answer detected username/password/passphrase prompts in the focused in-app
  credential row; focused Enter sends the response.
- Resolve a basic unmerged file by selecting it, editing its conflict markers,
  and staging it. The diff pane explains this path, and Commit remains disabled
  while any unmerged row remains.

The action rows wrap inside narrow sidebars. Stage, Unstage, side-by-side diff,
Stage All, Commit, and Cancel update from live selection, index, message,
conflict, and job state rather than advertising operations that cannot run.

Limitations:

- Commands run asynchronously, but the view processes one active Git job at a
  time.
- Push and pull can be long-running. Output and detected credential prompts are
  visible, but prompt detection is heuristic rather than a Git protocol.
- Common paths with spaces, staged renames, and a real content-conflict
  edit/Stage/commit path are covered; exotic path bytes, rename conflicts, and
  multi-file recovery need more coverage.
- Merge/rebase orchestration, ours/theirs review, stash, and merge abort are not
  provided.

## Scene Authoring

Files ending in `.scene` or `.level` open in the 2D scene editor. Files ending
in `.vscn` open in the 3D scene editor. Both surfaces write every accepted edit
back to the active `Document.content`, so ordinary dirty-tab, Save, Save As,
session, recovery, and external-change rules remain authoritative.

The standard Edit menu and Ctrl/Command keybindings follow the active surface.
Select All selects all authored 2D objects or editable 3D hierarchy nodes.
Copy places a versioned `2d` or `3d` scene envelope on the text clipboard; Cut
copies first and removes the selection as one undoable transaction. Paste
accepts only the matching scene kind, works between same-kind tabs, selects the
new content, and creates one undo entry. The envelope is capped at 64 MB and
1,024 identities. Malformed, wrong-kind, stale, oversized, or partly
unreconstructable data is rejected without changing the destination.

Find also follows the active surface. In a 2D or 3D scene,
`Ctrl`/`Cmd`+`F` reveals the docked hierarchy or its full-width master,
then focuses the hierarchy query. It does not change the user's inspector
visibility preference. The 2D query matches object ID or type; the 3D query
matches node name. Matching is case-insensitive. Press Enter or choose Next to
advance, and choose Previous to move backward; both directions wrap.

Hierarchy Find is a jump operation, not a filter. Every represented row remains
visible, the target's ancestors expand, and the selected row scrolls into view.
The status says when matching is limited to the editor's bounded represented
hierarchy. Searching and jumping do not change canonical scene bytes, revision,
undo/redo, dirty state, 2D coordinates, or the 3D camera.

Both scene editors expose a **History** list on the Scene inspector tab.
Rows are oldest-first transaction labels followed by **Current**. Selecting an
older row runs the equivalent normal Undo sequence, so the exact canonical
bytes are restored and every later edit remains available through Redo. The 2D
timeline and its labels follow the owning document when tabs change; snapshot
count and aggregate bytes remain bounded, and presentation labels never become
part of the scene format.

The 2D toolbar switches among Select, Paint, Erase, Rectangle, Line, Ellipse,
Fill, Pick, and Object modes. Paint, Fill, and the shape tools composite the
selected active-layer atlas frame under the pointer before anything is written.
Captured Rectangle, Line, and Ellipse gestures show image-true translucent
frames on every cell their release will commit; a multi-cell region stamp shows
each non-empty source frame in its grid-anchored footprint. Missing atlas frames
fall back to bounded outlines. Palette and tool changes repaint immediately,
tool switches clear a stamp, and the complete tool choice follows the owning
scene tab. All preview pixels are workspace-only.

The Tile behavior inspector's default-on **Live runtime autotile preview**
resolves base-layer tiles with the runtime's exact N/E/S/W mask order and
first-64-rule precedence. Authored cells keep their base IDs: Studio substitutes
only the displayed atlas frame, then applies optional animation preview to that
resolved variant. A single-cell Paint or Erase hover also previews the adjacent
variants the next click would change. The toggle follows its owning scene tab
and never changes canonical bytes, revision, dirty state, or history.

Paint and Erase interpolate missed cells during a fast pointer stroke and
capture the pointer until release. Escape restores the pre-stroke scene exactly
and adds no history; a changed release commits the entire stroke once.

Rectangle draws a non-destructive inclusive cell and atlas preview while
captured, then fills the forward or reverse drag once on release. Line uses an
integer walk and Ellipse samples the inclusive drag boundary; their previews
and commits share the same cell generator. Escape discards any captured shape
without changing the scene. Fill replaces the clicked cell's exact
four-connected active-layer region, so diagonal-only neighbors remain separate.
Pick samples the clicked active-layer tile into the numeric tile field and
returns to Paint without changing canonical scene data. Tile ID `0` is empty,
so Rectangle and Fill can also clear regions. Each changed shape or Fill is one
undo entry, and exact no-ops add none. Tool and selected-tile choices follow the
owning scene tab and bounded session state.

In Select mode, a plain click on an unselected object replaces the selection;
pressing an already-selected object preserves the complete selection so it can
be dragged as a group. Shift-click adds an object, while Ctrl/Command-click
toggles it without beginning a move.

Press and drag from a blank canvas cell to draw an inclusive authored-cell
selection box. A plain box replaces the selection, Shift unions its objects
with the selection, and Ctrl/Command toggles every object it contains. A plain
empty box clears the selection; a modified empty box preserves it. The box
matches each object's authored point cell rather than sprite or collider
bounds. The canvas captures the pointer until release, and Escape cancels the
box without changing selection. Selection and box feedback are workspace-only:
they do not change scene JSON, revision, dirty state, or history.

Select mode drags objects on the tile grid, and one complete stroke or object
drag creates one undo entry. After the canvas has been clicked in Select mode,
Arrow keys nudge the complete selection by one authored pixel and Shift+Arrow
uses one tile on the relevant axis. Inspector fields and the Objects TreeView
keep their native arrow behavior. Middle-drag pans and the wheel zooms.

The default-on **Rulers** action adds a numbered X ruler above the canvas and Y
ruler at its left edge. Ticks follow every scroll and zoom, including centered
small scenes. Click a top-ruler cell boundary to add a vertical guide or a
left-ruler boundary to add a horizontal guide; click the same coordinate again
to remove it. Drag from empty ruler space to create at another boundary, or
drag a cyan marker to reposition its guide with a live canvas preview. Escape
restores the exact source, a duplicate destination is refused, and dragging
outside the visible ruler removes the guide. Pointer capture keeps release
deterministic even outside the widget. Guides are capped at 64 per axis and
follow the owning tab. Ruler visibility, guides, live drag state, and the amber
live-cell cue are workspace presentation only: they do not dirty the scene or
add undo history.

The Scene inspector's layer navigator summarizes each row with `●` visible or
`○` hidden, `⊘` locked, `SOLO` when isolated, and an opacity percentage when
the authored value is below 100%. Double-click a row, or focus it and press
Enter, to toggle authored visibility as one undoable edit. Lock prevents every
tile-writing tool from changing that layer; Solo previews only that layer.
Lock and Solo are workspace-only choices that follow the owning open scene tab
and the same live layer through Add, Remove, Up, and Down. Removing their layer
prunes the choice, while a canonical reload clears these process-local indices.
Drag a row to the insertion marker to reorder it directly; the list
auto-scrolls near either edge. With the list focused, Alt+Up/Down requests the
same adjacent move. Direct gestures and the explicit Up/Down buttons all keep
the moved row active, preserve Lock/Solo identity, and commit one exact
canonical history transaction. Add, Remove, Up, and Down wrap at compact
widths and disable when the action would remove the final layer or cross a
draw-order boundary.

The Scene properties group authors level-wide adapter metadata independently
of placed objects. Select New or an existing row, enter a key, choose String,
Integer, Float, Boolean, or Null, and select Set. Changing a selected key
renames it; Remove deletes it. Each accepted change is one undo entry, exact
no-ops and invalid values leave history untouched, and the selected row belongs
to the active scene tab and session. The editor preserves scalar kinds in
canonical scene JSON, but it does not yet validate keys against a
project-defined scene-wide schema. Project components described below target
placed objects and 3D nodes, not the scene-level table.

The Objects TreeView presents real parent/child rows and retains collapse state
through refreshes. Ctrl/Command-click adds or removes rows and Shift-click
selects a visible range. Drag above or below a row to place the complete
selected subtree block before or after that sibling; drag onto the middle to
make the selected top-level roots the target's last children. Cycle, depth,
detached-target, and exact no-op requests leave content and history untouched.
Accepted drops preserve absolute object positions, remap selection, and create
one undo entry. Up/Down performs the same operation against the previous or
next sibling for a single selection.

Use **Add Root** to create an independent object. With exactly one object
selected, **Add Child** creates a child at the selected canvas cell (or the
parent's position when no cell is active) and commits the object plus parent
link together, so Undo never reveals an intermediate root. The Parent chooser
is the keyboard-accessible reparent path: choose Scene root or a displayed
object, then select **Reparent**. It omits the selected subtrees and
depth-invalid destinations, supports several selected top-level roots, appends
them at the destination, and preserves every absolute X/Y coordinate. The
chooser follows the outliner's 4,096-row realization bound; hidden scene data
remains preserved.

Dragging any selected object in Select mode still moves the complete group on
the tile grid while preserving offsets. Duplicate copies every selected
object, its typed properties, and parent links whose endpoints are both in the
selection; Remove deletes the selection and promotes unselected direct
children through the runtime contract. Each batch is one undo entry and
restores the exact prior content and selection on undo. Reserved object fields
remain single-selection operations. With several objects selected, Object
properties becomes a deliberate batch editor: Set writes one typed key/value
to every row and Remove removes the entered key wherever present. The complete
batch is verified and committed once; invalid values or rejected writes restore
the prior content and selection.

The Object layout row aligns every selected X or Y coordinate to the primary
object. Distribute X/Y requires at least three objects, keeps the two extrema,
and places intermediate authored point positions at deterministic rounded
intervals. Each accepted arrangement is one undo entry; choosing a layout the
selection already satisfies is a history no-op.

Cross-document 2D paste assigns unique object IDs, offsets each object by one
destination tile, preserves null, Boolean, integer, floating-point, and string
properties, and restores parent links wholly inside the copied selection.
Parents outside the clipboard selection become scene roots.

Each layer can reference a PNG, JPEG, BMP, or GIF tileset image. Relative paths
resolve beside a saved `.scene`; untitled-scene relative paths resolve from the
working directory until the scene has a path. The inspector shows a scrollable
eight-column palette for the first 512 atlas frames. **−**/**+** scales the
palette from 50% to 300% with nearest-neighbor artwork and exact zoom-aware
pointer hits; this is presentation-only and never resamples the source or
changes scene bytes. Enter a one-based numeric **Tile ID** and choose **Find**
to select and reveal that frame. Clicking a frame likewise selects tile ID
`frame + 1`, switches to Paint, and the canvas composites real atlas frames
across visible layers. The numeric tile field remains available for larger
atlases.

Tileset references are external and are not embedded in `.scene`. Choose Image
and Clear Image each create one canonical undo entry when the reference changes;
choosing the same image is a no-op. Invalid candidates leave content and history
untouched. Project Layer Images opens a non-modal searchable view of supported
images from every open workspace root. It incrementally reuses the project file
index and opens in a responsive thumbnail grid; **Grid**/**List** switches to a
complete compact list without changing the canonical selection. Image cards
show real bounded pixels. Supported 3D import cards frame the asset's complete
mesh bounds through a neutral windowless Canvas3D render, while other asset
kinds retain an explicit file icon. Only cards intersecting the visible grid
render, at most one per frame; scrolling reveals and schedules more. The grid
retains at most 48 cards and the alternate list at most 512 rows.

Click any thumbnail or its filename to select it, then use the browser's
contextual action. The detail area retains the existing larger image preview
and now renders selected 3D assets as well. Drag either a card or a compact-list
row directly onto a compatible scene surface. Grid/list switching, filtering,
preview rendering, selection, and dragging are presentation-only; scene bytes
change only after **Use Image**, **Import Asset**, **Use Map**, or the
corresponding drop is accepted. A native picker remains available for files
outside the workspace.

Studio checks one referenced layer image per 250 ms polling opportunity.
Metadata changes quietly reread the pixels and redraw the canvas without
dirtying the document or adding undo history. Reload Image remains available
for an explicit reread, including same-size changes that a coarse filesystem
timestamp may not distinguish. Studio accepts sources up to 16 MB, decodes at
most 4,194,304 pixels per image and 8,388,608 pixels across the scene, and
applies the same
8,388,608-pixel bound to scaled preview caches. Missing, invalid, over-budget,
or out-of-range frames use a deterministic placeholder and report their state
in the layer inspector.

A project can also make the 2D canvas resemble its running game by declaring
`scenePreview2D` in the owning root's `scene-components.json` (schema version
8). The profile may name one fallback background or map an integer scene
property to bounded project-relative images with an optional integer offset.
Studio composites the resolved image behind layers and objects in fixed
screen space using `cover`, `contain`, or `stretch`; it never executes game
code or writes preview pixels into the scene. A profile can choose the
first-open grid default, while a restored per-tab grid choice still wins.
Background decoding, scaling, and external-change polling share the same
bounded image policy as layer and object previews. See
[scene-components.md](scene-components.md#version-8-2d-scene-backgrounds)
for the exact fail-closed schema. The scene test gate opens Xenoscape's real
150-by-17 region 01 fixture, retains its authored player-start framing and
canonical bytes, and captures the production canvas so game/editor visual
parity cannot regress behind synthetic fixtures alone.

The 3D viewport starts in **Shaded** mode. It renders the live scene hierarchy,
visibility, meshes, PBR materials, assigned maps, and scene lights through the
runtime's windowless software renderer inside the Studio window. Choose
**Shaded** in the View row to switch to **Wireframe**, which renders authored
triangle edges rather than replacing meshes with node boxes. The button label,
status row, tooltip, and accessibility description report the active mode.
Mode choice belongs to the scene tab and survives session restore without
dirtying the VSCN or creating history. Grid lines, parent links, node markers,
selection, and transform handles remain editor overlays in either mode. Studio
rerenders when content, camera, selection, mode, or viewport dimensions change;
an allocation/readback failure reports a fallback and preserves the
deterministic marker view.

A project can align that viewport with its running game through
`scenePreview3D`. The profile maps typed scene-root metadata to the initial
player position plus eye height and yaw, gameplay FOV, atmosphere, and overlay
defaults. **Gameplay View** restores the exact eye after navigation without
editing the scene; responsive hierarchy or inspector changes preserve that eye
while deliberate camera navigation releases the anchor.

Schema version 9 can additionally map exact integer root metadata to one of
1–64 project-relative `.scene3d`/`.vscn` environment prefabs. Studio draws the
resolved environment under the same camera, depth, lighting, sky, fog, and
shaded/wireframe mode as the editable scene. It remains transient and
non-pickable: the hierarchy, VSCN, dirty state, and undo history contain only
canonical nodes. Bake procedural environments into ordinary self-contained
preview assets rather than executing project code in Studio. See
[scene-components.md](scene-components.md#version-9-3d-scene-environments)
for the fail-closed path and variant contract.

Schema version 10 can add a portable post-processing recipe to that same
profile. Complete tonemap, bloom, color-grade, and vignette groups plus optional
FXAA are validated against runtime bounds and applied in a fixed order to
Shaded and Shaded+Wire views. Studio reads the offscreen canvas only after
frame finalization, then adds selections and gizmos, so the scene resembles the
game without degrading authoring overlays. Pure Wireframe stays unprocessed.
See
[scene-components.md](scene-components.md#version-10-3d-post-processing)
for the fields, ranges, and portable-effect boundary.

Click anywhere inside a visible mesh's transformed bounds to select the nearest
depth hit; if no mesh is hit, bounded node-origin markers keep empty
organizational nodes selectable. A plain click replaces the selection,
Shift-click adds, Ctrl/Command-click toggles, and a plain blank click clears.
Gizmo handles have first priority, and shaded/wireframe modes use the same
camera and geometry query. The hierarchy offers the same multi-selection
modifiers and renders actual expandable parent/child rows. Collapse state
survives ordinary edits and canonical reloads. The primary row owns the visible Move, Rotate, or Scale
handles; they follow that node's parent-aware local transform projection.
Dragging applies the same axis delta to each selected node's local transform,
so relative local offsets are preserved for Move and each node rotates or
scales around its own local origin. The optional Snap toggle quantizes the
resulting primary value—not each pointer frame—to 0.5 units, 15 degrees, or 0.1
scale for the active tool. A drag captures the pointer, updates the inspector
live, commits one VSCN history snapshot on release, and restores every origin
when Escape cancels it. Tool and snap selection belong to the scene tab and
survive session restore. With the viewport or one of its transform-tool buttons
focused, W selects Move, E selects Rotate, and R selects Scale. Those keys
remain ordinary input while an inspector, editor, terminal, or other workbench
control owns focus. Middle-drag orbits and Shift+middle-drag pans in the current
camera plane. Holding right-mouse enters fly navigation: mouse movement looks,
WASD moves in the camera basis, Q/E moves vertically, Shift accelerates, and
the wheel adjusts fly speed until release or Escape. Outside fly mode the wheel
zooms, and Frame All/Frame Selected reposition the view around the complete
scene or selection.

The top-right **VIEW** navigator snaps to Top, Front, Right, Bottom, Back, or
Left without moving the view target. Its projection chip switches between
Perspective and Orthographic, the highlighted face follows the current camera,
and the complete panel blocks scene picks and orbit/fly input beneath it.
Navigator actions, selection, orbit, pan, zoom, framing, projection, and camera
bookmarks are per-document workspace state: they do not add VSCN history or
dirty the scene.

Move adds filled, outlined **XY**, **XZ**, and **YZ** squares near the primary
pivot. Drag inside a square to translate on both named axes; the status line
names the handle so selection does not depend on color. A plane that is nearly
edge-on is hidden and cannot capture input. Studio solves pointer motion against
both projected axes together, snaps both values from the gesture origin, and
preserves selection spacing. Local planes use parent-relative position axes.
World planes use absolute axes and the runtime's exact matrix conversion in
parent-before-child order. Release creates one history entry; Escape or any
rejected member restores the complete group.

For a multi-node selection, the numeric inspector fields switch to neutral
relative values: position and rotation start at zero and are added to every
node's local transform; scale starts at one and multiplies every local scale.
Apply deltas commits the complete group once and resets the fields to neutral.

The Parent row reparents existing nodes rather than requiring hierarchy
recreation. Its chooser omits every selected root and descendant, rejects moves
that would exceed Studio's bounded hierarchy depth, and disables Reparent for
the unchanged current parent. With several rows selected, selected descendants
travel through their selected ancestor and independent top-level roots move
together in one VSCN transaction. **Keep world transform** is on by default.
The runtime derives new local TRS only when it can reproduce each complete
prior world matrix exactly; a singular destination or conversion requiring
shear rejects and restores the whole canonical scene plus selection. Clear the
option to keep local transforms intentionally. Selection is remapped by live
node identity after preorder changes, and undo restores the exact prior
hierarchy and transforms.

The Sibling order row moves a selected node, or one contiguous selection of
direct siblings, one position Earlier or Later. A block keeps its internal
order, parent, local transforms, and complete multi-selection. Mixed-parent,
ancestor/descendant, gapped, and boundary selections disable the unavailable
actions and remain content/history no-ops. Each accepted block move uses the
runtime's stable child-order primitive, serializes once, and round-trips through
VSCN and undo/redo.

You can perform the same structural work directly in the hierarchy. Drag a row
onto the middle of a target to reparent the selected top-level roots beneath
it; drag into the top or bottom region to place the complete root block before
or after the target. Dragging an unselected row first makes it the sole
selection. Direct drops use the **Keep world transform** setting, cycle/depth
validation, stable sibling ordering, selection remapping, and one canonical
undo transaction. Invalid, lossy, self, descendant, and already-satisfied drops
leave document bytes and history unchanged.

Visibility is group-aware. A uniform selection shows its shared checked state;
a mixed visible/hidden selection shows the native indeterminate mark. Choosing
a determined value sets every selected node, verifies the complete live graph,
and commits one VSCN history entry. Undo restores the exact prior mixture and
the complete node selection.

The hierarchy action strip makes those frequent operations available without
leaving the tree. **● Show** is enabled when any selected node is hidden;
**○ Hide** is enabled when any selected node is visible. **⊘ Lock Pick** keeps
the complete selection visible and editable through the hierarchy and
inspector while excluding it from viewport clicks and marquee selection. A
`⊘` row cue identifies locked nodes, and an all-locked selection changes the
action to **Unlock Pick**. Mixed lock state resolves to locked. These lock
choices follow the owning open scene tab, survive ordinary hierarchy refreshes,
and remap to the same live nodes after reorder or reparent; they do not change
VSCN bytes, dirty state, revision, or undo history. Show, Hide, Lock, and Unlock
are also available from the hierarchy context menu.

Duplicate preserves each selected top-level node's complete serializable
subtree, components, and original parent, gives the clone a unique Copy name,
and offsets its local X position. Delete and Duplicate ignore a selected
descendant when its ancestor is also selected, preventing double removal or
duplicate nested clones. Both are one history transaction. Name and material
controls remain single-selection operations, while visibility is group-aware.
Duplicate Selection
uses `Ctrl`/`Cmd`+`Shift`+`D`; Delete uses the platform Delete key. Both
keyboard routes require hierarchy or viewport/tool focus, so Delete remains
native in inspector text fields and neither command escapes material controls.

Cross-document 3D paste applies the same selected-ancestor collapse as
Duplicate, places the pasted roots beside the primary destination node, assigns
unique root names, offsets local X by one unit, and restores selection across
the complete pasted hierarchy.

The Gameplay metadata group authors durable node data separately from the
display name and rendering components. Select one hierarchy node, choose
String, Integer, Float, Boolean, or Null, enter a non-empty key, and use Set.
Selecting a row loads its exact key, kind, and value for rename or update;
Remove deletes that value. Common uses include `game.role`, stable IDs, spawn
configuration, trigger targets, and component parameters.

Keys are limited to 128 bytes, nodes to 256 values, and string values to 64
KiB. Float values must be finite. Duplicate rename targets, stale selections,
invalid values, rejected bounds, and already-equal edits leave content and
history unchanged. Each accepted create, rename, update, or remove is one
canonical undo entry. Scalar kinds remain exact through VSCN v6, so a float
entered as `4.0` does not become an integer and complete `i64` values remain
lossless. Metadata-row selection belongs to the scene tab and survives bounded
session restore. Arbitrary raw metadata editing remains single-node.

### Reusable Project Components

Put `scene-components.json` at a workspace root to define reusable typed field
groups for `2d-object`, `3d-node`, or `both`. The Project components inspector
automatically uses the schema belonging to the saved scene's longest matching
workspace root. An unsaved scene uses the only open root; Studio does not guess
when several roots are open.

Select one or more objects/nodes, choose a compatible component, and use Add
Missing. Studio preflights every selected item and field. Missing values receive
the schema default, existing values of the same type remain unchanged, and one
wrong-type value rejects the complete action before mutation. Successful
application serializes once and creates one undo entry; a complete component is
a no-op and a failed write or serialization restores the prior scene.

Edit Field copies the selected template field into the ordinary raw property or
metadata controls. The 2D batch property controls can use that draft for a
selection. The 3D raw editor still requires one node, so Edit Field is disabled
for a node group even though Add Missing remains available.

Use **Edit Schema** to expand the project-file authoring form. Its component
dropdown is intentionally unfiltered: either editor can maintain 2D-only,
3D-only, and shared definitions even though the Add Missing picker shows only
compatible components. New/Save/Delete and Earlier/Later controls maintain
component and field identity, labels, descriptions, targets, scalar kinds, and
typed defaults. New Component creates a missing file with a valid starter field.

Each accepted form action reparses and atomically replaces one complete schema
state. Unknown version-1 members on retained JSON objects survive, invalid
drafts leave disk unchanged, and another process's edit causes a conflict
instead of an overwrite. **Undo Schema** and **Redo Schema** retain 20 exact
project-file states independently of scene history; schema work never dirties
the scene. External changes are checked periodically, or use **Reload** to
accept them immediately and clear stale file history.

The schema is a project vocabulary, not a runtime ECS or script attachment.
Game code reads the resulting ordinary typed scene data. The complete JSON
shape, default rules, limits, multi-root behavior, and runtime accessors are in
[scene-components.md](scene-components.md). Renaming or deleting a definition
does not migrate or remove values already stored in scenes.

The 3D inspector's Material component edits base RGB, alpha, metallic,
roughness, ambient occlusion, opaque/mask/blend mode, double-sided, and unlit
state. Create Material adds a PBR material to a node without one. When an
imported material is shared, the action becomes Apply as Unique: Studio clones
the material before changing exposed fields, preserving sibling appearance plus
unexposed texture maps and custom parameters. Applying, removing, undoing, and
redoing are canonical VSCN transactions; unchanged values do not create a
history entry. Keyboard input in the color picker or another inspector control
does not activate W/E/R transform shortcuts.

With several nodes selected, numeric fields use the runtime Spinner's `Mixed`
state, base color and Boolean fields use indeterminate checkboxes, and alpha
mode uses the Dropdown's `Mixed` placeholder. The primary value remains only
as an editing seed. Applying resolves fields that show a concrete value and
leaves every still-mixed field unchanged on each node. Missing materials start
from documented white PBR defaults only when at least one resolved field is
applied. Studio stages all clones before assigning any node, retains the full
selection, and commits the group once; allocation or serialization failure
restores canonical bytes. Removing materials likewise skips missing components
and commits once.

The Texture maps group selects Albedo, Normal, Metallic / Roughness, Ambient
Occlusion, or Emissive independently. Choose Map accepts PNG, JPEG, BMP, GIF,
or strictly validated KTX2 files up to 16 MB and embeds the selected source in
the VSCN document. Raster maps above 16,777,216 decoded pixels are rejected
before they can be retained by scene history. Project Texture Maps searches the
open multi-root workspace and previews supported images before Use Map; the
hierarchy's Project 3D Imports browser does the same bounded search for VSCN,
glTF, GLB, FBX, OBJ, and STL assets. Replacing or clearing a map first clones a
shared material and creates exactly one undo entry. With multiple nodes
selected, one decoded map is staged across the complete selection and Clear Map
removes only that slot wherever present. Clearing an already-empty selection is
a no-op. Map summaries report none/some/all coverage and show a thumbnail only
when the selected nodes share one decoded source identity. The map selector and
buttons retain keyboard focus, so W/E/R
remain ordinary control input rather than changing the transform tool.

The selected assigned slot shows a persistent bounded thumbnail and its source
dimensions. The preview is derived from the live material's decoded pixels, not
the picker path, so it follows VSCN load, undo/redo, import, clone, and
cross-document scene operations. Switching slots reuses the thumbnail while the
material and decoded source identity are unchanged.

### 3D Light Authoring

Use **+ Light** to create a named point-light node, or select one node and use
the Light component to add, inspect, convert, or remove its attached light.
The type picker covers Directional, Point, Ambient, Spot, Rectangle Area,
Sphere Area, and Volume. Common controls author normalized color, intensity,
enabled state, and supported shadow state. Type-specific groups expose the
light-local position and direction, attenuation, decay mode, finite range,
rectangle width/height, sphere or volume radius, and spot inner/outer cone
angles. The node transform remains the coarse placement and orientation;
`SceneGraph.Draw` composes those local light values through the hierarchy.

Studio reconstructs a complete independent light for each accepted edit instead
of mutating the attached runtime object. This preserves imported components
that share identity. A normalized replacement equal to the current light is a
no-op. Add, apply or type conversion, and remove each serialize once and create
one VSCN history entry; save failure reloads the preceding canonical scene.
Undo, redo, reopen, duplicate, and cross-document subtree operations therefore
use the same persisted light component rather than editor-only metadata.

Hierarchy rows carry a `[light]` badge, and a meshless light uses the `✦`
identity marker. The viewport draws the authored color and enabled state,
connects a light-local offset to the node origin, shows applicable direction,
and frames a bounded finite range. Light-only nodes remain pickable at their
actual transformed light position. The viewport status and accessibility text
report the authored light count.

Light editing is deliberately single-node. With zero or multiple nodes
selected, apply/remove controls are disabled and the inspector explains the
scope instead of treating one primary light as a batch value. VSCN may preserve
more lights than the preview backend can activate: the runtime submits up to 16
on software and 64 on clustered GPU backends and exposes dropped-light
telemetry. Studio does not delete or impose an obsolete eight-light authoring
cap.

Both editors retain invalid source bytes unchanged and explain the parse/load
failure instead of replacing the document. Current authoring limits include no
tagged asset library/import-settings pipeline, cubemap/lightmap picker,
automatic component-schema/scene-data migration, generalized runtime component
composition, advanced Tiled margin/spacing or
image-collection editing, or tile animation/collision/metadata editing. The 3D
Local/World
control follows the scene tab: Local uses the parent-relative basis of the
local TRS fields, while World aligns to absolute axes and applies one snapped
delta around each selected node's own pivot. World operations commit only when
the runtime can reproduce every requested matrix as exact parent-relative TRS;
any singular or shear-producing member restores the complete group. Hierarchy
reparenting is available
through both the explicit Parent chooser and direct before/into/after row
dragging, with exact preserve-world behavior by default and a preserve-local
opt-out.

Move and Scale draw conditioned XY/XZ/YZ plane squares in the active Local or
World basis. Scale squares add crossed diagonals so the active operation is
visible without relying on color. A plane drag solves both projected axes
together; one complete handle width means one scale unit per axis in Scale
mode. Optional snapping resolves each target from the immutable primary origin
at 0.1 steps. Every selected node receives the same additive two-axis delta.
World scaling applies each delta around that node's own pivot in hierarchy
order and commits only if every requested matrix remains exact parent-relative
TRS; shear, singularity, Escape, or commit failure restores the complete group.

Rotate draws projected X/Y/Z rings in the active Local or World basis. Dragging
a ring decodes angular motion on its complete projected ellipse, including
wrap-safe movement across the ±180-degree seam. Optional snapping quantizes the
result to 15-degree steps. Nearly edge-on rings are omitted and cannot capture
input; one accepted group gesture creates one undo entry, while Escape or an
inexact World conversion restores the complete selection.

## Settings

Settings are stored in `settings.ini` in the platform config directory. Current
settings include:

- Editor font size and optional font path.
- Theme.
- Minimap visibility.
- Live diagnostics.
- Code folding.
- Status bar visibility.
- Fullscreen.
- Word wrap.
- Line numbers.
- Insert spaces.
- Trim trailing whitespace.
- Ensure final newline.
- Auto-save.
- Save all before build/debug.
- Restore session.
- Restore last project.
- Confirm on exit with unsaved changes.
- Diagnostics delay.
- Completion delay.
- Tab width.
- UI zoom.

Unknown INI sections are preserved when settings are saved.

The settings file is deliberately human-editable. If the UI gets into a bad
state, users can close the IDE and edit or remove the relevant key. New settings
should keep this property: simple values, clear defaults, validation on load,
and compatibility with older files.

## Session Restore And Recovery

On shutdown, Zanna Studio stores:

- Last project root.
- Open tabs.
- Active tab.
- Cursor and scroll state.
- Recent projects.
- Recent files.
- Bounded recovery content for modified editable text buffers.

Recovery content is capped at 200000 characters per document. Read-only previews
and oversized modified buffers are not persisted as recovery text.

## Troubleshooting

### Build says it cannot find `zanna`

Set `ZANNA_BINARY` to the full path of the desired `zanna` executable or add the
binary to `PATH`.

### Completion or diagnostics feel stale

Save or wait for the editor debounce interval, then run "Run Check Now". For
project navigation, allow workspace indexing to finish or narrow the workspace.

### BASIC semantic commands feel incomplete

BASIC definition, references, rename, workspace symbol, call hierarchy, and
signature help are scanner-backed. Project navigation now runs in the background,
but ambiguous/dynamic code can still need compiler-only semantic context. A
status ending in `(limited)` means the explicit workspace/result safety budget
was reached; narrow the workspace. Rename never applies in that state.

### A scene opens in the wrong surface

Use `.scene` or `.level` for the 2D editor and `.vscn` for the 3D editor.
Unknown scene-like extensions intentionally fall back to the text editor because
Studio cannot safely infer a serialized format from file contents alone.

### Terminal is unavailable

The runtime PTY layer may be unavailable on the platform or unable to launch the
configured shell. The terminal panel reports `Zanna.System.Pty.LastError()` when
startup fails.

### Source Control actions appear to hang

Source Control commands run as background `Zanna.System.Process` jobs, but the
view only pumps one active Git job at a time. Local status, stage, unstage,
commit, and diff are expected to be quick. Push and pull can still wait on
network, authentication, or remote state and currently expose only basic
status/error text.

### A command is visible but unavailable

Some commands stay visible in the command palette so users can discover that the
feature exists for another language or state. The command registry should report
a status/toast reason. For example, Rename Symbol is available for Zia but not
for BASIC.

### A scene file does not show visual tools

Confirm the extension is `.scene`, `.level`, or `.vscn`. Unknown scene-like
extensions intentionally remain text documents because Studio cannot safely
infer their serialization format. If a recognized file opens the visual
surface but shows a repair message, the original bytes failed bounded scene
parsing and remain unchanged; repair the source or create a new scene.
