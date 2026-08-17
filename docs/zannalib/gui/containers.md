---
status: active
audience: public
last-verified: 2026-07-17
---

# Containers & Advanced
> ScrollView, SplitPane, FloatingPanel, TabBar, TreeView, and CodeEditor

**Part of [Zanna Runtime Library](../README.md) › [GUI Widgets](README.md)**

---

## Container Widgets

### ScrollView

Scrollable container for content larger than the viewport.

Direct children are arranged as vertically stacked content items inside the scroll region. When a direct child does not report its own width it stretches to the viewport width, and scroll clamping now uses the true viewport after scrollbar gutters are reserved. For more complex nested layouts, place a `VBox`, `HBox`, or other container inside the `ScrollView` and add content there.
Scrollbar thumb drags keep pointer capture until mouse-up, even if the cursor leaves the widget while dragging.
Child overlay widgets such as dropdown popups and tooltips now render outside the viewport clip instead of being cut off by the scroll region.
Mouse-wheel events bubble to parent widgets when the scroll position cannot change, which keeps nested scroll views usable. Changing the scroll direction also clears offsets on disabled axes, so vertical-only views cannot retain stale horizontal displacement.
`ScrollView` also exposes the common widget layout, state, tooltip, and drag/drop methods directly, including `SetFlex`, `SetMargin`, geometry getters, and enabled/visible state.

**Constructor:** `NEW Zanna.GUI.ScrollView(parent)`

| Method                         | Signature                  | Description                    |
|--------------------------------|----------------------------|--------------------------------|
| `GetScrollX()`                 | `Double()`                 | Get current horizontal scroll offset |
| `GetScrollY()`                 | `Double()`                 | Get current vertical scroll offset   |
| `SetContentSize(w, h)`         | `Void(Double, Double)`     | Set content size (0=auto)      |
| `SetScroll(x, y)`              | `Void(Double, Double)`     | Set scroll position            |

### Example

```zia
// Zia
var scroll = ScrollView.New(root);
scroll.SetSize(400, 300);
scroll.SetContentSize(800.0, 600.0);
scroll.SetScroll(0.0, 0.0);
```

---

### SplitPane

Two-pane split view with draggable divider.

Once a divider drag starts, the split keeps tracking until mouse-up even if the pointer leaves the splitter strip. While focused, `Left` / `Right` or `Up` / `Down` nudge the divider, and `Home` / `End` jump the logical split position to the start or end.

First/second minimums use logical UI units and are scaled exactly once for the owning app. An
explicit collapse overrides the corresponding minimum so the pane can reach zero size; `Restore()`
returns to the position captured before the current collapse sequence. Switching directly from one
collapsed side to the other retains that original restore position. Orientation values are `0` for
horizontal left/right and `1` for vertical top/bottom; collapsed-side values are `0` for neither,
`1` for first, and `2` for second.

**Constructor:** `NEW Zanna.GUI.SplitPane(parent, horizontal)`

| Property | Type   | Access | Description            |
|----------|--------|--------|------------------------|
| `First`  | Object | Read   | First pane widget      |
| `Second` | Object | Read   | Second pane widget     |

| Method                     | Signature          | Description                              |
|----------------------------|--------------------|------------------------------------------|
| `GetPosition()`            | `Double()`         | Get current split position (0.0-1.0)     |
| `SetPosition(pos)`         | `Void(Double)`     | Set split position and leave collapsed state |
| `SetMinFirst(size)`        | `Void(Double)`     | Set first-pane minimum in logical units  |
| `SetMinSecond(size)`       | `Void(Double)`     | Set second-pane minimum in logical units |
| `GetMinFirst()`            | `Double()`         | Get first-pane logical minimum           |
| `GetMinSecond()`           | `Double()`         | Get second-pane logical minimum          |
| `GetOrientation()`         | `Integer()`        | Get `0` horizontal or `1` vertical       |
| `CollapseFirst()`          | `Void()`           | Collapse the left/top pane               |
| `CollapseSecond()`         | `Void()`           | Collapse the right/bottom pane            |
| `Restore()`                | `Void()`           | Restore the pre-collapse divider position |
| `GetCollapsedSide()`       | `Integer()`        | Get none `0`, first `1`, or second `2`    |

