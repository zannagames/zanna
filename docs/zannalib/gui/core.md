---
status: active
audience: public
last-verified: 2026-07-17
---

# Core & Application
> App, Font, and common widget functions

**Part of [Zanna Runtime Library](../README.md) › [GUI Widgets](README.md)**

---

## Overview

The Zanna GUI library provides a comprehensive set of widgets for building desktop applications. All widgets are rendered using the ZannaGFX graphics library and support both dark and light themes.

### Basic Structure (Zia)

```zia
module GuiDemo;

bind Zanna.Terminal;
bind Zanna.GUI.App as App;
bind Zanna.GUI.Label as Label;
bind Zanna.GUI.Button as Button;

func start() {
    var app = App.New("My Application", 800, 600);
    var root = app.get_Root();

    var label = Label.New(root, "Hello, World!");
    var btn = Button.New(root, "Click Me");
    btn.SetSize(100, 30);
    btn.SetPosition(10, 50);

    while !app.get_ShouldClose() {
        app.Poll();

        if btn.WasClicked() {
            label.SetText("Button clicked!");
        }

        app.Render();
    }
}
```

### Basic Structure

```basic
' Create a GUI application
DIM app AS Zanna.GUI.App
app = NEW Zanna.GUI.App("My Application", 800, 600)

' Load a font
DIM font AS Zanna.GUI.Font
font = Zanna.GUI.Font.Load("arial.ttf")
app.SetFont(font, 14)

' Get root widget and add children
DIM root AS Object
root = app.Root

' Create widgets
DIM label AS Zanna.GUI.Label
label = NEW Zanna.GUI.Label(root, "Hello, World!")

DIM button AS Zanna.GUI.Button
button = NEW Zanna.GUI.Button(root, "Click Me")
button.SetSize(100, 30)
button.SetPosition(10, 50)

' Main loop
DO WHILE NOT app.ShouldClose
    app.Poll()

    ' Handle events
    IF button.WasClicked() THEN
        label.SetText("Button clicked!")
    END IF

    app.Render()
LOOP
```

---

## Typed Constant Namespaces

Public GUI APIs continue to accept `Integer` values for compatibility, but new code should use the
read-only static properties below. The values are stable across graphics backends and are also
available in graphics-disabled builds.

| Class | Stable properties |
|-------|-------------------|
| `Align` | `Start=0`, `Center=1`, `End=2`, `Stretch=3` |
| `Justify` | `Start=0`, `Center=1`, `End=2`, `SpaceBetween=3`, `SpaceAround=4`, `SpaceEvenly=5` |
| `FlexDirection` | `Row=0`, `Column=1`, `RowReverse=2`, `ColumnReverse=3` |
| `FlexWrap` | `NoWrap=0`, `Wrap=1`, `WrapReverse=2` |
| `Dock` | `Left=0`, `Top=1`, `Right=2`, `Bottom=3`, `Fill=4` |
| `ThemeMode` | `Dark=0`, `Light=1`, `System=2`, `Custom=3` |
| `AccessibleRole` | `None=0` through `Link=30`; see the generated reference for the complete semantic role list |
| `LiveRegionMode` | `Off=0`, `Polite=1`, `Assertive=2` |
| `DialogButtonRole` | `Normal=0`, `Default=1`, `Cancel=2`, `Destructive=3`, `Accept=4`, `Reject=5`, `Help=6` |
| `DialogStatus` | `Idle=0`, `Open=1`, `Accepted=2`, `Cancelled=3`, `Failed=4` |
| `ImageFilter` | `Nearest=0`, `Bilinear=1` |
| `SortDirection` | `None=0`, `Ascending=1`, `Descending=-1` |

These are typed runtime classes rather than copied language enums, so Zia and BASIC resolve the
same canonical registry getter. For example, use `Zanna.GUI.Dock.Fill`,
`Zanna.GUI.AccessibleRole.Button`, or `Zanna.GUI.ImageFilter.Bilinear` directly at the call site.

---

## Zanna.GUI.App

