---
status: active
audience: public
last-verified: 2026-07-16
---

# Basic Widgets
> Label, Button, TextInput, Checkbox, Slider, RadioButton, and more

**Part of [Zanna Runtime Library](../README.md) › [GUI Widgets](README.md)**

---

## Basic Widgets

### Label

Text display widget. Labels initialize from the current GUI theme or app font, so text participates
in measurement and painting as soon as a default font has been installed. `SetFont()` still
overrides that font for one label.

Single-line labels can fit overflow with a UTF-8-safe ellipsis. Wrapped labels apply `MaxLines` to
both measurement and rendering; when ellipsis is enabled, U+2026 appears only if the limit actually
omits source text. Selectable labels support pointer drag, Shift-click, Ctrl/Cmd+A, Ctrl/Cmd+C, and
Escape. `GetSelectedText()` returns the original source bytes, including content hidden by an
ellipsis after Select All.

**Constructor:** `NEW Zanna.GUI.Label(parent, text)`

| Method                    | Signature                  | Description                                      |
|---------------------------|----------------------------|--------------------------------------------------|
| `SetText(text)`           | `Void(String)`             | Set label text                                   |
| `SetFont(font, size)`     | `Void(Font, Double)`       | Set font and logical size                        |
| `SetColor(color)`         | `Void(Integer)`            | Set text color (ARGB)                            |
| `SetWordWrap(enabled)`    | `Void(Boolean)`            | Enable or disable word wrapping                  |
| `SetAlignment(value)`     | `Void(Integer)`            | Set horizontal alignment: left `0`, center `1`, right `2` |
| `GetAlignment()`          | `Integer()`                | Get horizontal alignment                         |
| `SetEllipsis(enabled)`    | `Void(Boolean)`            | Fit genuinely truncated text with U+2026         |
| `SetMaxLines(count)`      | `Void(Integer)`            | Limit wrapped lines; zero/negative is unlimited  |
| `SetSelectable(enabled)`  | `Void(Boolean)`            | Enable read-only text selection and copy         |
| `GetSelectedText()`       | `String()`                 | Copy the currently selected source text          |

```basic
DIM label AS Zanna.GUI.Label
label = NEW Zanna.GUI.Label(root, "Hello!")
label.SetColor(4294901760)  ' Red text
```

```zia
// Zia
var label = Label.New(root, "Hello!");
label.SetColor(0xFFFF0000);  // Red text
```

---

### Button

Clickable button widget.

**Constructor:** `NEW Zanna.GUI.Button(parent, text)`

| Method                    | Signature                  | Description                              |
|---------------------------|----------------------------|------------------------------------------|
| `SetText(text)`           | `Void(String)`             | Set button text                          |
| `SetFont(font, size)`     | `Void(Font, Double)`       | Set font and size                        |
| `SetStyle(style)`         | `Void(Integer)`            | Set style (0=default, 1=primary, 2=secondary, 3=danger, 4=text, 5=icon) |
| `SetIcon(icon)`           | `Void(String)`             | Set an icon glyph or short icon string   |
| `SetIconPosition(pos)`    | `Void(Integer)`            | Place icon on the left (`0`) or right (`1`) |

An out-of-range style resets the button to style `0`. Any icon-position value other than `1`
selects the left side.

```basic
DIM saveBtn AS Zanna.GUI.Button
saveBtn = NEW Zanna.GUI.Button(root, "Save")
saveBtn.SetStyle(1)  ' Primary style
saveBtn.SetSize(100, 32)

IF saveBtn.WasClicked() THEN
    PRINT "Save requested"
END IF
```

```zia
// Zia
var saveBtn = Button.New(root, "Save");
saveBtn.SetStyle(1);  // Primary style
saveBtn.SetSize(100, 32);

if saveBtn.WasClicked() { /* perform the save */ }
```

---

### TextInput

Single-line text input field.

Long single-line values stay clipped to the field bounds, the caret auto-scrolls horizontally as the cursor moves, and the caret blink is advanced automatically while the field is focused.
`Ctrl+Left` / `Ctrl+Right` now stop on punctuation and whitespace boundaries instead of treating only ASCII spaces as word breaks. `Ctrl/Cmd+Shift+Z` redoes the last undone edit, `Shift+Click` extends the current selection, and double-click selects the word under the pointer.
Text insertion rejects invalid Unicode scalar values such as surrogate codepoints or values above `U+10FFFF`; public text replacement and insertion also skip malformed UTF-8 byte sequences before storing text. Valid input remains UTF-8 encoded in the widget buffer. The native widget has multiline and maximum-length state, but neither is exposed by the current `Zanna.GUI.TextInput` runtime class.
Replacing a selection validates and allocates the full edit before mutating the
buffer, so a rejected insertion does not delete the previous selection.

**Constructor:** `NEW Zanna.GUI.TextInput(parent)`

| Property | Type   | Access | Description            |
|----------|--------|--------|------------------------|
| `Text`   | String | Read   | Current input text     |

| Method                       | Signature          | Description              |
|------------------------------|--------------------|--------------------------|
| `SetText(text)`              | `Void(String)`     | Replace the text, reset the undo/redo baseline, and fire the change callback when the content actually changes |
| `SetPlaceholder(text)`       | `Void(String)`     | Set placeholder text     |
| `SetFont(font, size)`        | `Void(Font, Double)`| Set font and size       |

```basic
DIM nameInput AS Zanna.GUI.TextInput
nameInput = NEW Zanna.GUI.TextInput(root)
nameInput.SetPlaceholder("Enter your name...")
nameInput.SetSize(200, 28)

' Get value
DIM name AS STRING
name = nameInput.Text
```

```zia
// Zia
var nameInput = TextInput.New(root);
nameInput.SetPlaceholder("Enter your name...");
nameInput.SetSize(200, 28);

var name = nameInput.get_Text();
```

---

### Checkbox

Toggle checkbox with label.
Checkbox labels also use the current GUI theme or app font by default, matching buttons, text inputs, dropdowns, and list boxes.

**Constructor:** `NEW Zanna.GUI.Checkbox(parent, text)`

| Method                        | Signature       | Description                                      |
|-------------------------------|-----------------|--------------------------------------------------|
| `IsChecked()`                 | `Boolean()`     | Get checked state                                |
| `IsIndeterminate()`           | `Boolean()`     | Get indeterminate tri-state state                |
| `SetChecked(checked)`         | `Void(Boolean)` | Set checked state                                |
| `SetIndeterminate(value)`     | `Void(Boolean)` | Show or clear indeterminate state; setting it clears checked state |
| `SetText(text)`               | `Void(String)`  | Set label text                                   |

```basic
DIM rememberMe AS Zanna.GUI.Checkbox
rememberMe = NEW Zanna.GUI.Checkbox(root, "Remember me")

IF rememberMe.IsChecked() THEN
    PRINT "Credentials will be remembered"
END IF
```