```basic
DIM split AS Zanna.GUI.SplitPane
split = NEW Zanna.GUI.SplitPane(root, 1)  ' Horizontal
split.SetPosition(0.3)  ' 30% / 70%

' Add content to panes
DIM leftPane AS Object = split.First
DIM rightPane AS Object = split.Second
```

```zia
// Zia
var split = SplitPane.New(root, 1);  // Horizontal
split.SetPosition(0.3);  // 30% / 70%

var leftPane = split.get_First();
var rightPane = split.get_Second();
```

---

### FloatingPanel

A floating overlay panel that appears above normal content. Floating panels are still regular widgets in the tree, so their children participate in hit testing and destruction normally, but the panel itself paints during the overlay pass. Clicking the panel background lets the user drag it to a new position.

**Constructor:** `NEW Zanna.GUI.FloatingPanel(root)`

| Method                  | Signature                  | Description                                |
|-------------------------|----------------------------|--------------------------------------------|
| `SetPosition(x, y)`     | `Void(Number, Number)`     | Set panel position in logical window coordinates |
| `CenterInParent()`      | `Void()`                   | Center the panel within the window, clamped to the top-left (size and attach it first) |
| `SetSize(w, h)`         | `Void(Number, Number)`     | Set panel dimensions in logical units      |
| `SetVisible(flag)`      | `Void(Boolean)`            | Show or hide the panel                     |
| `AddChild(widget)`      | `Void(Object)`             | Add a child widget to the panel            |
| `Destroy()`             | `Void()`                   | Destroy the panel and disconnect its runtime handle |

```zia
// Zia
var panel = FloatingPanel.New(root);
panel.SetPosition(100, 100);
panel.SetSize(200, 300);
panel.SetVisible(true);

var btn = Button.New(null, "Action");
panel.AddChild(btn);
```

---

### TabBar

Tab strip for switching between views.

Tabs can be reordered by dragging, close-button hit testing stays confined to the close glyph instead of the whole tab tail, and `WasChanged()` only reports real post-construction active-tab transitions. Hovering a tab surfaces that tab's tooltip through the standard widget tooltip system.
Keyboard navigation is available while the tab bar is focused: `Left` / `Right` switch tabs, `Home` / `End` jump to the edges, `Ctrl+W` closes the active closable tab, and `Ctrl+Shift+Left` / `Ctrl+Shift+Right` reorder the active tab.
Mouse activation and close actions now commit on mouse-up instead of mouse-down, which avoids accidental closes while starting a drag or sliding off a tab.

Successful pointer, keyboard, and programmatic reorders share one event contract.
`WasReordered()` is an independent consumable edge; `GetReorderedFrom()` and
`GetReorderedTo()` retain the most recent payload and do not consume it. `GetRevision()` is
non-consuming and advances for structural, active, reorder, and tab-metadata changes. Tab
application data is byte-exact, while visible titles follow the GUI UTF-8 display policy. Stable IDs
are copied, reject embedded NUL bytes atomically, and remain an application-level uniqueness
responsibility.

**Constructor:** `NEW Zanna.GUI.TabBar(parent)`

### Properties

| Property   | Type    | Access | Description                          |
|------------|---------|--------|--------------------------------------|
| `TabCount` | Integer | Read   | Number of tabs                       |

### Methods