Main application class that manages the window and widget tree.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.GUI.App(title, width, height)`

### Properties

| Property      | Type    | Access | Description                           |
|---------------|---------|--------|---------------------------------------|
| `ShouldClose` | Boolean | Read   | True if window close requested        |
| `Root`        | Object  | Read   | Root widget container                 |

### Methods

| Method                  | Signature            | Description                              |
|-------------------------|----------------------|------------------------------------------|
| `Destroy()`             | `Void()`             | Destroy the app and owned root widget tree |
| `GetDroppedFile(index)` | `String(Integer)`    | Get path of dropped file by index        |
| `GetDroppedFileCount()` | `Integer()`          | Number of files in the current drop event |
| `Poll()`                | `Void()`             | Poll events and update widget states     |
| `PollWait(timeoutMs)`   | `Boolean(Integer)`   | Wait up to 0–1000 ms for events, then poll; true when input woke the wait |
| `WatchProcess(process)` | `Boolean(ProcessHandle)` | Wake `PollWait` when the process has output or exits |
| `WatchPty(session)`     | `Boolean(PtySession)` | Wake `PollWait` when the PTY has output or exits |
| `Render()`              | `Void()`             | Render all widgets to the window         |
| `SetFont(font, size)`   | `Void(Font, Double)` | Set default font for all widgets         |
| `WasFileDropped()`      | `Boolean()`          | True if files were dropped on the window this frame |

`SetFont()` updates existing text-bearing widgets and becomes the regular role for newly created
ones, including `Slider` value text, `ProgressBar` percentage labels, and `Spinner` value text.
Code/output controls retain the theme's monospace role and titled group boxes use its bold role.
`Font.Destroy()` defers release while any app surface still references the backing font, then
reclaims it automatically after a later safe presentation generation. `SetFont()` and widget-level
`SetFont(font, size)` calls accept live managed or legacy Font handles; stale, destroyed, and
arbitrary object handles are ignored.

`WatchProcess()` and `WatchPty()` attach a lifetime-safe activity monitor to a live runtime
handle. Readiness posts a harmless native wake event; callbacks never execute Zia or mutate widgets
from the monitor thread. The methods return false if a monitor cannot be installed, allowing an
application to retain a bounded polling fallback. Destroying the App invalidates its wake target
before any window or producer is released.

### Window Management

| Method                    | Signature         | Description                                          |
|---------------------------|-------------------|------------------------------------------------------|
| `GetWidth()`              | `Integer()`       | Get window width in pixels                           |
| `GetHeight()`             | `Integer()`       | Get window height in pixels                          |
| `GetX()` / `GetY()`       | `Integer()`       | Get the window's top-left position in screen pixels  |
| `SetPosition(x, y)`       | `Void(Integer, Integer)` | Move the window in screen pixels              |
| `Minimize()`              | `Void()`          | Minimize the window                                  |
| `Maximize()`              | `Void()`          | Maximize the window                                  |
| `Restore()`               | `Void()`          | Restore from minimized or maximized state            |
| `IsMaximized()`           | `Boolean()`       | Check if the window is maximized                     |
| `IsMinimized()`           | `Boolean()`       | Check if the window is minimized                     |
| `IsFullscreen()`          | `Boolean()`       | Check if the window is in fullscreen mode            |
| `SetFullscreen(enabled)`  | `Void(Boolean)`   | Enter or exit fullscreen mode                        |
| `Focus()`                 | `Void()`          | Bring the window to the front and focus it           |
| `Activate()`              | `Void()`          | Alias for requesting foreground focus                |
| `IsFocused()`             | `Boolean()`       | Check if the window has focus                        |
| `SetPreventClose(prevent)` | `Void(Boolean)`  | Enable or disable close-button prevention            |
| `GetScale()`              | `Number()`        | Get display scale factor (e.g. 2.0 for HiDPI)        |
| `GetLogicalWidth()`       | `Integer()`       | Window width in logical (point) units (`GetWidth() / GetScale()`) |
| `GetLogicalHeight()`      | `Integer()`       | Window height in logical (point) units               |
| `ToLogical(px)`           | `Integer(Integer)` | Convert a physical-pixel value to logical units using the window scale |
| `ToPhysical(lu)`          | `Integer(Integer)` | Convert a logical (point) value to physical pixels using the window scale |
| `SetUiScale(scale)` / `GetUiScale()` | `Void(Number)` / `Number()` | Set/read the user UI zoom multiplier (clamped to 0.5–3.0) |
| `SetWheelSpeed(speed)` / `GetWheelSpeed()` | `Void(Number)` / `Number()` | Set/read process-wide GUI wheel sensitivity |
| `GetMonitorWidth()`       | `Integer()`       | Get monitor width in logical units                   |
| `GetMonitorHeight()`      | `Integer()`       | Get monitor height in logical units                  |
| `SetWindowSize(w, h)` / `SetSize(w, h)` | `Void(Integer, Integer)` | Resize the window in logical units     |
| `SetTitle(title)` / `GetTitle()` | `Void(String)` / `String()` | Set/read the window title                    |
| `GetFontSize()`           | `Number()`        | Get current UI font size                             |
| `SetFontSize(size)`       | `Void(Number)`    | Set UI font size                                     |
| `SetPartialPaint(enabled)` | `Void(Boolean)`  | Enable or disable damage-region rendering            |
| `GetPaintFramesFull()` / `GetPaintFramesPartial()` | `Integer()` | Read cumulative full/partial repaint counters |
| `WasCloseRequested()`     | `Boolean()`       | Consume the pending close-request edge               |

**Logical vs physical units:** `GetWidth()`/`GetHeight()` report *physical* pixels, but
floating panels and overlays are positioned and sized in *logical* (point) units. Use
`GetLogicalWidth()`/`GetLogicalHeight()` — or `ToLogical(px)` / `ToPhysical(lu)` for other
measurements — instead of dividing by `GetScale()` by hand. They apply consistent rounding and
pass values through unchanged on standard (1×) displays, so the same layout code is correct on
both standard and HiDPI screens.

### Lifecycle And Validation

`Destroy()` is idempotent. After an app is destroyed, app methods reject the stale handle without dereferencing it. Apps also install a runtime finalizer as a fallback, so leaked app handles release their native window and widget tree when collected, but explicit `app.Destroy()` remains the preferred deterministic cleanup path. The `Root` widget is owned by the app, so destroy the application with `app.Destroy()`; calling widget `Destroy()` on `app.Root` is ignored.

`SetSize()` and `SetWindowSize()` share the same window-size validation. Width and height are
clamped to at least one logical pixel and to the graphics backend's configured maximum after
accounting for display scale. Invalid font sizes, scale factors, and non-finite numeric values
fall back to bounded defaults.
When `SetPreventClose(true)` is active, a window close request no longer sets
`ShouldClose`; poll `WasCloseRequested()` after `Poll()` to decide whether to
save, confirm, or close manually. `WasCloseRequested()` is edge-consuming:
after it returns `true`, later calls return `false` until another close request arrives.

Low-level widget dispatch keeps focus, hover, modal, click, and capture state
separate for each recently dispatched root. When a subtree is hidden, detached,
or destroyed, cached event state for that subtree is cleared so later root
switches cannot restore stale widget handles.
Runtime widget methods validate handles before touching widget memory. App
handles, destroyed app handles, stale widget handles, and arbitrary object
pointers are ignored by the common `Widget.*` methods instead of being cast as
widgets.

Paint callbacks receive screen-space `x`/`y` for direct drawing compatibility;
use `vg_widget_get_bounds()` inside paint when a custom C widget needs its
parent-local layout rectangle. GUI key codes are ASCII-compatible for printable
keys, but special keys are not numerically identical to VGFX key codes; use
`vg_key_from_vgfx_key()` or `vg_event_from_platform()` for platform input.

### Example

```basic
DIM app AS Zanna.GUI.App
app = NEW Zanna.GUI.App("Calculator", 300, 400)