```zia
// Zia
var rememberMe = Checkbox.New(root, "Remember me");

if rememberMe.IsChecked() { /* persist the user's preference */ }
```

---

### RadioButton

Mutually exclusive radio button (use with RadioGroup).
Radio buttons unregister from their group when destroyed, and destroying a group clears the group reference on surviving radio buttons. This prevents stale selection handles after dynamic UI teardown.
Runtime-created radio groups are opaque handles. After `Destroy()`, that handle is invalid and cannot be reused to create more radio buttons; existing buttons become ungrouped and remain valid widgets.

The group owns an explicit selected-index model. `WasChanged()` consumes only selected-index
transitions, while `GetRevision()` is non-consuming and also advances as buttons register or leave.
Multiple unreported transitions coalesce into one edge. Button `Data` values are byte-exact runtime
strings, so embedded NUL bytes round-trip independently of the visible UTF-8 label.

**Constructor:** `NEW Zanna.GUI.RadioButton(parent, text, group)`

#### RadioGroup methods

| Method                       | Signature          | Description                                      |
|------------------------------|--------------------|--------------------------------------------------|
| `GetSelectedIndex()`         | `Integer()`        | Selected zero-based index, or `-1`               |
| `SetSelectedIndex(index)`    | `Boolean(Integer)` | Select an index, or `-1` to clear; false if invalid |
| `GetCount()`                 | `Integer()`        | Number of registered live buttons                |
| `WasChanged()`               | `Boolean()`        | Consume the independent selected-index edge      |
| `GetRevision()`              | `Integer()`        | Read the non-consuming group revision            |
| `Destroy()`                  | `Void()`           | Detach surviving buttons and invalidate the group handle |

#### RadioButton methods

| Method                     | Signature          | Description                                      |
|----------------------------|--------------------|--------------------------------------------------|
| `IsSelected()`             | `Boolean()`        | True if selected                                 |
| `SetSelected(selected)`    | `Void(Boolean)`    | Set selection state and update its group         |
| `SetText(text)`            | `Void(String)`     | Atomically replace visible label text            |
| `GetText()`                | `String()`         | Copy visible label text                          |
| `SetData(data)`            | `Void(String)`     | Store byte-exact application data                |
| `GetData()`                | `String()`         | Copy byte-exact application data                 |
| `WasChanged()`             | `Boolean()`        | Consume this button's selected-state edge        |
| `GetRevision()`            | `Integer()`        | Read this button's non-consuming revision        |

```basic
' Create a group
DIM group AS Zanna.GUI.RadioGroup
group = NEW Zanna.GUI.RadioGroup()

' Create radio buttons
DIM optA AS Zanna.GUI.RadioButton
DIM optB AS Zanna.GUI.RadioButton
DIM optC AS Zanna.GUI.RadioButton

optA = NEW Zanna.GUI.RadioButton(root, "Option A", group)
optB = NEW Zanna.GUI.RadioButton(root, "Option B", group)
optC = NEW Zanna.GUI.RadioButton(root, "Option C", group)

' Select default
optA.SetSelected(true)
```

```zia
// Zia
var group = RadioGroup.New();
var optA = RadioButton.New(root, "Option A", group);
var optB = RadioButton.New(root, "Option B", group);
var optC = RadioButton.New(root, "Option C", group);
optA.SetSelected(true);
```

---

### Slider

Draggable value slider.
Non-finite values are rejected or replaced with bounded defaults; ranges are normalized so the minimum is never greater than the maximum. Step snapping is clamped after rounding, so a stepped value cannot overshoot the range. Changing `SetStep()` immediately re-snaps the current value to the new grid, so a slider cannot remain between valid step positions. Runtime-created sliders inherit the app default font and size like other text-bearing controls, so value labels stay consistent after `App.SetFont()` or `App.SetFontSize()`.

**Constructor:** `NEW Zanna.GUI.Slider(parent, horizontal)`

| Property | Type   | Access | Description            |
|----------|--------|--------|------------------------|
| `Value`  | Double | Read   | Current slider value   |

| Method                     | Signature                  | Description                    |
|----------------------------|----------------------------|--------------------------------|
| `SetValue(value)`          | `Void(Double)`             | Set current value              |
| `SetRange(min, max)`       | `Void(Double, Double)`     | Set value range                |
| `SetStep(step)`            | `Void(Double)`             | Set step (0 for continuous)    |

```basic
DIM volume AS Zanna.GUI.Slider
volume = NEW Zanna.GUI.Slider(root, 1)  ' Horizontal
volume.SetRange(0, 100)
volume.SetValue(50)
volume.SetSize(200, 20)

' Use value
Zanna.Audio.Mixer.SetMasterVolume(INT(volume.Value))
```

```zia
// Zia
var volume = Slider.New(root, 1);  // Horizontal
volume.SetRange(0.0, 100.0);
volume.SetValue(50.0);
volume.SetSize(200, 20);
```

---

### Spinner

Numeric input with increment/decrement buttons.

The spinner now behaves as a real interactive widget: the up/down buttons repaint correctly, `Up` / `Down` keys adjust the value while focused, the mouse wheel steps the value when hovered, and the value field accepts direct keyboard entry.
Typing while the spinner is focused starts inline editing, `Enter` commits the typed number, and `Escape` restores the previous formatted value.
Programmatic values, ranges, steps, and decimal counts are clamped to finite supported ranges.
Reversed ranges are normalized, non-finite input is rejected or clamped, and long formatted decimal values resize the internal display buffer instead of truncating at a fixed length.
Spinners created after `App.SetFont()` inherit the app font and size for their value text.
Property inspectors can call `SetIndeterminate(true)` when a spinner represents
several different values. The control displays `Mixed` while retaining `Value`
as the range-clamped editing seed. `SetValue`, typed entry, arrow buttons,
`Up` / `Down`, and the mouse wheel resolve the mixed state; range and formatting
changes do not. `IsIndeterminate()` distinguishes the editing seed from a value
common to the complete selection.

**Constructor:** `NEW Zanna.GUI.Spinner(parent)`

| Property | Type   | Access | Description            |
|----------|--------|--------|------------------------|
| `Value`  | Double | Read   | Current spinner value  |

| Method                          | Signature                  | Description                                      |
|---------------------------------|----------------------------|--------------------------------------------------|
| `SetValue(value)`               | `Void(Double)`             | Set a concrete value and resolve mixed state     |
| `SetIndeterminate(mixed)`       | `Void(Boolean)`            | Present a mixed group value while retaining seed |
| `IsIndeterminate()`             | `Boolean()`                | Return whether the group value remains mixed     |
| `SetRange(min, max)`            | `Void(Double, Double)`     | Set value range                                  |
| `SetStep(step)`                 | `Void(Double)`             | Set step increment                               |
| `SetDecimals(decimals)`         | `Void(Integer)`            | Set decimal places                               |