| Method                   | Signature                 | Description                              |
|--------------------------|---------------------------|------------------------------------------|
| `AddTab(title, closable)` | `Object(String, Integer)` | Add tab, returns tab handle             |
| `GetActive()`            | `Object()`                | Get active tab handle                    |
| `GetActiveIndex()`       | `Integer()`               | Get active tab index                     |
| `GetCloseClickedIndex()` | `Integer()`               | Index of tab whose close was clicked, even when auto-close removes it immediately |
| `GetTabAt(index)`        | `Object(Integer)`         | Get tab handle by index                  |
| `RemoveTab(tab)`         | `Void(Object)`            | Remove a tab                             |
| `SetActive(tab)`         | `Void(Object)`            | Set active tab                           |
| `SetAutoClose(enabled)`  | `Void(Boolean)`           | Enable/disable auto-close on close click |
| `SetFont(font, size)`    | `Void(Font, Double)`      | Set the title font and logical size       |
| `MoveTab(from, to)`      | `Boolean(Integer, Integer)` | Reorder two valid, different indices    |
| `WasChanged()`           | `Boolean()`               | True if the active tab changed this frame |
| `WasCloseClicked()`      | `Boolean()`               | True if a close button was clicked        |
| `WasReordered()`         | `Boolean()`               | Consume the successful-reorder edge       |
| `GetReorderedFrom()`     | `Integer()`               | Last successful source index, or `-1`     |
| `GetReorderedTo()`       | `Integer()`               | Last successful destination index, or `-1` |
| `GetRevision()`          | `Integer()`               | Read the non-consuming state revision     |

### Tab Methods

Tab handles returned by `AddTab()` support these methods:
Tab handles are runtime-managed and become inert after `RemoveTab()`, `Clear()`, or tab-bar destruction; later tab method calls are safe no-ops.

| Method                    | Signature          | Description                    |
|---------------------------|--------------------|--------------------------------|
| `tab.SetTitle(title)`     | `Void(String)`     | Set tab title; the default tooltip follows it until overridden |
| `tab.GetTitle()`          | `String()`         | Copy the current visible title |
| `tab.SetData(data)`       | `Void(String)`     | Store byte-exact application data |
| `tab.GetData()`           | `String()`         | Copy byte-exact application data |
| `tab.SetClosable(flag)`   | `Void(Boolean)`    | Show/enable the close affordance |
| `tab.IsClosable()`        | `Boolean()`        | Test close state               |
| `tab.SetStableId(id)`     | `Void(String)`     | Set a copied application-stable ID |
| `tab.GetStableId()`       | `String()`         | Copy the stable ID             |
| `tab.SetTooltip(text)`    | `Void(String)`     | Set hover tooltip text         |
| `tab.SetModified(flag)`   | `Void(Boolean)`    | Set modified indicator (`" *"` participates in tab-width measurement) |

```basic
DIM tabs AS Zanna.GUI.TabBar
tabs = NEW Zanna.GUI.TabBar(root)

DIM tab1 AS Object = tabs.AddTab("File.txt", 1)
DIM tab2 AS Object = tabs.AddTab("Config.ini", 1)

tab1.SetModified(true)  ' Show unsaved indicator

IF tabs.WasChanged() THEN
    DIM active AS Object = tabs.GetActive()
    ' Handle tab switch
END IF

IF tabs.WasCloseClicked() THEN
    DIM idx AS INTEGER = tabs.GetCloseClickedIndex()
    DIM tab AS Object = tabs.GetTabAt(idx)
    tabs.RemoveTab(tab)
END IF
```

```zia
// Zia
var tabs = TabBar.New(root);
var tab1 = tabs.AddTab("File.txt", 1);
var tab2 = tabs.AddTab("Config.ini", 1);
tab1.SetModified(true);  // Unsaved indicator

if tabs.WasChanged() {
    var active = tabs.GetActive();
}
if tabs.WasCloseClicked() {
    var idx = tabs.GetCloseClickedIndex();
    tabs.RemoveTab(tabs.GetTabAt(idx));
}
```

---

### TreeView

Hierarchical tree view with expandable nodes.

Mouse hit testing is widget-local, so nested trees continue to select the correct row after layout shifts. Removing a node also clears any selected or hovered state inside the removed subtree, and `WasSelectionChanged()` reports removals and `Clear()` calls that actually change the selection.
Node icon text is rendered through the TreeView font, and lazy/loading nodes show an inline loading indicator instead of a blank row. Icon strings may contain a Unicode glyph or short text and round-trip exactly through `GetIcon()`. Long labels are ellipsized instead of hard-clipping mid-string, and drag-and-drop callbacks fire with validated `before` / `after` / `into` targets.
`node.SetData()` stores runtime strings with their explicit length, so embedded NUL bytes round-trip through `node.GetData()`. Runtime-owned node data is freed when the node or tree is removed.