' Set up font
DIM font AS Zanna.GUI.Font
font = Zanna.GUI.Font.Load("consola.ttf")
app.SetFont(font, 12)

' Main loop
DO WHILE NOT app.ShouldClose
    app.Poll()
    ' ... handle widget events ...
    app.Render()
LOOP
```

```zia
// Zia
var app = App.New("Calculator", 300, 400);
var root = app.get_Root();

while !app.get_ShouldClose() {
    app.Poll();
    // ... handle widget events ...
    app.Render();
}
```

---

## Zanna.GUI.Font

Managed font loading for GUI widgets. Supports TrueType (`.ttf`/supported first-face collection)
files plus zero-dependency regular and bold system UI roles.
TrueType outline rasterization preserves separate contour boundaries, so glyphs with holes or multiple independent contours render with the intended even-odd fill instead of connecting unrelated contour endpoints.
`Font.Load(path)` rejects paths containing embedded NUL bytes instead of passing a truncated path to the platform loader.
`LoadSystemUi` and `LoadSystemUiBold` try deterministic host paths and fall back to Zanna's embedded
font, so a graphics-enabled build has a portable face without adding a font library. Sizes are
logical points from 1 through 512; the preserved value is available through `GetLogicalSize` and
is scaled exactly once by the owning app.

**Type:** Static/Instance
**Constructor:** `Zanna.GUI.Font.Load(path)`

### Methods

| Method                   | Signature          | Description |
|--------------------------|--------------------|-------------|
| `Load(path)`             | `Font(String)`     | Load a managed font from a TTF file; returns null on failure |
| `LoadSystemUi(size)`     | `Result(Double)`   | Load the regular system UI role with embedded fallback |
| `LoadSystemUiBold(size)` | `Result(Double)`   | Load the bold system UI role with embedded fallback |
| `GetLogicalSize(font)`   | `Double(Font)`     | Return preserved unscaled size, or zero when unset/invalid |
| `Destroy()`              | `Void()`           | Invalidate this Font and retire its backing face while still referenced |

Font values are ordinary reference-counted runtime objects and can safely be stored in `Result` and
`ThemePalette`. Palettes retain managed wrappers per typography role. `Font.Destroy()` is safe to
call while a live app or widget still references the font: the wrapper becomes inert immediately,
while the backing face is retired in every app that uses it and reclaimed after its last render
reference and a later frame boundary. Repeated destroys and stale handles are ignored.

### Example

```basic
DIM app AS Zanna.GUI.App
app = NEW Zanna.GUI.App("Font Example", 480, 320)

