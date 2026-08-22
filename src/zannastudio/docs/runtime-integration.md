# Zanna Studio Runtime Integration

Zanna Studio is implemented in Zia, but most platform and GUI behavior is supplied
by the Zanna C runtime. This document records the runtime contracts the IDE uses
today and the important constraints at that boundary.

## Runtime Registry

Runtime APIs are registered through `src/il/runtime/runtime.def`. When adding or
changing runtime functions used by Zanna Studio, update:

- The C runtime implementation and header.
- `runtime.def`.
- Structured class bindings if a method/property surface changes.
- Graphics and non-graphics stubs where applicable.
- Runtime completeness checks.
- Zanna Studio docs and probes that cover the behavior.

Runtime ABI or cross-layer dependency changes require the repository's normal
ADR process.

## GUI Runtime

Main GUI families used by Zanna Studio:

- `Zanna.GUI.App`
- `Zanna.GUI.Window`
- Menus, menu items, buttons, labels, boxes, splitters, tabs, list boxes,
  tree views, inputs, check boxes, sliders, command palette, and toasts.
- `Zanna.GUI.CodeEditor`
- `Zanna.GUI.OutputPane`
- `Zanna.GUI.VirtualList`
- `Zanna.GUI.VirtualTree`
- GUI test harness helpers used by probes.

### CodeEditor

`CodeEditor` is the primary editing widget. Zanna Studio relies on:

- Text get/set and revision tracking.
- Cursor and selection state.
- Syntax/language mode.
- Line numbers, word wrap, minimap, folding, and gutter markers.
- Multi-cursor methods.
- Inline diagnostics/highlights.
- Performance counters used by probes.

IDE-side controllers should use editor revisions to avoid repeated full-buffer
copies. When adding semantic features, take a snapshot once per relevant
revision and reject stale results when the revision changes.

### ListBox

Retained ListBox rows may attach byte-exact string identities with
`ItemSetData`. `GetSelectedData()` returns a fresh `Seq[String]` containing one
entry per selected retained row in row order, including an empty entry when a
selected row has no data. It returns an empty sequence for invalid handles,
empty selection, and virtual mode. List-oriented Studio surfaces use this
contract instead of parsing labels. ADR 0156 defines the ABI and ownership
behavior; both scene hierarchies now use the retained TreeView counterpart in
ADR 0163.

### ScrollView

`ScrollView.ScrollTo(widget)` reveals any live descendant without changing
focus or widget ownership. The runtime validates both handles and treats null,
stale, wrong-type, foreign, and non-descendant targets as no-ops. The toolkit
owns nested geometry, scrollbar-reduced viewport size, axis-specific offsets,
and clamping. A live-ID-guarded request survives the next layout pass, so a
controller may restore a collapsed split pane and request descendant reveal in
the same command. ADR 0165 defines the additive C/runtime contract.

### OutputPane

`OutputPane` is used for:

- Build/run output.
- Terminal display and input capture in terminal mode.
- Some debug and textual panel surfaces.

Relevant methods include append, append line, append styled text, clear,
selection, select all, max-line limit, line count, font, auto-scroll,
terminal-mode toggle, input draining, cell measurement, `ColumnsForWidth()`,
and `RowsForHeight()`.

OutputPane stores logical lines in a bounded ring buffer. Appending beyond the
max-line limit evicts the oldest logical line without shifting the whole line
array, so long build logs and terminal sessions do not pay an O(n) cost per
eviction.

OutputPane terminal mode implements the bounded VT subset exercised by the
integrated-shell conformance corpus: alternate screens, scrolling regions,
line/cell edits, delayed autowrap, origin-relative cursor addressing, tab stops,
cursor/application modes, bracketed paste, status replies, and xterm-style key
encoding. It remains a deliberately bounded emulator rather than a claim of
complete VT/xterm compatibility.

### VirtualList And VirtualTree

`Zanna.GUI.VirtualList` and `Zanna.GUI.VirtualTree` are currently lightweight
runtime helpers for row ids, selection, counts, expansion, and visible-range
calculation. Most Zanna Studio tool panels are still rendered through concrete
ListBox/OutputPane widgets and should not be documented as fully virtualized
until the UI uses virtual row realization end to end.

