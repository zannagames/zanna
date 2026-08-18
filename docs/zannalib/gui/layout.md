---
status: active
audience: public
last-verified: 2026-08-17
---

# Layout Widgets
> VBox, HBox, and other layout containers

**Part of [Zanna Runtime Library](../README.md) › [GUI Widgets](README.md)**

---

## Layout Widgets

### VBox

Vertical box layout - arranges children top to bottom.

**Constructor:** `NEW Zanna.GUI.VBox()`

### HBox

Horizontal box layout - arranges children left to right.

**Constructor:** `NEW Zanna.GUI.HBox()`

HBox and VBox spacing accounts for child margins. Native alignment settings also account for those
margins when centering or end-aligning content.
The public runtime constructs `VBox`, `HBox`, wrapping `Flex`, two-dimensional `LayoutGrid`, and
edge-claiming `DockPanel` layouts. (`Zanna.GUI.Grid` is the interactive tabular data control, not a
layout manager.) These layouts apply preferred, minimum, and maximum size constraints during
measurement. Arrange passes clamp content and child dimensions at zero, so excessive padding or
margins cannot produce negative child sizes. Spacing and padding also sanitize non-finite or
negative values to zero.
Children positioned with `Widget.SetPosition(x, y)` are treated as manual children
and are skipped by parent layout passes. Use manual positioning for overlays or
absolute placement, and use preferred sizes, flex, margins, spacing, and padding
for children that should be arranged by a layout container.

`LayoutGrid` keeps declared row and column counts within a bounded runtime range and grows an
effective implicit row count for auto-flow or explicit placements beyond the declared rows. Extra
children are placed into real rows with nonzero cell bounds instead of falling back to the top-left
cell.

### Layout Methods

| Method                 | Signature          | Description                       |
|------------------------|--------------------|-----------------------------------|
| `SetSpacing(spacing)`  | `Void(Double)`     | Set space between children        |
| `SetPadding(padding)`  | `Void(Double)`     | Set internal padding              |

These methods are registered on `Zanna.GUI.Container`, but the public objects that implement
spacing are `VBox` and `HBox`. `Flex`, `LayoutGrid`, and `DockPanel` expose their own gap/padding
methods. Calling
`SetSpacing()` on another widget handle is safely ignored. `SetPadding()` uses the common widget
padding field and works on a valid widget handle.

### Typed layout constants

Use the read-only static properties below instead of numeric literals. Their ordinals are stable
runtime ABI values even when the lower C toolkit uses a different enum order.

| Class | Properties and values |
|-------|-----------------------|
| `Zanna.GUI.Align` | `Start=0`, `Center=1`, `End=2`, `Stretch=3` |
| `Zanna.GUI.Justify` | `Start=0`, `Center=1`, `End=2`, `SpaceBetween=3`, `SpaceAround=4`, `SpaceEvenly=5` |
| `Zanna.GUI.FlexDirection` | `Row=0`, `Column=1`, `RowReverse=2`, `ColumnReverse=3` |
| `Zanna.GUI.FlexWrap` | `NoWrap=0`, `Wrap=1`, `WrapReverse=2` |
| `Zanna.GUI.Dock` | `Left=0`, `Top=1`, `Right=2`, `Bottom=3`, `Fill=4` |

### Flex

