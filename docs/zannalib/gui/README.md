---
status: active
audience: public
last-verified: 2026-07-16
---

# GUI Widgets
> Window, layout, and widget types for building graphical user interfaces.

**Part of [Zanna Runtime Library](../README.md)**

---

## Contents

| File | Contents |
|------|----------|
| [Core & Application](core.md) | App, Font, and common widget functions |
| [Layout Widgets](layout.md) | VBox, HBox, and other layout containers |
| [Basic Widgets](widgets.md) | Label, Button, TextInput, Checkbox, Slider, RadioButton, and more |
| [Containers & Advanced](containers.md) | ScrollView, SplitPane, FloatingPanel, TabBar, TreeView, and CodeEditor |
| [Application Components](application.md) | MenuBar, Toolbar, StatusBar, Breadcrumb, Minimap, dialogs, notifications, utilities, themes, VideoWidget, and IDE helpers |
| [Themes and Palettes](themes.md) | Dark/light/system modes, custom tokens, validation, scaling, and accessibility composition |

Recent runtime updates include reliable `Tab` / `Shift+Tab` focus traversal, focus rejection through hidden/disabled ancestors, root-bounded first-show measurement/wrapping/clamping for widget tooltips, complete accessible `ColorSwatch`/`ColorPalette`/`ColorPicker` classes, edge-triggered tab close events, trailing-newline preservation in `CodeEditor`, safer list/dropdown/toolbar/statusbar/menu capacity handling, GUI text conversion that renders embedded NUL bytes as U+FFFD instead of truncating visible labels, drag/drop and path/filter C-string payloads that reject embedded NUL bytes, concrete widget classes exposing the common `Widget` layout/state/tooltip/drag-drop methods directly, fallback layout for simple widgets that receive runtime children through `AddChild`, active menu shortcut dispatch on non-native menu bars, per-frame shortcut trigger retention, runtime-managed subobject handles that stay safe after owner destruction, a `Command`/`CommandRegistry` binding layer that routes a menu item, toolbar button, keyboard shortcut and command-palette entry to one action, and corrected edge reporting for ListBox/TreeView removals.

## Complete Example (Zia)

```zia
module NoteEditor;

bind Zanna.GUI.App as App;
bind Zanna.GUI.VBox as VBox;
bind Zanna.GUI.HBox as HBox;
bind Zanna.GUI.Label as Label;
bind Zanna.GUI.Button as Button;
bind Zanna.GUI.CodeEditor as CodeEditor;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var app = App.New("Note Editor", 800, 600);
    var root = app.get_Root();

    // Layout
    var vbox = VBox.New();
    vbox.SetSpacing(5.0);
    vbox.SetPadding(10.0);
    root.AddChild(vbox);

    // Toolbar
    var toolbar = HBox.New();
    toolbar.SetSpacing(5.0);
    vbox.AddChild(toolbar);
    var newBtn = Button.New(toolbar, "New");
    newBtn.SetSize(60, 28);
    var saveBtn = Button.New(toolbar, "Save");
    saveBtn.SetStyle(1);
    saveBtn.SetSize(60, 28);

    // Editor
    var editor = CodeEditor.New(vbox);
    editor.SetSize(780, 500);

    // Status bar
    var status = Label.New(vbox, "Ready");

    while !app.get_ShouldClose() {
        app.Poll();

        if newBtn.WasClicked() {
            editor.SetText("");
            status.SetText("New file");
        }
        if saveBtn.WasClicked() {
            editor.ClearModified();
            status.SetText("Saved!");
        }
        if editor.IsModified() {
            status.SetText("Modified - " + Fmt.Int(editor.get_LineCount()) + " lines");
        }

        app.Render();
    }
}
```

## Complete Example

```basic
' Complete GUI Application Example
DIM app AS Zanna.GUI.App
app = NEW Zanna.GUI.App("Note Editor", 800, 600)

DIM root AS Zanna.GUI.Widget
root = app.Root

' Load font
DIM font AS Zanna.GUI.Font
font = Zanna.GUI.Font.Load("consola.ttf")
app.SetFont(font, 12)

' Create layout
DIM vbox AS Zanna.GUI.VBox
vbox = NEW Zanna.GUI.VBox()
vbox.SetSpacing(5)
vbox.SetPadding(10)
root.AddChild(vbox)

' Toolbar
DIM toolbar AS Zanna.GUI.HBox
toolbar = NEW Zanna.GUI.HBox()
toolbar.SetSpacing(5)
vbox.AddChild(toolbar)

DIM newBtn AS Zanna.GUI.Button
newBtn = NEW Zanna.GUI.Button(toolbar, "New")
newBtn.SetSize(60, 28)

DIM openBtn AS Zanna.GUI.Button
openBtn = NEW Zanna.GUI.Button(toolbar, "Open")
openBtn.SetSize(60, 28)

DIM saveBtn AS Zanna.GUI.Button
saveBtn = NEW Zanna.GUI.Button(toolbar, "Save")
saveBtn.SetStyle(1)  ' Primary
saveBtn.SetSize(60, 28)

' Editor
DIM editor AS Zanna.GUI.CodeEditor
editor = NEW Zanna.GUI.CodeEditor(vbox)
editor.SetSize(780, 500)

' Status bar
DIM status AS Zanna.GUI.Label
status = NEW Zanna.GUI.Label(vbox, "Ready")

' Main loop
DO WHILE NOT app.ShouldClose
    app.Poll()

    ' Handle buttons
    IF newBtn.WasClicked() THEN
        editor.SetText("")
        status.SetText("New file")
    END IF

    IF saveBtn.WasClicked() THEN
        ' Save logic here
        editor.ClearModified()
        status.SetText("Saved!")
    END IF

    ' Update status
    IF editor.IsModified() THEN
        status.SetText("Modified - " + STR$(editor.LineCount) + " lines")
    END IF

    app.Render()
LOOP
```

---


## See Also

- [Graphics](../graphics/README.md) - Low-level graphics with Canvas
- [Input](../input.md) - Keyboard and mouse input
- [Audio](../audio.md) - Sound effects and music
