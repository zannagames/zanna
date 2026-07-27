---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Zanna Studio

Zanna Studio (`src/zannastudio/`; formerly ZannaIDE, renamed in ADR 0118) is
the repository IDE application for editing,
building, running, and debugging Zanna projects. It is written in Zia and runs
on the `Zanna.GUI.*`, `Zanna.System.*`, `Zanna.Workspace.*`, and
language-service runtime surfaces.

Zanna Studio maintains its own detailed documentation set inside the application
tree — this page is a locator, not a duplicate:

| Document | Purpose |
|----------|---------|
| [`src/zannastudio/README.md`](../../../src/zannastudio/README.md) | Scope, current capabilities, and entry points |
| [`src/zannastudio/docs/architecture.md`](../../../src/zannastudio/docs/architecture.md) | Source layout, ownership, and layering rules |
| [`src/zannastudio/docs/source-map.md`](../../../src/zannastudio/docs/source-map.md) | Detailed module-by-module source guide |
| [`src/zannastudio/docs/status.md`](../../../src/zannastudio/docs/status.md) | Feature status and known gaps |
| [`src/zannastudio/docs/workflows.md`](../../../src/zannastudio/docs/workflows.md) | User and developer workflows |
| [`src/zannastudio/docs/runtime-integration.md`](../../../src/zannastudio/docs/runtime-integration.md) | Runtime APIs used by the IDE |
| [`src/zannastudio/docs/maintenance.md`](../../../src/zannastudio/docs/maintenance.md) | How to change Zanna Studio safely |
| [`src/zannastudio/docs/testing.md`](../../../src/zannastudio/docs/testing.md) | Probes, CTest entries, manual checks |

## Top-Level Layout (`src/zannastudio/`)

Paths in this table are relative to `src/zannastudio/`; later tables use full
repository paths.

| Directory | Purpose |
|-----------|---------|
| `src/app/` | Application shell, startup, session restore (13 files) |
| `src/basic/` | BASIC language-service integration (1 file) |
| `src/build/` | Build/run/debug job management; debug adapter client (8 files) |
| `src/commands/` | Command registry, palette actions, project-wide replace (19 files) |
| `src/core/` | Documents, projects, settings, keybinding overrides (9 files) |
| `src/editor/` | Multi-tab editing on `Zanna.GUI.CodeEditor` (25 files) |
| `src/probes/` | Headless CTest probes (94 files) |
| `src/scm/` | Git view: status, commit history, interactive push/pull (4 files) |
| `src/services/` | Workspace indexing, diff engine, project templates, theme palette access (16 files) |
| `src/terminal/` | Integrated PTY terminal via `Zanna.System.Pty` (2 files) |
| `src/tests/` | In-tree test helpers (4 files) |
| `src/ui/` | Panels, dialogs, explorer, welcome, diff view, status bar (60 files) |
| `src/zia/` | Zia language-service integration (10 files) |
| `bin/`, `scripts/` | Launch and packaging helpers |

## Flagship-program landmarks (plan suite `misc/plans/zannastudio/`)

The 2026-07 Zanna Studio program landed capability across the IDE, the GUI
toolkit, the runtime, and the VM. Each plan document carries an as-built
record with file-level detail; the load-bearing locations:

| Capability | Where |
|-----------|-------|
| Brand palettes + WCAG contrast gate | `src/lib/gui/src/core/vg_theme.c`, `src/lib/gui/tests/test_vg_theme_contrast.c` |
| Vector icon library (ADR 0137) | `src/lib/gui/src/core/vg_icon_vector.c`, `src/lib/gui/include/vg_icon_vector.h` |
| Windows UIA accessibility provider | `src/runtime/graphics/gui/rt_gui_accessibility_win32.c` |
| Smooth scrolling + present pacing | `src/lib/gui/src/core/vg_widget.c`, `src/lib/gui/src/widgets/vg_scrollview.c` |
| Gamma-correct AA, GSUB ligatures, font fallback | `src/lib/gui/src/font/` (`vg_gsub.c`, `vg_font.c`, `vg_ttf.c`) |
| Side-by-side diff engine and view | `src/zannastudio/src/services/diff_engine.zia`, `src/zannastudio/src/ui/diff_view.zia` |
| Project templates + new-project wizard | `src/zannastudio/src/services/project_templates.zia` |
| Keybinding overrides | `src/zannastudio/src/commands/command_registry.zia`, `src/zannastudio/src/core/settings.zia` |
| Native Windows file dialogs, cursor set | `src/lib/gui/src/dialogs/vg_filedialog_native_win32.c`, `src/lib/graphics/` |
| Debugger class-field expansion (ADR 0138) | `src/vm/debug/VMDebug.cpp`, `src/frontends/zia/DebugLayoutExport.hpp` |
| Terminal emulator (regions, modes, replies) | `src/lib/gui/src/widgets/vg_outputpane.c`, `src/zannastudio/src/terminal/` |
| SCM history, job queue, credential prompts | `src/zannastudio/src/scm/scm_git.zia`, `src/zannastudio/src/scm/scm_view.zia` |
| Direct persisted workspace docking | `src/zannastudio/src/ui/workbench_shell.zia`, `src/zannastudio/src/ui/primary_sidebar_dock.zia`, `src/zannastudio/src/ui/workspace_dock.zia`, `src/zannastudio/src/ui/tool_panel_groups.zia`, `src/zannastudio/src/ui/tool_panel_shell.zia` |
| Stable scene multi-selection (ADR 0156) | `src/runtime/graphics/gui/rt_gui_controls.c`, `src/zannastudio/src/ui/scene_selection.zia` |
| Scene-aware Edit commands and typed cross-document clipboard | `src/zannastudio/src/commands/main_command_dispatcher.zia`, `src/zannastudio/src/ui/scene_clipboard.zia` |
| Focus-safe scene Duplicate/Delete and transactional batch inspectors | `src/zannastudio/src/commands/main_command_dispatcher.zia`, `src/zannastudio/src/ui/scene_editor_2d.zia`, `src/zannastudio/src/ui/scene_editor_3d.zia` |
| Scene precision layout and hierarchy organization | `src/zannastudio/src/ui/scene_layout_2d.zia`, `src/zannastudio/src/ui/scene_hierarchy_3d.zia`, `src/zannastudio/src/ui/scene_editor_2d.zia`, `src/zannastudio/src/ui/scene_editor_3d.zia` |