## Process Runtime

`Zanna.System.Process` powers:

- Build jobs.
- Run jobs.
- Debug adapter process launch.
- Git Source Control jobs.

Zanna Studio uses:

- `StartWithEnvOverlay(program, args, cwd, env)` for Studio-authored overrides
  while preserving the host environment.
- `ReadOutputResult()` for ordered, stream-tagged build output.
- `ReadStdoutResult()`.
- `ReadStderrResult()`.
- `WriteStdin()`.
- `IsRunning()`.
- `ExitCode()`.
- `Kill()`.
- `Destroy()`.
- `GUI.App.WatchProcess(handle)` after each launch, so output readiness and exit
  interrupt the native event wait without polling the UI thread.

Important constraints:

- Jobs are started with explicit argv sequences, not shell strings.
- Process startup PATH-searches bare names, while Studio additionally resolves
  installed/developer `zanna` layouts and project-relative manifest tools so
  launch behavior is independent of Studio's own current directory.
- Windows stdin is accepted through a bounded background writer; UI frame code
  never performs a potentially blocking pipe write.
- Each process handle owns a POSIX process group or Windows Job Object, so Stop
  and shutdown terminate ordinary compiler/game/debugger descendant trees.
- IDE-side retained output is bounded to avoid runaway memory usage.
- Runtime process buffers are finite. Zanna Studio uses result reads so overflow
  returns `{ text, truncated }` instead of trapping the IDE; callers append a
  visible truncation marker and still keep their own retained-output caps.
- One shared tagged runtime queue backs stdout, stderr, and ordered reads; bytes
  are not retained in three native buffers. The activity monitor coalesces one
  wake until Studio drains and rearms it. Monitor-allocation failure is explicit
  and selects Studio's four-millisecond fallback only for that process.

## PTY Runtime

`Zanna.System.Pty` powers the integrated terminal.

Zanna Studio uses:

- `Pty.IsSupported()`.
- `Pty.LastError()`.
- `Pty.OpenWithEnvOverlayResult(program, args, cwd, env, cols, rows)` with
  `TERM=xterm-256color` and `COLORTERM=truecolor`, preserving the inherited
  executable path, home, locale, and shell environment.
- `PtySession.ReadResult()`.
- `PtySession.Write()`.
- `PtySession.Resize()`.
- `PtySession.IsRunning()`.
- `PtySession.ExitCode()`.
- `PtySession.Kill()`.
- `PtySession.Destroy()`.
- `GUI.App.WatchPty(session)` after open.

Platform behavior:

- POSIX uses the runtime PTY implementation.
- Windows uses the runtime ConPTY path when available.
- Unsupported platforms report through `IsSupported()` and `LastError()`.

IDE constraints:

- The terminal controller derives columns and rows from OutputPane cell metrics
  through `ColumnsForWidth()` and `RowsForHeight()`.
- Hidden panels do not auto-start shells, but already-running PTY sessions are
  drained into a bounded replay buffer and flushed when the terminal becomes
  visible again.
- Output deltas are bounded per update. Runtime and frame-level truncation are
  surfaced in the terminal stream instead of being dropped silently.
- Stop kills and destroys the PTY session.
- Restart destroys the previous session and opens a new one.
- PTY readiness uses the same coalesced App wake contract; EOF retires the
  monitored descriptor so a closed slave cannot create a HUP wake loop.

## Exec Runtime

`Zanna.System.Exec` is still available to probes and small helper code, but the
main Zanna Studio build/run/debug/SCM paths use `Zanna.System.Process` when stdout,
stderr, exit code, cancellation, and non-blocking UI behavior matter.

Current constraints:

- `Exec.CaptureArgs` captures stdout but does not provide a reliable exit code.
- `Exec.RunArgs` provides an exit code but lets child output go to inherited
  stdout.
- Do not use `Exec` for new long-running IDE workflows.
- Do not use shell strings when an argv-sequence API can express the command.

