---
status: active
audience: public
last-verified: 2026-07-26
---

# Game UI Widgets

> Lightweight in-game UI widgets for HUDs, menus, and overlays.

**Part of [Zanna Runtime Library — Game](../game.md)**

## Contents

- [Zanna.Game.UI.HudLabel](#zannagameuihudlabel)
- [Zanna.Game.UI.HudBar](#zannagameuihudbar)
- [Zanna.Game.UI.HudPanel](#zannagameuihudpanel)
- [Zanna.Game.UI.HudNineSlice](#zannagameuihudnineslice)
- [Zanna.Game.UI.HudMenuList](#zannagameuihudmenulist)
- [Zanna.Game.UI.HudTextInput](#zannagameuihudtextinput)
- [Zanna.Game.UI.HudTable](#zannagameuihudtable)
- [Zanna.Game.UI.HudTableClickResult](#zannagameuihudtableclickresult)
- [Zanna.Game.UI.HudModal](#zannagameuihudmodal)
- [Zanna.Game.UI.HudSlider](#zannagameuihudslider)
- [Zanna.Game.UI.HudDropdown](#zannagameuihuddropdown)
- [Zanna.Game.UI.HudTooltip](#zannagameuihudtooltip)
- [Zanna.Game.UI.HudButton](#zannagameuihudbutton)
- [Zanna.Game.Dialogue](#zannagamedialogue)
- [Usage Example](#usage-example)

---

## Zanna.Game.UI.HudLabel

Positioned text widget with optional BitmapFont support and integer scaling.

**Type:** Instance (obj)
**Constructor:** `HudLabel.New(x, y, text, color)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `X` | Integer | Read | X position |
| `Y` | Integer | Read | Y position |
| `Color` | Integer | Write | Text color (0xRRGGBB) |
| `Font` | BitmapFont | Write | Custom font (null = built-in 8x8) |
| `Scale` | Integer | Write | Integer scale (1 = normal, 2 = double, etc.) |
| `Visible` | Boolean | Write | Visibility toggle |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetText(text)` | `void(String)` | Update label text (max 511 bytes, UTF-8-safe truncation) |
| `SetPosition(x, y)` | `void(Integer, Integer)` | Move to new position |
| `Draw(canvas)` | `void(Canvas)` | Render to canvas |

The assigned `Font` must be `null` or a `BitmapFont`. It is retained by the label and released when replaced or when the label is freed.

---

## Zanna.Game.UI.HudBar

Progress/health/XP bar with configurable fill direction, colors, and optional border.

**Type:** Instance (obj)
**Constructor:** `Bar.New(x, y, width, height, fgColor, bgColor)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Value` | Integer | Read | Current value |
| `Max` | Integer | Read | Maximum value |
| `Border` | Integer | Write | Border color (0 = no border) |
| `Direction` | Integer | Write | Fill direction: 0=left-to-right, 1=right-to-left, 2=bottom-to-top, 3=top-to-bottom |
| `Visible` | Boolean | Write | Visibility toggle |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetValue(value, max)` | `void(Integer, Integer)` | Set current/max values. Value clamped to [0, max], max clamped to >= 1 |
| `SetPosition(x, y)` | `void(Integer, Integer)` | Move to new position |
| `SetSize(w, h)` | `void(Integer, Integer)` | Resize the bar |
| `SetColors(fg, bg)` | `void(Integer, Integer)` | Update fill and background colors |
| `Draw(canvas)` | `void(Canvas)` | Render to canvas |

Positive non-zero values always draw at least one pixel of fill, so low health/progress does not disappear before reaching zero.

---

## Zanna.Game.UI.HudPanel

Semi-transparent rectangular panel with optional border and rounded corners. Ideal for HUD backgrounds, dialogue boxes, and overlays.

**Type:** Instance (obj)
**Constructor:** `Panel.New(x, y, width, height, bgColor, alpha)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `CornerRadius` | Integer | Write | Rounded corner radius (0 = sharp) |
| `Visible` | Boolean | Write | Visibility toggle |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y)` | `void(Integer, Integer)` | Move to new position |
| `SetSize(w, h)` | `void(Integer, Integer)` | Resize the panel |
| `SetColor(color, alpha)` | `void(Integer, Integer)` | Update background color and alpha (0-255) |
| `SetBorder(color, thickness)` | `void(Integer, Integer)` | Set border color and thickness; color 0 or thickness <= 0 disables the border |
| `Draw(canvas)` | `void(Canvas)` | Render to canvas |

Rounded corners are preserved for both opaque and alpha-blended panel fills. Border thickness is clamped to fit inside the current panel size.

---

## Zanna.Game.UI.HudNineSlice

Scalable bordered UI element that draws a source image using the 9-slice technique. Corners stay fixed, edges tile along one axis, center tiles to fill. Perfect for dialogue boxes, buttons, and inventory slots.

**Type:** Instance (obj)
**Constructor:** `NineSlice.New(pixels, left, top, right, bottom)`

The margins define the sizes of the corner/edge regions in the source Pixels image. `NineSlice` requires a real `Pixels` handle and retains it, so callers do not need to keep a separate owner alive after construction.

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Tint` | Integer | Write | Multiplicative color tint applied at draw time (0 = no tint) |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Draw(canvas, x, y, w, h)` | `void(Canvas, Integer, Integer, Integer, Integer)` | Draw at position with target size |

When the target rectangle is smaller than the configured margins, corners and edges are cropped to the target size instead of drawing outside it. Tinted sources are cached and refreshed when the tint or source pixels change.

---

## Zanna.Game.UI.HudMenuList

Vertical menu with selection highlight and keyboard-friendly wrap-around navigation.

**Type:** Instance (obj)
**Constructor:** `MenuList.New(x, y, itemHeight)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Selected` | Integer | Read/Write | Currently selected item index |
| `Count` | Integer | Read | Number of items |
| `Font` | BitmapFont | Write | Custom font (null = built-in 8x8) |
| `Visible` | Boolean | Write | Visibility toggle |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddItem(text)` | `void(String)` | Append a menu item (max 64 items; exceeding the cap traps) |
| `Clear()` | `void()` | Remove all items and reset selection |
| `MoveUp()` | `void()` | Move selection up (wraps to bottom at index 0) |
| `MoveDown()` | `void()` | Move selection down (wraps to top at last item) |
| `SetColors(text, selected, highlight)` | `void(Integer, Integer, Integer)` | Set text, selected text, and highlight background colors |
| `Draw(canvas)` | `void(Canvas)` | Render to canvas |
| `HandleInput(up, down, confirm)` | `Integer(Boolean, Boolean, Boolean)` | Move selection and return selected index on confirm, otherwise -1 |

### Navigation Behavior

- `MoveUp()` at index 0 wraps to `Count - 1`
- `MoveDown()` at `Count - 1` wraps to 0
- Navigation on an empty list is a no-op
- Hidden menus ignore `HandleInput`
- If both `up` and `down` are true in `HandleInput`, selection does not move; `confirm` is still honored
- Item text is copied into fixed buffers and truncated on UTF-8 codepoint boundaries
- The assigned `Font` must be `null` or a `BitmapFont`; it is retained and released when replaced or when the menu is freed

---

## Zanna.Game.UI.HudTextInput

Editable single-line text field with UTF-8-safe cursoring, selection, placeholder text, and optional password display.

**Type:** Instance (obj)
**Constructor:** `HudTextInput.New(x, y, width, height)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Text` | String | Read/Write | Current text; storage grows as needed, `SetMaxCodepoints` can enforce a codepoint limit, and embedded NUL bytes terminate the stored visible text |
| `Visible` | Boolean | Read/Write | Visibility toggle |
| `IsEnabled` | Boolean | Read/Write | Input enable toggle |
| `Focused` | Boolean | Read/Write | Keyboard focus |
| `TextColor` | Integer | Read/Write | Text color |
| `BackgroundColor` | Integer | Read/Write | Fill color |
| `Font` | BitmapFont | Write | Optional retained font |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `TextLength()` | `Integer()` | Return codepoint count |
| `GetCursor()` / `SetCursor(pos)` | `Integer()` / `Void(Integer)` | Read or move the cursor by codepoint index |
| `SelectAll()` / `ClearSelection()` | `Void()` | Manage selection |
| `HasSelection()` / `GetSelectedText()` | `Boolean()` / `String()` | Query selected text |
| `DeleteSelection()` | `Void()` | Delete selected text |
| `HandleKey(key, shift)` | `Integer(Integer, Boolean)` | Handle arrows, home/end, backspace/delete, and select-all key code 1 |
| `HandleText(text)` | `Integer(String)` | Insert typed text when enabled and focused, ignoring embedded NUL bytes; returns 1 on change |
| `HandleMouseClick(x, y, shift)` / `HandleMouseDrag(x, y)` | `Void(...)` | Focus and update cursor/selection from mouse input |
| `SetCursorColor(color)` | `Void(Integer)` | Set caret color |
| `SetSelectionColor(color)` | `Void(Integer)` | Set selection highlight color |
| `SetBorderColor(color)` / `SetBorderColorFocused(color)` | `Void(Integer)` | Set border colors |
| `SetPasswordMode(enabled)` | `Void(Boolean)` | Draw masked characters while preserving real text |
| `SetPlaceholder(text)` | `Void(String)` | Draw placeholder when empty |
| `SetMaxCodepoints(count)` | `Void(Integer)` | Limit text length; `0` disables the limit |
| `Update(deltaMs)` / `Draw(canvas)` | `Void(...)` | Advance caret blink and render |

Text and placeholder buffers start with a 512-byte reservation and grow on demand. Text
entry is still UTF-8 codepoint aware, and `SetMaxCodepoints(count)` is the user-visible
length policy (`0` disables it).

---

## Zanna.Game.UI.HudTable

Sortable table widget for compact scoreboards, inventories, debug panels, and settings lists.

**Type:** Instance (obj)
**Constructor:** `Table.New(x, y, width, height)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `ColumnCount` | Integer | Read | Number of columns |
| `RowCount` | Integer | Read | Number of rows |
| `SortColumn` | Integer | Read | Current sort column, or -1 |
| `SortDescending` | Boolean | Read | Current sort direction |
| `Scroll` | Integer | Read/Write | First visible row |
| `SelectedRow` | Integer | Read/Write | Selected row, or -1 |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddColumn(title, width, align)` | `Integer(String, Integer, Integer)` | Add a column; `align` is 0 left, 1 center, 2 right |
| `SetColumnSortable(col, sortable, numeric)` | `Void(Integer, Boolean, Boolean)` | Enable string or numeric sorting |
| `AddRow()` / `RemoveRow(row)` / `ClearRows()` | `Integer()` / `Void(...)` | Manage rows |
| `SetCell(row, col, text)` / `GetCell(row, col)` | `Void(...)` / `String(...)` | Manage cell text |
| `SortBy(col, descending)` | `Void(Integer, Boolean)` | Stable sort by a sortable column |
| `HandleClick(x, y)` | `Integer(Integer, Integer)` | Row index on a row hit, `-2` for a header click, `-1` for no hit |
| `HandleScroll(delta)` / `HandleKey(key)` | `Void(Integer)` | Update scroll/selection |
| `Draw(canvas)` | `Void(Canvas)` | Render the table |

`HandleClick(x, y)` performs row selection and sortable-header toggles, then reports the
outcome through its return value: the selected row index, `-2` when a sortable header was
toggled, or `-1` when the point missed the table.

Tables start with room for 16 columns and 512 rows, then grow their column, row, and cell
storage on demand. Cell text is copied into fixed-size per-cell storage and clipped on
UTF-8 codepoint boundaries.

---

## Zanna.Game.UI.HudTableClickResult

Structured result object returned by `Table.HandleClick(x, y)`.

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Kind` | Integer | Read | `0` no hit, `1` row, `2` header |
| `IsNone` | Boolean | Read | True when the click missed table rows and headers |
| `IsRow` | Boolean | Read | True when the click selected a body row |
| `IsHeader` | Boolean | Read | True when the click hit a column header |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `RowOption()` | `Option[Integer]()` | Selected row index, or `None` for header/miss |
| `ColumnOption()` | `Option[Integer]()` | Header column index, or `None` for row/miss |

---

## Zanna.Game.UI.HudModal

Modal dialog with title, content text, buttons, and optional child widgets.

**Type:** Instance (obj)
**Constructor:** `Modal.New(width, height)` or `Modal.NewAt(x, y, width, height)`

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `IsOpen` | Boolean | Read | True while dialog is open |
| `Result` | Integer | Read | Return value from the last triggered button, or -1 |

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetTitle(text)` / `SetContent(text)` | `Void(String)` | Set dialog strings |
| `AddButton(text, value)` | `Integer(String, Integer)` | Add a button and return its index |
| `SetDefaultButton(index)` / `SetCancelButton(index)` | `Void(Integer)` | Configure enter/escape behavior |
| `AddChild(widget)` | `Void(Object)` | Retain and draw a child widget |
| `Open()` / `Close()` | `Void()` | Show or hide the dialog |
| `HandleKey(key, shift)` | `Integer(Integer, Boolean)` | Handle tab, enter, escape, and child text input |
| `HandleClick(x, y)` | `Integer(Integer, Integer)` | Trigger button clicks and child focus |
| `Draw(canvas)` | `Void(Canvas)` | Render overlay, content, children, and buttons |

Modals start with room for 4 buttons and 16 children, then grow those backing arrays on
demand. Child widgets are retained until the modal is released.

---

## Zanna.Game.UI.HudSlider

Integer slider with keyboard and mouse-drag handling.

**Type:** Instance (obj)
**Constructor:** `HudSlider.New(x, y, width, height, min, max)`

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Value` | Integer | Read/Write | Current value, clamped to `[min, max]` and snapped to `Step` |

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetStep(step)` | `Void(Integer)` | Set positive step size; invalid values become 1 |
| `SetLabel(text)` | `Void(String)` | Optional label drawn next to the value |
| `HandleKey(key)` | `Boolean(Integer)` | Arrow/home/end key handling |
| `HandleMouseDown(x, y)` / `HandleMouseDrag(x)` / `HandleMouseUp()` | `Boolean(...)` | Pointer interaction |
| `Draw(canvas)` | `Void(Canvas)` | Render the control |

---

## Zanna.Game.UI.HudDropdown

Selectable option list with keyboard and pointer handling.

**Type:** Instance (obj)
**Constructor:** `HudDropdown.New(x, y, width, height)`

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Selected` | Integer | Read/Write | Selected option index, or -1 |
| `IsOpen` | Boolean | Read | True while option list is open |

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddOption(text)` | `Void(String)` | Append an option |
| `ClearOptions()` | `Void()` | Remove options and reset selection |
| `GetSelectedText()` | `String()` | Return selected option text or empty string |
| `Open()` / `Close()` | `Void()` | Toggle list state |
| `HandleClick(x, y)` / `HandleKey(key)` | `Boolean(...)` | Input handling |
| `Draw(canvas)` | `Void(Canvas)` | Render closed control and open options |

Dropdowns start with room for 32 options and grow on demand. Option labels are copied
into fixed-size option storage and clipped on UTF-8 codepoint boundaries.

---

## Zanna.Game.UI.HudTooltip

Hover-delayed tooltip bubble.

**Type:** Instance (obj)
**Constructor:** `HudTooltip.New()`

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetText(text)` | `Void(String)` | Set tooltip text |
| `SetHoverDelayMs(ms)` | `Void(Integer)` | Delay before display; negative values clamp to 0 |
| `Update(x, y, hovering, deltaMs)` | `Void(Integer, Integer, Boolean, Integer)` | Track hover state and anchor point |
| `Draw(canvas)` | `Void(Canvas)` | Draw when delay has elapsed |

---

## Zanna.Game.UI.HudButton

Standalone styled button for custom game menus and HUD layouts.

**Type:** Instance (obj)
**Constructor:** `GameButton.New(x, y, width, height, text)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `X` | Integer | Read/Write | X position |
| `Y` | Integer | Read/Write | Y position |
| `Width` | Integer | Read | Width in pixels |
| `Height` | Integer | Read | Height in pixels |
| `Visible` | Boolean | Read/Write | Visibility toggle |
| `TextScale` | Integer | Read/Write | Built-in font scale, clamped to 1-16 |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetText(text)` | `void(String)` | Update button text (max 63 bytes, UTF-8-safe truncation; null clears text) |
| `SetSize(width, height)` | `void(Integer, Integer)` | Resize the button |
| `SetColors(normal, selected)` | `void(Integer, Integer)` | Set fill colors for normal and selected states |
| `SetTextColors(normal, selected)` | `void(Integer, Integer)` | Set text colors for normal and selected states |
| `SetBorder(width, color)` | `void(Integer, Integer)` | Set border width and color; width <= 0 disables the border |
| `Draw(canvas, isSelected)` | `void(Canvas, Boolean)` | Draw the button using the selected or normal colorway |

Button text is centered and clipped to the inner button width on UTF-8 codepoint boundaries, so long labels do not draw outside the control. Invalid constructor and `SetSize` width/height values are clamped to 1 pixel. Large widget dimensions are capped at 16384 pixels.

---

## Zanna.Game.Dialogue

Queued typewriter dialogue box with speaker labels, UTF-8-safe reveal timing, word wrapping,
and optional BitmapFont rendering.

**Type:** Instance (obj)
**Constructor:** `Dialogue.New(x, y, width, height)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Speed` | Integer | Write | Reveal speed; values `<= 0` reveal the line instantly |
| `Font` | BitmapFont | Write | Custom font for speaker/body/indicator text (null = built-in 8x8) |
| `TextColor` | Integer | Write | Dialogue body color (0xRRGGBB) |
| `SpeakerColor` | Integer | Write | Speaker label color (0xRRGGBB) |
| `BorderColor` | Integer | Write | Border color (`-1` disables the frame) |
| `Padding` | Integer | Write | Inner padding in pixels |
| `TextScale` | Integer | Write | Integer text scale (minimum 1) |
| `IsActive` | Boolean | Read | True while a line is being shown |
| `IsLineComplete` | Boolean | Read | True when the current line is fully revealed |
| `IsFinished` | Boolean | Read | True once the last line has been acknowledged |
| `IsWaiting` | Boolean | Read | True when waiting for the caller to advance |
| `LineCount` | Integer | Read | Number of queued lines |
| `CurrentLine` | Integer | Read | Zero-based active line index |
| `Speaker` | String | Read | Speaker name of the active line, or empty string |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetBackgroundColor(color, alpha)` | `void(Integer, Integer)` | Set background fill color and alpha |
| `SetPosition(x, y)` | `void(Integer, Integer)` | Move the dialogue box |
| `SetSize(width, height)` | `void(Integer, Integer)` | Resize the dialogue box |
| `Say(speaker, text)` | `void(String, String)` | Queue a spoken line |
| `SayText(text)` | `void(String)` | Queue narration with no speaker label |
| `Clear()` | `void()` | Remove all queued lines and reset playback state |
| `Update(dtMs)` | `void(Integer)` | Advance the typewriter by milliseconds |
| `Advance()` | `void()` | Skip the current line or move to the next one |
| `Skip()` | `void()` | Reveal the current line immediately |
| `Draw(canvas)` | `void(Canvas)` | Render the panel, speaker label, wrapped text, and wait indicator |

### Notes

- Reveal timing counts UTF-8 codepoints, not bytes, so multi-byte characters are revealed as one character.
- The dialogue box stores UTF-8 safely; oversized speaker/text inputs are truncated on codepoint boundaries.
- Position and size setters clamp geometry to finite 32-bit pixel values; width and height stay at least 1.
- Background alpha is clamped to 0-255, padding is non-negative, and non-positive update deltas are ignored.
- One-way use case: `Advance()` behaves like a confirm button. If the line is mid-reveal it completes the line first; only the next call advances the queue.

---

## Usage Example

```zia
bind Zanna.Graphics;
bind Zanna.Game.UI;

// Create a HUD panel + health bar + score label
var hudPanel = Panel.New(0, 0, 320, 48, 0x000000, 180);
var healthBar = Bar.New(12, 12, 150, 20, 0xFF0000, 0x333333);
healthBar.Border = 0xFFFFFF;
healthBar.SetValue(75, 100);

var scoreLabel = HudLabel.New(180, 16, "SCORE: 0", 0xFFFFFF);
scoreLabel.Scale = 2;

// Create a pause menu
var menu = MenuList.New(120, 100, 28);
menu.AddItem("Resume");
menu.AddItem("Options");
menu.AddItem("Quit");

// In the draw loop:
hudPanel.Draw(canvas);
healthBar.Draw(canvas);
scoreLabel.Draw(canvas);

// When paused:
menu.Draw(canvas);

// Handle input:
if upPressed { menu.MoveUp(); }
if downPressed { menu.MoveDown(); }
```

---

## Limits

| Limit | Value |
|-------|-------|
| Label text length | 511 bytes (UTF-8-safe truncation) |
| GameButton text length | 63 bytes (UTF-8-safe truncation) |
| MenuList max items | 64 |
| MenuList item text length | 127 bytes (UTF-8-safe truncation) |
| Dialogue queued lines | 64 |
| Dialogue text payload | 511 bytes (UTF-8-safe truncation) |
| Dialogue speaker label | 63 bytes (UTF-8-safe truncation) |
| HudTextInput text bytes | Dynamic, starts at 512-byte reservation |
| Table rows/columns | Dynamic, starts at 512 rows / 16 columns |
| HudDropdown options | Dynamic, starts at 32 options |
| Modal buttons/children | Dynamic, starts at 4 buttons / 16 children |
| Panel alpha range | 0-255 (clamped) |
| Bar value | Clamped to [0, max] |
| Bar max | Clamped to >= 1 |
| UI widget dimensions | Clamped to 1-16384 pixels |

---

## See Also

- [Canvas & Color](../graphics/canvas.md) — Low-level drawing primitives
- [Fonts](../graphics/fonts.md) — BitmapFont for custom font rendering
- [ScreenFX](effects.md) — Screen shake, flash, and fade effects
