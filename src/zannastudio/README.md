# Zanna Studio

Zanna Studio is the repository IDE application for editing, building, running, and
debugging Zanna projects. It is written in Zia and runs on the `Zanna.GUI.*`,
`Zanna.System.*`, `Zanna.Workspace.*`, and language-service runtime surfaces.

This directory is application code, not a design archive. The current
documentation is:

- [docs/status.md](docs/status.md): current feature status and known gaps.
- [docs/workflows.md](docs/workflows.md): user and developer workflows.
- [docs/architecture.md](docs/architecture.md): source layout, ownership, and
  layering rules.
- [docs/source-map.md](docs/source-map.md): detailed module-by-module source
  guide.
- [docs/runtime-integration.md](docs/runtime-integration.md): runtime APIs used
  by the IDE and the constraints they impose.
- [docs/maintenance.md](docs/maintenance.md): how to change Zanna Studio safely.
- [docs/testing.md](docs/testing.md): probes, CTest entries, manual checks, and
  coverage gaps.

## Current Scope

Zanna Studio is currently a project-aware code editor and workbench for Zia and
Zanna BASIC. It includes:

- Multi-tab text editing backed by `Zanna.GUI.CodeEditor`.
- Zia completion, diagnostics, hover, signature help, document symbols,
  definition, references, rename, and selected refactors.
- BASIC completion, diagnostics, hover, signature help, document symbols,
  definition, references, rename, call hierarchy, and workspace symbols.
- Project explorer with file-type icons, multi-root workspace support, Quick
  Open, project search with project-wide replace, workspace symbols, and
  recent files.
- Side-by-side diff view (compare with saved, HEAD, or any commit).
- Build and run jobs through `Zanna.System.Process`; a new-project wizard
  with console/GUI/library templates.
- VM-backed debug adapter launch through `zanna run --debug-adapter`, with
  structured expansion of collections and class-instance fields.
- Integrated PTY terminal through `Zanna.System.Pty`, emulating the
  alternate-screen/scroll-region/mode sequences full-screen programs emit.
- Git source-control view: status, staging, commit, paged history, queued
  operations, and PTY-backed push/pull with in-app credential prompts.
- Rebindable keyboard shortcuts, settings search, session restore, settings
  persistence, external-change detection, and crash-recovery snapshots for
  small modified text buffers.
- Direct persisted workspace layout: drag the primary sidebar and its activity
  rail between left and right, drag/merge the primary tool group, split selected
  tools into simultaneous left/bottom/right groups or one movable, resizable
  in-window floating group from their headers or the Command Palette, reorder
  tabs within groups, resize active splits, or recover the defaults with Reset
  Workspace Layout.