```basic
DIM quantity AS Zanna.GUI.Spinner
quantity = NEW Zanna.GUI.Spinner(root)
quantity.SetRange(1, 100)
quantity.SetStep(1)
quantity.SetDecimals(0)
quantity.SetValue(1)
```

```zia
// Zia
var quantity = Spinner.New(root);
quantity.SetRange(1.0, 100.0);
quantity.SetStep(1.0);
quantity.SetDecimals(0);
quantity.SetValue(1.0);
```

---

### ProgressBar

Progress indicator bar.

Styles `0`, `1`, and `2` render as a horizontal bar, a circular ring indicator, and an indeterminate sliding bar respectively. Indeterminate progress bars created through the underlying C GUI layer now advance their animation during the normal app render loop.
Programmatic values are clamped to the `0.0` through `1.0` range, with non-finite values treated as `0.0`.
Progress bars created after `App.SetFont()` inherit the app font and size for optional percentage text.

**Constructor:** `NEW Zanna.GUI.ProgressBar(parent)`

| Property | Type   | Access | Description                  |
|----------|--------|--------|------------------------------|
| `Value`  | Double | Read   | Progress value (0.0 to 1.0)  |

| Method                     | Signature          | Description                                              |
|----------------------------|--------------------|----------------------------------------------------------|
| `SetValue(value)`          | `Void(Double)`     | Set progress (0.0-1.0)                                   |
| `SetStyle(style)`          | `Void(Integer)`    | Set style: `0` bar, `1` circular, `2` indeterminate      |
| `ShowPercentage(show)`     | `Void(Integer)`    | Show or hide percentage text                             |

```basic
DIM progress AS Zanna.GUI.ProgressBar
progress = NEW Zanna.GUI.ProgressBar(root)
progress.SetSize(300, 20)

' Update as work advances (0.0 through 1.0)
progress.SetValue(0.5)
```

```zia
// Zia
var progress = ProgressBar.New(root);
progress.SetSize(300, 20);
progress.SetValue(0.75);  // 75%
```

---

### Dropdown

Dropdown selection list.

Item additions, removals, selection changes, and font changes invalidate the widget immediately. Removing or clearing the selected item also raises the selection-change callback with the new effective selection. The closed control and popup width now size to the widest visible item instead of using a fixed narrow default. Popup hit-testing is screen-correct even when the control is nested or repositioned, long headers clip cleanly, and the popup panel tracks fractional scroll instead of stepping a whole row at a time. Keyboard typeahead selects matching items while closed and moves the hovered row while open; it decodes the first UTF-8 codepoint of each item instead of comparing raw bytes. `PageUp` / `PageDown` navigate long menus.

**Constructor:** `NEW Zanna.GUI.Dropdown(parent)`

| Property       | Type    | Access | Description                |
|----------------|---------|--------|----------------------------|
| `Selected`     | Integer | Read   | Selected item index (-1=none) |
| `SelectedText` | String  | Read   | Selected item text            |

| Method                 | Signature          | Description                     |
|------------------------|--------------------|---------------------------------|
| `AddItem(text)`        | `Integer(String)`  | Add item, returns index         |
| `Clear()`              | `Void()`           | Remove all items                |
| `RemoveItem(index)`    | `Void(Integer)`    | Remove item by index            |
| `SetPlaceholder(text)` | `Void(String)`     | Set placeholder text            |
| `SetSelected(index)`   | `Void(Integer)`    | Set selected index              |

```zia
// Zia example
var dropdown = Dropdown.New(root);
dropdown.SetPlaceholder("Pick one...");
dropdown.AddItem("Red");
dropdown.AddItem("Green");
dropdown.AddItem("Blue");
dropdown.SetSelected(1);
Say("Selected: " + dropdown.get_SelectedText());
```

```basic
DIM colorPicker AS Zanna.GUI.Dropdown
colorPicker = NEW Zanna.GUI.Dropdown(root)
colorPicker.SetPlaceholder("Select color...")
colorPicker.AddItem("Red")
colorPicker.AddItem("Green")
colorPicker.AddItem("Blue")
colorPicker.SetSize(150, 28)

IF colorPicker.Selected >= 0 THEN
    PRINT "Selected: "; colorPicker.SelectedText
END IF
```

---

### ScrollView

Scrollable container for arbitrary nested widget content.

`ScrollTo(widget)` reveals a live descendant at any nesting depth. It changes
only the scroll axes needed to make the target fully visible and accounts for
the current scrollbar-reduced viewport before clamping to the content extent.
The request is retained through the next layout pass, so it also works while an
enclosing split pane is being restored from a collapsed zero-sized state.
Null, stale, destroyed, detached, foreign, and non-descendant widget handles
are safe no-ops.

**Constructor:** `NEW Zanna.GUI.ScrollView(parent)`

| Method                          | Signature                    | Description |
|---------------------------------|------------------------------|-------------|
| `SetScroll(x, y)`               | `Void(Double, Double)`       | Set clamped content offsets |
| `SetContentSize(width, height)` | `Void(Double, Double)`       | Override the scrollable extent; non-positive axes return to measured content |
| `ScrollTo(widget)`              | `Void(Zanna.GUI.Widget)`     | Reveal one live descendant without changing focus |
| `GetScrollX()`                  | `Double()`                   | Read the horizontal content offset |
| `GetScrollY()`                  | `Double()`                   | Read the vertical content offset |

```zia
var scroll = ScrollView.New(root);
var content = VBox.New();
scroll.AddChild(content);
var validationField = TextInput.New(content);

// Reveal first, then choose whether focus should move.
scroll.ScrollTo(validationField);
validationField.Focus();
```

---

### ListBox

Scrollable list of selectable items with enhanced item management.

Hit-testing uses widget-local coordinates, so nested list boxes select the correct row. Measured height also follows the actual item count for short lists instead of reserving five rows unconditionally. In multi-select mode, `Ctrl/Cmd+Click` toggles rows, `Shift+Click` extends the active range, and keyboard navigation with `Shift` extends the current selection. Toggling the current row off now clears or moves the current selection instead of leaving `Selected` / `SelectedIndex` pointed at an unselected row.
Item text is clipped to the viewport and item add/remove/clear/select operations invalidate the list immediately so the visual state updates on the same frame.
Binding a `Zanna.GUI.VirtualList` enables the same lower virtual mode through the public runtime.
Cache invalidation refreshes visible rows on the next paint even when the visible range has not
changed.
`SelectIndex(index)` leaves the current selection unchanged when `index` is negative or outside the current item range (including a native virtual-list total), and `ItemSetText()` preserves the old text if allocation fails.
`ItemSetData()` stores runtime strings with their explicit length, so embedded NUL bytes round-trip through `ItemGetData()`. Runtime-owned item data is freed automatically by `RemoveItem()` and `Clear()`.
Item handles are runtime-managed and become inert after `RemoveItem()`, `Clear()`, or list-box destruction; later item method calls return empty/0 values or no-op safely.
Native virtual-list selection and cache growth fail closed if allocation or size
checks fail; the widget does not publish a larger virtual item count until its
selection bitmap can represent that range.
In virtual mode, `Count` reports the logical model total rather than the number of materialized
cache rows. `WasSelectionChanged()` reports real selection transitions, including selected-item
removal and `Clear()` calls that remove a selection.