## Workspace Runtime

### FileIndex

`Zanna.Workspace.FileIndex` is used for:

- Project tree exclusion checks.
- Quick Open cache population.
- Project search enumeration.
- Completion workspace-symbol source discovery.
- Zia workspace indexing source discovery.

It applies hard excludes, `.gitignore` support implemented by the runtime, and
project manifest ignore/exclude patterns.

`FileIndex.Page(root, extensionsCsv, excludesCsv, includeDirs, offset, limit)`
returns bounded pages with the same entry shape as `Enumerate`. The API is
stateless from the caller's perspective. The runtime may keep a private
process-local cursor for sequential calls with the same key and offset so large
workspace consumers can avoid repeatedly walking from the root. Callers must
still treat `nextOffset`, `done`, and `truncated` as the protocol; there is no
public cursor handle to close.

User-facing project search and completion workspace-symbol discovery must use
`Page`-backed discovery. A visible explorer tree or partially warmed Quick Open
cache is allowed as a UI hint, but it is not authoritative enough for complete
workspace queries.

### Workspace.Edit

`Zanna.Workspace.Edit.ApplyInRoot` is used for existing-file saves and
workspace-edit operations. Zanna Studio depends on it to validate existing file
metadata, stage temporary writes, commit replacements, and report failure
without leaving a truncated target file.

## Language Runtime

### Zia

Zanna Studio uses structured Zia runtime APIs for:

- Completion.
- Diagnostics/toolchain checks.
- Hover.
- Signature help.
- Symbols.
- Project indexing.
- Definition, references, call hierarchy, and rename edits.

Zia work should use structured records and handles. Avoid adding new
tab-separated, colon-separated, or display-row-parsed contracts when a map,
sequence, or stable handle can be returned.

### BASIC

BASIC uses `Zanna.Basic.LanguageService` for in-process completion,
diagnostics, hover, and document symbols. Project navigation, references,
rename, workspace symbols, call hierarchy, and signature help are implemented by
the IDE-side BASIC semantic scanner in `zannastudio/src/basic/semantic_scan.zia`.
That keeps the public runtime ABI unchanged while giving BASIC documents the
same editor command surface as Zia.

## Game Scene Runtime

`Zanna.Game2D.SceneDocument` exists in the runtime and is covered by IDE-facing
probes for scene data behavior. Relevant scene runtime capabilities include loading,
saving, JSON round-trip, diagnostics, scene-owned mutators, properties, and
tilemap render-copy creation. ADR 0158 adds deterministic `Keys()`, exact
`PropertyKind(key)`, and explicit `SetNull(key)` operations for scene-level
metadata. These mirror the object-property authoring surface so Studio can
render and edit typed level configuration without parsing canonical JSON or
coercing null/Boolean/numeric values through strings.

ADR 0171 adds `FloodFill(layer, x, y, tile)` for Studio's contiguous tile tool.
The runtime owns the potentially million-cell four-connected traversal,
allocates its complete bounded queue and visited storage before mutation, and
returns the exact changed-cell count. Invalid, already-equal, and allocation
failure paths return zero without a partial edit. Rectangle paint continues to
use `FillTiles`; Studio owns preview, history, cancellation, and picker state.

ADR 0164 adds `ObjectParent(index)` and
`TrySetObjectParent(index, parent)` as the formal 2D organizational hierarchy.
The runtime rejects invalid links and cycles, keeps parent indices correct
through move/remove/duplicate operations, and stores non-root links in the
reserved typed version-1 property `zanna.hierarchy.parentIndex`. Generic object
property APIs hide and protect that key. Studio therefore composes row drops
against runtime-owned invariants while keeping object positions absolute and
retaining compatibility with older version-1 readers.

## Debug Adapter Protocol

`zanna run --debug-adapter <file>` speaks a newline-delimited JSON control
protocol. Commands arrive on stdin as one JSON object per line; events are
emitted on stderr, each prefixed with the `@@VDBG@@ ` sentinel so they stay
distinct from the debuggee's own stderr. `DebugSession` in
`zannastudio/src/build/debug_session.zia` is the IDE-side client.