Lazy children use a callback-free polling contract. Set `node.SetHasChildren(true)` before children
exist, then expand or toggle the node. The tree marks it loading, records a monotonic revision, and
raises `WasLoadChildrenRequested()`. `GetLoadRequestedNodeOption()` returns the requested live node;
populate it with `AddNode()` and call `node.SetLoading(false)` when complete. Reading the Option does
not consume the edge, and reading the edge does not clear the payload. Removing the node clears the
payload safely. Activation follows the same split-observer rule through `WasActivated()` and
`GetActivatedNodeOption()`.

**Constructor:** `NEW Zanna.GUI.TreeView(parent)`

### Methods

| Method                               | Signature                | Description                                      |
|--------------------------------------|--------------------------|--------------------------------------------------|
| `AddNode(parent, text)`              | `Object(Object, String)` | Add node, returns a tree-owned managed handle    |
| `Clear()`                            | `Void()`                 | Remove all nodes                                 |
| `Collapse(node)`                     | `Void(Object)`           | Collapse a node                                  |
| `Expand(node)`                       | `Void(Object)`           | Expand a node and request lazy children as needed|
| `Toggle(node)`                       | `Void(Object)`           | Toggle expanded state                            |
| `ScrollTo(node)`                     | `Void(Object)`           | Bring a node into view with minimal scrolling    |
| `GetSelected()`                      | `Object()`               | Get selected node handle                         |
| `GetLoadRequestedNodeOption()`       | `Option()`               | Last live lazy-load request target               |
| `GetActivatedNodeOption()`           | `Option()`               | Last live activated node                         |
| `RemoveNode(node)`                   | `Void(Object)`           | Remove node and descendants                      |
| `Select(node)`                       | `Void(Object)`           | Select a node                                    |
| `SetFont(font, size)`                | `Void(Font, Double)`     | Set font                                         |
| `WasSelectionChanged()`              | `Boolean()`              | Consume the legacy selection edge                |
| `WasChanged()`                       | `Boolean()`              | Consume the independent common change edge       |
| `WasActivated()`                     | `Boolean()`              | Consume the activation edge                      |
| `WasLoadChildrenRequested()`         | `Boolean()`              | Consume the lazy-child-request edge              |
| `GetRevision()`                      | `Integer()`              | Read non-consuming monotonic state revision      |

### Node Methods

Node handles returned by `AddNode()` support these methods:

Keyboard navigation keeps the selected node scrolled into view, matching mouse selection and
explicit `Select(node)` behavior.
Node handles are runtime-managed and become inert after `RemoveNode()`, `Clear()`, or tree destruction; later node method calls return empty/0 values or no-op safely.

| Method                         | Signature          | Description                                      |
|--------------------------------|--------------------|--------------------------------------------------|
| `node.GetText()`               | `String()`         | Get node display text                            |
| `node.SetText(text)`           | `Void(String)`     | Atomically replace display text                  |
| `node.SetIcon(icon)`           | `Void(String)`     | Set UTF-8 glyph or short icon text               |
| `node.GetIcon()`               | `String()`         | Get icon text                                    |
| `node.SetHasChildren(value)`   | `Void(Boolean)`    | Advertise lazy or materialized children          |
| `node.HasChildren()`           | `Boolean()`        | Test the child affordance                        |
| `node.SetLoading(value)`       | `Void(Boolean)`    | Set the loading indicator                        |
| `node.IsLoading()`             | `Boolean()`        | Test loading state                               |
| `node.SetStableId(id)`         | `Void(String)`     | Set an application-stable ID                     |
| `node.GetStableId()`           | `String()`         | Get the stable ID                                |
| `node.SetData(data)`           | `Void(String)`     | Set byte-exact runtime string data               |
| `node.GetData()`               | `String()`         | Get runtime string data                          |
| `node.IsExpanded()`            | `Boolean()`        | True if node is expanded                         |

Stable IDs are copied, may be empty, and reject embedded NUL bytes without changing the old value.
The base TreeView does not require IDs to be unique; `VirtualTree` enforces uniqueness when a
virtual model is bound.