DIM font AS Zanna.GUI.Font
font = Zanna.GUI.Font.Load("fonts/roboto.ttf")
app.SetFont(font, 14)  ' A failed/stale font handle is ignored safely
```

```zia
// Zia
var font = Font.Load("fonts/roboto.ttf");
app.SetFont(font, 14);

var regularResult = Font.LoadSystemUi(14.0);
if regularResult.IsOk() {
    app.SetFont(regularResult.Unwrap(), 14.0);
}
```

---

## Common Widget Functions

All widgets share these common functions:

Concrete widget classes such as `Button`, `Label`, `TextInput`, `ScrollView`,
`TreeView`, `ListBox`, `Image`, `VBox`, `HBox`, and `FloatingPanel` expose these
common methods directly. The `Widget` receiver remains useful when code stores
heterogeneous widget handles.

Focusable widgets now participate in standard desktop keyboard traversal: `Tab` moves to the next focusable widget and `Shift+Tab` moves to the previous one, respecting modal roots when a dialog or popup is active.
Passing `NULL` to `Focus()` is a no-op. Detached widgets no longer inherit the current app's font or theme until they are attached to an app-owned tree or explicitly configured.
Common numeric setters clamp invalid input: negative sizes, margins, padding, flex factors, and layout gaps become zero, very large layout values are bounded, and non-finite doubles use safe defaults. Widget identifiers are generated from a 64-bit counter for long-running GUI sessions.
`SetSize()` is ignored on `App.Root`; use `App.SetWindowSize()` to resize the window while the root remains controlled by the app layout pass.
`AddChild()` and widget constructors accept `App.Root`, an app handle, or a
container-like parent such as `VBox`, `HBox`, `ScrollView`, `SplitPane`,
`FloatingPanel`, or a custom native container. Simple widgets that do not provide custom
child arrangement can also host runtime children; they use a fallback vertical
flow so children are measured and positioned instead of becoming invisible
descendants.
`SetPosition()` marks the widget as manually positioned. Parent layout managers
leave manually positioned children at their assigned coordinates, so use
preferred size, flex, and margins for children that should remain managed by
VBox/HBox/Flex/Grid/Dock layout.
The app root owns the window-sized layout surface, so `SetSize()`,
`SetPreferredSize()`, and `SetMaxSize()` are ignored when called on the root
widget returned by `app.Root`.

| Method                        | Signature                | Description                              |
|-------------------------------|--------------------------|------------------------------------------|
| `AddChild(child)`             | `Void(Object)`           | Add a child widget                       |
| `ClearTooltip()`              | `Void()`                 | Remove tooltip from widget               |
| `Destroy()`                   | `Void()`                 | Destroy this widget and its descendants (ignored for `App.Root`) |
| `Focus()`                     | `Void()`                 | Request keyboard focus                   |
| `GetDropData()`               | `String()`               | Get the dropped data payload             |
| `GetDropType()`               | `String()`               | Get the type of the dropped data         |
| `GetFlex()`                   | `Double()`               | Get flex factor                          |
| `GetHeight()`                 | `Integer()`              | Get widget height in pixels              |
| `GetWidth()`                  | `Integer()`              | Get widget width in pixels               |
| `GetX()`                      | `Integer()`              | Get widget X position in pixels          |
| `GetY()`                      | `Integer()`              | Get widget Y position in pixels          |
| `IsBeingDragged()`            | `Boolean()`              | True if widget is being dragged          |
| `IsDragOver()`                | `Boolean()`              | True if a drag is hovering over widget   |
| `IsEnabled()`                 | `Boolean()`              | True if widget is enabled                |
| `IsFocused()`                 | `Boolean()`              | True if widget has keyboard focus        |
| `IsHovered()`                 | `Boolean()`              | True if mouse is over widget             |
| `IsPressed()`                 | `Boolean()`              | True if widget is pressed                |
| `IsVisible()`                 | `Boolean()`              | True if widget is visible                |
| `SetAcceptedDropTypes(types)` | `Void(String)`           | Set accepted drop type(s)                |
| `SetDragData(type, data)`     | `Void(String, String)`   | Set drag data type and payload           |
| `SetDraggable(enabled)`       | `Void(Boolean)`          | Enable/disable drag source               |
| `SetDropTarget(enabled)`      | `Void(Boolean)`          | Enable/disable drop target               |
| `SetEnabled(enabled)`         | `Void(Boolean)`          | Set enabled state |
| `SetFlex(flex)`               | `Void(Double)`           | Set flex factor for layout               |
| `SetMargin(margin)`           | `Void(Integer)`          | Set uniform outer margin in logical units |
| `SetMaxSize(width, height)`   | `Void(Double, Double)`   | Set maximum layout size; use `0` to clear a maximum |
| `SetPosition(x, y)`          | `Void(Integer, Integer)` | Set a manual parent-local position       |
| `SetPreferredSize(width, height)` | `Void(Double, Double)` | Set preferred layout size while preserving min/max constraints |
| `SetSize(width, height)`      | `Void(Integer, Integer)` | Set fixed logical size; use `0` on an axis to restore auto sizing |
| `SetTabIndex(index)`          | `Void(Integer)`          | Set explicit tab order (`-1` restores document order) |
| `SetCursor(type)` / `ResetCursor()` | `Void(Integer)` / `Void()` | Set/reset the process cursor using the [Cursor](application.md#cursor) values |
| `SetTooltip(text)`            | `Void(String)`           | Set tooltip text for this widget         |
| `SetTooltipRich(title, body)` | `Void(String, String)`   | Set rich tooltip with title and body     |
| `SetVisible(visible)`         | `Void(Boolean)`          | Set visibility      |
| `WasClicked()`                | `Boolean()`              | True if widget was clicked this frame    |
| `WasDropped()`                | `Boolean()`              | True if something was dropped this frame |

### Drag and Drop

Drag/drop type and payload strings are stored as C-string payloads by the GUI layer. `SetDragData(type, data)` and `SetAcceptedDropTypes(types)` reject strings containing embedded NUL bytes instead of truncating them.

| Method                        | Signature              | Description                                              |
|-------------------------------|------------------------|----------------------------------------------------------|
| `SetDraggable(enabled)`       | `Void(Boolean)`        | Enable or disable dragging this widget                   |
| `SetDragData(type, data)`     | `Void(String, String)` | Set the type and data payload for drag operations        |
| `IsBeingDragged()`            | `Boolean()`            | True if this widget is currently being dragged           |
| `SetDropTarget(enabled)`      | `Void(Boolean)`        | Mark this widget as a drop target                        |
| `SetAcceptedDropTypes(types)` | `Void(String)`         | Specify which drag data types are accepted               |
| `IsDragOver()`                | `Boolean()`            | True if a dragged item is hovering over this widget      |
| `WasDropped()`                | `Boolean()`            | True if a drop occurred on this widget this frame        |
| `GetDropType()`               | `String()`             | Get the type of the dropped data                         |
| `GetDropData()`               | `String()`             | Get the data payload that was dropped                    |

### Example

```basic
DIM button AS Zanna.GUI.Button
button = NEW Zanna.GUI.Button(root, "Submit")

' Configure
button.SetSize(120, 32)
button.SetPosition(50, 100)

' State checks in game loop
IF button.IsHovered() THEN
    ' Show hover effect
END IF

IF button.WasClicked() THEN
    ' Handle click
END IF
```

```zia
// Zia
var btn = Button.New(root, "Submit");
btn.SetSize(120, 32);
btn.SetPosition(50, 100);

if btn.IsHovered() { /* hover effect */ }
if btn.WasClicked() { /* handle click */ }
```

---


## See Also

- [Layout Widgets](layout.md)
- [Basic Widgets](widgets.md)
- [Containers & Advanced](containers.md)
- [Application Components](application.md)
- [GUI Widgets Overview](README.md)
- [Zanna Runtime Library](../README.md)