The app includes built-in visual authoring surfaces for scene documents.
`.scene2d` and `.scene3d` are the canonical extensions (ADR 0182); the legacy
`.scene`, `.level`, and `.vscn` spellings remain recognized. The 2D editor covers layers, real atlas rendering, a tile
palette, captured cancelable freehand paint/erase, inclusive rectangle paint,
four-connected bucket fill, active-layer tile picking, placed objects,
scene-wide and per-object typed properties, point and inclusive cell-marquee selection with
replace/Shift-add/Ctrl-or-Command-toggle behavior, stable multi-object
selection, focus-safe pixel/tile nudging, primary-axis alignment,
deterministic distribution, a true expandable multi-selection object
TreeView, transactional before/into/after subtree drops, one-step **Add
Root**/**Add Child** creation, an explicit cycle-safe Parent chooser for
selected subtree groups, hierarchy-preserving duplicate/paste, batch
drag/delete and typed-property set/remove, Tiled import, and document history.
Pressing an already-selected canvas object preserves the complete group for
dragging. Canvas selection and its visible box remain workspace-only state;
they do not dirty or rewrite the scene. Object positions remain absolute while
organizational parent and sibling order round-trip through canonical version-1
scenes.
Scene-wide metadata selection is retained independently per tab and session.
The 3D editor covers hierarchy, primitive/model import, camera framing,
multi-node Move/Rotate/Scale with per-scene Local/World axes, exact
parent-relative world-matrix conversion and complete-group rollback,
filled Move-plane and crossed Scale-plane XY/XZ/YZ handles with conditioned
two-axis dragging, relative numeric batch transforms, projected X/Y/Z rotation
rings with wrap-safe angular dragging, subtree-aware batch duplicate/delete,
cycle-safe
exact preserve-world reparenting with a preserve-local opt-out, a true
expandable multi-selection TreeView with
transactional before/into/after drag reparenting and sibling ordering,
mixed-state batch visibility, truthful mixed-value batch PBR materials,
batch embedded texture maps,
single-node authoring for all seven runtime light types with hierarchy and
viewport markers, typed per-node gameplay metadata, and history. Node roles,
IDs, trigger/spawn settings, and
component parameters can be authored as null, Boolean, integer, float, or
string values without encoding them in display names; metadata selection stays
with its scene tab and session.
An optional project-root `scene-components.json` supplies reusable typed
templates to both editors. **Add Missing** applies one component across the
selection as a single history action, preserves same-kind authored overrides,
and aborts the whole batch on a type conflict. **Edit Field** transfers a
schema field into the raw property or metadata editor. **Edit Schema** opens a
shared structured form for creating, updating, deleting, and ordering complete
component and field definitions. Its writes are parser-validated, atomic,
conflict-aware, independently undoable, and never dirty the active scene.
Both editors can search supported assets from the open multi-root workspace
without leaving the inspector, while native file pickers remain available.
`Ctrl`/`Cmd`+`F` follows the active surface: in a scene it reveals and focuses
the hierarchy query, then Enter or the Previous/Next buttons wrap through
case-insensitive matches. The 2D query searches object IDs and types; the 3D
query searches node names. Matching selects and reveals a row without filtering
away the rest of the hierarchy or changing scene content, history, dirty state,
or camera state.
Standard Cut, Copy, Paste, and Select All commands follow the active visual
editor. Duplicate Selection
(`Ctrl`/`Cmd`+`Shift`+`D`) and Delete also target a scene selection while its
hierarchy or viewport owns focus, without escaping inspector text fields. A
typed, bounded clipboard envelope
supports same-kind cross-document object or subtree transfer with one-step
history and exact rollback. Beyond per-node authoring, the 3D editor ships scene-driven game
workflows: VSCN v7 prefab reference instances (import as instance, reload,
unpack, re-link), mixed-value batch light editing, terrain sculpting on
canonical scene meshes, vertex snap and surface drop/align, a project
material library (`materials.scene3d`), lightmap/probe/navmesh baking with
refuse-rather-than-guess preflights, an optional tagged asset library per
workspace root, schema-v19 typed Scene Settings forms, a non-destructive
Game View, and embedded native Play with scene hot reload. New Project
scaffolds runnable `game2d`/`game3d` starters. Remaining limits are honest:
no automatic scene-data/schema migration, no generalized runtime component
composition, no import-settings pipeline, and no animation authoring UI yet
(see docs/status.md for the deferral list).
The 3D viewport renders the live SceneGraph's authored meshes, PBR materials,
and node-attached lights through a windowless runtime Canvas3D, with a
per-scene triangle-wireframe mode and editor grid, hierarchy, selection,
transform, and light overlays. Viewport clicks
pick triangle-accurately against visible mesh geometry (ADR 0193) before
falling back to node-origin markers; Shift adds, Ctrl/Command toggles, and an unmodified blank click clears
the selection. Shift plus middle- or right-drag pans in the camera plane, while
an unmodified auxiliary drag continues to orbit.

## Reading The Documentation

Start with [docs/status.md](docs/status.md) when deciding whether a feature is
real, partial, or absent. That document is deliberately blunt about limitations
because Zanna Studio has historically had documentation that sounded more complete
than the implementation.

Use [docs/workflows.md](docs/workflows.md) when you are using the app or trying
to reproduce a user-visible workflow. It explains how project opening, editing,
search, build/run, debugging, terminal, Source Control, settings, and recovery
behave from the user's point of view.

Use [docs/scene-components.md](docs/scene-components.md) for the exact
`scene-components.json` format, structured authoring and file-history rules,
limits, multi-root ownership, and runtime consumption rules.

Use [docs/architecture.md](docs/architecture.md) for the high-level ownership
model and [docs/source-map.md](docs/source-map.md) for the practical module
tour. The architecture document explains the layering rules; the source map
names the files that currently implement each subsystem and the reasons to touch
them.

Use [docs/runtime-integration.md](docs/runtime-integration.md) before changing
anything that crosses the Zia/C runtime boundary. Zanna Studio is unusually
dependent on runtime APIs for editor widgets, processes, PTYs, workspace edits,
language services, and future scene work.

Use [docs/maintenance.md](docs/maintenance.md) before adding features. It
documents the common change paths: adding commands, document kinds, language
features, panels, runtime APIs, settings, probes, and docs.

## Directory Layout

```text
zannastudio/
    README.md                  This overview.
    CMakeLists.txt             Native menu/runtime probe target.
    zanna.project              Project manifest; entry point is src/main.zia.
    bin/                       Generated IDE binaries; ignored by git.

    docs/
        architecture.md         Contributor-facing source map.
        runtime-integration.md  Runtime API boundary.
        status.md               Current feature matrix and known gaps.
        testing.md              Automated and manual verification.
        workflows.md            Build, run, edit, debug, terminal, and SCM use.
        editor-first-class-...  Archived performance dogfood report.

    src/
        main.zia                App bootstrap, frame loop, and dispatch glue.
        app/                    Frame-loop helpers and shared app services.
        build/                  Build/run jobs, diagnostics, breakpoints, debug.
        commands/               Command catalog, registry, and handlers.
        core/                   Documents, projects, settings, session state.
        editor/                 Editor engine, semantic controllers, indexing.
        probes/                 Focused IDE probe entry points.
        scm/                    Git command layer and Source Control view.
        services/               File kinds, locations, text, workspace edits.
        terminal/               PTY session and integrated-terminal controller.
        tests/                  Local GUI/runtime probe source.
        ui/                     App shell, panels, activity bar, overlays.
        zia/                    Pure Zia formatting, scanning, bind/refactor code.
```

## Build

Build the full repository first when runtime or compiler changes are involved:

```sh
./scripts/build_zanna_mac.sh
./scripts/build_zanna_linux.sh
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1
```

Build only the IDE native binary from the repository root:

```sh
./scripts/build_ide.sh
```

On Windows:

```powershell
.\scripts\build_ide_win.ps1
```

The IDE build scripts write `src/zannastudio/bin/zannastudio` or
`src\zannastudio\bin\zannastudio.exe` by default. They also write
`zannastudio.buildinfo` beside the generated binary and refresh a compatibility
copy under `build/zannastudio/` unless `ZANNA_IDE_SKIP_COMPAT_COPY=1` is set. On
macOS the lowercase file is a compatibility launcher for the sibling native
payload `Zanna Studio`; the authored payload name is what Cocoa exposes in the
system application menu.

Buildinfo schema 1 identifies the exact Studio/toolchain version and records
the UTC build time, source revision/state, target architecture, binary byte
size, lowercase SHA-256, published path, and compiler path. Windows builds
compile to a same-directory stage and publish the executable/buildinfo pair
with rollback, so a failed rebuild preserves the previous matched pair.

Full repository builds also build and install Zanna Studio when
`ZANNA_INSTALL_ZANNASTUDIO=ON` (the default). `cmake --install` stages the IDE as
`bin/zannastudio` or `bin\zannastudio.exe`, and the toolchain installer wrappers keep
that option enabled so macOS, Windows, and Linux installers include the IDE. A
macOS install also stages the sibling `bin/Zanna Studio` native payload required
by the launcher.

Useful IDE build variables:

| Variable | Effect |
| --- | --- |
| `ZANNA_IDE_OUTPUT` | Override the output entry-point path (the macOS payload remains a sibling named `Zanna Studio`). |
| `ZANNA_IDE_COMPAT_OUTPUT` | Override the compatibility-copy path. |
| `ZANNA_IDE_SKIP_COMPAT_COPY=1` | Skip the compatibility copy. |
| `ZANNA_BINARY` | Force Zanna Studio build/run jobs to use a specific `zanna` binary. |
| `ZANNA_BUILD_DIR` | Help Zanna Studio locate a developer build tree. |

## Run

Run the app through the repository toolchain:

```sh
zanna run src/zannastudio/
```

Or from inside `src/zannastudio/`:

```sh
zanna run .
```

After `./scripts/build_ide.sh`, run the native binary directly:

```sh
./src/zannastudio/bin/zannastudio
```

For installer and automation smoke checks:

```sh
zannastudio --version
```

## Project Configuration

Zanna Studio reads a small `zanna.project` manifest for project metadata, file
associations, build/run overrides, and ignore rules. The IDE's own manifest is
`zannastudio/zanna.project`.

Visual scene projects may also define reusable typed 2D-object and 3D-node
templates in `scene-components.json`; see
[Project Scene Components](docs/scene-components.md).

Supported build/run override keys:

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

`build-args` and `run-args` are split into an argument vector. Quoted spans are
preserved, backslash escapes are handled, and `{target}` is replaced with the
project root or active file path. Commands are not run through a shell.

Project ignore keys (`ignore`, `ignore-patterns`, `exclude`) are forwarded to
the runtime workspace index and apply to the explorer, Quick Open, search, and
workspace indexing.

## Settings And Session Files

Settings are stored in a platform config directory:

- Windows: `%APPDATA%\ZannaStudio\settings.ini`
- macOS: `~/Library/Application Support/ZannaStudio/settings.ini`
- Linux: `$XDG_CONFIG_HOME/zannastudio/settings.ini` or
  `~/.config/zannastudio/settings.ini`

If no platform-native settings file exists, existing legacy
`~/.zannastudio/settings.ini` files are still read.

The same INI file stores:

- `[settings]` editor and workbench settings.
- `[session]` last project, open tabs, active tab, cursor, and scroll state.
- `[session.file.N]` per-file session entries and bounded crash-recovery text.
- `[recent]` recent projects.
- `[recentFiles]` recent files.

## Verification

The most relevant focused tests are documented in [docs/testing.md](docs/testing.md).
Common targeted runs after a repository build are:

```sh
ctest --test-dir build -R zia_smoke_zannastudio_project_compile --output-on-failure
ctest --test-dir build -L zannastudio --output-on-failure
ctest --test-dir build -R zia_zannastudio_terminal --output-on-failure
ctest --test-dir build -R zia_zannastudio_debug --output-on-failure
ctest --test-dir build -R zia_zannastudio_edge_case_corpus --output-on-failure
```

Display-dependent probes are labelled `requires_display`. Run them only in an
environment with the GUI runtime available.