```zia
// Zia example
var tree = TreeView.New(root);
tree.SetSize(250, 400);

var rootNode = tree.AddNode(null, "Project");
var srcNode = tree.AddNode(rootNode, "src");
tree.AddNode(srcNode, "main.zia");
tree.AddNode(srcNode, "utils.zia");
var docsNode = tree.AddNode(rootNode, "docs");
tree.AddNode(docsNode, "README.md");

tree.Expand(rootNode);
```

```basic
DIM tree AS Zanna.GUI.TreeView
tree = NEW Zanna.GUI.TreeView(root)
tree.SetSize(250, 400)

' Build tree structure
DIM rootNode AS Object = tree.AddNode(NOTHING, "Project")
DIM srcNode AS Object = tree.AddNode(rootNode, "src")
tree.AddNode(srcNode, "main.bas")
tree.AddNode(srcNode, "utils.bas")
DIM docsNode AS Object = tree.AddNode(rootNode, "docs")
tree.AddNode(docsNode, "README.md")

' Expand root by default
tree.Expand(rootNode)
```

---

## Advanced Widgets

### CodeEditor

Full-featured code editor with syntax highlighting.

The focused caret blink is advanced automatically during the app render loop, matching the existing `TextInput` caret behavior.
When word wrap is enabled, the editor uses wrapped visual rows for painting, cursor up/down movement, pixel hit-testing, scrollbar math, and `ScrollToLine`; only the stored document text remains unchanged.
Hiding line numbers fully collapses the line-number gutter. `SetLineNumberWidth(width)` is measured in character cells, so the gutter scales with the active font metrics instead of staying pinned to stale pixels. Fold regions now render in the gutter and hide folded body lines from cursor movement, scrolling, and pixel-position helpers.
Syntax-colored text now renders in contiguous same-color runs instead of issuing one draw call per byte, which keeps large highlighted files responsive.
Keyboard text input and pasted text preserve valid multi-byte UTF-8 sequences as complete byte ranges. Document replacement builds the new line array before swapping it into the editor, so allocation failure leaves the previous document intact instead of installing partially initialized lines.
`SetText()` uses the runtime string byte length when replacing the document, so the `Text` property
round-trips embedded NUL bytes instead of truncating at the first NUL.
`GetWordAtCursor()` and `ReplaceWordAtCursor()` keep non-ASCII UTF-8 bytes with the surrounding identifier instead of splitting a multibyte word in the middle.
Gutter icon slots are validated as `0..3`; invalid slots are ignored. Prefer
`TakeGutterClick()`, which atomically consumes the pending event and returns a map with `clicked`,
`line`, and `slot`. With the legacy accessors, read `GetGutterClickLine()` and
`GetGutterClickSlot()` before consuming the event with `WasGutterClicked()`; consuming the event
clears the stored line and slot back to `-1`.
`ScrollTopLine` exposes the zero-based source line nearest the top of the viewport, so applications can persist and restore editor scroll state without inferring it from pixels.
Selection range getters return normalized zero-based source coordinates for the requested cursor; if the cursor has no active selection or the cursor index is invalid, they return `0`.
Mouse editing supports `Shift` + click to extend the primary selection and `Ctrl`/`Cmd` + click to add a secondary cursor at the clicked text position.

**Constructor:** `NEW Zanna.GUI.CodeEditor(parent)`

| Property     | Type    | Access | Description                                                    |
|--------------|---------|--------|----------------------------------------------------------------|
| `Text`       | String  | Read   | Editor content                                                 |
| `LineCount`  | Integer | Read   | Number of lines                                                |
| `CursorLine` | Integer | Read   | Current cursor line                                            |
| `CursorCol`  | Integer | Read   | Current cursor column                                          |
| `ScrollTopLine` | Integer | R/W | Source line nearest the top of the viewport                    |