`Zanna.GUI.Flex.New()` creates a wrapping flexbox-style container. All distances are logical units;
invalid direction, wrap, align, or justify values normalize to their leading/default policy.

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetDirection(direction)` | `Void(Integer)` | Set row/column and forward/reverse main axis |
| `SetWrap(wrap)` | `Void(Integer)` | Set single-line, wrap, or reverse-wrap policy |
| `SetAlign(align)` | `Void(Integer)` | Set cross-axis child alignment |
| `SetJustify(justify)` | `Void(Integer)` | Set main-axis distribution |
| `SetGap(gap)` | `Void(Double)` | Set logical spacing between children and wrapped lines |
| `SetPadding(padding)` | `Void(Double)` | Set uniform logical inner padding |

### LayoutGrid

`Zanna.GUI.LayoutGrid.New()` creates a fixed/auto/fractional two-dimensional layout. Positive track
sizes are fixed logical units, zero is content/auto, and negative values represent fractional
weights. Attach a child with `Widget.AddChild` before `Place`; invalid placement returns false and
does not change either tree.

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetRows(count)` / `SetColumns(count)` | `Void(Integer)` | Set declared track counts |
| `SetRowSize(row, size)` / `SetColumnSize(column, size)` | `Void(Integer, Double)` | Configure fixed, auto, or fractional tracks |
| `SetGap(horizontal, vertical)` | `Void(Double, Double)` | Set logical inter-track gaps |
| `SetPadding(padding)` | `Void(Double)` | Set uniform logical inner padding |
| `Place(child, row, column, rowSpan, columnSpan)` | `Boolean(Object, Integer...)` | Assign one attached direct child atomically |

### DockPanel

`Zanna.GUI.DockPanel.New()` consumes the remaining rectangle one edge at a time. `DockChild` may
attach a detached widget or update an existing direct child; cross-parent moves and invalid docks
return false without mutation. A `Fill` child receives the space remaining after edge-docked
siblings.

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPadding(padding)` | `Void(Double)` | Set uniform logical inner padding |
| `SetGap(gap)` | `Void(Double)` | Set logical spacing between docked children |
| `DockChild(child, dock)` | `Boolean(Object, Integer)` | Attach/update one child with a `Zanna.GUI.Dock` value |

### Example

```basic
' Create a vertical layout
DIM vbox AS Zanna.GUI.VBox
vbox = NEW Zanna.GUI.VBox()
vbox.SetSpacing(10)
vbox.SetPadding(20)

' Add to the root variable obtained from app.Root
root.AddChild(vbox)

' Add widgets to vbox
DIM label1 AS Zanna.GUI.Label
label1 = NEW Zanna.GUI.Label(vbox, "First")

DIM label2 AS Zanna.GUI.Label
label2 = NEW Zanna.GUI.Label(vbox, "Second")

DIM label3 AS Zanna.GUI.Label
label3 = NEW Zanna.GUI.Label(vbox, "Third")
```

```zia
// Zia
var vbox = VBox.New();
vbox.SetSpacing(10.0);
vbox.SetPadding(20.0);
root.AddChild(vbox);

var l1 = Label.New(vbox, "First");
var l2 = Label.New(vbox, "Second");
var l3 = Label.New(vbox, "Third");
```

---

### GroupBox

A titled container for grouping related controls. The constructor attaches the group box to its
parent immediately; `AddChild()` attaches a child to its content area.

**Constructor:** `NEW Zanna.GUI.GroupBox(parent, title)`

| Method            | Signature          | Description                              |
|-------------------|--------------------|------------------------------------------|
| `AddChild(child)` | `Void(Object)`     | Add a widget to the group box            |
| `SetTitle(title)` | `Void(String)`     | Replace the displayed title              |
| `Destroy()`       | `Void()`           | Destroy the group box and its descendants |

```basic
DIM group AS Zanna.GUI.GroupBox
group = NEW Zanna.GUI.GroupBox(root, "Connection")

DIM host AS Zanna.GUI.TextInput
host = NEW Zanna.GUI.TextInput(group)
host.SetPlaceholder("Host name")
```

```zia
var group = GroupBox.New(root, "Connection");
var host = TextInput.New(group);
host.SetPlaceholder("Host name");
```

---

> **Compatibility:** Integer-taking methods remain valid. Typed constant classes are additive and
> are the preferred spelling in new code.


## See Also

- [Core & Application](core.md)
- [Basic Widgets](widgets.md)
- [Containers & Advanced](containers.md)
- [Application Components](application.md)
- [GUI Widgets Overview](README.md)
- [Zanna Runtime Library](../README.md)