Retained lists can opt into application-directed ordering with
`SetReorderable(true)`. A captured row drag paints an insertion marker and
auto-scrolls near the viewport edges; Alt+Up/Down provides the keyboard
equivalent. The control does not mutate its linked rows. Instead,
`WasReorderRequested()` publishes one edge and the source/final-target getters
identify the requested move so an application can validate, update its model,
commit or roll back, and rebuild the presentation. A click below the drag
threshold and a drop back at the source are no-ops. Escape cancels a drag.
Virtual lists do not publish retained-index reorder requests.

**Constructor:** `NEW Zanna.GUI.ListBox(parent)`

### Properties

| Property             | Type    | Access | Description                              |
|----------------------|---------|--------|------------------------------------------|
| `Count`              | Integer | Read   | Number of items in the list              |
| `Selected`           | Object  | Read   | Currently selected item handle           |
| `SelectedIndex`      | Integer | Read   | Index of selected item (-1 if none)      |

### Methods

| Method                    | Signature              | Description                       |
|---------------------------|------------------------|-----------------------------------|
| `AddItem(text)`           | `Object(String)`       | Add item, returns item handle     |
| `Clear()`                 | `Void()`               | Remove all items                  |
| `GetSelectedData()`       | `Seq[String]()`        | Copy selected retained-row data in row order |
| `GetSelectedText()`       | `String()`             | Get selected text, or empty if none |
| `GetReorderSourceIndex()` | `Integer()`            | Most recently requested retained source, or -1 |
| `GetReorderTargetIndex()` | `Integer()`            | Most recently requested final target, or -1 |
| `ItemGetData(item)`       | `String(Object)`       | Get item user data                |
| `ItemGetText(item)`       | `String(Object)`       | Get item display text             |
| `ItemSetData(item, data)` | `Void(Object, String)` | Set item user data                |
| `ItemSetText(item, text)` | `Void(Object, String)` | Set item display text             |
| `ItemSetTextColor(item, color)` | `Void(Object, Integer)` | Set item text color          |
| `RemoveItem(item)`        | `Void(Object)`         | Remove item                       |
| `ScrollToBottom()`        | `Void()`               | Scroll to the final row           |
| `ScrollToTop()`           | `Void()`               | Scroll to the first row           |
| `Select(item)`            | `Void(Object)`         | Select item (NULL to deselect)    |
| `SelectIndex(index)`      | `Void(Integer)`        | Select item by index              |
| `SetFont(font, size)`     | `Void(Font, Double)`   | Set font for list items           |
| `SetMultiSelect(enabled)` | `Void(Boolean)`        | Enable or disable multiple selection |
| `SetReorderable(enabled)` | `Void(Boolean)`        | Enable direct retained-row reorder requests |
| `SetVirtualModel(model)`  | `Boolean(VirtualList)` | Bind a viewport-backed model without copying all rows |
| `ClearVirtualModel()`     | `Void()`               | Detach the model and restore retained-item mode |
| `GetVisibleFirst()`       | `Integer()`            | First model row intersecting the viewport |
| `GetVisibleCount()`       | `Integer()`            | Number of rows requested for this viewport |
| `WasReorderRequested()`   | `Boolean()`            | Consume one application reorder edge |
| `WasSelectionChanged()`   | `Boolean()`            | True if selection changed this frame |

`GetSelectedData()` preserves one entry per selected retained row, including an
empty string for a row without item data. It returns an empty sequence for
virtual lists; use the bound `VirtualList` model's stable row IDs in that mode.

### Example

```basic
DIM fileList AS Zanna.GUI.ListBox
fileList = NEW Zanna.GUI.ListBox(root)
fileList.SetSize(200, 300)

' Add items with display text
DIM item1 AS Object = fileList.AddItem("document.txt")
DIM item2 AS Object = fileList.AddItem("image.png")
DIM item3 AS Object = fileList.AddItem("data.csv")

' Attach user data to items (e.g., file paths)
fileList.ItemSetData(item1, "/home/user/documents/document.txt")
fileList.ItemSetData(item2, "/home/user/pictures/image.png")
fileList.ItemSetData(item3, "/home/user/data/data.csv")

' Get item count via property
PRINT "Total items: "; fileList.Count

' Select by index
fileList.SelectIndex(0)  ' Select first item

' Check for selection changes in main loop
DO WHILE NOT app.ShouldClose
    app.Poll()

    IF fileList.WasSelectionChanged() THEN
        IF fileList.SelectedIndex >= 0 THEN
            DIM selected AS Object = fileList.Selected
            PRINT "Selected: "; fileList.ItemGetText(selected)
            PRINT "Path: "; fileList.ItemGetData(selected)
        END IF
    END IF

    app.Render()
LOOP
```

### File Browser Example

```basic
' Build a simple file browser
DIM fileList AS Zanna.GUI.ListBox
fileList = NEW Zanna.GUI.ListBox(root)
fileList.SetSize(250, 400)

' Set custom font
DIM monoFont AS Zanna.GUI.Font
monoFont = Zanna.GUI.Font.Load("consola.ttf")
fileList.SetFont(monoFont, 11)

' Populate with regular files. Dir.Files returns names without the directory prefix.
DIM path AS STRING = Zanna.IO.Dir.Current()
DIM files AS Zanna.Collections.Seq = Zanna.IO.Dir.Files(path)
FOR i = 0 TO Zanna.Collections.Seq.get_Count(files) - 1
    DIM name AS STRING = Zanna.Collections.Seq.GetStr(files, i)
    DIM item AS Object = fileList.AddItem(name)
    fileList.ItemSetData(item, Zanna.IO.Path.Join(path, name))
NEXT i

' Handle a selection change
IF fileList.WasSelectionChanged() THEN
    IF fileList.SelectedIndex >= 0 THEN
        DIM selected AS Object = fileList.Selected
        PRINT "Selected path: "; fileList.ItemGetData(selected)
    END IF
END IF
```

```zia
// Zia
var fileList = ListBox.New(root);
fileList.SetSize(200, 300);

var item1 = fileList.AddItem("document.txt");
var item2 = fileList.AddItem("image.png");
fileList.ItemSetData(item1, "/home/user/doc.txt");
fileList.SelectIndex(0);

if fileList.WasSelectionChanged() && fileList.get_SelectedIndex() >= 0 {
    // fileList.GetSelectedText() is the selected row's text.
}
```

### TreeView