| Method                                     | Signature                        | Description                              |
|--------------------------------------------|----------------------------------|------------------------------------------|
| `AddCursor(line, col)`                     | `Void(Integer, Integer)`         | Add an additional cursor                 |
| `AddFoldRegion(startLine, endLine)`        | `Void(Integer, Integer)`         | Add a foldable region                    |
| `AddHighlight(startLine, startCol, endLine, endCol, color)` | `Void(Int, Int, Int, Int, Int)` | Add colored text highlight region |
| `ClearCursors()`                           | `Void()`                         | Remove extra cursors (keep primary)      |
| `ClearFoldRegions()`                       | `Void()`                         | Remove all fold regions                  |
| `ClearGutterIcons(slot)`                   | `Void(Integer)`                  | Clear every gutter icon in a slot        |
| `ClearHighlights()`                        | `Void()`                         | Remove all highlights                    |
| `ClearModified()`                          | `Void()`                         | Clear modified flag                      |
| `Copy()`                                   | `Integer()`                      | Copy selection to clipboard; 1 on success |
| `CursorHasSelection(cursorIdx)`            | `Boolean(Integer)`               | Check if cursor has selection            |
| `Cut()`                                    | `Integer()`                      | Cut selection to clipboard; 1 on success |
| `Fold(line)`                               | `Void(Integer)`                  | Fold region at line                      |
| `FoldAll()`                                | `Void()`                         | Fold all registered regions              |
| `GetCursorCount()`                         | `Integer()`                      | Get number of cursors (multi-cursor)     |
| `GetColAtPixel(x, y)`                     | `Integer(Integer, Integer)`      | Map a screen-space pixel to a logical column using the current wrap/scroll geometry |
| `GetGutterClickLine()`                     | `Integer()`                      | Get line of gutter click                 |
| `GetGutterClickSlot()`                     | `Integer()`                      | Get gutter icon slot of the current click |
| `TakeGutterClick()`                        | `Map()`                          | Atomically consume click; returns `clicked`, `line`, and `slot` |
| `GetLineAtPixel(y)`                       | `Integer(Integer)`               | Map a screen-space pixel to a logical line using the current wrap/scroll geometry |
| `GetSelectionStartLineAt(cursorIdx)`       | `Integer(Integer)`               | Get normalized selection start line for a cursor |
| `GetSelectionStartColAt(cursorIdx)`        | `Integer(Integer)`               | Get normalized selection start column for a cursor |
| `GetSelectionEndLineAt(cursorIdx)`         | `Integer(Integer)`               | Get normalized selection end line for a cursor |
| `GetSelectionEndColAt(cursorIdx)`          | `Integer(Integer)`               | Get normalized selection end column for a cursor |
| `GetSelectedText()`                        | `String()`                       | Get currently selected text (empty string if no selection) |
| `IsFolded(line)`                           | `Boolean(Integer)`               | Check if line is folded                  |
| `IsModified()`                             | `Boolean()`                      | Check if modified                        |
| `Paste()`                                  | `Integer()`                      | Paste from clipboard; 1 on success       |
| `Redo()`                                   | `Void()`                         | Redo last undone edit                    |
| `RemoveFoldRegion(startLine)`              | `Void(Integer)`                  | Remove the fold region starting on a line |
| `ScrollToLine(line)`                       | `Void(Integer)`                  | Scroll to line                           |
| `SetAutoFoldDetection(enabled)`            | `Void(Boolean)`                  | Enable or disable automatic fold detection |
| `SelectAll()`                              | `Void()`                         | Select all text                          |
| `SetCursor(line, col)`                     | `Void(Integer, Integer)`         | Set cursor position                      |
| `SetFont(font, size)`                      | `Void(Font, Double)`             | Set the editor font (normally monospace) |
| `SetGutterIcon(line, icon, slot)`          | `Void(Integer, Object, Integer)` | Set a gutter icon in slot 0–3 on a line  |
| `SetLanguage(name)`                        | `Void(String)`                   | Set syntax highlighting language         |
| `SetLineNumberWidth(width)`                | `Void(Integer)`                  | Set line-number gutter width in character cells |
| `SetShowFoldGutter(show)`                  | `Void(Boolean)`                  | Show or hide fold markers in the gutter  |
| `SetShowLineNumbers(show)`                 | `Void(Boolean)`                  | Show/hide line numbers                   |
| `SetText(text)`                            | `Void(String)`                   | Set editor content                       |
| `ReplaceAllText(text)`                     | `Boolean(String)`                | Replace all text as one undoable edit while retaining and clamping editor state |
| `SetTokenColor(tokenType, color)`          | `Void(Integer, Integer)`         | Set color for a token type               |
| `ToggleFold(line)`                         | `Void(Integer)`                  | Toggle fold state at line                |
| `Unfold(line)`                             | `Void(Integer)`                  | Unfold region at line                    |
| `UnfoldAll()`                              | `Void()`                         | Unfold all registered regions            |
| `Undo()`                                   | `Void()`                         | Undo last edit                           |
| `WasGutterClicked()`                       | `Boolean()`                      | True if a gutter was clicked             |

