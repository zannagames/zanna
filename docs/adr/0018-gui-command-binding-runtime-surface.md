---
status: active
audience: contributors
last-verified: 2026-06-29
---

# ADR 0018: GUI Command Binding (Zanna.GUI.Command / CommandRegistry)

## Status

Accepted (runtime implemented; Zanna Studio is the intended first consumer). Driven
by the GUI runtime-additions review, recommendation **R1**
(review document retired 2026-07-15; the recommendation ids are the surviving record).

## Context

Every desktop-style GUI app exposes the same command (action) from several
surfaces at once: a menu item, a toolbar button, a keyboard shortcut, and a
command palette entry. The Zanna runtime already ships the *pieces* —
`Zanna.GUI.Shortcuts` (keyboard), `Zanna.GUI.CommandPalette` (palette), and
`Zanna.GUI.CommandState` (enabled/checked state) — but nothing binds them to each
other or to the `MenuItem` / `ToolbarItem` widgets, and no widget exposes a
click callback (the toolkit is polled). As a result an application must:

- register the command in three separate places and keep them in sync,
- poll each menu item, toolbar item, shortcut, and the palette by hand every
  frame and OR the results together, and
- push enabled/checked state to each bound widget by hand.

In Zanna Studio this glue is `commands/command_registry.zia` (336 LOC) +
`commands/main_command_dispatcher.zia` (486 LOC) + `app/dispatch_helpers.zia`
(`Triggered`), and there are **65 `WasClicked()` poll sites**. Any Zanna GUI app
with menus + toolbars + shortcuts re-derives the same dispatcher. This is missing
runtime infrastructure, not application logic.

Adding runtime classes is a runtime C-ABI surface change, which requires an ADR.

## Decision

Add two reference-counted runtime classes that unify the existing pieces.

### `Zanna.GUI.Command`

A single command: identity + display + state + the widgets it drives.

- `New(id: str, title: str) -> Command`
- `GetId() -> str`, `GetTitle() -> str`
- `SetShortcut(keys: str)` / `GetShortcut() -> str` — `SetShortcut` also registers
  the chord with the global `Zanna.GUI.Shortcuts` registry under `id`, so the
  shortcut and the command share one identifier (best-effort: a no-op until a GUI
  app exists, exactly like `Shortcuts.Register` today).
- `SetEnabled(b: i1)` / `IsEnabled() -> i1`
- `SetCheckable(b: i1)` / `IsCheckable() -> i1`, `SetChecked(b: i1)` / `IsChecked() -> i1`
- `BindMenuItem(item: obj)`, `BindToolbarItem(item: obj)` — associate the command
  with widgets it should drive and read.
- `Poll() -> i1` — read the bound menu item, bound toolbar item, and shortcut;
  push current enabled/checked state to the bound widgets; return 1 if the command
  was invoked this frame (disabled commands never report invoked). For standalone
  commands not held by a registry.
- `WasInvoked() -> i1` — the cached invoked flag from the most recent `Poll`
  (command-level or registry-level).
- `Snapshot() -> Map` — `{id, title, shortcut, enabled, checkable, checked, invoked}`.

### `Zanna.GUI.CommandRegistry`

Owns a set of commands and routes all four input sources in one call.

- `New() -> CommandRegistry`
- `Add(command: obj)` — retains the command (registry co-owns it).
- `Count() -> i64`
- `Find(id: str) -> Command` — the command with that id, or `null`.
- `BindPalette(palette: obj)` — the `CommandPalette` whose selection routes to commands.
- `Poll() -> str` — read the bound palette's selection once, then poll every
  command (menu/toolbar/shortcut + palette match); push state; return the id of an
  invoked command this frame, or `""`. Every polled command's `WasInvoked()` is
  also updated.
- `Clear()` — release all commands.

### Lifetime / ownership

- `Command` and `CommandRegistry` are `rt_obj_new_i64` reference-counted objects
  with finalizers, mirroring `Zanna.GUI.CommandState`.
- The registry **retains** (`rt_obj_retain_known`) each added command and releases
  them in `Clear` / finalize, so a command stays alive while registered even if the
  script drops its handle. `Find` returns a retained (owned) reference.
- A command stores its bound `MenuItem` / `ToolbarItem` / palette as **raw**
  handles (no retain) and only ever touches them through the public self-guarding
  accessors (`rt_menuitem_was_clicked`, `rt_menuitem_set_enabled`,
  `rt_toolbaritem_*`, `rt_commandpalette_*`), which validate liveness on every call
  and no-op on a destroyed widget. This matches the existing `FindBar`→`CodeEditor`
  binding pattern and avoids rooting subhandles whose owning menubar it cannot keep
  alive anyway.
- A bound widget's click is **consumed on read**, so a bound widget must be polled
  by exactly one owner: when a widget is bound to a command, the command owns its
  click polling (the app stops calling `WasClicked()` on it directly). The palette's
  `WasSelected` is likewise read once, at the registry.

### Implementation

Both classes live in `src/runtime/graphics/gui/rt_gui_ide.cpp` (alongside
`CommandState`), declared in `rt_gui_ide.h`, with graphics-disabled twins in
`src/runtime/graphics/common/rt_disabled_runtime_stubs.c`. No new `.c`/`.h` file,
so no `source_health` surface change. Two new class-id tags are added
(`RT_GUI_COMMAND_CLASS_ID`, `RT_GUI_COMMAND_REGISTRY_CLASS_ID`); the leaf names
`Command` and `CommandRegistry` are globally unique.

## Consequences

- **Adoption:** a command is declared once and bound to its widgets; one
  `registry.Poll()` replaces the per-widget `WasClicked()` polling and the
  three-registry sync. Zanna Studio can collapse much of `command_registry.zia` /
  `main_command_dispatcher.zia`; other GUI apps get a dispatcher for free.
- **Determinism / cross-platform:** pure bookkeeping over existing platform
  widgets; no new OS surface, no platform `#ifdef`. The disabled-graphics build
  keeps the same symbols as no-ops.
- **No behavior risk for existing callers:** purely additive. `Shortcuts`,
  `CommandPalette`, `CommandState`, `MenuItem`, and `ToolbarItem` are unchanged;
  apps that keep hand-polling continue to work.

## Alternatives Considered

- **A callback/event API on widgets (`button.OnClick(fn)`).** Rejected for now:
  it would introduce function-pointer/closure marshaling across the runtime ABI and
  a retain-graph for handlers — a much larger surface than unifying the polled
  pieces the runtime already exposes. The command binder gets the same ergonomic win
  within the existing frame-polled model.
- **Extend `CommandState` in place.** Rejected: `CommandState` is a pure state
  snapshot with no widget knowledge; overloading it with binding + polling would
  change its meaning and its existing surface. A new class keeps both focused.
- **A Zia-only library in Zanna Studio.** Rejected: that is exactly the status quo
  (`command_registry.zia` + `main_command_dispatcher.zia`); every Zanna GUI app
  re-derives it, and a Zia layer still cannot read widget clicks without the
  per-widget polling this ADR removes.