`TreeView` is the retained hierarchical control for project browsers, scene
outliners, and other structured editors. `AddNode(parent, text)` accepts
`null` for a top-level row or a live `TreeView.Node` for a real child; hierarchy
is not encoded in display text. Nodes own copied text, icon text, stable IDs,
and byte-exact string data until they are removed or the tree is destroyed.

Retained trees are single-select by default. After `SetMultiSelect(true)`,
Ctrl/Command-click toggles a row, Shift-click and Shift+Up/Down select an
inclusive visible range, and ordinary navigation replaces the selection.
Programmatic `Select(node)` is additive in multi-select mode so an editor can
restore several rows after rebuilding; `Select(null)` always clears all rows.
`GetSelected()` returns the primary row. `GetSelectedData()` returns the data
of every selected retained node in complete structural preorder, including
selected descendants hidden beneath a collapsed parent. Missing node data
contributes an empty string. Virtual trees remain model-owned single-selection
controls and return an empty selected-data sequence.

Application-directed drag and drop has explicit modes:

| Mode | Meaning |
|------|---------|
| `0` | Disabled |
| `1` | Legacy container-only `INTO` drops |
| `2` | Row-aware `BEFORE` / `INTO` / `AFTER` drops |

`SetDragDropEnabled(true)` is retained as a mode-1 compatibility wrapper.
Mode 2 accepts leaf targets and divides each row into top 30%, middle 40%, and
bottom 30% regions. `GetDropPosition()` returns `0` for `BEFORE`, `1` for
`INTO`, and `2` for `AFTER`. A completed drop is latched until `ClearDrop()`;
the TreeView does not mutate its nodes, so the application can validate,
commit, roll back, and refresh one transaction.

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddNode(parent, text)` | `Object(Object, String)` | Append a retained child; null parent means top level |
| `RemoveNode(node)` / `Clear()` | `Void(Object)` / `Void()` | Retire a subtree or every row |
| `Expand(node)` / `Collapse(node)` / `Toggle(node)` | `Void(Object)` | Change retained expansion |
| `Select(node)` / `GetSelected()` | `Void(Object)` / `Object()` | Change/read the primary selection |
| `SetMultiSelect(enabled)` | `Void(Boolean)` | Enable retained modifier and range selection |
| `GetSelectedData()` | `Seq[String]()` | Copy selected node data in complete preorder |
| `SetDragDropMode(mode)` | `Void(Integer)` | Select disabled, legacy INTO, or row-aware DnD |
| `WasDropReceived()` / `ClearDrop()` | `Boolean()` / `Void()` | Poll and consume a completed drop |
| `GetDropSourceData()` / `GetDropTargetData()` | `String()` | Read stable data from the latched rows |
| `GetDropPosition()` | `Integer()` | Read 0=before, 1=into, or 2=after |
| `WasSelectionChanged()` | `Boolean()` | Consume one coalesced selection-change edge |

```zia
var hierarchy = TreeView.New(panel);
hierarchy.SetMultiSelect(true);
hierarchy.SetDragDropMode(2);

var world = hierarchy.AddNode(null, "World");
TreeView.Node.SetData(world, "node:world");
var player = hierarchy.AddNode(world, "Player");
TreeView.Node.SetData(player, "node:player");
hierarchy.Expand(world);

if hierarchy.WasDropReceived() {
    var sourceId = hierarchy.GetDropSourceData();
    var targetId = hierarchy.GetDropTargetData();
    var position = hierarchy.GetDropPosition();
    // Validate and commit the model transaction, then rebuild the retained rows.
    hierarchy.ClearDrop();
}
```

### Virtual list and tree models

`VirtualList` and `VirtualTree` keep application data separate from visual controls. A binding is
non-owning in both directions: destroying the model disables virtual mode on its control, while
destroying or clearing the control immediately invalidates the model's raw control pointer. Call
`Unbind()` when changing screens early; it is safe to omit it during normal endpoint destruction.

Stable IDs must be non-empty and unique. Duplicate insertion traps with
`GUI model ID must be unique: <id>` and leaves the existing model unchanged. A list row without an
explicit ID uses its canonical decimal index (`"0"`, `"1"`, …); explicit IDs may not collide with
those implicit IDs. `SetCount` also rejects a growth that would introduce such a collision.

#### VirtualList

| Member | Signature | Description |
|--------|-----------|-------------|
| `New(count, rowHeight, viewportHeight)` | `VirtualList(Integer, Integer, Integer)` | Create a sparse logical model; sizes are compatibility pixel values |
| `SetCount(count)` | `Void(Integer)` | Resize and prune sparse IDs/text outside the new range |
| `SetRowId(row, id)` | `Void(Integer, String)` | Assign a unique stable ID |
| `SetRowText(row, text)` | `Void(Integer, String)` | Store display text only for this row |
| `InvalidateRow(row)` | `Void(Integer)` | Refresh one visible provider cache entry |
| `Bind(listBox)` / `Unbind()` | `Boolean(Object)` / `Void()` | Attach or detach the visual ListBox |
| `VisibleRange(scrollY)` | `Map(Integer)` | Compute a standalone overscanned range |
| `SelectId(id)` | `Void(String)` | Select by stable ID in expected O(1) time |
| `GetSelectedId()` / `GetSelectedIndex()` | `String()` / `Integer()` | Read synchronized model/control selection |

The lower ListBox owns a packed selection bitmap and a viewport-sized text cache, not retained item
objects. It invokes the model provider only during viewport painting. `GetVisibleFirst` and
`GetVisibleCount` are O(1), do not invoke the provider, and include up to two safety rows used by
the renderer. `SetRowText` replaces embedded NUL bytes with visible U+FFFD so the C renderer cannot
silently truncate the rest of a runtime string.

```zia
// Zia — a 100k-row model with only the visible text populated.
var rows = VirtualList.New(100000, 24, 480);
rows.SetRowId(42000, "search-result:42000");
rows.SetRowText(42000, "Matched symbol");

var results = ListBox.New(panel);
results.SetVirtualModel(rows);
rows.SelectId("search-result:42000");