### Stop events

A `stopped` event carries the top-frame `locals` array. Each local is:

```json
{ "name": "nums", "value": "List(3)", "type": "List", "varRef": 4, "childCount": 3 }
```

- `varRef` is `0` for a leaf (scalar, string, class instance) and `> 0` for a
  composite that can be expanded. `childCount` is the number of children a
  composite has (`0` for leaves).
- These two fields are **additive**: a client that ignores them sees the
  historical flat `{name,value,type}` locals unchanged.

### Expanding a composite

Send a `variables` command while stopped to fetch one level of children, paged:

```json
→ { "type": "variables", "varRef": 4, "start": 0, "count": 100 }
← { "type": "variables", "varRef": 4, "start": 0,
    "vars": [ { "name": "[0]", "value": "10", "type": "i64", "varRef": 0, "childCount": 0 }, ... ] }
```

Rules:

- **Lazy, one level per request.** A child that is itself a composite carries its
  own `varRef > 0`; the client re-requests to drill down. The adapter never
  recurses, so depth is bounded by how far the user expands.
- **Refs are valid only at the stop that produced them.** They are backed by a
  per-stop provider that is discarded when execution resumes (continue / step /
  terminate), matching DAP semantics. Requesting a stale or unknown ref yields an
  empty `vars` list rather than an error.
- **Paging.** `start`/`count` select a window (the IDE requests `<= 100` at a
  time). When `count` is omitted the adapter defaults to 100.

### What is expandable

Value formatting during a stop never invokes user code (no `toString` dispatch),
because the adapter runs on the paused VM thread. Consequently:

- `List`, `Seq`, and the canonical `Map` expand into their elements / key-value
  pairs. Boxed primitives and strings render with their real values.
- Class instances and other collection kinds are **leaves** (`varRef 0`,
  displayed as `<TypeName>`); per-field expansion would require a runtime
  reflection ABI and is future work.

Watch/evaluate results (`evaluated` events) are always scalar leaves today and
carry `varRef: 0` / `childCount: 0` so the IDE can render locals and watches
through one code path.

Zanna Studio mounts built-in 2D and 3D scene editors for the corresponding
scene document kinds. They retain per-document workspace state and history,
provide hierarchy/layer and property editing, render interactive
canvas/viewport previews, search bounded project assets, and serialize through
the runtime scene document surface. The 3D editor's canonical text path is
fully in-memory (ADR 0190): every accepted edit derives `Document.content`
from `SceneGraph.SaveToText`, and document loads flow through
`SceneAsset.LoadTextResult` with the document path naming the relative-prefab
base directory — no staged sibling files are created. Standard Edit commands route to the active
scene controller. Standard Find uses the shared bounded hierarchy matcher and
`ScrollView.ScrollTo` to reveal the query or selected row without mutating scene
data. A versioned, typed text envelope carries canonical source plus
stable selection identities; controllers validate it before reconstructing
selected 2D objects or 3D subtrees as a single history transaction. Richer
runtime component composition, advanced tileset metadata and animation, and
play-in-editor remain product gaps. Projected rotation-ring and
Move/Scale-plane geometry and input remain
controller-side because they operate on already-exposed camera projection and
transform surfaces. Existing batch
inspector operations stay controller-side: 2D typed property set/remove uses
`SceneDocument` property APIs, while 3D relative transforms mutate selected
`SceneNode` local values before one canonical serialization.

The 2D labeled History list is likewise controller presentation over the
existing bounded canonical snapshots. Tile-palette zoom/search needs no new
runtime or ABI surface: Studio scales the existing nearest-filtered `GUI.Image`,
maps pointer coordinates back through that presentation ratio, and uses
`ScrollView.SetScroll` to reveal a validated numeric frame.

The 2D canvas rulers and guides also require no runtime or ABI surface. Studio
renders bounded `Pixels` into ordinary top/left `GUI.Image` widgets, derives
ticks from the same logical scroll origin, display-cell size, and centering
padding as the windowed canvas, and maps ruler pointer coordinates back to
cell boundaries. Guide coordinates live only in `Scene2DWorkspaceState`; their
lines are drawn with the existing editor overlays after the tile grid.