### Advanced Methods

| Method                       | Signature          | Description                                      |
|------------------------------|--------------------|--------------------------------------------------|
| `CanUndo()`                  | `Boolean()`        | Check if undo is available                       |
| `CanRedo()`                  | `Boolean()`        | Check if redo is available                       |
| `SetTabSize(size)`           | `Void(Integer)`    | Set tab width in spaces                          |
| `GetTabSize()`               | `Integer()`        | Get current tab width in spaces                  |
| `SetWordWrap(enabled)`       | `Void(Boolean)`    | Enable or disable word wrapping                  |
| `GetWordWrap()`              | `Integer()`        | Read word-wrap state (`1` enabled, `0` disabled) |

`SetText` establishes a new cold-load baseline and resets document editing
state. Use `ReplaceAllText` when transforming the current document: it retains
prior undo history, cursors, selections, folds, and scroll position and records
the replacement as one undo unit. Detached `EditorBuffer` values expose the
same `ReplaceAllText(text) -> Boolean` contract.

#### Performance diagnostics

`GetPerfStats()` is the preferred diagnostics API. It returns a consistent versioned snapshot
instead of requiring monitoring code to call several counters independently. The Map always has
`schemaVersion = 1` and these raw, monotonic, saturating integer keys:

| Key | Meaning |
|-----|---------|
| `totalHeightLinearScans` | Source lines visited while accumulating total document height |
| `totalVisualRowLinearScans` | Source lines visited while accumulating total wrapped rows |
| `visualRowLinearScans` | Lines visited while mapping a source position to a visual row |
| `locateVisualRowLinearScans` | Lines visited while locating a source line for a visual row |
| `lineHighlightCalls` | Per-line syntax-highlighter invocations |
| `syntaxStateLineScans` | Lines scanned while reconstructing cached lexical state |
| `highlightSpanChecks` | Explicit highlight spans inspected during paint |
| `fullTextCopies` | Full-document text materializations |
| `fullTextCopyBytes` | Bytes copied by those materializations |

Call `ResetPerfStats()` immediately before measuring a scenario. The existing individual getters
remain callable for compatibility; `GetLayoutLinearScanCount()` is the saturating sum of the first
four scan counters. Invalid editors and graphics-disabled runtimes return the complete schema with
zero values, so portable diagnostics do not need a capability branch.

```basic
DIM editor AS Zanna.GUI.CodeEditor
editor = NEW Zanna.GUI.CodeEditor(root)
editor.SetSize(600, 400)

' Load file
DIM content AS STRING
content = Zanna.IO.File.ReadAllText("main.bas")
editor.SetText(content)

' Save on Ctrl+S
IF (Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.LeftControl) OR Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.RightControl)) AND Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.S) THEN
    IF editor.IsModified() THEN
        Zanna.IO.File.WriteAllText("main.bas", editor.Text)
        editor.ClearModified()
    END IF
END IF
```

```zia
// Zia
var editor = CodeEditor.New(root);
editor.SetSize(600, 400);
editor.SetLanguage("zia");
editor.SetShowLineNumbers(true);
editor.SetText("func main() {\n    Say(\"hello\");\n}");

if editor.IsModified() {
    editor.ClearModified();
}
```

---


## See Also

- [Core & Application](core.md)
- [Layout Widgets](layout.md)
- [Basic Widgets](widgets.md)
- [Application Components](application.md)
- [GUI Widgets Overview](README.md)
- [Zanna Runtime Library](../README.md)