// Refill only the application's currently relevant window.
var first = results.GetVisibleFirst();
var count = results.GetVisibleCount();
```

#### VirtualTree

| Member | Signature | Description |
|--------|-----------|-------------|
| `New()` | `VirtualTree()` | Create an empty hash-indexed tree model |
| `AddNode(parentId, id, text)` | `Void(String, String, String)` | Add one uniquely identified node |
| `MoveNode(id, parentId)` | `Boolean(String, String)` | Reparent without changing identity; cycles reject atomically |
| `SetNodeText(id, text)` | `Boolean(String, String)` | Replace a node label without re-adding it |
| `Expand(id)` / `Collapse(id)` | `Map(String)` / `Void(String)` | Change expansion; Expand reports lazy-population need |
| `VisibleRowsRange(first, count)` | `Seq(Integer, Integer)` | Materialize only the requested flattened slice |
| `VisibleRows()` | `Seq()` | Compatibility operation that explicitly requests every visible row |
| `RefreshSubtree(id)` | `Void(String)` | Remove descendants and mark the node for lazy repopulation |
| `Bind(treeView)` / `Unbind()` | `Boolean(Object)` / `Void()` | Attach or detach the visual TreeView |

`TreeView.SetVirtualModel(model)` renders directly from the model's flattened stable-ID index; it
does not manufacture `TreeView.Node` handles. Pointer selection, arrow navigation, expansion, and
activation route back to the model. The first flattened-index build is linear after structural
changes; warm viewport slices are O(requested rows), and painting is bounded by the viewport.
Ordinary retained nodes already present in the TreeView are preserved and become visible again
after `ClearVirtualModel()`.

---

### OutputPane

Scrollable text surface for build/run logs, search results, and the integrated
terminal (`SetTerminalMode`). It inherits the app's default font. Use `SetFont()` with a
monospace font when column and row counts must describe a fixed terminal grid; the pane does not
enforce monospace by itself.

**Constructor:** `NEW Zanna.GUI.OutputPane(parent)`

**Text & cell metrics** — use these instead of hardcoding a pixels-per-character
guess; they track the pane's actual font and DPI scale:

| Method               | Signature         | Description                                                       |
|----------------------|-------------------|-------------------------------------------------------------------|
| `GetCellWidth()`     | `Integer()`       | Pixel advance of one character cell (`"M"`); `0` if no font        |
| `GetCellHeight()`    | `Integer()`       | Pixel height of one line; `0` if no font                          |
| `MeasureText(text)`  | `Integer(String)` | Pixel width of `text` in the pane's font; `0` if no font/empty     |
| `ColumnsForWidth()`  | `Integer()`       | Whole columns that fit across the pane: `floor(width / cellWidth)` |
| `RowsForHeight()`    | `Integer()`       | Whole rows that fit down the pane: `floor(height / cellHeight)`    |

`ColumnsForWidth() == GetWidth() / GetCellWidth()` and `MeasureText("M") == GetCellWidth()`.
Metric methods return `0` when the pane has no usable font (or, where applicable, no size/text).
An attached pane normally inherits the app font, so an explicit `SetFont()` is not required unless
you need a particular face such as a monospace terminal font.

**Content & terminal:** `Append(text)`, `AppendLine(text)`,
`AppendStyled(text, fg, bg, bold)`, `Clear()`, `ScrollToTop()`, `ScrollToBottom()`,
`SetAutoScroll(on)`, `SetMaxLines(n)`, `get_LineCount`, `GetSelection()`,
`SelectAll()`, `SetFont(font, size)`, `SetTerminalMode(on)`, and `TakeInput()`
(drains captured keystrokes in terminal mode).

### Example

```zia
// Zia — size a terminal grid from the real font instead of guessing 8x18 px.
var pane = OutputPane.New(root);
pane.SetFont(monoFont, 14.0);
pane.SetTerminalMode(true);
var cols = pane.ColumnsForWidth();   // was: pane.GetWidth() / 8
var rows = pane.RowsForHeight();     // was: pane.GetHeight() / 18
```

---

### Image

Image display widget.

`SetScaleMode()` and `SetOpacity()` affect the rendered output directly. If you do not call `SetSize()`, the widget measures to the image's natural pixel dimensions once pixels have been assigned.
`SetPixels()` accepts a `Zanna.Graphics.Pixels` object whose elements are packed as `0xRRGGBBAA`; the GUI layer converts them to byte RGBA. Width or height values less than or equal to zero use the source dimensions, requested sizes larger than the source are clamped, and `NULL` pixels clear the image.
`TrySetPixels()` provides the same conversion and size behavior but reports failure and never
clears the previous image when its source is invalid or storage cannot be allocated.
`UpdateRegion()` converts only a validated source rectangle and copies it atomically into an
existing image. This is the preferred path for streaming previews, canvases, and video frames.
Nearest-neighbor filtering remains the compatibility default; select `ImageFilter.Bilinear` for
smoother deliberate resizing. Repeated paints reuse a scaled cache, and unchanged dimensions
reuse both source and cache storage.
The native C widget `LoadFile()` now decodes BMP directly and uses platform image decoders for PNG/JPEG where available; the Zanna runtime binding continues to route through `Zanna.Graphics.Pixels` for PNG, BMP, JPEG, and GIF.

**Constructor:** `NEW Zanna.GUI.Image(parent)`

| Method                         | Signature                              | Description                    |
|--------------------------------|----------------------------------------|--------------------------------|
| `SetPixels(pixels, w, h)`      | `Void(Pixels, Integer, Integer)`       | Set image from a `Zanna.Graphics.Pixels` buffer |
| `TrySetPixels(pixels, w, h)`   | `Boolean(Pixels, Integer, Integer)`    | Atomically upload pixels and report success |
| `UpdateRegion(pixels, sx, sy, w, h, dx, dy)` | `Boolean(Pixels, Integer × 6)` | Atomically copy one source rectangle |
| `LoadFile(path)`               | `Integer(String)`                      | Load PNG, BMP, JPEG, or GIF file directly (1=ok, 0=fail) |
| `Clear()`                      | `Void()`                               | Clear image                    |
| `SetScaleMode(mode)`           | `Void(Integer)`                        | 0=none, 1=fit, 2=fill, 3=stretch |
| `SetFilter(filter)` / `GetFilter()` | `Void(Integer)` / `Integer()`     | Select/read nearest (0) or bilinear (1) filtering |
| `SetOpacity(opacity)`          | `Void(Double)`                         | Set opacity, clamped to 0.0–1.0; non-finite values become 1.0 |

```basic
DIM preview AS Zanna.GUI.Image
preview = NEW Zanna.GUI.Image(root)
preview.SetSize(200, 200)
preview.SetScaleMode(1)  ' Fit

' Load directly from file (PNG, BMP, JPEG, or GIF)
preview.LoadFile("photo.png")
```

```zia
// Zia
var preview = Image.New(root);
preview.SetSize(200, 200);
preview.SetScaleMode(1);  // Fit
preview.SetFilter(ImageFilter.Bilinear);