ADR 0205 adds an opt-in, application-directed retained `GUI.ListBox` reorder
surface: `SetReorderable`, `WasReorderRequested`,
`GetReorderSourceIndex`, and `GetReorderTargetIndex`, with matching additive C
ABI entry points. The lower control owns threshold recognition, pointer
capture, insertion feedback, edge scrolling, Escape cancellation, and
Alt+Arrow requests, but never mutates row linkage or revision. Studio consumes
the request and applies the existing `SceneDocument.MoveLayer` operation
through one canonical transaction. Lock and Solo indices remain only in
`Scene2DWorkspaceState`; Studio remaps them around live layer edits and clears
them on canonical reload. No scene schema or serialization surface changes.

The 3D corner orientation navigator likewise adds no runtime or ABI surface.
`SceneEditor3D` draws its six camera targets, projection chip, and XYZ cue into
the existing post-readback editor-overlay pixels. Its hit geometry shares those
exact dimensions, receives pointer priority ahead of gizmos and scene picking,
and updates only the existing per-document camera workspace fields.

The hierarchy action strip and pick-lock row cues also use only existing GUI
and scene surfaces. Buttons and context-menu items dispatch to the established
one-serialization visibility transaction; `TreeView.Node.SetText` refreshes
the `⊘` presentation cue. Pick paths remain process-local in
`Scene3DWorkspaceState`, while live structural refreshes temporarily retain
`SceneNode` identities to remap or prune those paths. No runtime class, C ABI,
VSCN schema, or serialization surface changes.

ADR 0168 adds `Canvas3D.NewOffscreen(RenderTarget3D)` and read-only
`IsOffscreen`. `SceneEditor3D` retains one windowless canvas, explicit target,
and projection-switchable camera sized to the embedded GUI image. Orthographic
half-height is `viewportHeight / (2 * pixelsPerUnit)`; the perspective eye
distance derives from the same target scale, and both projections match
Studio's marker/gizmo coordinates exactly. `SceneGraph.Draw` therefore
supplies the authored hierarchy, visibility, transforms, meshes, PBR
materials, maps, and scene lights for both shaded and triangle-wireframe modes.
Studio reads the target back only when scene or camera state is dirty, then
draws the editor grid, hierarchy links, node markers, selection, and gizmos on
that copy. A target/readback allocation failure retains a deterministic marker
fallback and never changes VSCN content.

ADR 0204 reuses the existing `PostFX3D` and `Canvas3D.SetPostFX` surface for
project-owned scene-preview recipes. `SceneEditor3D` retains one validated
portable chain per schema/canvas and attaches it only to the main shaded
viewport. A render target's `AsPixels()` view is pre-finalization, so an active
chain is read with `Canvas3D.ScreenshotFinal()` instead; this is the boundary
that actually runs tonemap, bloom, color grade, vignette, and FXAA. Editor
overlays are composed onto that returned `Pixels` object afterward. This adds
no runtime ABI: pure wireframe detaches the chain, and schemas omit
backend-dependent or temporal effects.

ADR 0191 adds `Canvas3D.NewOffscreenAccelerated(RenderTarget3D)`: the same
windowless contract, but requesting the platform GPU backend with the standard
software fallback and truth channel (`BackendName`, `IsBackendFallback`).
Studio's viewport requests acceleration at startup
(`ZANNA_STUDIO_SOFTWARE_VIEWPORT=1` opts out); probes never request it, so
their pixels stay byte-deterministic. Camera framing always derives from the
logical viewport, letting interactive camera/gizmo drags render into a
reduced-resolution target that Studio upscales before drawing overlays — a
full-resolution frame re-renders on release. Metal creates a layer-free
context, D3D11 creates a device without an HWND/swap chain (hardware then
WARP), and Linux OpenGL creates a surfaceless EGL pbuffer without requiring
Wayland integration. Missing platform GPU facilities still fail construction
cleanly and select the truthful software fallback.

ADR 0172 exposes the light component already retained and serialized by the
native scene graph as a typed read/write `SceneNode.Light` property. The setter
retains valid `Light3D` values, accepts `null` to clear, and rejects a wrong
runtime class before mutation. `Light3D.InnerConeDegrees`,
`OuterConeDegrees`, and `SetSpotCone(inner, outer)` close the spot-light
round-trip gap between degree-based constructors and cosine-based renderer
storage; the paired setter sanitizes both values and advances mutation state
once.

Studio treats that public surface as the only canonical light boundary.
`scene_light_3d` reconstructs a complete independent object for all seven
types, and `SceneEditor3D` assigns it only after normalized no-op comparison.
This is required because imported scene templates may share mutable components.
Add/apply/remove then reuse the existing single VSCN serialization, rollback,
dirty-state, and history path. Hierarchy labels and viewport light markers read
the live attached component and remain workspace presentation. Multi-selection
is disabled until a future sparse batch-light contract can preserve each
unresolved type-specific field.

ADR 0169 adds canonical `Zanna.Input.Key.LeftSuper` and `RightSuper` constants
for Command/Windows-key state without exposing backend-private key codes.
Studio treats either Control or Super side as the primary selection modifier.
The same ADR keeps viewport picking on runtime geometry: `Camera3D`
unprojection supplies the ray and `SceneGraph.RaycastNodes` supplies the
closest visible transformed mesh-bounds hit. Studio owns selection policy,
meshless marker fallback, and camera-plane pan because those are editor
workspace interactions; none mutate the runtime scene or canonical VSCN.

ADR 0166 adds `SceneNode.TrySetWorldMatrix(Mat4)`. Studio's World transform
space constructs a complete translated, rotated, single-axis-scaled, or
two-axis-scaled matrix around each selected node's own pivot and asks this
runtime boundary for an exact parent-relative TRS. Invalid Mat4 handles,
singular parents, degenerate or projective bases, shear, and decomposition
drift reject before mutation.
Studio captures every selected local TRS first and restores the complete group
if any member rejects, so a world command or live drag is one canonical
transaction rather than a sequence of partial component writes.

ADR 0157 adds read-only decoded `*MapPixels` properties to `Material3D`.
Studio uses those managed borrowed views for bounded assigned-map thumbnails,
so the inspector follows the canonical material after load, undo/redo, import,
clone, and VSCN round trips without parsing scene text or caching picker paths.

The shared project asset browser requires no additional runtime ABI. Ordinary
image cards use bounded `Pixels.Load`/`Resize`; 3D cards reuse
`SceneAsset.LoadResult`, subtree `BoundsMin`/`BoundsMax`, windowless
`RenderTarget3D`/`Canvas3D`, and `Camera3D.LookAt`. The browser schedules only
cards intersecting its live scroll viewport and uploads at most one resulting
square per frame. `GUI.Image.SetPixels` copies the upload into widget-owned
storage, so temporary asset, canvas, camera, and Pixels handles do not become
long-lived browser state.

ADR 0167 adds `Spinner.SetIndeterminate(Boolean)` and
`Spinner.IsIndeterminate()`. A mixed spinner retains the primary node's bounded
numeric value only as an editing seed and displays `Mixed`; concrete assignment
or user input resolves it. The 3D material inspector combines that state with
checkbox indeterminate values and Dropdown's no-selection placeholder to build
a sparse material patch. `SceneEditor3D` stages every required clone before
mutation, reuses one staged clone for selected nodes sharing a source material,
preserves unresolved PBR fields/maps per node, and commits the complete batch
once. Batch map assignment decodes one bounded source and follows the same
stage-before-mutate rule.

`SceneNode.Name` follows the runtime's owned string-return convention. The
native getter retains the node's stored name and the graphics-disabled stub
returns an owned empty string; native callers must balance the result with
`rt_string_unref`. This keeps repeated Studio hierarchy and inspector refreshes
from consuming the node-owned name reference.