// Load directly from file (PNG, BMP, JPEG, or GIF)
preview.LoadFile("photo.png");
```

---

### Color Widgets

`ColorSwatch`, `ColorPalette`, and `ColorPicker` are public managed controls. Their color contract
is deliberately uniform: every public color is an opaque `0x00RRGGBB` integer. `ColorPicker`
represents alpha separately in `[0,255]`, and `SetColor()` preserves the current alpha component.
Upper bits supplied to a color constructor or setter are ignored.

All three controls are focusable and expose `WasChanged()` plus a non-consuming `GetRevision()`.
`WasChanged()` consumes only its own coalesced edge; reading it never resets the revision. Swatches
activate with Space or Enter. Palettes navigate with arrows, Home, and End, and activate with Space
or Enter. Pickers use Up/Down to choose a channel, Left/Right to adjust it, Shift+Left/Right for
steps of ten, and Home/End for 0/255. Focus rings, selection borders, and channel labels use live
theme colors, including high-contrast themes.

#### ColorSwatch

**Constructor:** `ColorSwatch.New(parent, color)`

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetColor(color)` | `Void(Integer)` | Replace the low-24-bit RGB color |
| `GetColor()` | `Integer()` | Return `0x00RRGGBB` |
| `SetSelected(selected)` | `Void(Boolean)` | Set visual and semantic selection |
| `IsSelected()` | `Boolean()` | Query selection |
| `WasChanged()` | `Boolean()` | Consume a color-or-selection change edge |
| `GetRevision()` | `Integer()` | Read the non-consuming state revision |

#### ColorPalette

**Constructor:** `ColorPalette.New(parent)`

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddColor(color)` | `Void(Integer)` | Append a copied `0x00RRGGBB` entry |
| `RemoveColor(index)` | `Boolean(Integer)` | Remove and compact one entry; false if invalid |
| `Clear()` | `Void()` | Remove every entry and clear selection |
| `GetColorCount()` | `Integer()` | Return the entry count |
| `GetColorAt(index)` | `Integer(Integer)` | Return RGB, or 0 if invalid (black is also 0) |
| `SetSelectedIndex(index)` | `Void(Integer)` | Select an entry; -1 clears selection |
| `GetSelectedIndex()` | `Integer()` | Return the selected index or -1 |
| `WasChanged()` | `Boolean()` | Consume a structural-or-selection edge |
| `GetRevision()` | `Integer()` | Read the non-consuming state revision |

Removing an entry before the selection shifts the selected index so it continues to identify the
same surviving color. Removing the selected entry clears selection. Use `GetColorCount()` to
validate an index before `GetColorAt()` when black (`0`) must be distinguished from absence.

#### ColorPicker

**Constructor:** `ColorPicker.New(parent)`

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetColor(color)` | `Void(Integer)` | Set RGB while preserving alpha |
| `GetColor()` | `Integer()` | Return `0x00RRGGBB` |
| `SetAlphaEnabled(enabled)` | `Void(Boolean)` | Show or hide alpha editing |
| `IsAlphaEnabled()` | `Boolean()` | Query whether alpha editing is enabled |
| `GetRed/Green/Blue()` | `Integer()` | Return an RGB component in `[0,255]` |
| `GetAlpha()` | `Integer()` | Return alpha in `[0,255]` (default 255) |
| `WasChanged()` | `Boolean()` | Consume an RGB-or-alpha component edge |
| `GetRevision()` | `Integer()` | Read the non-consuming state revision |

```zia
// Zia — a compact palette feeding a full keyboard-accessible picker.
var palette = ColorPalette.New(root);
palette.AddColor(0x2D7FF9);
palette.AddColor(0x22A06B);
palette.AddColor(0xE5484D);

var picker = ColorPicker.New(root);
picker.SetAlphaEnabled(true);
if palette.WasChanged() && palette.GetSelectedIndex() >= 0 {
    picker.SetColor(palette.GetColorAt(palette.GetSelectedIndex()));
}
```

---

### Grid

Viewport-aware tabular data control with cached automatic column sizing, sparse virtual rows,
selection, keyboard navigation, sorting requests, pointer resizing, controller-driven editing,
scrolling, accessibility semantics, and independent event edges. Existing display-table code stays
compatible: the Grid remains non-interactive and cannot take focus until selection, sorting,
resizing, or editing is explicitly enabled.

**Constructor:** `NEW Zanna.GUI.Grid(parent)`

#### Content and virtualization

| Member | Signature | Description |
|--------|-----------|-------------|
| `SetColumns(count)` | `Void(Integer)` | Atomically set columns and clear all header/row/interactive state |
| `SetHeader(col, text)` | `Void(Integer, String)` | Copy a header; empty clears it |
| `SetCell(row, col, text)` | `Void(Integer, Integer, String)` | Set a dense compatibility cell and grow dense rows |
| `SetVirtualRowCount(count)` | `Void(Integer)` | Switch to sparse mode without allocating per-row storage |
| `SetVirtualCell(row, col, text)` | `Void(Integer, Integer, String)` | Materialize/replace one sparse cell; empty removes it |
| `GetCell(row, col)` | `String(Integer, Integer)` | Copy dense or sparse cell text; empty if absent/invalid |
| `SetViewportRows(first, count)` | `Void(Integer, Integer)` | Set first/max painted rows; count `0` derives it from height |
| `Clear()` | `Void()` | Clear dense or sparse rows while retaining columns/headers |
| `RowCount` | `Integer` (property) | Full logical dense-or-virtual row count |
| `ColumnCount` | `Integer` (property) | Column count |

Virtual mode stores only non-empty values supplied through `SetVirtualCell`; a logical 10,000-row
table with 40 supplied cells owns 40 cell strings, not a 10,000 × column matrix. Painting starts at
the viewport's sparse lower bound and visits only visible rows × columns. Automatic widths are
cached when content/font changes, so paint does not rescan the model. Calling dense `SetCell` exits
virtual mode deliberately.

#### Selection and activation

| Member | Signature | Description |
|--------|-----------|-------------|
| `SetSelectable(enabled)` | `Void(Boolean)` | Enable pointer/keyboard selection; disabling clears it |
| `SelectCell(row, col)` | `Boolean(Integer, Integer)` | Select a valid cell |
| `GetSelectedRow()` / `GetSelectedColumn()` | `Integer()` | Selected coordinates, or `-1` |
| `ClearSelection()` | `Void()` | Clear selection |
| `WasSelectionChanged()` | `Boolean()` | Consume only the selection edge |
| `WasActivated()` | `Boolean()` | Consume double-click/Enter activation |

Arrow keys move by cell, Home/End move to the first/last column, and navigation keeps the selected
row visible. Enter records activation. Pointer and keyboard operations share the same state and edge
paths. The Grid's accessibility value reports one-based row/column coordinates while the public API
uses zero-based indices.

#### Sorting, widths, and editing