ADR 0159 adds bounded typed gameplay metadata directly to `SceneNode`.
`MetadataKeys`, `MetadataKind`, `MetadataHas`, typed getters/setters, explicit
null, and removal let Studio render and transact against the live scene model
without parsing VSCN. Keys are deterministically ordered; setters report bound
or validation rejection. A metadata-bearing graph serializes as VSCN v6 with
tagged scalar values and decimal-string integers, preserving complete `i64`
values and the distinction between an integer and an integral-looking float.
Studio's presentation-only metadata inspector returns intent to
`SceneEditor3D`; that controller owns validation, one-step history, rollback,
dirty state, and per-document selection persistence.

ADR 0160 layers project-defined `scene-components.json` templates over those
existing typed runtime APIs without adding another runtime or scene-format
surface. Studio parses the bounded root-local schema, preflights a complete
selection, writes only missing values through `SceneDocument.ObjectSet*` or
`SceneNode.MetadataSet*`, verifies their exact kinds, and serializes once.
Same-kind authored values remain untouched; one conflict or rejected write
aborts the complete component action. Games consume ordinary persisted scene
data and remain responsible for mapping stable keys to gameplay systems.
Structured schema authoring stays entirely on the Studio project-file side:
raw version-1 JSON edits are reparsed, atomically persisted with optimistic
conflict checks, and kept in a separate bounded file history. It adds no runtime
component surface and cannot dirty or revise the active scene.

ADR 0161 adds allocation-free `SceneNode.TryMoveChild(child, index)` for stable
direct-sibling ordering. It requires an existing direct child and a strict
index, validates the complete native child table before mutation, and leaves
parent links, ownership, and retained references unchanged. Studio's
Earlier/Later controls use it only after proving the current selection is one
contiguous same-parent block. A complete block move is then serialized once;
any runtime or serialization rejection restores the prior canonical graph and
selection.

ADR 0162 adds
`SceneNode.TryAddChildPreserveWorld(child)` as the exact hierarchy-conversion
primitive. It computes the prospective local matrix from the inverse
destination world matrix, accepts only finite non-degenerate orthogonal TRS,
and verifies that recomposition reproduces the child's complete prior world
matrix. Reflections retain a signed scale; singular destinations and required
shear return false before mutation. Studio enables this path by default in the
Parent chooser, offers `TryAddChild` as the explicit preserve-local mode, and
restores canonical VSCN plus selection if any root in a group or the final
serialization fails.

ADR 0163 completes the shared retained `TreeView` contract needed by scene
outliners. `SetMultiSelect` adds Ctrl/Command toggle, visible Shift ranges, and
additive programmatic restoration while `GetSelectedData` returns byte-exact
node data in complete retained preorder. `SetDragDropMode(2)` latches
row-aware before/into/after targets (`0/1/2`) without mutating the widget;
mode 1 and `SetDragDropEnabled(true)` retain the Explorer's container-only
into behavior. Studio maps each 2D and 3D row to its current scene identity.
The 2D controller turns one latch into a stable subtree order/parent
transaction; the 3D controller turns it into a validated preserve-world
reparent/order transaction.

## Cross-Platform Rules

Runtime and IDE changes must remain cross-platform:

- Use `src/common/PlatformCapabilities.hpp` or `src/runtime/rt_platform.h` for
  platform decisions in C/C++.
- Raw platform preprocessor checks belong only in approved adapter layers.
- Zia code should use runtime platform APIs such as `Zanna.System.Machine`,
  `Zanna.IO.Path`, and runtime process/PTY abstractions.
- Do not introduce external product dependencies.

Cross-platform-sensitive changes should run:

```sh
./scripts/lint_platform_policy.sh
```

## Runtime Boundary Checklist

When changing a runtime API for Zanna Studio:

- Keep the IL/runtime spec and ADR requirements in mind.
- Update C implementation, headers, `runtime.def`, and stubs.
- Verify native and VM paths when both are relevant.
- Add focused runtime tests.
- Add or update a Zanna Studio probe when the app depends on the behavior.
- Document limitations in this file.