| Member | Signature | Description |
|--------|-----------|-------------|
| `SetSortable(col, enabled)` | `Void(Integer, Boolean)` | Enable sort requests for one header |
| `SetSort(col, direction)` | `Void(Integer, Integer)` | Record `-1` descending, `0` none, or `1` ascending |
| `GetSortColumn()` / `GetSortDirection()` | `Integer()` | Current normalized sort request |
| `WasSortChanged()` | `Boolean()` | Consume only the sort edge |
| `SetColumnWidth(col, width)` | `Void(Integer, Number)` | Set logical explicit width; `0` restores automatic sizing |
| `SetColumnResizable(col, enabled)` | `Void(Integer, Boolean)` | Enable pointer dragging at its right boundary |
| `GetColumnWidth(col)` | `Integer(Integer)` | Legacy physical-pixel effective width |
| `WasColumnResized()` | `Boolean()` | Consume an effective-width edge |
| `GetResizedColumn()` | `Integer()` | Most recently resized column, or `-1` |
| `SetEditable(enabled)` | `Void(Boolean)` | Enable controller-driven editing |
| `BeginEdit(row, col)` | `Boolean(Integer, Integer)` | Mark a valid cell as the active edit target |
| `CommitEdit(text)` | `Boolean(String)` | Copy replacement text and close the edit |
| `CancelEdit()` / `IsEditing()` | `Void()` / `Boolean()` | Cancel or query edit mode |
| `WasCellEdited()` | `Boolean()` | Consume only an effective committed-text edge |

Header activation cycles ascending → descending → none. Sorting records a request but never
reorders caller-owned data; apply the request to your model, then refresh dense or virtual cells.
Editing is likewise controller-driven: F2 or double-click enters edit mode, an application may show
its preferred editor, and `CommitEdit` or Escape closes it. This keeps virtual model ownership and
validation in the application rather than embedding a second text model inside the Grid.

#### Scrolling, fonts, and observation

| Member | Signature | Description |
|--------|-----------|-------------|
| `ScrollToRow(row)` / `GetScrollRow()` | `Void(Integer)` / `Integer()` | Set/query first viewport row with end clamping |
| `SetFont(font, size)` | `Void(Object, Number)` | Set font with logical size and refresh width caches |
| `WasChanged()` | `Boolean()` | Consume the common content/selection change edge |
| `GetRevision()` | `Integer()` | Read non-consuming monotonic state revision |

Wheel input scrolls only when movement is possible; at either boundary it bubbles to an ancestor
scroll view. `GetRevision` is suitable for multiple observers. Each specialized `Was…` query is an
independent consume-on-read edge and does not hide another event kind or reduce the revision.

### Example

```zia
// Zia — large sparse table with external sort/edit handling.
var grid = Grid.New(panel);
grid.SetColumns(2);
grid.SetHeader(0, "ID");
grid.SetHeader(1, "Value");
grid.SetVirtualRowCount(10000);
grid.SetViewportRows(5000, 40);
grid.SetVirtualCell(5000, 0, "5000");
grid.SetVirtualCell(5000, 1, "ready");
grid.SetSelectable(true);
grid.SetSortable(1, true);
grid.SetEditable(true);

if grid.WasSortChanged() {
    // Reorder the application model using GetSortColumn/GetSortDirection,
    // then repopulate only the requested viewport slice.
}

if grid.WasCellEdited() {
    var row = grid.GetSelectedRow();
    var value = grid.GetCell(row, grid.GetSelectedColumn());
}
```

---

### PopupList

A caret-anchored, filterable, keyboard-navigable popup list rendered above content — the
reusable core of an autocomplete/quick-pick UI. The **host drives keyboard and visibility** and
supplies pre-ranked items (language-specific ranking stays in the host); the widget owns the
popup mechanics (filter, selection, anchor, accept). Created hidden.

**Constructor:** `NEW Zanna.GUI.PopupList(root)`

| Method                   | Signature              | Description                                                  |
|--------------------------|------------------------|--------------------------------------------------------------|
| `AddItem(text)`          | `Void(String)`         | Append an item (host adds them in its preferred rank order)  |
| `Clear()`                | `Void()`               | Remove all items and reset the filter/selection              |
| `SetFilter(text)`        | `Void(String)`         | Keep items containing `text` using byte-oriented, ASCII-style case-insensitive matching |
| `VisibleCount`           | `Integer` (property)   | Number of items matching the filter                          |
| `NavigateUp()` / `NavigateDown()` | `Void()`      | Move the selection within the visible items (clamped)        |
| `SetSelectedIndex(i)`    | `Void(Integer)`        | Set the selection within the visible items                   |
| `GetSelectedIndex()`     | `Integer()`            | Selection index within the visible items, or `-1` if none    |
| `GetSelected()`          | `String()`             | Text of the selected visible item (empty if none)            |
| `AcceptSelected()`       | `Void()`               | Mark the current selection accepted                          |
| `WasAccepted()`          | `Boolean()`            | Whether `AcceptSelected` was called since the last query (consume-on-read) |
| `AnchorAt(x, y)`         | `Void(Number, Number)` | Position the popup top-left (e.g. at the caret pixel)        |
| `SetWidth(w)`            | `Void(Number)`         | Set the popup width                                          |
| `SetMaxRows(n)`          | `Void(Integer)`        | Maximum visible rows before clamping height                 |
| `SetFont(font, size)`    | `Void(Object, Number)` | Set the item font                                           |
| `SetVisible(on)` / `IsVisible()` | `Void(Boolean)` / `Boolean()` | Show/hide the popup                       |

The host typically: clears + adds candidates, `SetFilter(typed)`, `AnchorAt(caretX, caretY)`,
`SetVisible(true)`; then on arrow keys calls `NavigateUp/Down`, on Enter calls `AcceptSelected` and
reads `WasAccepted` to perform the insertion, and hides the popup on edit or escape.

### Example

```zia
// Zia — a caret-anchored completion popup.
var popup = PopupList.New(root);
popup.AddItem("Console");
popup.AddItem("Convert");
popup.AddItem("Canvas");
popup.SetFilter("con");                 // -> "Console", "Convert"
popup.AnchorAt(editor.GetCursorPixelX(), editor.GetCursorPixelY() + 20);
popup.SetVisible(true);
// On Enter:
popup.AcceptSelected();
if popup.WasAccepted() {
    editor.InsertAtCursor(popup.GetSelected());
    popup.SetVisible(false);
}
```

---

### CodeEditor — InsertAndPlaceCursor

`Zanna.GUI.CodeEditor` is the syntax-highlighting multi-line editor (insert, cursor, find, and
selection APIs). One method worth calling out for snippet-style insertion:

| Method                              | Signature                  | Description                                                              |
|-------------------------------------|----------------------------|--------------------------------------------------------------------------|
| `InsertAndPlaceCursor(text, caretOffset)` | `Void(String, Integer)` | Insert `text` at the cursor, then place the caret `caretOffset` characters into the inserted text (counting newlines) |

Use it to drop the caret *inside* a multi-line insertion instead of at its end — e.g. inserting
`"if  {\n\n}"` and placing the caret after `if ` — without walking the inserted text by hand.

```zia
// Zia — insert a snippet and land the caret between the braces.
editor.InsertAndPlaceCursor("if () {\n    \n}", 4); // caret after "if ("
```

---


## See Also

- [Core & Application](core.md)
- [Layout Widgets](layout.md)
- [Containers & Advanced](containers.md)
- [Application Components](application.md)
- [GUI Widgets Overview](README.md)
- [Zanna Runtime Library](../README.md)
